/**
 * @file        WindowQueue_Main.h
 * @brief       LinuxARM-PublicLib-滑动窗口队列-内部数据结构定义头文件
 * @details     IMX6ULL平台
 *              本文件定义滑动窗口队列的内部数据结构，仅供库内部使用。
 *              外部用户通过 include/WindowQueue.h 访问公共API。
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-09
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-09
 * @Version     V1.0.0
 * @brief       创建文件，定义滑动窗口队列内部结构体
 * @author      zlzksrl
 */
#ifndef __WindowQueue_Main_H__
#define __WindowQueue_Main_H__

#ifdef __cplusplus
 extern "C" {
#endif

/* ========================== 标准库头文件 ========================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* 公共头：获取 T_WindowQueueMsg 前向声明、回调类型、统计结构体 */
#include "../include/WindowQueue.h"

/* ========================== 宏定义 ========================== */

/**
 * @brief 队列名称最大长度（不含末尾'\0'），名称缓冲区为 MAX+1 字节
 */
#define MAX_WINDOWQUEUENAME_LEN 32


/* ================================================================== */
/*                                                                    */
/*     WindowQueue - 滑动窗口队列内部结构体                            */
/*                                                                    */
/*  特点:                                                             */
/*  - 有界环形缓冲区，连续内存 size*element_size，值拷贝存储            */
/*  - 满则丢弃最老数据（移动 lget 读指针，零拷贝 O(1)）                 */
/*  - 无条件变量（本队列无阻塞等待需求）                               */
/*                                                                    */
/*  环形缓冲区示意图 (size=8, lget=2, lput=5, nData=3):               */
/*    [0] [1] [2] [3] [4] [5] [6] [7]                                 */
/*          ^get         ^put                                         */
/*          |---数据---|  空闲                                        */
/*                                                                    */
/*  队列满: nData == size（此时 lput 回绕到 lget）                     */
/*  队列空: nData == 0                                                 */
/*                                                                    */
/* ================================================================== */

/**
 * @brief 滑动窗口队列结构体（环形缓冲区，值拷贝实现）
 * @details 数据由用户结构体按值拷贝进来，队列持有副本。
 *          buffer 为连续字节内存，按 lget/lput 环形寻址。
 *          注意：本结构体在公共头中以 opaque pointer 形式前向声明
 *          (typedef struct T_WINDOWQUEUEMSG T_WindowQueueMsg;)，
 *          此处补全其完整定义（不再重复 typedef）。
 */
struct T_WINDOWQUEUEMSG
{
    unsigned char *buffer;     /**< 环形缓冲区，连续内存 size*element_size 字节 */

    int size;                  /**< 容量（元素个数） */
    int element_size;          /**< 单个元素字节数 */
    int lget;                  /**< 读偏移（最老数据位置），[0,size-1]，到 size 回绕 */
    int lput;                  /**< 写偏移（下一个写入位置），[0,size-1]，到 size 回绕 */
    int nData;                 /**< 当前数据条数 [0,size] */

    int init_done;             /**< 初始化完成标志 0/1 */
    int is_closed;             /**< 关闭标志 0/1（关闭后 Put 返回 -2） */

    /* ---- 入队回调（Push 轻量处理） ---- */
    WindowQueuePutCb put_callback;   /**< 入队回调函数，NULL=未注册 */
    void            *put_cb_ctx;     /**< 回调上下文 */
    const void     **view;           /**< 预分配指针数组（size 槽），回调时构建窗口视图 */

    /* ---- 运行统计 ---- */
    unsigned long ulTotalPut;        /**< 累计成功 Put 次数 */
    unsigned long ulTotalDiscarded;  /**< 累计丢弃条数 */
    int           iPeakLength;       /**< 峰值窗口长度 */

    pthread_mutex_t mux;             /**< 互斥锁，保护所有字段并发访问 */

    char name[MAX_WINDOWQUEUENAME_LEN + 1]; /**< 队列名称，调试日志用 */
};


#ifdef __cplusplus
 }
#endif

#endif
