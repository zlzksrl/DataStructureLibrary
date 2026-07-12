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
#include <stdatomic.h>

/* 公共头 */
#include "../include/FileWriter.h"

/* 依赖库头 */
#include <StreamBuffer.h>
#include <ThreadManage.h>

/* ========================== 宏定义 ========================== */

#define MAX_FILEWRITERNAME_LEN      32
#define FW_FORMAT_BUF_SIZE          1024    /* Write 内部栈 buffer 大小（单条日志上限，超长截断） */
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
#define FW_DEFAULT_DESTROY_WAIT_MS  500     /* Destroy 等 in-flight Writer 出保护区的默认超时 */
#define FW_DESTROY_POLL_US          100     /* Destroy spin-wait 的轮询步长(us) */


/* ================================================================== */
/*                                                                    */
/*     FileWriter - 内部结构体                                         */
/*                                                                    */
/*  并发访问约定:                                                     */
/*    - "受 file_lock 保护"字段：读或写前必须持 file_lock；           */
/*      任何以 _locked 结尾的内部函数都假定调用者已持锁。             */
/*    - volatile 字段：跨线程读写，靠 volatile 保证可见性 + 消费线程  */
/*      每轮都会经过 mutex（间接建立内存屏障）。                       */
/*    - StreamBuffer 内部自持锁，可在任意时刻并发调用。                */
/*    - fp/current_* 与 fwrite/查询接口共享 file_lock，因此 IO 抖动    */
/*      期间查询接口会被阻塞——这是明确的设计权衡（简单 > 无锁读）。   */
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
    pthread_mutex_t     file_lock;                         /**< 保护 fp / current_* / file_seq / file_written / current_date / stats */

    /* ---- 依赖库句柄 ---- */
    T_StreamBuffer     *sb;                                /**< StreamBuffer 攒批缓冲 */

    /* ---- 消费线程 ---- */
    pthread_t           thread_id;                         /**< 消费线程 ID（ThreadManage 创建） */
    volatile int        thread_running;                    /**< 线程运行标志 0/1（跨线程访问，volatile） */
    volatile int        shutting_down;                     /**< 关闭标志（Destroy 时置 1，通知线程退出） */

    /* ---- 抗并发销毁（原子字段，无锁访问） ---- */
    atomic_int          ref_count;                         /**< 正在保护区的 Writer 数（含查询/Rotate/Flush）。
                                                                Write 入保护区 fetch_add(1)，出保护区 fetch_sub(1)。
                                                                Init 时初始化为 0（消费线程不计）。 */
    atomic_int          destroying;                        /**< Destroy 已开始，新的保护区进入应拒绝 */
    atomic_int          destroy_pending;                   /**< Phase B：Destroy 已超时放弃等待，
                                                                最后一个出保护区的 Writer 负责最终 free */

    /* ---- 统计信息（受 file_lock 保护） ---- */
    unsigned long       stat_bytes_written;                /**< 累计已写盘字节数（fwrite 成功计数） */
    unsigned long       stat_bytes_lost;                   /**< 累计 fwrite 失败丢失字节数 */
    unsigned long       stat_rotate_count;                 /**< 累计成功轮转次数 */
    unsigned long       stat_rotate_fail;                  /**< 累计轮转失败次数 */

    /* ---- 状态 ---- */
    int                 init_done;                         /**< 初始化完成标志 */
    char                name[MAX_FILEWRITERNAME_LEN + 1];  /**< 实例名称 */
};


#ifdef __cplusplus
 }
#endif

#endif
