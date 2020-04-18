#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/fast_mblock.h"
#include "fastcommon/sched_thread.h"
#include "sf/sf_global.h"
#include "../server_global.h"
#include "trunk_prealloc.h"
#include "trunk_allocator.h"

static bool g_allocator_inited = false;
static struct fast_mblock_man g_trunk_allocator;
static struct fast_mblock_man g_free_node_allocator;
static UniqSkiplistFactory g_skiplist_factory;

static int compare_trunk_info(const void *p1, const void *p2)
{
    int64_t sub;
    sub = ((FSTrunkFileInfo *)p1)->id_info.id -
        ((FSTrunkFileInfo *)p2)->id_info.id;
    if (sub < 0) {
        return -1;
    } else if (sub > 0) {
        return 1;
    } else {
        return 0;
    }
}

static void trunk_free_func(void *ptr, const int delay_seconds)
{
    FSTrunkFileInfo *trunk_info;
    trunk_info = (FSTrunkFileInfo *)ptr;

    if (delay_seconds > 0) {
        fast_mblock_delay_free_object(&g_trunk_allocator, trunk_info,
                delay_seconds);
    } else {
        fast_mblock_free_object(&g_trunk_allocator, trunk_info);
    }
}

int trunk_allocator_init(FSTrunkAllocator *allocator,
        FSStoragePathInfo *path_info)
{
    int result;
    int bytes;

    if (!g_allocator_inited) {
        g_allocator_inited = true;
        if ((result=fast_mblock_init_ex2(&g_trunk_allocator,
                        "trunk_file_info", sizeof(FSTrunkFileInfo),
                        16384, NULL, NULL, true, NULL, NULL, NULL)) != 0)
        {
            return result;
        }

        if ((result=fast_mblock_init_ex2(&g_free_node_allocator,
                        "trunk_free_node", sizeof(FSTrunkFreeNode),
                        8 * 1024, NULL, NULL, true, NULL, NULL, NULL)) != 0)
        {
            return result;
        }

        if ((result=uniq_skiplist_init_ex(&g_skiplist_factory,
                        FS_TRUNK_SKIPLIST_MAX_LEVEL_COUNT,
                        compare_trunk_info, trunk_free_func, STORAGE_CFG.
                        store_path.count + STORAGE_CFG.write_cache.count,
                        SKIPLIST_DEFAULT_MIN_ALLOC_ELEMENTS_ONCE,
                        FS_TRUNK_SKIPLIST_DELAY_FREE_SECONDS)) != 0)
        {
            return result;
        }
    }

    if ((result=init_pthread_lock(&allocator->lock)) != 0) {
        logError("file: "__FILE__", line: %d, "
                "init_pthread_lock fail, errno: %d, error info: %s",
                __LINE__, result, STRERROR(result));
        return result;
    }
        
    if ((result=pthread_cond_init(&allocator->cond, NULL)) != 0) {
        logError("file: "__FILE__", line: %d, "
                "pthread_cond_init fail, errno: %d, error info: %s",
                __LINE__, result, STRERROR(result));
        return result;
    }

    if ((allocator->sl_trunks=uniq_skiplist_new(&g_skiplist_factory,
            FS_TRUNK_SKIPLIST_INIT_LEVEL_COUNT)) == NULL)
    {
        return ENOMEM;
    }

    bytes = sizeof(FSTrunkFreelist) * path_info->write_thread_count;
    allocator->freelists = (FSTrunkFreelist *)malloc(bytes);
    if (allocator->freelists == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }
    memset(allocator->freelists, 0, bytes);

    allocator->priority_array.alloc = allocator->priority_array.count = 0;
    allocator->priority_array.trunks = NULL;
    allocator->path_info = path_info;
    return 0;
}

int trunk_allocator_add(FSTrunkAllocator *allocator,
        const FSTrunkIdInfo *id_info, const int64_t size,
        FSTrunkFileInfo **pp_trunk)
{
    FSTrunkFileInfo *trunk_info;
    int result;

    trunk_info = (FSTrunkFileInfo *)fast_mblock_alloc_object(
            &g_trunk_allocator);
    if (trunk_info == NULL) {
        if (pp_trunk != NULL) {
            *pp_trunk = NULL;
        }
        return ENOMEM;
    }

    trunk_info->status = FS_TRUNK_STATUS_NONE;
    trunk_info->id_info = *id_info;
    trunk_info->size = size;
    trunk_info->used.bytes = 0;
    trunk_info->used.count = 0;
    trunk_info->free_start = 0;

    pthread_mutex_lock(&allocator->lock);
    result = uniq_skiplist_insert(allocator->sl_trunks, trunk_info);
    pthread_mutex_unlock(&allocator->lock);

    if (result != 0) {
        fast_mblock_free_object(&g_trunk_allocator, trunk_info);
        trunk_info = NULL;
    }
    if (pp_trunk != NULL) {
        *pp_trunk = trunk_info;
    }
    return result;
}

