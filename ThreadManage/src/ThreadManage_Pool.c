/**
 * @file        ThreadManage_Pool.c
 * @brief       LinuxARM-PublicLib-线程管理-线程池实现
 * @details     IMX6ULL平台
 *              本文件实现线程池的完整功能，包括创建、销毁、任务添加和状态查询。
 *
 *              架构设计:
 *              - 使用红黑树（RedBlackTree）管理工作线程，按 pthread_t 索引，O(log n) 查找
 *              - 使用内核链表（KernelLinkedList）跟踪忙碌线程，O(1) 插入/删除
 *              - 使用线程队列（ThreadQueue）作为线程安全的任务队列
 *              - 使用 ThreadAPI_ThreadCreate 创建工作线程，支持完整的线程属性配置
 *
 *              线程池工作流程:
 *              1. ThreadAPI_ThreadPoolCreate: 创建线程池，初始化最小数量的工作线程
 *              2. ThreadAPI_ThreadPoolAddTask: 添加任务到队列，必要时动态扩容
 *              3. ThreadPool_WorkerThread: 工作线程主循环，从队列获取并执行任务
 *              4. ThreadAPI_ThreadPoolDestroy: 设置关闭标志，等待所有线程退出，释放资源
 *
 *              动态扩缩容机制:
 *              - 扩容: 当所有存活线程都忙碌且未达到最大线程数时，自动创建新的工作线程
 *              - 缩容: 当线程空闲超过 iIdleTimeoutMs 且存活线程数超过最小线程数时，
 *                空闲线程自动退出并释放资源
 *              - 通过原子化"检查+预留"避免竞态条件
 *
 * @author      zlzksrl
 * @Version     V1.1.0
 * @date        2026-05-08
 * @copyright   copyright (C) 2025
 */
 
/**
 * @date        2025-10-01
 * @Version     V1.0.1
 * @brief       创建文件
 * @author      zlzksrl
 *
 * @date        2026-05-08
 * @Version     V1.1.0
 * @brief       新增: 任务队列长度查询、等待所有任务完成、带超时/非阻塞任务添加、
 *              动态调整线程池大小、线程池统计信息、空闲线程超时缩容机制
 * @author      zlzksrl
 */

#include "ThreadManage_Pool.h"
#include "ThreadManage_Maketime.h"

#include <time.h>


/* ======================== 内部常量定义 ======================== */

/**
 * @macro   THREADPOOL_MAGIC
 * @brief   线程池句柄魔术字（用于检测野指针和重复销毁）
 * @details 值为 0x5448504C（ASCII "THPL"），创建时写入 uiMagic，
 *          销毁时清零。若传入已释放的句柄，uiMagic 不匹配即可检测到。
 */
#define THREADPOOL_MAGIC  0x5448504Cu

/**
 * @macro   IDLE_CHECK_INTERVAL_MS
 * @brief   空闲线程超时检查的基础轮询间隔(毫秒)
 * @details 工作线程使用此值作为 ThreadQueueAPI_GetMsg 的超时参数，
 *          同时作为空闲缩容的最小时间粒度。
 *          空闲线程连续 (iIdleTimeoutMs / IDLE_CHECK_INTERVAL_MS) 次
 *          未获取到任务后触发缩容检查。
 */
#define IDLE_CHECK_INTERVAL_MS  100

/**
 * @func    ThreadPool_Validate
 * @brief   线程池句柄校验函数（检查NULL指针和魔术字，防止野指针/重复销毁）
 * @details 在所有公共API入口处使用，确保传入的句柄有效。
 *          检查两项: 1) 指针非NULL  2) 魔术字匹配 THREADPOOL_MAGIC
 *          校验失败返回 -1，调用者应根据返回值决定是否提前退出。
 *          相比宏实现，内联函数避免了隐藏的 return 语句，使控制流更清晰。
 * @param[in] h: 待校验的线程池句柄指针
 * @return  int 校验结果
 * @retval   0: 句柄有效
 * @retval  -1: 句柄无效（NULL指针或魔术字不匹配）
 */
static inline int ThreadPool_Validate(T_ThreadPoolHandle *h)
{
    if (h == NULL)
    {
        ThreadManage_printx("handle is NULL");
        return -1;
    }
    if (h->uiMagic != THREADPOOL_MAGIC)
    {
        ThreadManage_printx("invalid handle or double destroy (magic=0x%x)", h->uiMagic);
        return -1;
    }
    return 0;
}


/* ======================== 内部静态函数 ======================== */

/**
 * @func        ThreadPool_TaskReleaseCallback
 * @brief       任务释放回调函数
 * @details     当销毁 ThreadQueue 时，如果队列中仍有未处理的残留任务，
 *              ThreadQueue 会调用此回调函数逐条释放任务内存。
 *              此函数会在 ThreadQueueAPI_DestroyMsg 内部被调用。
 *
 *              调用时机:
 *              - ThreadQueueAPI_DestroyMsg 销毁队列时，自动释放队列中的残留 T_PoolTask
 *              - 确保线程池销毁时不会泄漏已分配但未执行的任务内存
 *
 * @param[in]   pData: T_PoolTask 指针（由 ThreadAPI_ThreadPoolAddTask 中 malloc 分配）
 * @warning     此函数仅释放 T_PoolTask 结构体本身，不释放 pUserArg
 *              （pUserArg 的生命周期由调用者管理）
 */
static void ThreadPool_TaskReleaseCallback(void *pData)
{
    if (pData != NULL)
    {
        free(pData);
    }
}


/* ======================== 红黑树比较函数 ======================== */

/**
 * @func         ThreadPool_WorkerCmp
 * @brief        红黑树节点比较函数（按 pthread_t 比较）
 * @details      用于红黑树的查找、插入和删除操作。
 *               将 pthread_t（Linux 下为 unsigned long）转换为无符号长整型进行比较。
 *
 * @param[in]    a: 红黑树节点a（对应 T_WorkerNode.tRbNode）
 * @param[in]    b: 红黑树节点b（对应 T_WorkerNode.tRbNode）
 * @param[in]    pArg: 用户自定义参数（未使用，传入 NULL）
 * @return       比较结果
 * @retval       -1: a节点的pthread_t < b节点的pthread_t
 * @retval        1: a节点的pthread_t > b节点的pthread_t
 * @retval        0: 两个节点的pthread_t相等（同一个线程）
 */
int ThreadPool_WorkerCmp(struct rb_node *a, struct rb_node *b, void *pArg)
{
    /* 从红黑树节点反查宿主结构体 T_WorkerNode */
    T_WorkerNode *pWA = RedBlackTree_entry(a, T_WorkerNode, tRbNode);
    T_WorkerNode *pWB = RedBlackTree_entry(b, T_WorkerNode, tRbNode);
    (void)pArg; /* 消除未使用参数的编译器警告 */

    /* 将 pthread_t 转为 unsigned long 进行数值比较 */
    unsigned long idA = (unsigned long)pWA->tThreadConfig.tThreadPid;
    unsigned long idB = (unsigned long)pWB->tThreadConfig.tThreadPid;

    if (idA < idB) return -1;
    if (idA > idB) return  1;
    return 0;
}


/* ======================== 辅助函数实现 ======================== */

/**
 * @func         ThreadPool_FindWorker
 * @brief        通过 pthread_t 在红黑树中查找工作线程节点
 * @param[in]    ptPool: 线程池句柄（包含红黑树根）
 * @param[in]    ptThreadConfig: 线程配置（使用其中的 tThreadPid 作为查找键）
 * @return       T_WorkerNode 指针
 * @retval       非 NULL: 找到的工作线程节点
 * @retval       NULL: 未找到或参数无效
 */
