#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "fastcommon/process_ctrl.h"
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/sched_thread.h"
#include "sf/sf_func.h"
#include "sf/sf_global.h"
#include "sf/sf_nio.h"
#include "sf/sf_service.h"
#include "sf/sf_util.h"
#include "common/fs_proto.h"
#include "common/fs_types.h"
#include "server_types.h"
#include "server_global.h"
#include "server_func.h"
#include "service_handler.h"
#include "cluster_handler.h"
#include "replica_handler.h"
#include "cluster_relationship.h"
#include "server_storage.h"
#include "server_binlog.h"
#include "server_replication.h"
#include "server_recovery.h"
#include "dio/trunk_io_thread.h"

static bool daemon_mode = true;
static int setup_server_env(const char *config_filename);
static int setup_mblock_stat_task();

static int init_nio_task(struct fast_task_info *task)
{
    task->connect_timeout = SF_G_CONNECT_TIMEOUT;
    task->network_timeout = SF_G_NETWORK_TIMEOUT;
    ((FSServerTaskArg *)task->arg)->task_version =
        __sync_add_and_fetch(&NEXT_TASK_VERSION, 1);
    return 0;
}

int main(int argc, char *argv[])
{
    char *config_filename;
    char *action;
    char g_pid_filename[MAX_PATH_SIZE];
    pthread_t schedule_tid;
    int wait_count;
    bool stop;
    int result;

    stop = false;
    if (argc < 2) {
        sf_usage(argv[0]);
        return 1;
    }

    config_filename = argv[1];
    log_init2();
    log_set_time_precision(&g_log_context, LOG_TIME_PRECISION_USECOND);

    result = get_base_path_from_conf_file(config_filename,
            SF_G_BASE_PATH, sizeof(SF_G_BASE_PATH));
    if (result != 0) {
        log_destroy();
        return result;
    }

    snprintf(g_pid_filename, sizeof(g_pid_filename), 
             "%s/serverd.pid", SF_G_BASE_PATH);

    sf_parse_daemon_mode_and_action(argc, argv, &daemon_mode, &action);
    result = process_action(g_pid_filename, action, &stop);
    if (result != 0) {
        if (result == EINVAL) {
            sf_usage(argv[0]);
        }
        log_destroy();
        return result;
    }

    if (stop) {
        log_destroy();
        return 0;
    }

    sf_enable_exit_on_oom();
    srand(time(NULL));
    fast_mblock_manager_init();

    //sched_set_delay_params(300, 1024);
    do {
        if ((result=setup_server_env(config_filename)) != 0) {
            break;
        }

        if ((result=sf_startup_schedule(&schedule_tid)) != 0) {
            break;
        }

        if ((result=server_group_info_setup_sync_to_file_task()) != 0) {
            break;
        }

        //sched_print_all_entries();

        if ((result=sf_socket_server()) != 0) {
            break;
        }

        if ((result=sf_socket_server_ex(&REPLICA_SF_CTX)) != 0) {
            break;
        }

        if ((result=sf_socket_server_ex(&CLUSTER_SF_CTX)) != 0) {
            break;
        }

        if ((result=write_to_pid_file(g_pid_filename)) != 0) {
            break;
        }

        if ((result=service_handler_init()) != 0) {
            break;
        }

        if ((result=cluster_handler_init()) != 0) {
            break;
        }

        if ((result=replica_handler_init()) != 0) {
            break;
        }

        if ((result=trunk_io_thread_init()) != 0) {
            break;
        }

        if ((result=server_storage_init()) != 0) {
            break;
        }

        if ((result=server_binlog_init()) != 0) {
            break;
        }

        if ((result=server_replication_init()) != 0) {
            break;
        }

        if ((result=storage_allocator_prealloc_trunk_freelists()) != 0) {
            return result;
        }

        if ((result=server_recovery_init()) != 0) {
            break;
        }

        fs_proto_init();
        //sched_print_all_entries();

        if ((result=cluster_relationship_init()) != 0) {
            break;
        }

        result = sf_service_init_ex2(&CLUSTER_SF_CTX,
                cluster_alloc_thread_extra_data,
                cluster_thread_loop_callback, NULL,
                fs_proto_set_body_length, cluster_deal_task,
                cluster_task_finish_cleanup, cluster_recv_timeout_callback,
                1000, sizeof(FSProtoHeader), sizeof(FSServerTaskArg),
                init_nio_task);
        if (result != 0) {
            break;
        }
        sf_enable_thread_notify_ex(&CLUSTER_SF_CTX, true);
        sf_set_remove_from_ready_list_ex(&CLUSTER_SF_CTX, false);

        result = sf_service_init_ex2(&REPLICA_SF_CTX,
                replica_alloc_thread_extra_data,
                replica_thread_loop_callback, NULL,
                fs_proto_set_body_length, replica_deal_task,
                replica_task_finish_cleanup, replica_recv_timeout_callback,
                1000, sizeof(FSProtoHeader), sizeof(FSServerTaskArg),
                init_nio_task);
        if (result != 0) {
            break;
        }
        sf_enable_thread_notify_ex(&REPLICA_SF_CTX, true);
        sf_set_remove_from_ready_list_ex(&REPLICA_SF_CTX, false);

        result = sf_service_init_ex2(&g_sf_context,
                service_alloc_thread_extra_data, NULL, NULL,
                fs_proto_set_body_length, service_deal_task,
                service_task_finish_cleanup, NULL, 1000,
                sizeof(FSProtoHeader), sizeof(FSServerTaskArg),
                init_nio_task);
        if (result != 0) {
            break;
        }
        sf_set_remove_from_ready_list(false);

        result = replication_common_start();
    } while (0);

    if (result != 0) {
        lcrit("program exit abnomally");
        log_destroy();
        return result;
    }

    setup_mblock_stat_task();
    //sched_print_all_entries();

    sf_accept_loop_ex(&CLUSTER_SF_CTX, false);
    sf_accept_loop_ex(&REPLICA_SF_CTX, false);
    sf_accept_loop();
    if (g_schedule_flag) {
        pthread_kill(schedule_tid, SIGINT);
    }

    trunk_io_thread_terminate();
    server_binlog_terminate();
    server_replication_terminate();
    server_recovery_terminate();

    wait_count = 0;
    while ((SF_G_ALIVE_THREAD_COUNT != 0 || SF_ALIVE_THREAD_COUNT(
                    CLUSTER_SF_CTX) != 0) || g_schedule_flag)
    {
        usleep(10000);
        if (++wait_count > 1000) {
            lwarning("waiting timeout, exit!");
            break;
        }
    }

    server_storage_destroy();
    sf_service_destroy();

    delete_pid_file(g_pid_filename);
    logInfo("file: "__FILE__", line: %d, "
            "program exit normally.\n", __LINE__);
    log_destroy();
    return 0;
}

static int mblock_stat_task_func(void *args)
{
    //fast_mblock_manager_stat_print_ex(false, FAST_MBLOCK_ORDER_BY_ELEMENT_SIZE);
    //fast_mblock_manager_stat_print_ex(true, FAST_MBLOCK_ORDER_BY_ALLOC_BYTES);
    return 0;
}

static int setup_mblock_stat_task()
{
    ScheduleEntry schedule_entry;
    ScheduleArray schedule_array;

    INIT_SCHEDULE_ENTRY(schedule_entry, sched_generate_next_id(),
            0, 0, 0, 300,  mblock_stat_task_func, NULL);

    //schedule_entry.new_thread = true;

    schedule_array.count = 1;
    schedule_array.entries = &schedule_entry;
    return sched_add_entries(&schedule_array);
}

static int setup_server_env(const char *config_filename)
{
    int result;

    sf_set_current_time();
    if ((result=server_load_config(config_filename)) != 0) {
        return result;
    }

    if (daemon_mode) {
        daemon_init(false);
    }
    umask(0);

    result = sf_setup_signal_handler();

    log_set_cache(true);
    return result;
}
