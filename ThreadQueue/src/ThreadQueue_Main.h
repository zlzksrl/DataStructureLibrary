/**
 * @file        ThreadQueue_Main.h
 * @brief       LinuxARM-PublicLib-线程通讯队列-内部数据结构定义头文件
 * @details     IMX6ULL平台
 *              本文件定义了线程队列和最新数据队列的内部数据结构，
 *              仅供库内部实现使用，外部用户通过 ThreadQueue.h 访问公共API。
 *
 * @author      zlzksrl
 * @Version     V1.1.0
 * @date        2026-05-07
 * @copyright   copyright (C) 2024
 */

/**
 * @date        2024-08-15
 * @Version     V1.0.0
 * @brief       创建文件
 * @author      zlzksrl
 *
 * @date        2026-05-07
 * @Version     V1.1.0
 * @brief       新增 is_closed 队列关闭标志;
 *              新增 T_LatestQueueMsg 最新数据队列结构体;
 *              新增 #include <sys/time.h> 修复 timeval 不完整类型问题
 * @author      zlzksrl
 */
#ifndef __ThreadQueue_Main_H__
#define __ThreadQueue_Main_H__

#ifdef __cplusplus
 extern "C" {
#endif

/* ========================== 标准库头文件 ========================== */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>    /* gettimeofday() 所需头文件 */

/* ========================== 宏定义 ========================== */

/**
 * @brief 队列名称的最大长度（不含末尾'\0'）
 *        队列名称存储缓冲区为 MAX_THREADQUEUENAME_LEN+1 字节
 */
#define MAX_THREADQUEUENAME_LEN 32

/* ================================================================== */
/*                                                                    */
/*              ThreadQueue - 环形缓冲区线程安全队列                     */
/*                                                                    */
/*  特点:                                                             */
/*  - 固定大小的环形缓冲区，支持多生产者多消费者                          */
/*  - 队列满时生产者阻塞，队列空时消费者阻塞                              */
/*  - 支持关闭/重新打开/刷新/销毁操作                                    */
/*  - 关闭后生产者无法写入，消费者可继续读取剩余数据                       */
/*                                                                    */
/* ================================================================== */

/**
 * @brief 线程队列结构体（环形缓冲区实现）
 *
 * 数据流向:  生产者 -> [buffer环形缓冲区] -> 消费者
 *
 * 环形缓冲区示意图 (size=8, lget=2, lput=5, nData=3):
 *   [0] [1] [2] [3] [4] [5] [6] [7]
 *            ^get         ^put
 *         已读  |---数据---|  空闲
 *
 * 队列满条件: lget == lput && nData > 0
 * 队列空条件: lget == lput && nData == 0
 */
typedef struct T_THREADQUEUEMSG
{
    void** buffer;          /**< 环形缓冲区，存储 void* 指针数组
                                 每个元素指向用户数据，由用户负责分配/释放 */

    int size;               /**< 环形缓冲区总容量（元素个数）
                                 建议设置稍大的值以减少生产者阻塞进入内核态的次数 */

    int lget;               /**< 取数据偏移量（消费者读取位置）
                                 范围: [0, size-1]，到达 size 时回绕为 0 */

    int lput;               /**< 放数据偏移量（生产者写入位置）
                                 范围: [0, size-1]，到达 size 时回绕为 0 */

    int nData;              /**< 缓冲区中当前数据个数
                                 用于判断队列满/空:
                                 - nData == size → 队列满
                                 - nData == 0    → 队列空 */

    int nFullThread;        /**< 因队列满而阻塞在 cond_put 的生产者线程数量
                                 用于: 唤醒决策（仅当有等待线程时才 signal） */

    int nEmptyThread;       /**< 因队列空而阻塞在 cond_get 的消费者线程数量
                                 用于: 唤醒决策（仅当有等待线程时才 signal） */

    int init_done;          /**< 初始化完成标志
                                 0: 未初始化或已销毁
                                 1: 已初始化，可正常使用 */

    int is_closed;          /**< 队列关闭标志
                                 0: 正常运行状态
                                 1: 已关闭，阻止新数据写入(Put返回-2)，
                                    但允许消费者读取剩余数据(Get直到队列为空) */

    pthread_mutex_t mux;                /**< 互斥锁，保护所有字段的并发访问 */
    pthread_cond_t cond_get, cond_put;  /**< 条件变量:
                                             cond_get: 消费者等待数据可用
                                             cond_put: 生产者等待队列有空间 */

    /**
     * @brief 数据释放回调函数
     *
     * 当 ThreadQueueAPI_DestroyMsg() 发现队列中有残留数据时调用此回调逐条释放。
     * 如果设置为 NULL，则残留数据被直接丢弃（打印警告），可能导致内存泄漏。
     *
     * 典型用法:
     * @code
     *   void my_release(void* data) { free(data); }
     *   ThreadQueueAPI_InitMsg(&queue, 100, "myQueue", my_release);
     * @endcode
     */
    void (*release_callback)(void* data);

    char name[MAX_THREADQUEUENAME_LEN+1]; /**< 队列名称，用于调试日志标识 */
} T_ThreadQueueMsg;


/* ================================================================== */
/*                                                                    */
/*              LatestQueue - 最新数据队列（只保留最新一条）              */
/*                                                                    */
/*  特点:                                                             */
/*  - 不使用环形缓冲区，仅保存一条最新的 void* 数据指针                   */
/*  - Put时如果存在未读旧数据，通过回调自动释放，避免内存泄漏              */
/*  - 消费者 Get 到的始终是最新数据，不会读到过期内容                     */
/*  - 适用于传感器数据、状态信息等只关心最新值的场景                       */
/*                                                                    */
/* ================================================================== */

/**
 * @brief 最新数据队列结构体（仅保留最新一条数据）
 *
 * 数据流向:  生产者 -> [data单条存储] -> 消费者
 *
 * 工作原理:
 *   1. 生产者 Put 时，如果 has_data==1（有未读旧数据），
 *      先通过 release_callback 释放旧数据，再存入新数据
 *   2. 消费者 Get 时，取出 data 并清空 has_data
 *   3. 如果没有数据(has_data==0)，消费者阻塞等待 cond_get
 */
typedef struct T_LATESTQUEUEMSG
{
    void *data;             /**< 当前最新数据指针（仅保存一条）
                                 NULL 表示无数据 */

    int has_data;           /**< 是否有未读数据标志
                                 0: 无数据（data无效）
                                 1: 有数据（data指向有效数据） */

    int is_closed;          /**< 队列关闭标志（同 T_ThreadQueueMsg） */

    int init_done;          /**< 初始化完成标志（同 T_ThreadQueueMsg） */

    int nEmptyThread;       /**< 因无数据而阻塞在 cond_get 的消费者线程数量
                                 注意: LatestQueue 不会因"满"而阻塞，
                                 所以不需要 nFullThread */

    pthread_mutex_t mux;    /**< 互斥锁，保护所有字段的并发访问 */

    pthread_cond_t cond_get;/**< 条件变量: 消费者等待新数据可用 */

    /**
     * @brief 数据释放回调函数
     *
     * 当 ThreadQueueAPI_Latest_PutMsg() 发现存在未读旧数据时调用此回调释放旧数据。
     * 如果设置为 NULL，则旧数据被直接丢弃（打印警告），可能导致内存泄漏。
     *
     * 典型用法:
     * @code
     *   void my_release(void* data) { free(data); }
     *   ThreadQueueAPI_Latest_InitMsg(&queue, "sensor", my_release);
     * @endcode
     */
    void (*release_callback)(void* data);

    char name[MAX_THREADQUEUENAME_LEN+1]; /**< 队列名称，用于调试日志标识 */
} T_LatestQueueMsg;


#ifdef __cplusplus
 }
#endif

#endif