T_WorkerNode *ThreadPool_FindWorker(T_ThreadPoolHandle *ptPool,
                                     T_ThreadCreateConfig *ptThreadConfig)
{
    if (ptPool == NULL || ptThreadConfig == NULL)
    {
        return NULL;
    }

    /* 构造临时查询节点（栈上分配，仅 tThreadConfig.tThreadPid 有效） */
    T_WorkerNode tKey;
    memset(&tKey, 0, sizeof(T_WorkerNode));
    memcpy(&tKey.tThreadConfig, ptThreadConfig, sizeof(T_ThreadCreateConfig));
    RedBlackTree_init_node(&tKey.tRbNode);

    /* 在红黑树中搜索（O(log n)） */
    struct rb_node *pFound = RedBlackTree_search(
            &ptPool->tWorkerTree, &tKey.tRbNode, ThreadPool_WorkerCmp, NULL);
    if (pFound == NULL)
    {
        return NULL;
    }

    /* 从红黑树节点转换为 T_WorkerNode 指针 */
    return RedBlackTree_entry(pFound, T_WorkerNode, tRbNode);
}


/**
 * @func         ThreadPool_CreateWorker
 * @brief        创建并启动一个工作线程
 * @details      完整的工作线程创建流程:
 *               1. 原子化检查线程数上限并预留槽位（防止竞态条件）
 *               2. 分配并初始化 T_WorkerNode 结构体
 *               3. 配置线程名称（使用自增序列号避免重名）
 *               4. 调用 ThreadAPI_ThreadCreate 创建线程
 *               5. 将工作线程节点插入红黑树
 *
 * @param[in]    ptPool: 线程池句柄
 * @return       int ret
 * @retval       0: 成功创建并启动工作线程
 * @retval       -1: 失败（已达上限、内存不足、线程创建失败）
 */
int ThreadPool_CreateWorker(T_ThreadPoolHandle *ptPool)
{
    if (ptPool == NULL)
    {
        return -1;
    }

    /*
     * 原子化检查并预留线程槽位
     * 在互斥锁保护下同时检查上限并递增计数器，
     * 防止多个线程同时通过检查导致超过最大线程数（TOCTOU竞态）
     */
    pthread_mutex_lock(&ptPool->tMutex);
    /* 检查线程池是否已关闭（防止在关闭期间创建新线程） */
    if (ptPool->iShutdown)
    {
        pthread_mutex_unlock(&ptPool->tMutex);
        return -1;
    }
    if (ptPool->iLiveThreadNum >= ptPool->tConfig.iMaxNum)
    {
        pthread_mutex_unlock(&ptPool->tMutex);
        return -1;
    }
    /* 预留槽位：先递增计数，如果后续创建失败再回退 */
    ptPool->iLiveThreadNum++;
    /* 在锁内预取并递增工作线程序列号，避免并发时序列号重复 */
    unsigned int uiSeq = ptPool->uiWorkerSeq++;
    pthread_mutex_unlock(&ptPool->tMutex);

    /* ======================== 分配工作线程节点 ======================== */
    T_WorkerNode *ptWorker = (T_WorkerNode *)malloc(sizeof(T_WorkerNode));
    if (ptWorker == NULL)
    {
        ThreadManage_printx("malloc T_WorkerNode fail");
        /* 创建失败，回退预留的槽位并清除扩容标志 */
        pthread_mutex_lock(&ptPool->tMutex);
        ptPool->iLiveThreadNum--;
        ptPool->iExpanding = 0;
        pthread_mutex_unlock(&ptPool->tMutex);
        return -1;
    }
    memset(ptWorker, 0, sizeof(T_WorkerNode));

    /* 初始化工作线程节点的各个字段 */
    ptWorker->ptPool    = ptPool;     /* 反向引用所属线程池 */
    ptWorker->iIsBusy   = 0;          /* 初始状态为空闲 */
    ptWorker->iExiting  = 0;          /* 初始状态为正常运行 */
    ptWorker->iRegistered = 0;        /* 尚未注册到红黑树 */
    INIT_LIST_HEAD(&ptWorker->tBusyEntry);    /* 初始化忙碌链表节点 */
    RedBlackTree_init_node(&ptWorker->tRbNode); /* 初始化红黑树节点 */

    /* ======================== 配置线程创建参数 ======================== */
    T_ThreadCreateConfig *ptConfig = &ptWorker->tThreadConfig;
    memset(ptConfig, 0, sizeof(T_ThreadCreateConfig));

    /*
     * 生成唯一线程名称
     * 使用线程池句柄中的 uiWorkerSeq 自增计数器生成序列号，
     * 避免多个工作线程重名（便于调试和日志追踪）
     */
    char *sName = (char *)malloc(32);
    if (sName == NULL)
    {
        ThreadManage_printx("malloc thread name fail");
        free(ptWorker);
        /* 创建失败，回退预留的槽位并清除扩容标志 */
        pthread_mutex_lock(&ptPool->tMutex);
        ptPool->iLiveThreadNum--;
        ptPool->iExpanding = 0;
        pthread_mutex_unlock(&ptPool->tMutex);
        return -1;
    }
    snprintf(sName, 32, "PoolWorker_%u", uiSeq);
    ptConfig->sThreadName = sName;

    /* 设置线程入口函数和参数 */
    ptConfig->pThreadFunc        = ThreadPool_WorkerThread; /* 工作线程主循环函数 */
    ptConfig->pThreadFuncUserArg = ptWorker;  /* 传递自身指针，便于回调时访问 */

    /*
     * 线程属性配置:
     * - eSetAttr = 0: 使用默认属性（不自定义栈大小、调度策略等）
     * - 工作线程在入口函数 ThreadPool_WorkerThread 中立即 pthread_detach，
     *   此处 eDetachState 仅作为默认值占位，实际分离由线程自行完成
     */
    ptConfig->eSetAttr      = 0;
    ptConfig->eDetachState  = PTHREAD_CREATE_JOINABLE;

    /* ======================== 创建线程 ======================== */
    int ret = ThreadAPI_ThreadCreate(ptConfig);
    if (ret != 0)
    {
        ThreadManage_printx("ThreadAPI_ThreadCreate fail ret = [%d]", ret);
        free(sName);
        free(ptWorker);
        /* 创建失败，回退预留的槽位并清除扩容标志 */
        pthread_mutex_lock(&ptPool->tMutex);
        ptPool->iLiveThreadNum--;
        ptPool->iExpanding = 0;
        pthread_mutex_unlock(&ptPool->tMutex);
        return -1;
    }

    /*
     * 线程创建成功，ptConfig->tThreadPid 已被填充
     * 将工作线程节点插入红黑树（按 tThreadPid 索引）
     *
     * 重要: 以下操作全部在互斥锁内完成:
     * 1. RedBlackTree_insert — 将节点插入红黑树
     * 2. 打印日志 — 访问 ptWorker 字段（必须在锁内，防止工作线程提前 free）
     * 3. 设置 iRegistered=1 — 通知工作线程可以进入主循环
     * 4. broadcast — 唤醒等待的工作线程
     *
     * 工作线程在 cond_wait 返回后必须重新获取互斥锁，
     * 因此在 unlock 之前，工作线程无法运行，保证了上述操作的原子性
     */
    pthread_mutex_lock(&ptPool->tMutex);
    RedBlackTree_insert(&ptPool->tWorkerTree, &ptWorker->tRbNode,
                        ThreadPool_WorkerCmp, NULL);

    ThreadManage_printx("Worker [%s][%lu] created, live = [%d]"
            , ptConfig->sThreadName
            , (unsigned long)ptConfig->tThreadPid
            , ptPool->iLiveThreadNum);

    /*
     * 设置注册完成标志并唤醒工作线程
     * 工作线程等待此标志后才检查 shutdown 或进入主循环
     * 这确保了节点一定在红黑树中，工作线程才能安全地 RedBlackTree_delete
     */
    ptWorker->iRegistered = 1;
    /* 清除扩容标志，允许后续 AddTask 再次触发扩容 */
    ptPool->iExpanding = 0;
    pthread_cond_broadcast(&ptPool->tCond);
    pthread_mutex_unlock(&ptPool->tMutex);

    return 0;
}


