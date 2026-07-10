/**
 * @file        MemoryPool_Main.h
 * @brief       LinuxARM-PublicLib-内存池-内部数据结构定义头文件
 * @details     IMX6ULL平台
 *              本文件定义内存池的内部数据结构，仅供库内部使用。
 *              外部用户通过 include/MemoryPool.h 访问公共API。
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-10
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-10
 * @Version     V1.0.0
 * @brief       创建文件，定义内存池内部结构体
 * @author      zlzksrl
 */
#ifndef __MemoryPool_Main_H__
#define __MemoryPool_Main_H__

#ifdef __cplusplus
 extern "C" {
#endif

/* ========================== 标准库头文件 ========================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>      /* clock_gettime / CLOCK_MONOTONIC */
#include <errno.h>     /* ETIMEDOUT */

/* 公共头：获取 T_MemPool 前向声明、MemPoolMode、配置/统计结构体 */
#include "../include/MemoryPool.h"

/* ========================== 宏定义 ========================== */

/**
 * @brief 队列名称最大长度（不含末尾'\0'）
 */
#define MAX_MEMORYPOOLNAME_LEN 32

/**
 * @brief 槽位对齐粒度
 * @details 取一个含最大对齐需求类型的 union 的 sizeof，作为对齐粒度。
 *          不依赖 C11 max_align_t，跨平台可移植。槽位大小向上补齐到此粒度的倍数，
 *          保证返回地址可存任意类型(double/long long/指针/long double)。
 */
typedef union { long double ld; void *p; long long ll; double d; } mempool_align_t;
#define MEMPOOL_ALIGN ((int)sizeof(mempool_align_t))


/* ================================================================== */
/*                                                                    */
/*     chunk 节点（GROW 扩容用，DROP/BLOCK 也至少一个）               */
/*                                                                    */
/* ================================================================== */

/**
 * @struct T_MEMPOOLCHUNK
 * @brief 一次 malloc 的大内存块节点
 * @details 池的所有内存由若干 chunk 组成（Init 一个 + GROW 扩容的若干），
 *          用链表串起，Destroy 时遍历释放。每个 chunk 含 count 个连续槽位。
 */
typedef struct T_MEMPOOLCHUNK
{
    struct T_MEMPOOLCHUNK *next;   /**< 链表下一块 */
    unsigned char         *mem;    /**< malloc 的连续内存(count × align_size 字节) */
    int                   count;   /**< 本 chunk 的槽位数 */
} T_MemPoolChunk;


/* ================================================================== */
/*                                                                    */
/*     MemoryPool - 内存池内部结构体                                   */
/*                                                                    */
/*  特点:                                                             */
/*  - 预分配连续内存 chunk，分成 align_size 字节槽位                   */
/*  - 空闲链表 LIFO(头取头插)，空闲槽开头内嵌 next 指针(零额外内存)    */
/*  - GROW 扩容的 chunk 串入同一 free_list                            */
/*  - mutex 保护 free_list；BLOCK 模式用 cond(Free signal 唤醒)        */
/*                                                                    */
/* ================================================================== */

/**
 * @brief 内存池结构体（固定大小对象池实现）
 * @details 槽位是 align_size 字节的纯字节块，库不解释内容。空闲槽位开头内嵌 next
 *          指针串成 free_list(LIFO)。本结构体在公共头以 opaque pointer 形式前向声明
 *          (typedef struct T_MEMPOOL T_MemPool;)，此处补全完整定义(不再 typedef)。
 */
struct T_MEMPOOL
{
    /* ---- 配置 ---- */
    int           element_size;   /**< 用户请求的元素大小 */
    int           align_size;     /**< 对齐后实际槽位大小(>=element_size，>=sizeof(void*)) */
    int           init_count;     /**< 初始槽位数 */
    MemPoolMode   mode;           /**< Alloc() 默认模式 */
    int           grow_count;     /**< GROW 每次扩容槽位数 */
    int           block_timeo;    /**< BLOCK 默认阻塞超时(ms)，0=无限 */

    /* ---- 内存块链表 ---- */
    T_MemPoolChunk *chunks;       /**< 所有 chunk(初始 + GROW 扩容)，Destroy 时遍历释放 */

    /* ---- 空闲链表(LIFO，内嵌 next) ---- */
    void         *free_list;      /**< 空闲槽位链表头；空闲槽开头存 next(下一空闲槽或NULL) */
    int           free_count;     /**< 当前空闲槽位数 */
    int           total_count;    /**< 总槽位数(含 GROW 扩容) */

    /* ---- 同步 ---- */
    pthread_mutex_t mux;          /**< 互斥锁，保护 free_list 与所有字段 */
    pthread_cond_t  cond;         /**< 条件变量(BLOCK 模式)：Free 时 signal 唤醒等待者 */

    /* ---- 统计 ---- */
    unsigned long total_alloc;    /**< 累计 Alloc 成功次数 */
    unsigned long total_free;     /**< 累计 Free 归还次数 */
    unsigned long total_drop;     /**< 累计 DROP 丢弃次数(池满返回 NULL) */
    unsigned long total_grow;     /**< 累计 GROW 扩容槽位数 */
    int           peak_used;      /**< 峰值已用槽位数 */

    int           init_done;      /**< 初始化完成标志 0/1 */
    char          name[MAX_MEMORYPOOLNAME_LEN + 1]; /**< 名称，调试日志用 */
};


#ifdef __cplusplus
 }
#endif

#endif
