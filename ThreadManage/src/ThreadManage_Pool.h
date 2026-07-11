/**
 * @file        ThreadManage_Pool.h
 * @brief       LinuxARM-PublicLib-线程管理-线程池内部头文件
 * @details     IMX6ULL平台
 *              本文件定义线程池内部使用的数据结构和函数声明，
 *              不对外暴露，仅由 ThreadManage_Pool.c 包含。
 *
 *              包含内容:
 *              1. T_PoolTask - 任务结构体（封装任务函数和用户参数）
 *              2. T_WorkerNode - 工作线程节点（红黑树节点 + 忙碌链表节点）
 *              3. 线程池内部函数声明（工作线程入口、红黑树操作等）
 *
 *              数据结构关系:
 *              ┌──────────────── T_WorkerNode ────────────────┐
 *              │ ┌──────────────────────────────────────┐     │
 *              │ │ tThreadConfig (T_ThreadCreateConfig)  │     │
 *              │ │   ├─ tThreadPid (红黑树键)             │     │
 *              │ │   ├─ sThreadName                      │     │
 *              │ │   └─ ...其他线程配置                    │     │
 *              │ └──────────────────────────────────────┘     │
 *              │ tRbNode (红黑树节点，按 tThreadPid 索引)      │
 *              │ tBusyEntry (忙碌链表节点)                     │
 *              │ ptPool (反向引用所属线程池)                    │
 *              │ iIsBusy (忙碌标志)                            │
 *              └──────────────────────────────────────────────┘
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

#ifndef __ThreadManage_Pool_H__
#define __ThreadManage_Pool_H__

#ifdef __cplusplus
 extern "C" {
#endif

/* ======================== 项目头文件 ======================== */
/*
 * 标准库头文件（stdio/unistd/stdlib/pthread/string）已通过
 * ThreadManage_Main.h 间接包含，此处不再重复包含
 */
#include "../include/ThreadManage.h"
#include "ThreadManage_Main.h"


/* ======================== 线程池内部数据结构 ======================== */

/**
 * @struct      T_POOLTASK
 * @brief       任务结构体（通过 ThreadQueue 在生产者和消费者之间传递）
 * @details     封装用户注册的任务函数和对应的参数。
 *              生命周期:
 *              1. ThreadAPI_ThreadPoolAddTask 中 malloc 分配并填充
 *              2. 通过 ThreadQueueAPI_PutMsg 放入任务队列
 *              3. 工作线程通过 ThreadQueueAPI_GetMsg 取出
 *              4. 工作线程执行任务函数后 free 释放
 *              5. 如果线程池销毁时仍有残留任务，由 release_callback 释放
 *
 *              内存管理责任:
 *              - T_PoolTask 本身: 由线程池负责分配和释放
 *              - pUserArg 指向的内存: 由调用者负责管理生命周期
 */
typedef struct T_POOLTASK {
    ThreadFunctionType pTaskFunc;    /**< 任务函数指针（void* (*)(void*)） */
    void *pUserArg;                  /**< 任务函数的用户参数（可为 NULL） */
} T_PoolTask;

/**
 * @struct      T_WORKERNODE
 * @brief       工作线程节点（同时嵌入红黑树节点和忙碌链表节点）
 * @details     每个工作线程对应一个 T_WorkerNode，存储在线程池的红黑树中。
 *              采用侵入式设计，节点嵌入红黑树和链表，无需额外分配索引内存。
 *
 *              红黑树索引:
 *              - 键: tThreadConfig.tThreadPid (pthread_t)
 *              - 用途: 按线程ID快速查找工作线程节点
 *              - 操作: 插入(O(log n)), 删除(O(log n)), 查找(O(log n))
 *
 *              忙碌链表:
 *              - 节点: tBusyEntry
 *              - 用途: 跟踪当前正在执行任务的工作线程
 *              - 操作: 插入(O(1)), 删除(O(1))
 *              - 状态: iIsBusy == 1 时在链表中, iIsBusy == 0 时不在链表中
 *
 *              生命周期:
 *              1. ThreadPool_CreateWorker 中 malloc 分配并初始化
 *              2. 线程创建成功后插入红黑树
 *              3. 工作线程运行期间持续使用
 *              4. 工作线程退出时自行从红黑树删除并 free 释放
 */