/* ======================== 工作线程入口函数 ======================== */

/**
 * @func         ThreadPool_WorkerThread
 * @brief        工作线程入口函数（所有工作线程共用）
 * @details      工作线程的主循环逻辑，每个工作线程创建后都进入此函数:
 *
 *               主循环流程:
 *               1. 从 ThreadQueue 获取任务（超时 IDLE_CHECK_INTERVAL_MS）
 *                  ├─ 获取到任务: 执行任务，重置空闲计数
 *                  └─ 超时(NULL): 检查关闭/退出标志，检查空闲缩容
 *               2. 标记自身为忙碌（加入忙碌链表）
 *               3. 执行任务函数
 *               4. 释放任务内存
 *               5. 标记自身为空闲（从忙碌链表移除）
 *               6. 广播条件变量（通知 WaitAllDone）
 *               7. 回到步骤1
 *
 *               空闲缩容机制:
 *               - 当 iIdleTimeoutMs > 0 时启用
 *               - 连续空闲 (iIdleTimeoutMs / IDLE_CHECK_INTERVAL_MS) 次后
 *               - 若 iLiveThreadNum > iMinNum，线程自我移除并退出
 *               - 自我移除的线程会 detach，无需外部 join
 *
 * @param[in]    pArg: T_WorkerNode 指针（工作线程自身的节点）
 * @return       void* 始终返回 NULL
 */
void *ThreadPool_WorkerThread(void *pArg)
{
    T_WorkerNode *ptWorker = (T_WorkerNode *)pArg;
    T_ThreadPoolHandle *ptPool = ptWorker->ptPool;
    int iIdleCount = 0;     /* 连续空闲计数（每单位 = IDLE_CHECK_INTERVAL_MS） */

    /* 线程创建后立即分离，退出时自动回收线程资源，无需外部 pthread_join */
    pthread_detach(pthread_self());

    /*
     * 等待创建者完成红黑树注册
     * 在 ThreadPool_CreateWorker 中，线程先被创建（此处开始执行），
     * 然后才被插入红黑树。必须等待 iRegistered==1 才能安全操作红黑树。
     *
     * 注意: 即使线程池正在关闭(iShutdown)，也必须等待注册完成，
     * 因为只有注册完成后节点才在红黑树中，才能安全地 RedBlackTree_delete。
     * CreateWorker 保证线程创建成功后一定会完成注册（在锁内设置 iRegistered=1）。
     */
    pthread_mutex_lock(&ptPool->tMutex);
    while (!ptWorker->iRegistered)
    {
        pthread_cond_wait(&ptPool->tCond, &ptPool->tMutex);
    }
    /* iRegistered==1 保证: 节点已在红黑树中，CreateWorker 已完成所有操作 */

    if (ptPool->iShutdown)
    {
        /*
         * 线程池在注册完成后、工作线程进入主循环前就开始关闭
         * 节点一定在红黑树中（因为 iRegistered==1），可以安全删除
         */
        RedBlackTree_delete(&ptPool->tWorkerTree, &ptWorker->tRbNode);
        ptPool->iLiveThreadNum--;
        pthread_cond_broadcast(&ptPool->tCond);
        pthread_mutex_unlock(&ptPool->tMutex);
        if (ptWorker->tThreadConfig.sThreadName != NULL)
        {
            free((void *)ptWorker->tThreadConfig.sThreadName);
            ptWorker->tThreadConfig.sThreadName = NULL;
        }
        free(ptWorker);
        return NULL;
    }
    pthread_mutex_unlock(&ptPool->tMutex);

    ThreadManage_printx("Worker [%s][%lu] started"
            , ptWorker->tThreadConfig.sThreadName
            , (unsigned long)ptWorker->tThreadConfig.tThreadPid);

    while (1)
    {
        /*
         * 从任务队列获取任务
         * 超时时间 IDLE_CHECK_INTERVAL_MS (100ms):
         *   - 队列有任务时立即返回
         *   - 队列为空时阻塞等待，最多等100ms
         *   - 超时返回 NULL，用于定期检查线程池关闭标志和空闲缩容
         */
        T_PoolTask *ptTask = (T_PoolTask *)ThreadQueueAPI_GetMsg(
                ptPool->ptTaskQueue, IDLE_CHECK_INTERVAL_MS);

        if (ptTask == NULL)
        {
            /*
             * 获取任务失败（超时或队列已关闭且为空）
             * 检查线程池关闭标志和线程退出标志
             */
            pthread_mutex_lock(&ptPool->tMutex);
            int iShutdown = ptPool->iShutdown;
            int iExiting = ptWorker->iExiting;
            int iIdleTimeoutMs = ptPool->iIdleTimeoutMs;  /* 在锁内读取，避免数据竞争 */
            pthread_mutex_unlock(&ptPool->tMutex);

            if (iShutdown || iExiting)
            {
                /* 被标记为需要退出（关闭或动态缩容） */
                break;
            }

            /* ======================== 空闲缩容检查 ======================== */
            iIdleCount++;
            if (iIdleTimeoutMs > 0
                && iIdleCount >= iIdleTimeoutMs / IDLE_CHECK_INTERVAL_MS)
            {
                /*
                 * 空闲时间超过阈值，尝试缩容
                 * 在互斥锁保护下进行双重检查:
                 * 1. 线程确实空闲（非忙碌）
                 * 2. 线程池未关闭
                 * 3. 线程未被标记退出
                 * 4. 存活线程数仍超过最小值
                 */
                pthread_mutex_lock(&ptPool->tMutex);
                if (!ptWorker->iIsBusy
                    && !ptPool->iShutdown
                    && !ptWorker->iExiting
                    && ptPool->iLiveThreadNum > ptPool->tConfig.iMinNum)
                {
                    /*
                     * 自我移除: 从红黑树删除节点，递减存活线程计数
                     * 此后该线程不再被线程池管理，需自行清理
                     */
                    RedBlackTree_delete(&ptPool->tWorkerTree, &ptWorker->tRbNode);
                    ptPool->iLiveThreadNum--;
                    pthread_cond_broadcast(&ptPool->tCond);

                    ThreadManage_printx("Worker [%s][%lu] shrinking, live = [%d]"
                            , ptWorker->tThreadConfig.sThreadName
                            , (unsigned long)ptWorker->tThreadConfig.tThreadPid
                            , ptPool->iLiveThreadNum);

                    pthread_mutex_unlock(&ptPool->tMutex);

                    /* 释放线程名称和工作线程节点（线程已分离，退出即回收） */
                    if (ptWorker->tThreadConfig.sThreadName != NULL)
                    {
                        free((void *)ptWorker->tThreadConfig.sThreadName);
                        ptWorker->tThreadConfig.sThreadName = NULL;
                    }
                    free(ptWorker);
                    return NULL;
                }
                pthread_mutex_unlock(&ptPool->tMutex);
                /*
                 * 未满足缩容条件（可能其他线程先退出了），
                 * 重置空闲计数，继续等待
                 */
                iIdleCount = 0;
            }
            /* 超时但未关闭/未缩容，继续等待新任务 */
            continue;
        }

        /*
         * 获取到任务，原子地递减队列计数并标记忙碌
         * 合并到同一个临界区，避免 iTaskQueueLen-- 和 iBusyThreadNum++ 之间的
         * 锁释放间隙导致 WaitAllDone 提前返回（BUG-2修复）
         */
        pthread_mutex_lock(&ptPool->tMutex);
        ptPool->iTaskQueueLen--;
        ptWorker->iIsBusy = 1;
        list_add_tail(&ptWorker->tBusyEntry, &ptPool->tBusyList);
        ptPool->iBusyThreadNum++;
        /* 更新峰值忙碌线程数统计 */
        if (ptPool->iBusyThreadNum > ptPool->tStats.iPeakBusyThreadNum)
        {
            ptPool->tStats.iPeakBusyThreadNum = ptPool->iBusyThreadNum;
        }
        pthread_mutex_unlock(&ptPool->tMutex);
        iIdleCount = 0;

        /* ======================== 执行任务 ======================== */
        if (ptTask->pTaskFunc != NULL)
        {
            ptTask->pTaskFunc(ptTask->pUserArg);
        }

        /* 释放任务结构体内存 */
        free(ptTask);

        /* ======================== 标记空闲 ======================== */
        pthread_mutex_lock(&ptPool->tMutex);
        ptWorker->iIsBusy = 0;
        list_del_init(&ptWorker->tBusyEntry);
        ptPool->iBusyThreadNum--;
        /* 更新已完成任务数统计 */
        ptPool->tStats.ulTotalTasksProcessed++;
        /*
         * 广播条件变量: 通知 ThreadAPI_ThreadPoolWaitAllDone
         * 重新检查"所有任务完成"条件
         */
        pthread_cond_broadcast(&ptPool->tCond);
        pthread_mutex_unlock(&ptPool->tMutex);
    }

    /* ======================== 线程退出清理（shutdown 或 iExiting） ======================== */
    /*
     * 统一退出路径: 从红黑树删除节点，递减计数，释放资源
     * 线程已在入口处 detach，退出后线程资源自动回收
     */
    pthread_mutex_lock(&ptPool->tMutex);
    RedBlackTree_delete(&ptPool->tWorkerTree, &ptWorker->tRbNode);
    ptPool->iLiveThreadNum--;
    int iLiveAfterExit = ptPool->iLiveThreadNum;
    /* 安全检查：处理线程在执行任务期间被取消等异常情况 */
    if (ptWorker->iIsBusy)
    {
        ptWorker->iIsBusy = 0;
        list_del_init(&ptWorker->tBusyEntry);
        ptPool->iBusyThreadNum--;
    }
    pthread_cond_broadcast(&ptPool->tCond);
    pthread_mutex_unlock(&ptPool->tMutex);

    ThreadManage_printx("Worker [%s][%lu] exited, live = [%d]"
            , ptWorker->tThreadConfig.sThreadName
            , (unsigned long)ptWorker->tThreadConfig.tThreadPid
            , iLiveAfterExit);

    /* 释放线程名称和工作线程节点 */
    if (ptWorker->tThreadConfig.sThreadName != NULL)
    {
        free((void *)ptWorker->tThreadConfig.sThreadName);
        ptWorker->tThreadConfig.sThreadName = NULL;
    }
    free(ptWorker);
    return NULL;
}


