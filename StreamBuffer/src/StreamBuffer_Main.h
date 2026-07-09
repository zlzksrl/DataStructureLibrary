/**
 * @file        StreamBuffer_Main.h
 * @brief       LinuxARM-PublicLib-流缓冲区-内部数据结构定义头文件
 * @details     IMX6ULL平台
 *              本文件定义流缓冲区的内部数据结构，仅供库内部使用。
 *              外部用户通过 include/StreamBuffer.h 访问公共API。
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-09
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-09
 * @Version     V1.0.0
 * @brief       创建文件，定义流缓冲区内部结构体
 * @author      zlzksrl
 */
#ifndef __StreamBuffer_Main_H__
#define __StreamBuffer_Main_H__

#ifdef __cplusplus
 extern "C" {
#endif

/* ========================== 标准库头文件 ========================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>    /* gettimeofday() 计算条件变量绝对超时 */

/* 公共头：获取 T_StreamBuffer 前向声明、回调类型、配置/统计结构体 */
#include "../include/StreamBuffer.h"

/* ========================== 宏定义 ========================== */

/**
 * @brief 队列名称最大长度（不含末尾'\0'），名称缓冲区为 MAX+1 字节
 */
#define MAX_STREAMBUFFERNAME_LEN 32


/* ================================================================== */
/*                                                                    */
/*     StreamBuffer - 字节流环形缓冲区内部结构体                       */
/*                                                                    */
/*  特点:                                                             */
/*  - 预分配连续内存 capacity 字节，变长字节紧凑存储，环形复用         */
/*  - capacity 强制为 2 的幂，用 mask = capacity-1 做 & 高效回绕        */
/*  - 满则丢新（不阻塞）；Close broadcast 唤醒；Reopen 可逆            */
/*  - 无后台线程，Wait/GetData/GetDataAddress/回调 由用户消费线程调用  */
/*                                                                    */
/*  环形缓冲区示意 (capacity=8, read=2, write=5, used=3):             */
/*    [0][1][2][3][4][5][6][7]                                        */
/*          ^read        ^write                                       */
/*          |--数据(3)--| 空闲(5)                                     */
/*                                                                    */
/*  满: used == capacity；空: used == 0                                */
/*  read/write 推进用 & mask 回绕；跨回绕读写分两段处理                 */
/*                                                                    */
/* ================================================================== */

/**
 * @brief 流缓冲区结构体（字节流环形缓冲实现）
 * @details 数据由用户字节流按值拷贝进来，队列持有副本。buffer 为连续字节内存，
 *          按 read/write 环形寻址。注意：本结构体在公共头中以 opaque pointer
 *          形式前向声明 (typedef struct T_STREAMBUFFER T_StreamBuffer;)，
 *          此处补全其完整定义（不再重复 typedef）。
 */
struct T_STREAMBUFFER
{
    unsigned char *buffer;     /**< 环形缓冲区，连续内存 capacity 字节 */

    int capacity;              /**< 容量(字节)，必须为 2 的幂 */
    int mask;                  /**< capacity-1，用于 & 高效回绕 */
    int flush_bytes;           /**< 触发出队的字节阈值 */
    int read;                  /**< 读偏移(消费者取出位置)，[0,capacity-1] */
    int write;                 /**< 写偏移(生产者写入位置)，[0,capacity-1] */
    int used;                  /**< 当前已用字节数 [0,capacity] */

    int init_done;             /**< 初始化完成标志 0/1 */
    int is_closed;             /**< 关闭标志 0/1（关闭后 PutData 返回 -2，可 Reopen） */

    /* ---- 零拷贝消费回调（可选） ---- */
    StreamBufferConsumeCb consume_cb;   /**< 零拷贝回调函数，NULL=未注册 */
    void                 *cb_ctx;       /**< 回调上下文 */

    /* ---- 运行统计 ---- */
    unsigned long total_put;            /**< 累计成功写入字节数 */
    unsigned long dropped;              /**< 累计因满丢弃字节数 */
    unsigned long consumed;             /**< 累计已出队字节数 */
    int           peak_used;            /**< 峰值已用字节数 */

    pthread_mutex_t mux;                /**< 互斥锁，保护所有字段并发访问 */
    pthread_cond_t  cond;               /**< 条件变量：Wait 等待触发（达阈值/Flush/Close） */

    char name[MAX_STREAMBUFFERNAME_LEN + 1]; /**< 名称，调试日志用 */
};


#ifdef __cplusplus
 }
#endif

#endif
