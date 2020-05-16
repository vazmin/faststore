#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "fastcommon/common_define.h"
#include "fastcommon/sched_thread.h"
#include "fs_fuse_wrapper.h"

#define FS_ATTR_TIMEOUT  1.0
#define FS_ENTRY_TIMEOUT 1.0

static struct fast_mblock_man fh_allocator;

static const char *hello_name = "hello";

static void fill_stat(const FDIRDEntryInfo *dentry, struct stat *stat)
{
    stat->st_ino = dentry->inode;
    stat->st_mode = dentry->stat.mode;
    stat->st_size = dentry->stat.size;
    stat->st_atime = dentry->stat.atime;
    stat->st_mtime = dentry->stat.mtime;
    stat->st_ctime = dentry->stat.ctime;
    stat->st_uid = dentry->stat.uid;
    stat->st_gid = dentry->stat.gid;
    //stat->st_nlink = 2;
}

static void fill_entry_param(const FDIRDEntryInfo *dentry,
        struct fuse_entry_param *param)
{
    memset(param, 0, sizeof(*param));
    param->ino = dentry->inode;
    param->attr_timeout = FS_ATTR_TIMEOUT;
    param->entry_timeout = FS_ENTRY_TIMEOUT;
    fill_stat(dentry, &param->attr);
}

static inline int fs_convert_inode(const fuse_ino_t ino, int64_t *new_inode)
{
    int result;
    static int64_t root_inode = 0;

    if (ino == FUSE_ROOT_ID) {
        if (root_inode == 0) {
            if ((result=fsapi_lookup_inode("/", &root_inode)) != 0) {
                return result;
            }
        }
        *new_inode = root_inode;
    } else {
        *new_inode = ino;
    }
    return 0;
}

static inline void do_reply_attr(fuse_req_t req, FDIRDEntryInfo *dentry)
{
    struct stat stat;
    memset(&stat, 0, sizeof(stat));
    fill_stat(dentry, &stat);
    fuse_reply_attr(req, &stat, FS_ATTR_TIMEOUT);
}

static void fs_do_getattr(fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *fi)
{
    int64_t new_inode;
    FDIRDEntryInfo dentry;

    if (fs_convert_inode(ino, &new_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    fprintf(stderr, "file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", new_inode: %"PRId64", fi: %p\n",
            __LINE__, __FUNCTION__, ino, new_inode, fi);

    if (fsapi_stat_dentry_by_inode(new_inode, &dentry) == 0) {
        do_reply_attr(req, &dentry);
    } else {
        fuse_reply_err(req, ENOENT);
    }
}

void fs_do_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
             int to_set, struct fuse_file_info *fi)
{
    FDIRDEntryInfo dentry;
    FDIRStatModifyFlags options;
    int result;

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fi: %p, sizeof(options): %d",
            __LINE__, __FUNCTION__, ino, fi, (int)sizeof(options));

    options.flags = 0;
    if ((to_set & FUSE_SET_ATTR_MODE)) {
        options.mode = 1;
    }

    if ((to_set & FUSE_SET_ATTR_UID)) {
        options.uid  = 1;
    }

    if ((to_set & FUSE_SET_ATTR_GID)) {
        options.gid  = 1;
    }

    if ((to_set & FUSE_SET_ATTR_SIZE)) {
        options.size  = 1;
    }

    if ((to_set & FUSE_SET_ATTR_CTIME)) {
        options.ctime = 1;
    }

    if ((to_set & FUSE_SET_ATTR_ATIME)) {
        options.atime = 1;
    } else if ((to_set & FUSE_SET_ATTR_ATIME_NOW)) {
        options.atime = 1;
        //attr->st_atime = get_current_time();
    }

    if ((to_set & FUSE_SET_ATTR_MTIME)) {
        options.mtime = 1;
    } else if ((to_set & FUSE_SET_ATTR_MTIME_NOW)) {
        options.mtime = 1;
        //attr->st_mtime = get_current_time();
    }

    logInfo("file: "__FILE__", line: %d, func: %s, flags: %"PRId64", atime bit: %d, mtime bit: %d",
            __LINE__, __FUNCTION__, options.flags,  options.atime, options.mtime);

    if ((result=fsapi_modify_dentry_stat(ino, attr,
                    options.flags, &dentry)) != 0)
    {
        fuse_reply_err(req, ENOENT);
        return;
    }

    do_reply_attr(req, &dentry);
}

static void fs_do_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    int result;
    int64_t parent_inode;
    FDIRDEntryInfo dentry;
    string_t nm;
    struct fuse_entry_param param;

    fprintf(stderr, "parent1: %"PRId64", name: %s\n", parent, name);

    if (fs_convert_inode(parent, &parent_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    fprintf(stderr, "parent2: %"PRId64", name: %s\n", parent_inode, name);

    FC_SET_STRING(nm, (char *)name);
    if ((result=fsapi_stat_dentry_by_pname(parent_inode, &nm, &dentry)) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    fill_entry_param(&dentry, &param);
    fuse_reply_entry(req, &param);
}

struct dirbuf {
	char *p;
	size_t size;
};

static void dirbuf_add(fuse_req_t req, struct dirbuf *b, const char *name,
		       fuse_ino_t ino)
{
	struct stat stbuf;
	size_t oldsize = b->size;
	b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
	b->p = (char *) realloc(b->p, b->size);
	memset(&stbuf, 0, sizeof(stbuf));
	stbuf.st_ino = ino;
	fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf,
			  b->size);
}

#define min(x, y) ((x) < (y) ? (x) : (y))

static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
			     off_t off, size_t maxsize)
{
	if (off < bufsize)
		return fuse_reply_buf(req, buf + off,
				      min(bufsize - off, maxsize));
	else
		return fuse_reply_buf(req, NULL, 0);
}