/* ======================== 公共API实现 ======================== */

/**
 * @func         ThreadAPI_ThreadPoolCreate
 * @brief        线程管理API-线程池创建
 * @param[in]    t_Config: 线程池配置参数
 * @return       T_ThreadPoolHandle 线程池句柄指针
 * @retval       非 NULL: 线程池创建成功
 * @retval       NULL: 线程池创建失败
 */
T_ThreadPoolHandle* ThreadAPI_ThreadPoolCreate(T_ThreadPoolConfig t_Config)
{
    /* ======================== 参数校验 ======================== */
    if (t_Config.iMinNum <= 0 || t_Config.iMaxNum <= 0
        || t_Config.iQueueMaxSize <= 0)
    {
        ThreadManage_printx("invalid config: min=[%d], max=[%d], queue=[%d]"
                , t_Config.iMinNum, t_Config.iMaxNum, t_Config.iQueueMaxSize);
        return NULL;
    }
    if (t_Config.iMinNum > t_Config.iMaxNum)
    {
        ThreadManage_printx("iMinNum[%d] > iMaxNum[%d]"
                , t_Config.iMinNum, t_Config.iMaxNum);
        return NULL;
    }

    /* ======================== 分配线程池句柄 ======================== */
    T_ThreadPoolHandle *ptPool = (T_ThreadPoolHandle *)malloc(
            sizeof(T_ThreadPoolHandle));
    if (ptPool == NULL)
    {
        ThreadManage_printx("malloc T_ThreadPoolHandle fail");
        return NULL;
    }
    memset(ptPool, 0, sizeof(T_ThreadPoolHandle));

    /* 存储用户配置 */
    ptPool->tConfig = t_Config;

    /*
     * 复制空闲超时配置（运行期间可独立于 tConfig 使用）
     * 容错处理: 如果用户未初始化 iIdleTimeoutMs（值为负数或垃圾值），
     * 自动修正为0（禁用缩容），确保向后兼容
     */
    ptPool->iIdleTimeoutMs = (t_Config.iIdleTimeoutMs > 0) ? t_Config.iIdleTimeoutMs : 0;
    ptPool->tConfig.iIdleTimeoutMs = ptPool->iIdleTimeoutMs;

    /* ======================== 初始化数据结构 ======================== */

    /* 初始化红黑树 */
    RedBlackTree_init_root(&ptPool->tWorkerTree);

    /* 初始化忙碌链表 */
    INIT_LIST_HEAD(&ptPool->tBusyList);

    /* 初始化互斥锁 */
    if (pthread_mutex_init(&ptPool->tMutex, NULL) != 0)
    {
        ThreadManage_printx("pthread_mutex_init fail");
        free(ptPool);
        return NULL;
    }

    /*
     * 初始化条件变量（使用 CLOCK_MONOTONIC 时钟）
     * 确保 WaitAllDone 的超时计算不受系统时间调整（NTP等）影响（BUG-3修复）
     */
    pthread_condattr_t tCondAttr;
    pthread_condattr_init(&tCondAttr);
    pthread_condattr_setclock(&tCondAttr, CLOCK_MONOTONIC);
    if (pthread_cond_init(&ptPool->tCond, &tCondAttr) != 0)
    {
        ThreadManage_printx("pthread_cond_init fail");
        pthread_condattr_destroy(&tCondAttr);
        pthread_mutex_destroy(&ptPool->tMutex);
        free(ptPool);
        return NULL;
    }
    pthread_condattr_destroy(&tCondAttr);

    /* 创建任务队列 */
    ptPool->ptTaskQueue = NULL;
    int ret = ThreadQueueAPI_InitMsg(&ptPool->ptTaskQueue,
            t_Config.iQueueMaxSize, "ThreadPool", ThreadPool_TaskReleaseCallback);
    if (ret != 0 || ptPool->ptTaskQueue == NULL)
    {
        ThreadManage_printx("ThreadQueueAPI_InitMsg fail");
        pthread_cond_destroy(&ptPool->tCond);
        pthread_mutex_destroy(&ptPool->tMutex);
        free(ptPool);
        return NULL;
    }

    /* 设置魔术字 */
    ptPool->uiMagic        = THREADPOOL_MAGIC;

    /* 初始化状态变量 */
    ptPool->iShutdown      = 0;
    ptPool->iLiveThreadNum = 0;
    ptPool->iBusyThreadNum = 0;
    ptPool->iTaskQueueLen  = 0;
    ptPool->uiWorkerSeq    = 0;
    ptPool->iExpanding     = 0;
    ptPool->iActiveCallers = 0;

    /* 初始化统计信息 */
    ptPool->tStats.ulTotalTasksSubmitted = 0;
    ptPool->tStats.ulTotalTasksProcessed = 0;
    ptPool->tStats.iPeakBusyThreadNum    = 0;
    ptPool->tStats.iPeakTaskQueueLen     = 0;

    /* ======================== 创建初始工作线程 ======================== */
    int i;
    for (i = 0; i < t_Config.iMinNum; i++)
    {
        if (ThreadPool_CreateWorker(ptPool) != 0)
        {
            ThreadManage_printx("CreateWorker fail, created [%d]/[%d]"
                    , i, t_Config.iMinNum);

            /* 创建失败: 回滚所有已创建的资源 */
            pthread_mutex_lock(&ptPool->tMutex);
            ptPool->iShutdown = 1;
            pthread_cond_broadcast(&ptPool->tCond);
            pthread_mutex_unlock(&ptPool->tMutex);
            ThreadQueueAPI_CloseMsg(ptPool->ptTaskQueue);

            /* 等待所有已创建的工作线程退出（线程已 detach，自动回收） */
            pthread_mutex_lock(&ptPool->tMutex);
            while (ptPool->iLiveThreadNum > 0)
            {
                pthread_cond_wait(&ptPool->tCond, &ptPool->tMutex);
            }
            pthread_mutex_unlock(&ptPool->tMutex);

            ThreadQueueAPI_DestroyMsg(&ptPool->ptTaskQueue);
            pthread_cond_destroy(&ptPool->tCond);
            pthread_mutex_destroy(&ptPool->tMutex);
            free(ptPool);
            return NULL;
        }
    }

    ThreadManage_printx("ThreadPool created: min=[%d], max=[%d], queue=[%d], idle_timeout=[%dms]"
            , t_Config.iMinNum, t_Config.iMaxNum, t_Config.iQueueMaxSize
            , t_Config.iIdleTimeoutMs);

    return ptPool;
}


