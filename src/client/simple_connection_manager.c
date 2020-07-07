#include <sys/stat.h>
#include <limits.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "client_global.h"
#include "client_func.h"
#include "client_proto.h"
#include "simple_connection_manager.h"

static ConnectionInfo *get_spec_connection(FSClientContext *client_ctx,
        const ConnectionInfo *target, int *err_no)
{
    return conn_pool_get_connection((ConnectionPool *)client_ctx->
            conn_manager.args, target, err_no);
}

static ConnectionInfo *make_connection(FSClientContext *client_ctx,
        FCAddressPtrArray *addr_array, int *err_no)
{
    FCAddressInfo **current;
    FCAddressInfo **addr;
    FCAddressInfo **end;
    ConnectionInfo *conn;

    if (addr_array->count <= 0) {
        *err_no = ENOENT;
        return NULL;
    }

    current = addr_array->addrs + addr_array->index;
    if ((conn=get_spec_connection(client_ctx, &(*current)->conn,
                    err_no)) != NULL)
    {
        return conn;
    }

    if (addr_array->count == 1) {
        return NULL;
    }

    end = addr_array->addrs + addr_array->count;
    for (addr=addr_array->addrs; addr<end; addr++) {
        if (addr == current) {
            continue;
        }

        if ((conn=get_spec_connection(client_ctx, &(*addr)->conn,
                        err_no)) != NULL)
        {
            addr_array->index = addr - addr_array->addrs;
            return conn;
        }
    }

    return NULL;
}

static ConnectionInfo *get_connection(FSClientContext *client_ctx,
        const int data_group_index, int *err_no)
{
    FCServerInfoPtrArray *server_ptr_array;
    FCServerInfo *server;
    ConnectionInfo *conn;
    FCAddressPtrArray *addr_array;
    uint32_t server_hash_code;
    int server_index;
    int i;

    server_ptr_array = &client_ctx->cluster_cfg.data_groups.mappings
        [data_group_index].server_group->server_array;

    server_hash_code = rand();
    server_index = server_hash_code % server_ptr_array->count;
    server = server_ptr_array->servers[server_index];

    addr_array = &FS_CFG_SERVICE_ADDRESS_ARRAY(client_ctx, server);
    if ((conn=make_connection(client_ctx, addr_array, err_no)) != NULL) {
        return conn;
    }
    
    if (server_ptr_array->count > 1) {
        for (i=0; i<server_ptr_array->count; i++) {
            if (i == server_index) {
                continue;
            }

            addr_array = &FS_CFG_SERVICE_ADDRESS_ARRAY(client_ctx,
                    server_ptr_array->servers[i]);
            if ((conn=make_connection(client_ctx, addr_array,
                            err_no)) != NULL)
            {
                return conn;
            }
        }
    }

    logError("file: "__FILE__", line: %d, "
            "data group index: %d, get_connection fail, "
            "configured server count: %d", __LINE__,
            data_group_index, server_ptr_array->count);
    return NULL;
}

#define CM_MASTER_CACHE_CONN(client_ctx, data_group_index) \
    (client_ctx->conn_manager.data_group_array.entries[data_group_index]. \
     master_cache.conn)

static ConnectionInfo *get_master_connection(FSClientContext *client_ctx,
        const int data_group_index, int *err_no)
{
    ConnectionInfo *conn;
    ConnectionInfo mconn;
    FSClientServerEntry master;

    mconn = *CM_MASTER_CACHE_CONN(client_ctx, data_group_index);
    if (mconn.port > 0) {
        if ((conn=get_spec_connection(client_ctx, &mconn, err_no)) != NULL) {
            ((FSConnectionParameters *)conn->args)->data_group_id =
                data_group_index + 1;
        }
        return conn;
    }

    do {
        if ((*err_no=fs_client_proto_get_master(client_ctx,
                        data_group_index, &master)) != 0)
        {
            break;
        }

        if ((conn=get_spec_connection(client_ctx, &master.conn,
                        err_no)) == NULL)
        {
            break;
        }

        ((FSConnectionParameters *)conn->args)->data_group_id =
            data_group_index + 1;
        conn_pool_set_server_info(CM_MASTER_CACHE_CONN(client_ctx,
                    data_group_index), conn->ip_addr, conn->port);
        return conn;
    } while (0);

    logError("file: "__FILE__", line: %d, "
            "get_master_connection fail, errno: %d",
            __LINE__, *err_no);
    return NULL;
}