int trunk_allocator_delete(FSTrunkAllocator *allocator, const int64_t id)
{
    FSTrunkFileInfo target;
    int result;

    target.id_info.id = id;
    pthread_mutex_lock(&allocator->lock);
    result = uniq_skiplist_delete(allocator->sl_trunks, &target);
    pthread_mutex_unlock(&allocator->lock);

    return result;
}

static int alloc_trunk(FSTrunkAllocator *allocator,
        FSTrunkFreelist *freelist)
{
    //int result;

    //TODO reclaim
    return 0;
}

static inline void trunk_to_space(FSTrunkFileInfo *trunk_info,
        FSTrunkSpaceInfo *space_info, const int size)
{
    space_info->id_info = trunk_info->id_info;
    space_info->offset = trunk_info->free_start;
    space_info->size = size;

    trunk_info->free_start += size;
    trunk_info->used.bytes += size;
    trunk_info->used.count++;
}

#define TRUNK_ALLOC_SPACE(allocator, trunk_info, space_info, size) \
    do { \
        space_info->store = &allocator->path_info->store; \
        trunk_to_space(trunk_info, space_info, size);   \
    } while (0)

static void remove_trunk_from_freelist(FSTrunkAllocator *allocator,
        FSTrunkFreelist *freelist)
{
    FSTrunkFreeNode *node;

    node = freelist->head;
    node->trunk_info->status = FS_TRUNK_STATUS_NONE;
    freelist->head = freelist->head->next;
    if (freelist->head == NULL) {
        freelist->tail = NULL;
    }
    freelist->count--;

    fast_mblock_free_object(&g_free_node_allocator, node);
    trunk_prealloc_push(allocator, freelist, allocator->path_info->
            prealloc_trunks);
}

void trunk_allocator_prealloc_trunks(FSTrunkAllocator *allocator)
{
    FSTrunkFreelist *freelist;
    FSTrunkFreelist *end;
    int count;
    int i;

    end = allocator->freelists + allocator->path_info->write_thread_count;
    for (freelist=allocator->freelists; freelist<end; freelist++) {
        count = allocator->path_info->prealloc_trunks - freelist->count;
        logInfo("%s prealloc count: %d", allocator->path_info->store.path.str, count);
        for (i=0; i<count; i++) {
            trunk_prealloc_push(allocator, freelist, allocator->path_info->
                    prealloc_trunks);
        }
    }
}

void trunk_allocator_add_to_freelist(FSTrunkAllocator *allocator,
        FSTrunkFreelist *freelist, FSTrunkFileInfo *trunk_info)
{
    FSTrunkFreeNode *node;
    bool notify;

    node = (FSTrunkFreeNode *)fast_mblock_alloc_object(
            &g_free_node_allocator);
    if (node == NULL) {
        return;
    }

    node->trunk_info = trunk_info;
    node->next = NULL;

    pthread_mutex_lock(&allocator->lock);
    if (freelist->head == NULL) {
        freelist->head = node;
        notify = true;
    } else {
        freelist->tail->next = node;
        notify = false;
    }
    freelist->tail = node;

    freelist->count++;
    trunk_info->status = FS_TRUNK_STATUS_ALLOCING;
    pthread_mutex_unlock(&allocator->lock);

    if (notify) {
        pthread_cond_signal(&allocator->cond);
    }
}

void trunk_allocator_array_to_freelists(FSTrunkAllocator *allocator,
        const FSTrunkInfoPtrArray *trunk_ptr_array)
{
    FSTrunkFileInfo **pp;
    FSTrunkFileInfo **end;

    end = trunk_ptr_array->trunks + trunk_ptr_array->count;
    for (pp=trunk_ptr_array->trunks; pp<end; pp++) {
        trunk_allocator_add_to_freelist(allocator,
                allocator->freelists + (pp - trunk_ptr_array->trunks) %
                allocator->path_info->write_thread_count, *pp);
    }
}