/**
 * @func         ThreadAPI_ThreadPoolDestroy
 * @brief        线程管理API-线程池注销（销毁）
 * @param[in]    pt_ThreadPoolHandle: 线程池句柄
 * @return       int ret
 * @retval       0: 销毁成功
 * @retval       -1: 失败
 */
int ThreadAPI_ThreadPoolDestroy(T_ThreadPoolHandle *pt_ThreadPoolHandle)
{
    /* 句柄校验（NULL指针 + 魔术字） */
    if (ThreadPool_Validate(pt_ThreadPoolHandle) != 0)
    {
        return -1;
    }

    ThreadManage_printx("ThreadPool destroying...");

    /*
     * 步骤1: 在互斥锁保护下同时清除魔术字和设置关闭标志
     *
     * 安全性说明:
     * 必须将 uiMagic=0 和 iShutdown=1 放在同一个临界区内，
     * 原因: 如果在锁外清除 uiMagic，另一个线程可能在此间隙通过 Validate
     * 并尝试获取互斥锁。若 Destroy 线程已完成等待、销毁互斥锁并释放内存，
     * 该线程将对已销毁的互斥锁调用 pthread_mutex_lock，导致未定义行为。
     *
     * 将 uiMagic=0 放在锁内可保证:
     * - 任何已通过 Validate 的线程一定能成功获取互斥锁
     *   （因为 Destroy 在持有锁期间不会销毁互斥锁）
     * - 获取锁后会看到 iShutdown==1 并立即返回
     * - Destroy 在等待工作线程退出时不会持有锁，因此不会阻塞其他线程
     */
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    pt_ThreadPoolHandle->uiMagic = 0;
    pt_ThreadPoolHandle->iShutdown = 1;
    pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    /* 步骤2: 关闭任务队列（唤醒所有阻塞的工作线程） */
    ThreadQueueAPI_CloseMsg(pt_ThreadPoolHandle->ptTaskQueue);

    /* 步骤3: 等待所有活跃API调用者完成和工作线程退出
     * iActiveCallers: 跟踪正在执行中的 AddTask 系列函数调用，
     *   防止在调用者仍访问池资源时 Destroy 释放内存导致 UAF。
     *   调用者在首次 shutdown 检查通过后递增，在函数返回前递减并广播。
     * iLiveThreadNum: 跟踪工作线程，线程退出时递减并广播。
     * 两者都归零后才可安全销毁同步原语和释放内存。
     */
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    while (pt_ThreadPoolHandle->iActiveCallers > 0
           || pt_ThreadPoolHandle->iLiveThreadNum > 0)
    {
        pthread_cond_wait(&pt_ThreadPoolHandle->tCond,
                          &pt_ThreadPoolHandle->tMutex);
    }
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    /* 步骤4: 所有线程已退出，红黑树应为空，销毁任务队列 */
    ThreadQueueAPI_DestroyMsg(&pt_ThreadPoolHandle->ptTaskQueue);

    /* 步骤5: 销毁同步原语 */
    pthread_cond_destroy(&pt_ThreadPoolHandle->tCond);
    pthread_mutex_destroy(&pt_ThreadPoolHandle->tMutex);

    ThreadManage_printx("ThreadPool destroyed");

    /* 步骤6: 释放线程池句柄 */
    free(pt_ThreadPoolHandle);

    return 0;
}


/**
 * @func         ThreadAPI_ThreadPoolAddTask
 * @brief        线程管理API-线程池添加任务（队列满时无限阻塞）
 * @param[in]    pt_ThreadPoolHandle: 线程池句柄
 * @param[in]    TaskFunc: 任务函数指针
 * @param[in]    pUserArg: 传递给任务函数的用户参数
 * @return       int ret
 * @retval       0: 任务添加成功
 * @retval       -1: 失败
 */
int ThreadAPI_ThreadPoolAddTask(
                        T_ThreadPoolHandle *pt_ThreadPoolHandle
                        ,ThreadFunctionType TaskFunc
                        ,void *pUserArg)
{
    /* 参数校验 */
    if (ThreadPool_Validate(pt_ThreadPoolHandle) != 0)
    {
        return -1;
    }
    if (TaskFunc == NULL)
    {
        ThreadManage_printx("invalid params: TaskFunc is NULL");
        return -1;
    }

    /* 检查线程池是否已关闭 + 动态扩容判断 */
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    if (pt_ThreadPoolHandle->iShutdown)
    {
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        ThreadManage_printx("pool is shutdown");
        return -1;
    }

    /* 动态扩容判断（使用 iExpanding 标志防止并发 AddTask 重复触发扩容）
     * 扩容条件: 忙碌线程数 + 队列中待处理任务数 >= 存活线程数
     * 不仅看当前忙碌的线程，还要看队列中积压的任务。
     * 如果队列中有任务等待处理，说明当前线程数不够用，需要扩容。
     */
    int iNeedNewWorker = 0;
    if (!pt_ThreadPoolHandle->iExpanding
        && (pt_ThreadPoolHandle->iBusyThreadNum + pt_ThreadPoolHandle->iTaskQueueLen) >= pt_ThreadPoolHandle->iLiveThreadNum
        && pt_ThreadPoolHandle->iLiveThreadNum < pt_ThreadPoolHandle->tConfig.iMaxNum)
    {
        iNeedNewWorker = 1;
        pt_ThreadPoolHandle->iExpanding = 1;
    }
    pt_ThreadPoolHandle->iActiveCallers++;
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    if (iNeedNewWorker)
    {
        ThreadPool_CreateWorker(pt_ThreadPoolHandle);
    }

    /* 分配任务结构体 */
    T_PoolTask *ptTask = (T_PoolTask *)malloc(sizeof(T_PoolTask));
    if (ptTask == NULL)
    {
        ThreadManage_printx("malloc T_PoolTask fail");
        pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
        pt_ThreadPoolHandle->iActiveCallers--;
        pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        return -1;
    }
    ptTask->pTaskFunc = TaskFunc;
    ptTask->pUserArg  = pUserArg;

    /*
     * 先递增镜像队列计数和统计信息，再入队（BUG-1修复）
     * 确保 WaitAllDone 不会因入队和计数更新之间的窗口而提前返回
     */
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    pt_ThreadPoolHandle->iTaskQueueLen++;
    pt_ThreadPoolHandle->tStats.ulTotalTasksSubmitted++;
    if (pt_ThreadPoolHandle->iTaskQueueLen > pt_ThreadPoolHandle->tStats.iPeakTaskQueueLen)
    {
        pt_ThreadPoolHandle->tStats.iPeakTaskQueueLen = pt_ThreadPoolHandle->iTaskQueueLen;
    }
    /* 入队前再次检查 shutdown（防止与 Destroy 竞态导致 Use-After-Free） */
    if (pt_ThreadPoolHandle->iShutdown)
    {
        pt_ThreadPoolHandle->iTaskQueueLen--;
        pt_ThreadPoolHandle->tStats.ulTotalTasksSubmitted--;
        pt_ThreadPoolHandle->iActiveCallers--;
        pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        free(ptTask);
        ThreadManage_printx("pool is shutdown before enqueue");
        return -1;
    }
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    /* 将任务放入 ThreadQueue（队列满时阻塞等待） */
    int ret = ThreadQueueAPI_PutMsg(pt_ThreadPoolHandle->ptTaskQueue, ptTask);
    if (ret != 0)
    {
        ThreadManage_printx("ThreadQueueAPI_PutMsg fail ret = [%d]", ret);
        /* 入队失败，回退镜像队列计数和统计信息 */
        pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
        pt_ThreadPoolHandle->iTaskQueueLen--;
        pt_ThreadPoolHandle->tStats.ulTotalTasksSubmitted--;
        pt_ThreadPoolHandle->iActiveCallers--;
        pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        free(ptTask);
        return -1;
    }

    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    pt_ThreadPoolHandle->iActiveCallers--;
    pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    return 0;
}