static void fs_do_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
			     off_t off, struct fuse_file_info *fi)
{
	(void) fi;

	if (ino != 1)
		fuse_reply_err(req, ENOTDIR);
	else {
		struct dirbuf b;

		memset(&b, 0, sizeof(b));
		dirbuf_add(req, &b, ".", 1);
		dirbuf_add(req, &b, "..", 1);
		dirbuf_add(req, &b, hello_name, 2);
		reply_buf_limited(req, b.p, b.size, off, size);
		free(b.p);
	}
}

static int do_open(fuse_req_t req, FDIRDEntryInfo *dentry,
        struct fuse_file_info *fi)
{
    int result;
    FSAPIFileInfo *fh;

    fh = (FSAPIFileInfo *)fast_mblock_alloc_object(&fh_allocator);
    if (fh == NULL) {
        return ENOMEM;
    }

    if ((result=fsapi_open_by_dentry(fh, dentry, fi->flags)) != 0) {
        fast_mblock_free_object(&fh_allocator, fh);
        if (!(result == EISDIR || result == ENOENT)) {
            result = EACCES;
        }
        return result;
    }

    fi->fh = (long)fh;
    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64"\n",
            __LINE__, __FUNCTION__, dentry->inode, fi->fh);

    fi->fh = (long)fh;
    return 0;
}

void fs_do_create(fuse_req_t req, fuse_ino_t parent, const char *name,
        mode_t mode, struct fuse_file_info *fi)
{
    int result;
    int64_t parent_inode;
    string_t nm;
    FDIRDEntryInfo dentry;
    struct fuse_entry_param param;

    if (fs_convert_inode(parent, &parent_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "parent ino: %"PRId64", fi: %p, name: %s",
            __LINE__, __FUNCTION__, parent, fi, name);

    FC_SET_STRING(nm, (char *)name);
    if ((result=fsapi_create_dentry_by_pname(parent_inode, &nm,
                    mode, &dentry)) != 0)
    {
        if (result != EEXIST) {
            result = ENOENT;
        }
        fuse_reply_err(req, result);
        return;
    }

    if ((result=do_open(req, &dentry, fi)) != 0) {
        fuse_reply_err(req, result);
        return;
    }

    fill_entry_param(&dentry, &param);
    fuse_reply_create(req, &param, fi);
}

static void fs_do_open(fuse_req_t req, fuse_ino_t ino,
			  struct fuse_file_info *fi)
{
    int result;
    FDIRDEntryInfo dentry;

    if ((result=fsapi_stat_dentry_by_inode(ino, &dentry)) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if ((result=do_open(req, &dentry, fi)) != 0) {
        fuse_reply_err(req, result);
        return;
    }

    fuse_reply_open(req, fi);
}

static void fs_do_release(fuse_req_t req, fuse_ino_t ino,
             struct fuse_file_info *fi)
{
    int result;
    FSAPIFileInfo *fh;

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64"\n",
            __LINE__, __FUNCTION__, ino, fi->fh);

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh != NULL) {
        result = fsapi_close(fh);
        fast_mblock_free_object(&fh_allocator, fh);
    } else {
        result = EBADF;
    }
    fuse_reply_err(req, result);
}

static void fs_do_read(fuse_req_t req, fuse_ino_t ino, size_t size,
			  off_t offset, struct fuse_file_info *fi)
{
    FSAPIFileInfo *fh;
    int result;
    int read_bytes;
    char fixed_buff[128 * 1024];
    char *buff;
    
    if (size < sizeof(fixed_buff)) {
        buff = fixed_buff;
    } else if ((buff=(char *)malloc(size)) == NULL) {
        logError("file: "__FILE__", line: %d, func: %s, "
                "malloc %d bytes fail", __LINE__, __FUNCTION__, (int)size);
        fuse_reply_err(req, ENOMEM);
        return;
    }

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh == NULL) {
        fuse_reply_err(req, EBADF);
        return;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", size: %"PRId64", offset: %"PRId64,
            __LINE__, __FUNCTION__, ino, size, offset);

    if ((result=fsapi_pread(fh, buff, size, offset, &read_bytes)) != 0) {
        fuse_reply_err(req, result);
        return;
    }

    fuse_reply_buf(req, buff, read_bytes);

    if (buff != fixed_buff) {
        free(buff);
    }
}

void fs_do_write(fuse_req_t req, fuse_ino_t ino, const char *buff,
        size_t size, off_t offset, struct fuse_file_info *fi)
{
    FSAPIFileInfo *fh;
    int result;
    int written_bytes;

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh == NULL) {
        fuse_reply_err(req, EBADF);
        return;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", size: %"PRId64", offset: %"PRId64,
            __LINE__, __FUNCTION__, ino, size, offset);

    if ((result=fsapi_pwrite(fh, buff, size, offset, &written_bytes)) != 0) {
        fuse_reply_err(req, result);
        return;
    }

    fuse_reply_write(req, written_bytes);
}

int fs_fuse_wrapper_init(struct fuse_lowlevel_ops *ops)
{
    int result;
    if ((result=fast_mblock_init_ex2(&fh_allocator, "fuse_fh",
                    sizeof(FSAPIFileInfo), 4096, NULL, NULL,
                    true, NULL, NULL, NULL)) != 0)
    {
        return result;
    }

    ops->lookup  = fs_do_lookup;
    ops->getattr = fs_do_getattr;
    ops->setattr = fs_do_setattr;
    ops->readdir = fs_do_readdir;
    ops->create  = fs_do_create;
    ops->open    = fs_do_open;
    ops->release = fs_do_release;
    ops->read    = fs_do_read;
    ops->write   = fs_do_write;

    return 0;
}