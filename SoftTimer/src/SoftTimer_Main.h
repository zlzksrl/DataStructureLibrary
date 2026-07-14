/**
 * @file        SoftTimer_Main.h
 * @brief       LinuxARM-PublicLib-软件定时器-内部数据结构定义头文件
 * @details     IMX6ULL平台
 *              本文件定义 SoftTimer 的内部数据结构，仅供库内部使用。
 *              外部用户通过 include/SoftTimer.h 访问公共API。
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-13
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-13
 * @Version     V1.0.0
 * @brief       创建文件
 * @author      zlzksrl
 */
#ifndef __SoftTimer_Main_H__
#define __SoftTimer_Main_H__

#ifdef __cplusplus
 extern "C" {
#endif

/* ========================== 标准库头文件 ========================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/* 公共头（含 typedef 前向声明与公共类型） */
#include "../include/SoftTimer.h"

/* 显式再声明一次 typedef，让 Main.h 自包含（不依赖 SoftTimer.h 的 include 顺序）。
 * C 允许对同一 struct 名多次 typedef 同名别名。 */
typedef struct T_SOFTTIMERHANDLE T_SoftTimerHandle;
typedef struct T_SOFTTIMERMGR    T_SoftTimerMgr;

/* 依赖库头 */
#include <ThreadManage.h>

/* ========================== 宏定义 ========================== */

#define MAX_SOFTTIMERNAME_LEN       32     /**< 名称最大长度（不含末尾'\0'），缓冲区为 MAX+1 字节 */
#define ST_HEAP_INIT_CAPACITY       16     /**< min-heap 初始容量 */
#define ST_POOL_ADDTASK_RETRY_MS    10     /**< 线程池满时节点回堆退避 ms（>tick 精度，避免忙自旋） */
#define ST_SCHED_PRIO_MAX           99     /**< SCHED_FIFO/RR 优先级上限 */

/**
 * @brief 调试用：wrapper 入口延迟 ms，用于人为拉长 DISPATCHED → RUNNING 的窗口，
 *        使 Delete Path C（DISPATCHED 期间取消）可被确定性触发。
 *        默认 0（关闭）；测试构建时通过 -DST_DEBUG_DISPATCH_DELAY_MS=200 打开。
 */
#ifndef ST_DEBUG_DISPATCH_DELAY_MS
#define ST_DEBUG_DISPATCH_DELAY_MS  0
#endif

/* ========================== 分级日志 ========================== */

/**
 * @brief 日志级别，用户可通过 -DST_LOG_LEVEL=N 裁剪：
 *          0=静默；1=只 ERROR；2=ERROR+WARN(默认)；3=全部(含 INFO)
 *        为不改变现有行为，默认 3。运行时都走 printf，与旧代码等价。
 */
#ifndef ST_LOG_LEVEL
#define ST_LOG_LEVEL  3
#endif

#if ST_LOG_LEVEL >= 1
#define ST_LOGE(fmt, ...)  printf("[SoftTimer][E] " fmt, ##__VA_ARGS__)
#else
#define ST_LOGE(fmt, ...)  do { } while(0)
#endif

#if ST_LOG_LEVEL >= 2
#define ST_LOGW(fmt, ...)  printf("[SoftTimer][W] " fmt, ##__VA_ARGS__)
#else
#define ST_LOGW(fmt, ...)  do { } while(0)
#endif

#if ST_LOG_LEVEL >= 3
#define ST_LOGI(fmt, ...)  printf("[SoftTimer][I] " fmt, ##__VA_ARGS__)
#else
#define ST_LOGI(fmt, ...)  do { } while(0)
#endif


/* ================================================================== */
/*                                                                    */
/*     SoftTimer - 内部结构体                                          */
/*                                                                    */
/*  并发访问约定:                                                     */
/*    - 所有可变字段均受 mgr->mux 保护，无原子字段、无 volatile；      */
/*      任何以 _locked 结尾的内部函数都假定调用者已持锁。             */
/*    - 两个 cond var 全部绑 CLOCK_MONOTONIC（Init 时通过 condattr）。 */
/*    - 死锁避免：cb 内自删除通过 pthread_equal(self, running_tid)    */
/*      检测；自删除路径不 cond_wait，wrapper 收尾释放 node。         */
/*                                                                    */
/* ================================================================== */

/**
 * @brief 单个定时器节点内部状态机
 *
 * 状态迁移图（触发方 / 动作）:
 *
 *                          SetAlarm
 *                             │
 *                             ▼
 *                     ┌────────────────┐
 *                Delete(B)             │  sched:pop+AddTaskTry
 *          ┌─────────►│   ST_IN_HEAP   ├──────────────────┐
 *          │          └───────┬────────┘                  │
 *          │  wrapper:续期回堆 │                          ▼
 *          │      (周期正常)   │                 ┌────────────────┐
 *          │                   │    Delete(C)    │  ST_DISPATCHED │
 *          │                   │  标 canceled ◄──┤                │
 *          │                   │  wrapper 早退   └────────┬───────┘
 *          │                   │                          │ wrapper 进入
 *          │                   │                          ▼
 *          │                   │                 ┌────────────────┐
 *          │                   │                 │   ST_RUNNING   │
 *          │                   │                 │  cb 执行中     │
 *          │                   │                 └──┬────────┬────┘
 *          │                   │       Delete(D1)   │        │ wrapper 完成
 *          │                   │   自删除:立即返回  │        │ (未 canceled)
 *          │                   │   node 转 ORPHANED │        │
 *          │                   │                    ▼        ▼
 *          │                   │           ┌────────────┐  ┌────────┐
 *          │                   │           │ST_ORPHANED │  │ 单次:  │
 *          │                   │           │wrapper 释放│  │ 墓碑化 │
 *          │                   │           │  node      │  │ 周期:  │
 *          │                   │           └────────────┘  │ 回IN_HP│
 *          │                   │           ▲               └────────┘
 *          │                   │           │Delete(D2 stop_flag -2)
 *          │                   │           │(handle 由 wrapper 释放)
 *          │                   │           │
 *          └───────────────────┘
 *          单次完成→handle 墓碑(node==NULL,is_consumed=1); Delete Path A 回收 handle
 *
 * 五条 Delete 路径与状态对照:
 *   Path A : node==NULL (墓碑)         → 仅 free(handle)
 *   Path B : ST_IN_HEAP                 → heap_remove + free(node,handle)
 *   Path C : ST_DISPATCHED              → 标 canceled，wrapper 早退释 node；Delete 释 handle
 *   Path D1: ST_RUNNING & self_tid      → node 转 ORPHANED, node->handle=NULL；立即 free(handle)
 *   Path D2: ST_RUNNING & other_tid     → cv_delete_wait，正常返回后 free(node,handle)
 *   Path D2 -2 分支: 等待期间 stop_flag  → node 转 ORPHANED (handle 保留)；wrapper 收尾时 free 二者
 */
enum E_SoftTimerState
{
    ST_IN_HEAP    = 0,   /**< 在堆中等待触发 */
    ST_DISPATCHED = 1,   /**< 已投递到线程池，wrapper 未开始执行 */
    ST_RUNNING    = 2,   /**< wrapper 正在执行 user_cb */
    ST_ORPHANED   = 3,   /**< 已从 heap/pool 脱离且 Delete 已取走 handle；wrapper 收尾释放 node */
};

/**
 * @brief 单个定时器节点（内部）
 */
struct T_SOFTTIMER
{
    /* ---- 反向指针 ---- */
    T_SoftTimerHandle *handle;          /**< 回指句柄；Delete 分离后置 NULL */
    struct T_SOFTTIMERMGR *mgr;         /**< 回指管理器（wrapper 拿 mgr 用） */

    /* ---- 计时（CLOCK_MONOTONIC） ---- */
    struct timespec deadline;           /**< 下次触发的绝对时刻 */
    struct timespec last_scheduled;     /**< 上一次预定触发时刻（FROM_SCHEDULED 计算用） */
    int             iPeriodMs;          /**< 周期(ms)，0=单次；SetAlarm 后不再修改 */
    E_SoftTimerPeriodMode ePeriodMode;  /**< SetAlarm 后不再修改 */

    /* ---- 用户回调 ---- */
    SoftTimerCb     cb;
    void           *user_ctx;

    /* ---- 状态（均在 mgr->mux 内改） ---- */
    enum E_SoftTimerState state;
    int                   is_canceled;   /**< 1 = Delete 已取消 */
    int                   running_count; /**< 0 或 1；ST_RUNNING 时为 1 */
    pthread_t             running_tid;   /**< 仅 running_count==1 时有效 */
    int                   heap_idx;      /**< 在 mgr->heap 中的索引，-1 = 不在堆中 */

    /* ---- 元数据 ---- */
    char timer_name[MAX_SOFTTIMERNAME_LEN + 1];
};
typedef struct T_SOFTTIMER T_SoftTimer;

/**
 * @brief 用户句柄（内部完整定义）
 */
struct T_SOFTTIMERHANDLE
{
    unsigned long long ullId;           /**< 单调递增序号（日志/统计用） */
    T_SoftTimer       *node;            /**< 内部节点回指；墓碑时为 NULL */
    int                is_consumed;     /**< 1 = 单次已 fire 且 node 已释放（墓碑） */
    T_SoftTimerHandle *next_in_set;     /**< live_set 链表下一节点 */
};

/**
 * @brief 管理器内部结构体
 */
struct T_SOFTTIMERMGR
{
    /* ---- 堆（按 deadline 升序，最小堆） ---- */
    T_SoftTimer     **heap;             /**< 指针数组，heap[0] 为堆顶 */
    int               heap_size;
    int               heap_capacity;

    /* ---- live_set（所有活跃/墓碑 handle） ---- */
    T_SoftTimerHandle *live_head;
    int                live_count;

    /* ---- ID 计数 ---- */
    unsigned long long next_id;

    /* ---- 调度线程 ---- */
    pthread_t         sched_tid;
    int               sched_created;    /**< 1 = 调度线程已创建（Destroy 需 join） */
    int               stop_flag;        /**< 1 = 请求调度线程退出 */

    /* ---- 同步原语 ---- */
    pthread_mutex_t   mux;
    pthread_cond_t    cv_sched;         /**< 调度线程等；SetAlarm/Delete/Destroy 唤醒 */
    pthread_cond_t    cv_delete;        /**< Delete 等 wrapper 完成；wrapper 唤醒 */

    /* ---- 线程池（ThreadManage） ---- */
    T_ThreadPoolHandle *pool;

    /* ---- 运行统计 ---- */
    unsigned long ulTotalSet;
    unsigned long ulTotalFired;
    unsigned long ulTotalCanceled;
    unsigned long ulTotalOverrun;
    int           iActiveCount;
    int           iPeakActive;

    /* ---- 元数据 ---- */
    char name[MAX_SOFTTIMERNAME_LEN + 1];
};


#ifdef __cplusplus
 }
#endif

#endif