/**
 * @func         ThreadAPI_ThreadPoolBusyThreadNumGet
 * @brief        线程管理API-获取线程池正在工作的线程个数
 * @param[in]    pt_ThreadPoolHandle: 线程池句柄
 * @return       int iBusyThreadNum
 * @retval       大于等于0: 正在工作的线程个数
 * @retval       -1: 执行失败
 */
int ThreadAPI_ThreadPoolBusyThreadNumGet(T_ThreadPoolHandle *pt_ThreadPoolHandle)
{
    if (ThreadPool_Validate(pt_ThreadPoolHandle) != 0)
    {
        return -1;
    }

    int iBusyThreadNum = 0;
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    iBusyThreadNum = pt_ThreadPoolHandle->iBusyThreadNum;
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    return iBusyThreadNum;
}


/**
 * @func         ThreadAPI_ThreadPoolLiveThreadNumGet
 * @brief        线程管理API-获取线程池存活的线程个数
 * @param[in]    pt_ThreadPoolHandle: 线程池句柄
 * @return       int iLiveThreadNum
 * @retval       大于等于0: 存活的线程个数
 * @retval       -1: 执行失败
 */
int ThreadAPI_ThreadPoolLiveThreadNumGet(T_ThreadPoolHandle *pt_ThreadPoolHandle)
{
    if (ThreadPool_Validate(pt_ThreadPoolHandle) != 0)
    {
        return -1;
    }

    int iLiveThreadNum = 0;
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    iLiveThreadNum = pt_ThreadPoolHandle->iLiveThreadNum;
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    return iLiveThreadNum;
}


/* ======================== 新增API实现 ======================== */

/**
 * @func         ThreadAPI_ThreadPoolTaskQueueLenGet
 * @brief        线程管理API-获取线程池任务队列中待处理的任务个数
 * @details      使用池互斥锁保护的镜像计数 iTaskQueueLen，与 WaitAllDone
 *               的判断条件保持一致，避免跨锁竞态。
 * @param[in]    pt_ThreadPoolHandle: 线程池句柄
 * @return       int 待处理任务个数
 * @retval       大于等于0: 待处理的任务个数
 * @retval       -1: 执行失败
 */
int ThreadAPI_ThreadPoolTaskQueueLenGet(T_ThreadPoolHandle *pt_ThreadPoolHandle)
{
    if (ThreadPool_Validate(pt_ThreadPoolHandle) != 0)
    {
        return -1;
    }

    /* 使用池锁保护的镜像计数，与 WaitAllDone 的判断条件一致 */
    int iLen = 0;
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    iLen = pt_ThreadPoolHandle->iTaskQueueLen;
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    return iLen;
}


/**
 * @func         ThreadAPI_ThreadPoolWaitAllDone
 * @brief        线程管理API-等待线程池中所有任务完成
 * @details      阻塞等待直到所有已提交的任务执行完毕
 *               （忙碌线程数为0且任务队列为空）。
 *               使用条件变量实现高效等待，避免忙轮询。
 *               工作线程在每次完成任务后广播条件变量。
 *
 * @param[in]    pt_ThreadPoolHandle: 线程池句柄
 * @param[in]    iTimeoutMs: 超时等待时间，单位:毫秒(ms)，0=无限等待
 * @return       int ret
 * @retval       0: 所有任务已完成
 * @retval       -1: 执行失败（参数无效或线程池已关闭）
 * @retval       -2: 等待超时
 */
int ThreadAPI_ThreadPoolWaitAllDone(T_ThreadPoolHandle *pt_ThreadPoolHandle, int iTimeoutMs)
{
    if (ThreadPool_Validate(pt_ThreadPoolHandle) != 0)
    {
        return -1;
    }

    struct timespec tStart;
    int iHasTimeout = (iTimeoutMs > 0);

    if (iHasTimeout)
    {
        clock_gettime(CLOCK_MONOTONIC, &tStart);
    }

    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    while (1)
    {
        /* 检查线程池是否已关闭 */
        if (pt_ThreadPoolHandle->iShutdown)
        {
            pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
            return -1;
        }

        /* 检查条件: 忙碌线程数为0且镜像队列计数为0（全在池锁保护下，无竞态） */
        if (pt_ThreadPoolHandle->iBusyThreadNum == 0
            && pt_ThreadPoolHandle->iTaskQueueLen == 0)
        {
            /* 所有任务已完成: 忙碌数为0且无待处理任务 */
            pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
            return 0;
        }

        /* 条件未满足，等待条件变量通知 */
        if (iHasTimeout)
        {
            struct timespec tNow;
            clock_gettime(CLOCK_MONOTONIC, &tNow);
            /* 修复跨秒边界时纳秒差值为负数导致elapsed_ms高估的问题 */
            long sec_diff = (long)(tNow.tv_sec - tStart.tv_sec);
            long nsec_diff = (long)(tNow.tv_nsec - tStart.tv_nsec);
            if (nsec_diff < 0)
            {
                sec_diff--;
                nsec_diff += 1000000000L;
            }
            long elapsed_ms = sec_diff * 1000L + nsec_diff / 1000000L;
            if (elapsed_ms >= iTimeoutMs)
            {
                pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
                return -2; /* 超时 */
            }
            /* 计算剩余等待时间 */
            long remaining_ms = iTimeoutMs - elapsed_ms;
            struct timespec tAbstime;
            clock_gettime(CLOCK_MONOTONIC, &tAbstime);
            tAbstime.tv_sec  += remaining_ms / 1000L;
            tAbstime.tv_nsec += (remaining_ms % 1000L) * 1000000L;
            if (tAbstime.tv_nsec >= 1000000000L)
            {
                tAbstime.tv_sec++;
                tAbstime.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&pt_ThreadPoolHandle->tCond,
                                   &pt_ThreadPoolHandle->tMutex, &tAbstime);
        }
        else
        {
            /* 无限等待 */
            pthread_cond_wait(&pt_ThreadPoolHandle->tCond,
                              &pt_ThreadPoolHandle->tMutex);
        }
    }
}


/**
 * @func         ThreadAPI_ThreadPoolAddTaskTimeout
 * @brief        线程管理API-线程池添加任务（带超时）
 * @details      当任务队列已满时，不会无限阻塞，而是等待最多iTimeoutMs毫秒。
 *               当iTimeoutMs为0时，行为与ThreadAPI_ThreadPoolAddTask相同。
 *
 *               实现说明:
 *               底层调用 ThreadQueue 的 ThreadQueueAPI_PutMsgTimeout 接口，
 *               队列满时由底层条件变量带超时等待，无需轮询，效率更高。
 *
 * @param[in]    pt_ThreadPoolHandle: 线程池句柄
 * @param[in]    TaskFunc: 任务函数
 * @param[in]    pUserArg: 任务函数用户参数
 * @param[in]    iTimeoutMs: 超时等待时间，单位:毫秒(ms)，0=无限等待
 * @return       int ret
 * @retval       0: 任务添加成功
 * @retval       -1: 失败（参数无效、线程池已关闭、内存不足）
 * @retval       -2: 等待超时，任务未添加
 */