static ConnectionInfo *get_readable_connection(FSClientContext *client_ctx,
        const int data_group_index, int *err_no)
{
    ConnectionInfo *conn;
    FSClientServerEntry server;

    do {
        if ((*err_no=fs_client_proto_get_readable_server(client_ctx,
                        data_group_index, &server)) != 0)
        {
            break;
        }

        if ((conn=get_spec_connection(client_ctx, &server.conn,
                        err_no)) == NULL)
        {
            break;
        }

        return conn;
    } while (0);

    logError("file: "__FILE__", line: %d, "
            "get_readable_connection fail, errno: %d",
            __LINE__, *err_no);
        return NULL;
}

static void release_connection(FSClientContext *client_ctx,
        ConnectionInfo *conn)
{
    if (((FSConnectionParameters *)conn->args)->data_group_id > 0) {
        ((FSConnectionParameters *)conn->args)->data_group_id = 0;
    }

    conn_pool_close_connection_ex((ConnectionPool *)client_ctx->
            conn_manager.args, conn, false);
}

static void close_connection(FSClientContext *client_ctx,
        ConnectionInfo *conn)
{
    if (((FSConnectionParameters *)conn->args)->data_group_id > 0) {
        int data_group_index;
        data_group_index = ((FSConnectionParameters *)conn->args)->
            data_group_id - 1;
        CM_MASTER_CACHE_CONN(client_ctx, data_group_index)->port = 0;
        ((FSConnectionParameters *)conn->args)->data_group_id = 0;
    }

    conn_pool_close_connection_ex((ConnectionPool *)client_ctx->
            conn_manager.args, conn, true);
}

static const struct fs_connection_parameters *get_connection_params(
        struct fs_client_context *client_ctx, ConnectionInfo *conn)
{
    return (FSConnectionParameters *)conn->args;
}

static int connect_done_callback(ConnectionInfo *conn, void *args)
{
    return fs_client_proto_join_server((FSClientContext *)args, conn,
            (FSConnectionParameters *)conn->args);
}

static int validate_connection_callback(ConnectionInfo *conn, void *args)
{
    FSResponseInfo response;
    int result;
    if ((result=fs_active_test(conn, &response, g_fs_client_vars.
                    network_timeout)) != 0)
    {
        fs_log_network_error(&response, conn, result);
    }

    return result;
}

static int init_data_group_array(FSClientContext *client_ctx,
        FSClientDataGroupArray *data_group_array)
{
    int bytes;
    FSClientDataGroupEntry *entry;
    FSClientDataGroupEntry *end;

    data_group_array->count = FS_DATA_GROUP_COUNT(client_ctx->cluster_cfg);
    bytes = sizeof(FSClientDataGroupEntry) * data_group_array->count;
    data_group_array->entries = (FSClientDataGroupEntry *)fc_malloc(bytes);
    if (data_group_array->entries == NULL) {
        return ENOMEM;
    }
    memset(data_group_array->entries, 0, bytes);

    end = data_group_array->entries + data_group_array->count;
    for (entry=data_group_array->entries; entry<end; entry++) {
        entry->master_cache.conn = &entry->master_cache.holder;
    }

    return 0;
}

int fs_simple_connection_manager_init_ex(FSClientContext *client_ctx,
        FSConnectionManager *conn_manager, const int max_count_per_entry,
        const int max_idle_time)
{
    const int socket_domain = AF_INET;
    int htable_init_capacity;
    ConnectionPool *cp;
    int result;

    if ((result=init_data_group_array(client_ctx, &conn_manager->
                    data_group_array)) != 0)
    {
        return result;
    }

    cp = (ConnectionPool *)fc_malloc(sizeof(ConnectionPool));
    if (cp == NULL) {
        return ENOMEM;
    }

    htable_init_capacity = 4 * FC_SID_SERVER_COUNT(client_ctx->
            cluster_cfg.server_cfg);
    if (htable_init_capacity < 256) {
        htable_init_capacity = 256;
    }
    if ((result=conn_pool_init_ex1(cp, g_fs_client_vars.connect_timeout,
                    max_count_per_entry, max_idle_time, socket_domain,
                    htable_init_capacity, connect_done_callback, client_ctx,
                    validate_connection_callback, NULL,
                    sizeof(FSConnectionParameters))) != 0)
    {
        return result;
    }

    conn_manager->args = cp;
    conn_manager->get_connection = get_connection;
    conn_manager->get_spec_connection = get_spec_connection;
    conn_manager->get_master_connection = get_master_connection;
    conn_manager->get_readable_connection = get_readable_connection;
    conn_manager->release_connection = release_connection;
    conn_manager->close_connection = close_connection;
    conn_manager->get_connection_params = get_connection_params;
    return 0;
}

void fs_simple_connection_manager_destroy(FSConnectionManager *conn_manager)
{
    ConnectionPool *cp;

    if (conn_manager->args != NULL) {
        cp = (ConnectionPool *)conn_manager->args;
        conn_pool_destroy(cp);
        free(cp);
        conn_manager->args = NULL;
    }
}
