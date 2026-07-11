/**
 * @file        ThreadManage_Main.h
 * @brief       LinuxARM-PublicLib-线程管理-主程序内部头文件
 * @details     IMX6ULL平台
 *              本文件定义线程管理库内部使用的数据结构和宏，
 *              不对外暴露，仅由 src/ 目录下的源文件包含。
 *
 *              包含内容:
 *              1. 调试打印宏 ThreadManage_printx（可编译开关控制）
 *              2. 线程池句柄结构体 T_ThreadPoolHandle（完整定义）
 *
 *              依赖关系:
 *              - include/ThreadManage.h: 公共API类型声明
 *              - KernelLinkedList.h: 内核链表（list_head）
 *              - RedBlackTree.h: 红黑树（rb_root, rb_node）
 *              - ThreadQueue.h: 线程队列（T_ThreadQueueMsg）
 * 
 * @author      zlzksrl
 * @Version     V1.0.1
 * @date        2025-10-01
 * @copyright   copyright (C) 2025
 */
 
/**
 * @date        2025-10-01
 * @Version     V1.0.1
 * @brief       创建文件
 * @author      zlzksrl
 */

#ifndef __ThreadManage_Main_H__
#define __ThreadManage_Main_H__

#ifdef __cplusplus
 extern "C" {
#endif

/* ======================== 标准库头文件 ======================== */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

/* ======================== 项目头文件 ======================== */
/* 公共API头文件（包含 T_ThreadCreateConfig 等公共类型定义） */
#include "../include/ThreadManage.h"

/* 依赖库头文件 */
#include <KernelLinkedList.h>   /* 内核风格侵入式双向循环链表 */
#include <RedBlackTree.h>       /* 内核风格侵入式红黑树 */
#include <ThreadQueue.h>        /* 线程安全环形缓冲区队列 */



/* ======================== 调试宏 ======================== */

/**
 * @macro       ThreadManage_printx
 * @brief       线程管理库调试打印宏
 * @details     格式化输出调试信息，自动附加源文件位置信息（行号、函数名）。
 *              输出格式: [Thread]-[Debug]-[用户消息@line:[行号]@func:[函数名]]
 *
 *              编译开关控制:
 *              - #if 1: 启用调试输出（默认）
 *              - #if 0: 禁用调试输出（编译为空操作，无性能开销）
 *
 *              使用示例:
 *              @code
 *              ThreadManage_printx("create thread [%s] fail, ret = [%d]", name, ret);
 *              // 输出: [Thread]-[Debug]-[create thread [Test1] fail, ret = [22]@line:[42]@func:[ThreadAPI_ThreadCreate]]
 *              @endcode
 *
 * @param       format: printf 兼容的格式字符串
 * @param       ...: 可变参数列表
 * @note        使用 ##__VA_ARGS__ 消除无额外参数时 format 后多余的逗号
 */
#if 1
                
#define ThreadManage_printx(format,...)\
                do\
                {\
                    printf("[Thread]-[Debug]-["format"@line:[%d]@func:[%s]]\r\n",##__VA_ARGS__,__LINE__,__FUNCTION__);\
                }while(0)
#else
                
#define ThreadManage_printx(format,...)\
                do\
                {\
                }while(0)
#endif

/* ======================== 线程池内部类型定义 ======================== */

/**
 * @struct      T_THREADPOOLHANDLE
 * @brief       线程池句柄结构体（完整定义，仅内部可见）
 * @details     线程池的核心数据结构，包含线程池运行所需的全部状态信息。
 *              在公共头文件 include/ThreadManage.h 中仅做前向声明，
 *              完整定义在此内部头文件中，实现了信息隐藏。
 *
 *              数据结构关系:
 *              ┌─────────────────────────────────────────┐
 *              │           T_ThreadPoolHandle             │
 *              │  ┌─────────────────────────────────┐    │
 *              │  │ tConfig (线程池配置参数)          │    │
 *              │  │   iMinNum / iMaxNum / iQueueMaxSize  │
 *              │  └─────────────────────────────────┘    │
 *              │  ┌─────────────────────────────────┐    │
 *              │  │ tWorkerTree (红黑树)              │    │
 *              │  │   管理所有存活工作线程              │    │
 *              │  │   按 pthread_t 索引, O(log n)     │    │
 *              │  └─────────────────────────────────┘    │
 *              │  ┌─────────────────────────────────┐    │
 *              │  │ tBusyList (内核链表)              │    │
 *              │  │   跟踪正在执行任务的线程           │    │
 *              │  │   O(1) 插入/删除                  │    │
 *              │  └─────────────────────────────────┘    │
 *              │  ┌─────────────────────────────────┐    │
 *              │  │ ptTaskQueue (ThreadQueue)        │    │
 *              │  │   线程安全任务队列                 │    │
 *              │  │   存放待执行的 T_PoolTask          │    │
 *              │  └─────────────────────────────────┘    │
 *              │  ┌─────────────────────────────────┐    │
 *              │  │ 同步原语                          │    │
 *              │  │   tMutex / tCond                 │    │
 *              │  └─────────────────────────────────┘    │
 *              │  ┌─────────────────────────────────┐    │
 *              │  │ 状态变量                          │    │
 *              │  │   iShutdown / iLiveThreadNum      │    │
 *              │  │   iBusyThreadNum / iTaskQueueLen  │    │
 *              │  │   uiWorkerSeq                     │    │
 *              │  └─────────────────────────────────┘    │
 *              └─────────────────────────────────────────┘
 *
 * @note        所有状态变量的读写都应在互斥锁(tMutex)保护下进行
 */
typedef struct T_THREADPOOLHANDLE {
    /* ======================== 魔术字（防重复销毁/野指针检测） ======================== */
    unsigned int uiMagic;           /**< 魔术字校验（创建时设为 0x5448504C，销毁时清零） */

    /* ======================== 配置 ======================== */
    T_ThreadPoolConfig tConfig;     /**< 线程池配置（创建时由用户传入，运行期间可由Resize调整） */
    int iIdleTimeoutMs;             /**< 空闲线程超时缩容时间(ms)，从tConfig.iIdleTimeoutMs复制，0=禁用缩容 */

    /* ======================== 线程管理数据结构 ======================== */
    /**
     * 工作线程红黑树（按 pthread_t 索引）
     * - 用途: 管理所有存活的工作线程，支持 O(log n) 查找
     * - 键: T_WorkerNode.tThreadConfig.tThreadPid (pthread_t)
     * - 生命周期: 线程创建时插入，线程退出并 join 后删除
     */
    struct rb_root tWorkerTree;

    /**
     * 忙碌线程链表
     * - 用途: 跟踪当前正在执行任务的工作线程，O(1) 插入/删除
     * - 节点: T_WorkerNode.tBusyEntry
     * - 生命周期: 任务开始时加入，任务完成时移除
     */
    struct list_head tBusyList;

    /**
     * 任务队列（ThreadQueue 线程安全环形缓冲区）
     * - 用途: 存放待执行的 T_PoolTask 任务
     * - 生产者: ThreadAPI_ThreadPoolAddTask
     * - 消费者: ThreadPool_WorkerThread（工作线程）
     * - 特性: 队列满时生产者阻塞，队列空时消费者阻塞
     */
    T_ThreadQueueMsg *ptTaskQueue;

    /* ======================== 同步原语 ======================== */
    pthread_mutex_t tMutex;     /**< 互斥锁（保护所有状态变量的并发访问） */
    pthread_cond_t tCond;       /**< 条件变量（用于WaitAllDone和任务完成通知） */

    /* ======================== 运行状态 ======================== */
    int iShutdown;          /**< 关闭标志（0=运行中, 1=正在关闭） */
    int iLiveThreadNum;     /**< 存活线程数（已创建且未退出的工作线程总数） */
    int iBusyThreadNum;     /**< 忙碌线程数（当前正在执行任务的工作线程数） */
    int iTaskQueueLen;      /**< 任务队列待处理任务数（镜像计数，在池锁保护下维护，避免跨锁查询） */
    unsigned int uiWorkerSeq; /**< 工作线程编号计数器（用于生成唯一线程名，避免使用static） */
    int iExpanding;           /**< 扩容进行中标志（防止并发AddTask重复触发扩容） */
    int iActiveCallers;       /**< 活跃API调用者计数（Destroy等待其归零后再释放资源，防止UAF） */

    /* ======================== 统计信息 ======================== */
    T_ThreadPoolStats tStats;   /**< 线程池统计信息（峰值、任务计数等） */
} T_ThreadPoolHandle;


#ifdef __cplusplus
 }
#endif

#endif