int ThreadAPI_ThreadPoolAddTaskTimeout(
                        T_ThreadPoolHandle *pt_ThreadPoolHandle
                        ,ThreadFunctionType TaskFunc
                        ,void *pUserArg
                        ,int iTimeoutMs)
{
    /* 参数校验 */
    if (ThreadPool_Validate(pt_ThreadPoolHandle) != 0)
    {
        return -1;
    }
    if (TaskFunc == NULL)
    {
        ThreadManage_printx("invalid params: TaskFunc is NULL");
        return -1;
    }

    /* iTimeoutMs <= 0 时退化为无限等待 */
    if (iTimeoutMs <= 0)
    {
        return ThreadAPI_ThreadPoolAddTask(pt_ThreadPoolHandle, TaskFunc, pUserArg);
    }

    /* 检查线程池是否已关闭 + 动态扩容判断 */
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    if (pt_ThreadPoolHandle->iShutdown)
    {
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        ThreadManage_printx("pool is shutdown");
        return -1;
    }
    int iNeedNewWorker = 0;
    if (!pt_ThreadPoolHandle->iExpanding
        && (pt_ThreadPoolHandle->iBusyThreadNum + pt_ThreadPoolHandle->iTaskQueueLen) >= pt_ThreadPoolHandle->iLiveThreadNum
        && pt_ThreadPoolHandle->iLiveThreadNum < pt_ThreadPoolHandle->tConfig.iMaxNum)
    {
        iNeedNewWorker = 1;
        pt_ThreadPoolHandle->iExpanding = 1;
    }
    pt_ThreadPoolHandle->iActiveCallers++;
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    if (iNeedNewWorker)
    {
        ThreadPool_CreateWorker(pt_ThreadPoolHandle);
    }

    /* 分配任务结构体 */
    T_PoolTask *ptTask = (T_PoolTask *)malloc(sizeof(T_PoolTask));
    if (ptTask == NULL)
    {
        ThreadManage_printx("malloc T_PoolTask fail");
        pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
        pt_ThreadPoolHandle->iActiveCallers--;
        pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        return -1;
    }
    ptTask->pTaskFunc = TaskFunc;
    ptTask->pUserArg  = pUserArg;

    /*
     * 先递增镜像队列计数和统计信息，再入队（BUG-1修复）
     * 确保 WaitAllDone 不会因入队和计数更新之间的窗口而提前返回
     */
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    pt_ThreadPoolHandle->iTaskQueueLen++;
    pt_ThreadPoolHandle->tStats.ulTotalTasksSubmitted++;
    if (pt_ThreadPoolHandle->iTaskQueueLen > pt_ThreadPoolHandle->tStats.iPeakTaskQueueLen)
    {
        pt_ThreadPoolHandle->tStats.iPeakTaskQueueLen = pt_ThreadPoolHandle->iTaskQueueLen;
    }
    /* 入队前再次检查 shutdown（防止与 Destroy 竞态） */
    if (pt_ThreadPoolHandle->iShutdown)
    {
        pt_ThreadPoolHandle->iTaskQueueLen--;
        pt_ThreadPoolHandle->tStats.ulTotalTasksSubmitted--;
        pt_ThreadPoolHandle->iActiveCallers--;
        pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        free(ptTask);
        ThreadManage_printx("pool is shutdown before enqueue");
        return -1;
    }
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    /* 使用底层 ThreadQueueAPI_PutMsgTimeout 直接带超时等待，无需轮询 */
    int ret = ThreadQueueAPI_PutMsgTimeout(pt_ThreadPoolHandle->ptTaskQueue, ptTask, iTimeoutMs);
    if (ret != 0)
    {
        /* 入队失败，回退镜像队列计数和统计信息 */
        pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
        pt_ThreadPoolHandle->iTaskQueueLen--;
        pt_ThreadPoolHandle->tStats.ulTotalTasksSubmitted--;
        pt_ThreadPoolHandle->iActiveCallers--;
        pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

        if (ret == -3)
        {
            /* 队列满且等待超时 */
            ThreadManage_printx("AddTaskTimeout: timeout after [%d]ms", iTimeoutMs);
            free(ptTask);
            return -2;
        }
        /* -1: 参数无效  -2: 队列已关闭 */
        ThreadManage_printx("ThreadQueueAPI_PutMsgTimeout fail ret = [%d]", ret);
        free(ptTask);
        return -1;
    }

    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    pt_ThreadPoolHandle->iActiveCallers--;
    pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    return 0;
}


/**
 * @func         ThreadAPI_ThreadPoolAddTaskTry
 * @brief        线程管理API-线程池尝试添加任务（非阻塞）
 * @details      尝试将任务添加到线程池。如果任务队列已满，立即返回失败。
 *
 *               注意: 存在极小的TOCTOU竞态窗口（检查队列长度和放入任务之间），
 *               使用 ThreadQueueAPI_PutMsgTimeout 带1ms超时缓解，最多阻塞1ms
 *               而非无限等待，保证近似非阻塞语义。
 *               对于绝大多数嵌入式应用场景，此行为可接受。
 *
 * @param[in]    pt_ThreadPoolHandle: 线程池句柄
 * @param[in]    TaskFunc: 任务函数
 * @param[in]    pUserArg: 任务函数用户参数
 * @return       int ret
 * @retval       0: 任务添加成功
 * @retval       -1: 失败（参数无效、线程池已关闭、内存不足）
 * @retval       -2: 任务队列已满，任务未添加
 */
int ThreadAPI_ThreadPoolAddTaskTry(
                        T_ThreadPoolHandle *pt_ThreadPoolHandle
                        ,ThreadFunctionType TaskFunc
                        ,void *pUserArg)
{
    /* 参数校验 */
    if (ThreadPool_Validate(pt_ThreadPoolHandle) != 0)
    {
        return -1;
    }
    if (TaskFunc == NULL)
    {
        ThreadManage_printx("invalid params: TaskFunc is NULL");
        return -1;
    }

    /* 检查线程池是否已关闭 + 动态扩容判断 */
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    if (pt_ThreadPoolHandle->iShutdown)
    {
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        return -1;
    }
    int iNeedNewWorker = 0;
    if (!pt_ThreadPoolHandle->iExpanding
        && (pt_ThreadPoolHandle->iBusyThreadNum + pt_ThreadPoolHandle->iTaskQueueLen) >= pt_ThreadPoolHandle->iLiveThreadNum
        && pt_ThreadPoolHandle->iLiveThreadNum < pt_ThreadPoolHandle->tConfig.iMaxNum)
    {
        iNeedNewWorker = 1;
        pt_ThreadPoolHandle->iExpanding = 1;
    }
    pt_ThreadPoolHandle->iActiveCallers++;
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    if (iNeedNewWorker)
    {
        ThreadPool_CreateWorker(pt_ThreadPoolHandle);
    }

    /* 快速检查队列是否已满（使用镜像计数，在池锁保护下，避免跨锁竞态） */
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    if (pt_ThreadPoolHandle->iTaskQueueLen >= pt_ThreadPoolHandle->tConfig.iQueueMaxSize)
    {
        pt_ThreadPoolHandle->iActiveCallers--;
        pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        return -2; /* 队列已满，立即返回 */
    }
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    /* 分配任务结构体 */
    T_PoolTask *ptTask = (T_PoolTask *)malloc(sizeof(T_PoolTask));
    if (ptTask == NULL)
    {
        ThreadManage_printx("malloc T_PoolTask fail");
        pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
        pt_ThreadPoolHandle->iActiveCallers--;
        pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        return -1;
    }
    ptTask->pTaskFunc = TaskFunc;
    ptTask->pUserArg  = pUserArg;

    /*
     * 先递增镜像队列计数和统计信息，再入队（BUG-1修复）
     * 确保 WaitAllDone 不会因入队和计数更新之间的窗口而提前返回
     */
    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    pt_ThreadPoolHandle->iTaskQueueLen++;
    pt_ThreadPoolHandle->tStats.ulTotalTasksSubmitted++;
    if (pt_ThreadPoolHandle->iTaskQueueLen > pt_ThreadPoolHandle->tStats.iPeakTaskQueueLen)
    {
        pt_ThreadPoolHandle->tStats.iPeakTaskQueueLen = pt_ThreadPoolHandle->iTaskQueueLen;
    }
    /* 入队前再次检查 shutdown（防止与 Destroy 竞态） */
    if (pt_ThreadPoolHandle->iShutdown)
    {
        pt_ThreadPoolHandle->iTaskQueueLen--;
        pt_ThreadPoolHandle->tStats.ulTotalTasksSubmitted--;
        pt_ThreadPoolHandle->iActiveCallers--;
        pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        free(ptTask);
        ThreadManage_printx("pool is shutdown before enqueue");
        return -1;
    }
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    /*
     * 尝试放入队列（带1ms超时，避免无限阻塞）
     * 使用 ThreadQueueAPI_PutMsgTimeout 替代 ThreadQueueAPI_PutMsg，
     * 确保即使快速检查与实际放入之间存在TOCTOU窗口，
     * 也最多阻塞1ms而非无限等待，保证非阻塞语义。
     */
    int ret = ThreadQueueAPI_PutMsgTimeout(pt_ThreadPoolHandle->ptTaskQueue, ptTask, 1);
    if (ret != 0)
    {
        /* 入队失败，回退镜像队列计数和统计信息 */
        pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
        pt_ThreadPoolHandle->iTaskQueueLen--;
        pt_ThreadPoolHandle->tStats.ulTotalTasksSubmitted--;
        pt_ThreadPoolHandle->iActiveCallers--;
        pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

        if (ret == -3)
        {
            /* 队列满且等待超时，视为队列已满 */
            free(ptTask);
            return -2;
        }
        /* -1: 参数无效  -2: 队列已关闭 */
        free(ptTask);
        return -1;
    }

    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    pt_ThreadPoolHandle->iActiveCallers--;
    pthread_cond_broadcast(&pt_ThreadPoolHandle->tCond);
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    return 0;
}