int trunk_allocator_alloc(FSTrunkAllocator *allocator,
        const uint32_t blk_hc, const int size,
        FSTrunkSpaceInfo *spaces, int *count)
{
    int aligned_size;
    int result;
    int remain_bytes;
    FSTrunkFreelist *freelist;
    FSTrunkSpaceInfo *space_info;
    FSTrunkFileInfo *trunk_info;

    aligned_size = MEM_ALIGN(size);
    space_info = spaces;

    pthread_mutex_lock(&allocator->lock);
    freelist = allocator->freelists + blk_hc % allocator->
        path_info->write_thread_count;

    if (freelist->head != NULL) {
        trunk_info = freelist->head->trunk_info;
        remain_bytes = trunk_info->size - trunk_info->free_start;
        if (remain_bytes < aligned_size) {
            TRUNK_ALLOC_SPACE(allocator, trunk_info,
                    space_info, remain_bytes);
            space_info++;

            aligned_size -= remain_bytes;
            remove_trunk_from_freelist(allocator, freelist);
       }
    }

    if (freelist->head != NULL) {
        result = 0;
    } else {
        result = alloc_trunk(allocator, freelist);
    }

    if (result == 0) {
        TRUNK_ALLOC_SPACE(allocator, freelist->head->trunk_info,
                space_info, aligned_size);
        space_info++;

        if (freelist->head->trunk_info->size - freelist->head->trunk_info->
                free_start < STORAGE_CFG.discard_remain_space_size)
        {
            remove_trunk_from_freelist(allocator, freelist);
        }
    }
    pthread_mutex_unlock(&allocator->lock);

    *count = space_info - spaces;
    return result;
}

int trunk_allocator_free(FSTrunkAllocator *allocator,
            const int id, const int size)
{
    FSTrunkFileInfo target;
    FSTrunkFileInfo *found;

    target.id_info.id = id;
    pthread_mutex_lock(&allocator->lock);
    if ((found=(FSTrunkFileInfo *)uniq_skiplist_find(
                    allocator->sl_trunks, &target)) != NULL)
    {
        found->used.bytes -= size;
        found->used.count--;
    }
    pthread_mutex_unlock(&allocator->lock);

    return found != NULL ? 0 : ENOENT;
}

static int check_alloc_trunk_ptr_array(FSTrunkInfoPtrArray *parray,
        const int target_count)
{
    int alloc;
    int bytes;
    FSTrunkFileInfo **trunks;

    if (parray->alloc >= target_count) {
        return 0;
    }

    if (parray->alloc == 0) {
        alloc = 64;
    } else {
        alloc = parray->alloc * 2;
    }

    while (alloc < target_count) {
        alloc *= 2;
    }

    bytes = sizeof(FSTrunkFileInfo *) * alloc;
    trunks = (FSTrunkFileInfo **)malloc(bytes);
    if (trunks == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }

    if (parray->trunks != NULL) {
        free(parray->trunks);
    }

    parray->trunks = trunks;
    parray->alloc = alloc;
    return 0;
}

const FSTrunkInfoPtrArray *trunk_allocator_free_size_top_n(
            FSTrunkAllocator *allocator, const int count)
{
    UniqSkiplistIterator it;
    FSTrunkFileInfo *trunk_info;
    FSTrunkFileInfo **pp;
    FSTrunkFileInfo **end;
    int64_t remain_size;

    allocator->priority_array.count = 0;
    if (check_alloc_trunk_ptr_array(&allocator->priority_array, count) != 0) {
        return &allocator->priority_array;
    }

    end = allocator->priority_array.trunks + count;
    uniq_skiplist_iterator(allocator->sl_trunks, &it);
    while ((trunk_info=uniq_skiplist_next(&it)) != NULL) {
        remain_size = trunk_info->size - trunk_info->free_start;
        if (remain_size < FS_FILE_BLOCK_SIZE) {
            continue;
        }
        if ((trunk_info->used.bytes > 0) && ((double)trunk_info->used.bytes /
                    (double)trunk_info->free_start <= 0.80))
        {
            continue;
        }

        if (allocator->priority_array.count < count) {
            allocator->priority_array.trunks[allocator->
                priority_array.count++] = trunk_info;
            continue;
        } else if (remain_size <= allocator->priority_array.trunks[0]->size -
                allocator->priority_array.trunks[0]->free_start)
        {
            continue;
        }

        pp = allocator->priority_array.trunks + 1;
        while ((pp < end) && (remain_size > (*pp)->size - (*pp)->free_start)) {
            *(pp - 1) = *pp;
            pp++;
        }
        *(pp - 1) = trunk_info;
    }

    return &allocator->priority_array;
}