typedef struct T_WORKERNODE {
    /**
     * 线程创建配置（含 tThreadPid 作为红黑树键）
     * - tThreadPid: 线程创建后由 pthread_create 填充，作为红黑树查找键
     * - sThreadName: 线程名称（动态分配，需在工作线程退出时释放）
     * - 其他字段: 线程属性配置（栈大小、调度策略等）
     */
    T_ThreadCreateConfig tThreadConfig;

    /**
     * 红黑树节点（嵌入到红黑树中）
     * - 按 tThreadConfig.tThreadPid 索引
     * - 通过 RedBlackTree_entry 宏从 rb_node 反查 T_WorkerNode
     */
    struct rb_node tRbNode;

    /**
     * 忙碌链表节点（嵌入到线程池的 tBusyList 中）
     * - 线程忙碌时: 通过 list_add_tail 加入 tBusyList
     * - 线程空闲时: 通过 list_del_init 从 tBusyList 移除
     * - 使用 list_del_init 确保节点可安全重新使用
     */
    struct list_head tBusyEntry;

    /**
     * 所属线程池句柄（反向引用）
     * - 工作线程通过此指针访问线程池的状态和同步原语
     * - 在 ThreadPool_WorkerThread 中频繁使用
     */
    T_ThreadPoolHandle *ptPool;

    /**
     * 忙碌标志
     * - 0: 空闲（等待任务）
     * - 1: 忙碌（正在执行任务）
     * - 与 tBusyEntry 同步: iIsBusy==1 时 tBusyEntry 在链表中
     */
    int iIsBusy;

    /**
     * 退出标志（缩容机制使用）
     * - 0: 正常运行
     * - 1: 被标记为需要退出（空闲超时或动态缩容）
     * - 工作线程在主循环中检查此标志，为1时主动退出
     */
    int iExiting;

    /**
     * 注册完成标志（防止创建者与工作线程之间的竞态）
     * - 0: 创建者尚未将节点插入红黑树
     * - 1: 创建者已完成红黑树插入，工作线程可安全进入主循环
     * - 工作线程在入口处等待此标志变为1，确保节点在红黑树中后才检查 shutdown
     */
    int iRegistered;
} T_WorkerNode;


/* ======================== 线程池内部函数声明 ======================== */

/**
 * @func        ThreadPool_WorkerThread
 * @brief       工作线程入口函数（所有工作线程共用）
 * @details     工作线程主循环:
 *              1. 从 ThreadQueue 获取任务（带100ms超时阻塞等待）
 *              2. 标记自身为忙碌状态（加入忙碌链表）
 *              3. 执行任务函数
 *              4. 标记自身为空闲状态（从忙碌链表移除）
 *              5. 检测关闭标志，若已关闭则退出循环
 *              6. 退出时递减存活线程计数并清理忙碌状态
 * @param[in]   pArg: T_WorkerNode 指针（工作线程自身的节点）
 * @return      void* 始终返回 NULL
 * @note        此函数由 ThreadAPI_ThreadCreate 在创建工作线程时注册为入口函数
 */
void *ThreadPool_WorkerThread(void *pArg);

/**
 * @func        ThreadPool_WorkerCmp
 * @brief       红黑树节点比较函数（按 pthread_t 比较）
 * @details     将 pthread_t 转为 unsigned long 进行数值比较，
 *              用于红黑树的查找、插入和删除操作。
 *              Linux 平台 pthread_t 为 unsigned long 类型。
 * @param[in]   a: 红黑树节点a（对应 T_WorkerNode.tRbNode）
 * @param[in]   b: 红黑树节点b（对应 T_WorkerNode.tRbNode）
 * @param[in]   pArg: 未使用的用户参数（传入 NULL）
 * @return      比较结果
 * @retval      <0: a的pthread_t < b的pthread_t
 * @retval      >0: a的pthread_t > b的pthread_t
 * @retval       0: 两个pthread_t相等
 */
int ThreadPool_WorkerCmp(struct rb_node *a, struct rb_node *b, void *pArg);

/**
 * @func        ThreadPool_FindWorker
 * @brief       通过 T_ThreadCreateConfig 在红黑树中查找工作线程节点
 * @details     构造临时查询节点（仅填充 tThreadPid），
 *              利用 RedBlackTree_search 进行 O(log n) 查找。
 * @param[in]   ptPool: 线程池句柄（包含红黑树根）
 * @param[in]   ptThreadConfig: 线程配置（使用 tThreadPid 作为查找键）
 * @return      T_WorkerNode 指针
 * @retval      非 NULL: 找到的工作线程节点
 * @retval      NULL: 未找到或参数无效
 */
T_WorkerNode *ThreadPool_FindWorker(T_ThreadPoolHandle *ptPool,
                                     T_ThreadCreateConfig *ptThreadConfig);

/**
 * @func        ThreadPool_CreateWorker
 * @brief       创建并启动一个工作线程
 * @details     完整流程:
 *              1. 原子化检查线程数上限并预留槽位
 *              2. 分配 T_WorkerNode 并初始化
 *              3. 配置线程名称（自增序列号避免重名）
 *              4. 调用 ThreadAPI_ThreadCreate 创建线程
 *              5. 将节点插入红黑树
 *              创建失败时会自动回退预留的线程计数槽位。
 * @param[in]   ptPool: 线程池句柄
 * @return      int
 * @retval       0: 成功
 * @retval      -1: 失败（已达上限、内存不足、线程创建失败）
 */
int ThreadPool_CreateWorker(T_ThreadPoolHandle *ptPool);


#ifdef __cplusplus
 }
#endif

#endif