/**
 * @func         ThreadAPI_ThreadPoolResize
 * @brief        线程管理API-动态调整线程池大小
 * @details      调整线程池的最小和最大线程数。
 *               - 扩容: 如果新的最小线程数大于当前存活线程数，立即创建新线程。
 *               - 缩容: 如果新的最大线程数小于当前存活线程数，标记空闲线程退出。
 *                 线程会在下一次空闲检查时（100ms内）退出。
 *                 注意: 忙碌线程不会被强制中断，会在完成当前任务后退出。
 *
 * @param[in]    pt_ThreadPoolHandle: 线程池句柄
 * @param[in]    iNewMinNum: 新的最小线程数
 * @param[in]    iNewMaxNum: 新的最大线程数
 * @return       int ret
 * @retval       0: 执行成功
 * @retval       -1: 失败（参数无效）
 */
int ThreadAPI_ThreadPoolResize(T_ThreadPoolHandle *pt_ThreadPoolHandle,
                               int iNewMinNum, int iNewMaxNum)
{
    if (ThreadPool_Validate(pt_ThreadPoolHandle) != 0)
    {
        return -1;
    }
    if (iNewMinNum <= 0 || iNewMaxNum <= 0 || iNewMinNum > iNewMaxNum)
    {
        ThreadManage_printx("invalid resize params: min=[%d], max=[%d]"
                , iNewMinNum, iNewMaxNum);
        return -1;
    }

    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);

    /* 检查线程池是否已关闭 */
    if (pt_ThreadPoolHandle->iShutdown)
    {
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
        return -1;
    }

    /* 更新配置 */
    pt_ThreadPoolHandle->tConfig.iMinNum = iNewMinNum;
    pt_ThreadPoolHandle->tConfig.iMaxNum = iNewMaxNum;

    int iCurrentLive = pt_ThreadPoolHandle->iLiveThreadNum;

    if (iNewMinNum > iCurrentLive)
    {
        /* 需要扩容: 创建新的工作线程 */
        int iNeedCreate = iNewMinNum - iCurrentLive;
        pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

        int i;
        int iCreated = 0;
        for (i = 0; i < iNeedCreate; i++)
        {
            if (ThreadPool_CreateWorker(pt_ThreadPoolHandle) == 0)
            {
                iCreated++;
            }
        }

        if (iCreated < iNeedCreate)
        {
            ThreadManage_printx("ThreadPool resize: partial expand, created [%d]/[%d] workers"
                    , iCreated, iNeedCreate);
            if (iCreated == 0)
            {
                /* 全部创建失败，回滚配置并返回错误 */
                pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
                pt_ThreadPoolHandle->tConfig.iMinNum = iCurrentLive;
                pt_ThreadPoolHandle->tConfig.iMaxNum = (iNewMaxNum > iCurrentLive) ? iNewMaxNum : iCurrentLive;
                pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);
                return -1;
            }
        }
        else
        {
            ThreadManage_printx("ThreadPool resize: expanding, created [%d] workers"
                    , iCreated);
        }
        return 0;
    }

    if (iNewMaxNum < iCurrentLive)
    {
        /*
         * 需要缩容: 标记空闲线程退出
         * 遍历红黑树，找到空闲且未被标记退出的线程，设置 iExiting = 1
         * 工作线程在下一次 ThreadQueueAPI_GetMsg 超时后会检查此标志并退出
         */
        int iNeedExit = iCurrentLive - iNewMaxNum;
        struct rb_node *pos;
        RedBlackTree_for_each(pos, &pt_ThreadPoolHandle->tWorkerTree)
        {
            if (iNeedExit <= 0)
            {
                break;
            }
            T_WorkerNode *ptWorker = RedBlackTree_entry(pos, T_WorkerNode, tRbNode);
            if (!ptWorker->iIsBusy && !ptWorker->iExiting)
            {
                ptWorker->iExiting = 1;
                iNeedExit--;
                ThreadManage_printx("Worker [%s][%lu] marked for exit (resize)"
                        , ptWorker->tThreadConfig.sThreadName
                        , (unsigned long)ptWorker->tThreadConfig.tThreadPid);
            }
        }

        if (iNeedExit > 0)
        {
            ThreadManage_printx("Resize: [%d] threads busy, will shrink when idle"
                    , iNeedExit);
        }
    }

    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    ThreadManage_printx("ThreadPool resize: min=[%d], max=[%d]"
            , iNewMinNum, iNewMaxNum);
    return 0;
}


/**
 * @func         ThreadAPI_ThreadPoolStatsGet
 * @brief        线程管理API-获取线程池统计信息
 * @details      在互斥锁保护下复制统计信息到用户提供的结构体中。
 *               统计信息包括:
 *               - 总提交任务数
 *               - 总完成任务数
 *               - 峰值忙碌线程数
 *               - 峰值任务队列长度
 *
 * @param[in]    pt_ThreadPoolHandle: 线程池句柄
 * @param[out]   pt_Stats: 统计信息输出结构体指针
 * @return       int ret
 * @retval       0: 执行成功
 * @retval       -1: 执行失败（参数无效）
 */
int ThreadAPI_ThreadPoolStatsGet(T_ThreadPoolHandle *pt_ThreadPoolHandle,
                                 T_ThreadPoolStats *pt_Stats)
{
    if (ThreadPool_Validate(pt_ThreadPoolHandle) != 0)
    {
        return -1;
    }
    if (pt_Stats == NULL)
    {
        return -1;
    }

    pthread_mutex_lock(&pt_ThreadPoolHandle->tMutex);
    *pt_Stats = pt_ThreadPoolHandle->tStats;
    pthread_mutex_unlock(&pt_ThreadPoolHandle->tMutex);

    return 0;
}
