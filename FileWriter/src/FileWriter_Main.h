/**
 * @file        FileWriter_Main.h
 * @brief       LinuxARM-PublicLib-异步文件写入-内部数据结构定义头文件
 * @details     IMX6ULL平台
 *              本文件定义 FileWriter 的内部数据结构，仅供库内部使用。
 *              外部用户通过 include/FileWriter.h 访问公共API。
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-11
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-11
 * @Version     V1.0.0
 * @brief       创建文件
 * @author      zlzksrl
 */
#ifndef __FileWriter_Main_H__
#define __FileWriter_Main_H__

#ifdef __cplusplus
 extern "C" {
#endif

/* ========================== 标准库头文件 ========================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>

/* 公共头 */
#include "../include/FileWriter.h"

/* 依赖库头 */
#include <StreamBuffer.h>
#include <MemoryPool.h>
#include <ThreadManage.h>

/* ========================== 宏定义 ========================== */

#define MAX_FILEWRITERNAME_LEN      32
#define FW_FORMAT_BUF_SIZE          1024    /* 格式化 buffer 大小（MemoryPool 元素大小） */
#define FW_FORMAT_POOL_COUNT        8       /* MemoryPool 初始槽位数 */
#define FW_DATE_STR_LEN             16      /* "2026_07_11" 长度 */
#define FW_DATETIME_STR_LEN         32      /* "2026-07-11-12-41-30" 长度 */
#define FW_TIMESTAMP_STR_LEN        32      /* "[16:50:44.789550] " 长度 */
#define FW_PATH_MAX                 512     /* 完整路径最大长度 */
#define FW_FILENAME_MAX             128     /* 文件名最大长度 */

/* 默认值（cfg 为 0/空时使用） */
#define FW_DEFAULT_BUFFER_CAPACITY  65536
#define FW_DEFAULT_FLUSH_BYTES      4096
#define FW_DEFAULT_FLUSH_MS         100
#define FW_DEFAULT_THREAD_PRIORITY  20


/* ================================================================== */
/*                                                                    */
/*     FileWriter - 内部结构体                                         */
/*                                                                    */
/* ================================================================== */

/**
 * @brief FileWriter 内部结构体
 */
struct T_FILEWRITER
{
    /* ---- 配置副本（Init 时 memcpy） ---- */
    T_FileWriterConfig  config;                            /**< 配置快照 */
    char                ext[16];                           /**< 实际使用的扩展名（file_ext 非空用 file_ext，否则按 file_type 选） */

    /* ---- 文件状态（受 file_lock 保护） ---- */
    FILE               *fp;                                /**< 当前打开的文件句柄 */
    char                current_filename[FW_FILENAME_MAX]; /**< 当前文件名（不含路径） */
    char                current_dirpath[FW_PATH_MAX];      /**< 当前文件所在目录绝对路径 */
    char                current_filepath[FW_PATH_MAX];     /**< 当前文件绝对路径（含文件名） */
    int                 file_seq;                          /**< 当前文件序号 */
    long                file_written;                      /**< 当前文件已写字节数（用于 max_file_size 轮转判断） */
    char                current_date[FW_DATE_STR_LEN];     /**< 当前日期 "2026_07_11"（跨日检测用） */
    pthread_mutex_t     file_lock;                         /**< 保护 fp / current_* / file_seq / file_written / current_date */

    /* ---- 依赖库句柄 ---- */
    T_StreamBuffer     *sb;                                /**< StreamBuffer 攒批缓冲 */
    T_MemPool          *pool;                              /**< MemoryPool 格式化 buffer 池 */

    /* ---- 消费线程 ---- */
    pthread_t           thread_id;                         /**< 消费线程 ID（ThreadManage 创建） */
    int                 thread_running;                    /**< 线程运行标志 0/1 */
    int                 shutting_down;                     /**< 关闭标志（Destroy 时置 1，通知线程退出） */

    /* ---- 状态 ---- */
    int                 init_done;                         /**< 初始化完成标志 */
    char                name[MAX_FILEWRITERNAME_LEN + 1];  /**< 实例名称 */
};


/* ================================================================== */
/*                                                                    */
/*     内部函数声明                                                   */
/*                                                                    */
/* ================================================================== */

/* ---- 路径与文件管理 ---- */
static int  fw_make_dirs(const char *path);
static int  fw_build_paths_locked(T_FileWriter *fw);
static int  fw_create_file_locked(T_FileWriter *fw);
static int  fw_rotate_locked(T_FileWriter *fw);
static int  fw_check_file_size_rotate_locked(T_FileWriter *fw, int bytes_written);
static int  fw_check_daily_rotate_locked(T_FileWriter *fw);
static int  fw_delete_oldest_locked(T_FileWriter *fw);
static int  fw_get_ext_from_type(FileWriterType type, char *out, int out_len);

/* ---- 时间工具 ---- */
static void fw_get_date_str(char *out, int out_len);
static void fw_get_datetime_str(char *out, int out_len);
static void fw_get_timestamp_str(char *out, int out_len);
static int  fw_date_changed_locked(T_FileWriter *fw);

/* ---- 消费线程 ---- */
static void *fw_consumer_thread(void *arg);


#ifdef __cplusplus
 }
#endif

#endif
