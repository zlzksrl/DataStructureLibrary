/**
 * @file        SoftTimer.c
 * @brief       LinuxARM-PublicLib-软件定时器-核心实现文件
 * @details     IMX6ULL平台
 *              基于 ThreadManage：调度线程用 ThreadAPI_ThreadCreate 单独创建，
 *              回调用 ThreadAPI_ThreadPool 分发。调度线程 pthread_cond_timedwait(CLOCK_MONOTONIC)
 *              睡到堆顶 deadline；到点后 pop 堆顶投递线程池；wrapper 内完成 cb 后按周期续期回堆。
 *
 *              并发保护：单一 mgr->mux 大锁，两个 cond：
 *                cv_sched  —— 调度线程等；SetAlarm/Delete/Destroy 唤醒（signal 即可，仅 1 waiter）
 *                cv_delete —— Delete D2 等；wrapper cb 收尾时 broadcast（可能有多 waiter）
 *
 *              内存所有权（参见"实现方案.md §5.2"）：
 *                Path A (墓碑)     : Delete 释放 handle
 *                Path B (IN_HEAP)  : Delete 释放 node + handle
 *                Path C (DISPATCH) : Delete 释放 handle；wrapper 早退释放 node
 *                Path D1 (ORPHAN)  : Delete 释放 handle 并置 node->handle=NULL；wrapper 收尾释放 node
 *                Path D2 (RUNNING) : Delete 释放 handle + node（等 cv_delete 唤醒后 free）
 *                Path D2 (-2 分支) : Delete 摘 live_set 但**保留** node->handle；wrapper 转 ORPHANED
 *                                    收尾释放 node **与** handle（cb 仍在锁外持 handle_snapshot，
 *                                    立即 free(handle) 会 UAF —— 见 C7 修复）
 *                单次正常完成       : wrapper 释放 node，handle 打墓碑等 Delete 回收
 *                周期正常轮转       : node/handle 均保留
 *                Destroy 扫尾       : Destroy 释放堆残余节点与所有 handle
 *                wrapper ORPHANED  : free(node)；若 node->handle 非 NULL 则 free(handle)
 *                                    —— D1 已在 Delete 内释放并置 NULL；D2 -2 保留待此释放
 *
 *              统计口径：
 *                ulTotalFired    = 成功投递到线程池次数（在调度线程内 ++，含 Path C 早退投递）
 *                ulTotalCanceled = Delete 返回 0（Path B/C/D1/D2）或 -2(D2 stop_flag) 的次数；
 *                                   Path A（墓碑清理）不计入，因语义上是"回收已完成定时器"而非取消
 *                ulTotalOverrun  = 三合一：FROM_END 超时 / FROM_SCHEDULED snap / 池满 requeue
 *                                   注：FROM_END 遇到池满 requeue 时可能双计（一次自 requeue、
 *                                   一次自 wrapper delta 判定），属已知粗粒度语义
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-13
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-13
 * @Version     V1.0.0
 * @brief       创建文件，实现软件定时器全套API
 * @author      zlzksrl
 */
#include "../include/SoftTimer.h"
#include "SoftTimer_Main.h"
#include "SoftTimer_Maketime.h"

#if ST_DEBUG_DISPATCH_DELAY_MS > 0
#include <unistd.h>
#endif


/* ========================== 内部前向声明 ========================== */

/* 名称 / 时间工具 */
static void      st_set_name(char *dst, const char *src);
static void      st_timespec_add_ms(struct timespec *out,
                                    const struct timespec *base, long long ms);
static int       st_timespec_cmp(const struct timespec *a,
                                 const struct timespec *b);
static long long st_timespec_diff_ms(const struct timespec *a,
                                     const struct timespec *b);

/* min-heap 操作（均要求持锁） */
static int       st_heap_ensure_capacity_locked(T_SoftTimerMgr *mgr);
static void      st_heap_swap_locked(T_SoftTimerMgr *mgr, int i, int j);
static void      st_heap_sift_up_locked(T_SoftTimerMgr *mgr, int idx);
static void      st_heap_sift_down_locked(T_SoftTimerMgr *mgr, int idx);
static int       st_heap_push_locked(T_SoftTimerMgr *mgr, T_SoftTimer *node);
static T_SoftTimer *st_heap_pop_top_locked(T_SoftTimerMgr *mgr);
static void      st_heap_remove_at_locked(T_SoftTimerMgr *mgr, int idx);
static T_SoftTimer *st_heap_peek_top_locked(T_SoftTimerMgr *mgr);

/* live_set 操作（均要求持锁） */
static void      st_liveset_add_locked(T_SoftTimerMgr *mgr, T_SoftTimerHandle *h);
static int       st_liveset_contains_locked(T_SoftTimerMgr *mgr, T_SoftTimerHandle *h);
static void      st_liveset_remove_locked(T_SoftTimerMgr *mgr, T_SoftTimerHandle *h);

/* 集中初值（calloc 已把结构体清零，这里只设置非零默认） */
static void      st_mgr_init_defaults(T_SoftTimerMgr *mgr,
                                      const T_SoftTimerConfig *cfg);

/* 调度线程 & wrapper（均为 ThreadFunctionType 签名） */
static void     *st_sched_thread(void *arg);
static void     *st_wrapper_task(void *arg);

/* 计算下一次 deadline（周期定时器续期） */
static void      st_compute_next_deadline_locked(T_SoftTimer *node,
                                                 const struct timespec *cb_end);

/* ========================== 名称 / 时间工具实现 ========================== */

/**
 * @brief 安全拷贝名称到定长内联缓冲，超长截断，末尾保证 '\0'
 */
static void st_set_name(char *dst, const char *src)
{
    if(NULL == dst)
    {
        return;
    }
    if(NULL == src)
    {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while(i < MAX_SOFTTIMERNAME_LEN && src[i] != '\0')
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/**
 * @brief out = base + ms （规范化 tv_nsec 到 [0, 1e9)）
 */
static void st_timespec_add_ms(struct timespec *out,
                               const struct timespec *base, long long ms)
{
    if(NULL == out || NULL == base)
    {
        return;
    }
    long long add_sec  = ms / 1000LL;
    long long add_nsec = (ms % 1000LL) * 1000000LL;

    long long sec  = (long long)base->tv_sec  + add_sec;
    long long nsec = (long long)base->tv_nsec + add_nsec;

    while(nsec >= 1000000000LL)
    {
        nsec -= 1000000000LL;
        sec  += 1;
    }
    while(nsec < 0)
    {
        nsec += 1000000000LL;
        sec  -= 1;
    }
    out->tv_sec  = (time_t)sec;
    out->tv_nsec = (long)nsec;
}

/**
 * @brief 比较两个 timespec，a<b 返回 -1，a==b 返回 0，a>b 返回 1
 */
static int st_timespec_cmp(const struct timespec *a, const struct timespec *b)
{
    if(a->tv_sec < b->tv_sec)
    {
        return -1;
    }
    if(a->tv_sec > b->tv_sec)
    {
        return  1;
    }
    if(a->tv_nsec < b->tv_nsec)
    {
        return -1;
    }
    if(a->tv_nsec > b->tv_nsec)
    {
        return  1;
    }
    return 0;
}

/**
 * @brief 返回 (a - b) 的毫秒差值；a 早于 b 时返回负值
 */
static long long st_timespec_diff_ms(const struct timespec *a,
                                     const struct timespec *b)
{
    long long ds = (long long)a->tv_sec  - (long long)b->tv_sec;
    long long dn = (long long)a->tv_nsec - (long long)b->tv_nsec;
    return ds * 1000LL + dn / 1000000LL;
}

/* ========================== min-heap 实现（持锁） ========================== */

/**
 * @brief 确保堆有 heap_size+1 的空间，不足则倍增
 * @retval 0 成功，-1 分配失败
 */
static int st_heap_ensure_capacity_locked(T_SoftTimerMgr *mgr)
{
    if(mgr->heap_size < mgr->heap_capacity)
    {
        return 0;
    }
    int new_cap = mgr->heap_capacity > 0
                  ? mgr->heap_capacity * 2
                  : ST_HEAP_INIT_CAPACITY;
    T_SoftTimer **new_heap = (T_SoftTimer **)realloc(mgr->heap,
                                sizeof(T_SoftTimer *) * (size_t)new_cap);
    if(NULL == new_heap)
    {
        ST_LOGE("[%s] heap realloc fail, cap=%d\n",
                mgr->name, new_cap);
        return -1;
    }
    mgr->heap          = new_heap;
    mgr->heap_capacity = new_cap;
    return 0;
}

/**
 * @brief 交换 heap[i] 与 heap[j] 并同步 heap_idx
 */
static void st_heap_swap_locked(T_SoftTimerMgr *mgr, int i, int j)
{
    T_SoftTimer *tmp = mgr->heap[i];
    mgr->heap[i]     = mgr->heap[j];
    mgr->heap[j]     = tmp;
    mgr->heap[i]->heap_idx = i;
    mgr->heap[j]->heap_idx = j;
}

/**
 * @brief 自 idx 向上调整（子往父浮）
 */
static void st_heap_sift_up_locked(T_SoftTimerMgr *mgr, int idx)
{
    while(idx > 0)
    {
        int parent = (idx - 1) / 2;
        if(st_timespec_cmp(&mgr->heap[idx]->deadline,
                           &mgr->heap[parent]->deadline) < 0)
        {
            st_heap_swap_locked(mgr, idx, parent);
            idx = parent;
        }
        else
        {
            break;
        }
    }
}

/**
 * @brief 自 idx 向下调整（父往子沉）
 */
static void st_heap_sift_down_locked(T_SoftTimerMgr *mgr, int idx)
{
    int n = mgr->heap_size;
    while(1)
    {
        int l = idx * 2 + 1;
        int r = idx * 2 + 2;
        int min = idx;
        if(l < n && st_timespec_cmp(&mgr->heap[l]->deadline,
                                    &mgr->heap[min]->deadline) < 0)
        {
            min = l;
        }
        if(r < n && st_timespec_cmp(&mgr->heap[r]->deadline,
                                    &mgr->heap[min]->deadline) < 0)
        {
            min = r;
        }
        if(min == idx)
        {
            break;
        }
        st_heap_swap_locked(mgr, idx, min);
        idx = min;
    }
}

/**
 * @brief 将 node 插入堆
 * @retval 0 成功，-1 扩容失败（此时 node 未入堆）
 */
static int st_heap_push_locked(T_SoftTimerMgr *mgr, T_SoftTimer *node)
{
    if(st_heap_ensure_capacity_locked(mgr) < 0)
    {
        return -1;
    }
    int idx = mgr->heap_size;
    mgr->heap[idx]   = node;
    node->heap_idx   = idx;
    mgr->heap_size  += 1;
    st_heap_sift_up_locked(mgr, idx);
    return 0;
}

/**
 * @brief 弹出堆顶（最小 deadline 节点）
 * @return 堆顶节点；堆空返回 NULL
 */
static T_SoftTimer *st_heap_pop_top_locked(T_SoftTimerMgr *mgr)
{
    if(mgr->heap_size <= 0)
    {
        return NULL;
    }
    T_SoftTimer *top = mgr->heap[0];
    top->heap_idx    = -1;
    mgr->heap_size  -= 1;
    if(mgr->heap_size > 0)
    {
        mgr->heap[0]           = mgr->heap[mgr->heap_size];
        mgr->heap[0]->heap_idx = 0;
        st_heap_sift_down_locked(mgr, 0);
    }
    return top;
}

/**
 * @brief 从堆中删除任意位置节点
 */
static void st_heap_remove_at_locked(T_SoftTimerMgr *mgr, int idx)
{
    if(idx < 0 || idx >= mgr->heap_size)
    {
        return;
    }
    mgr->heap[idx]->heap_idx = -1;
    mgr->heap_size          -= 1;
    if(idx == mgr->heap_size)
    {
        return;
    }
    mgr->heap[idx]           = mgr->heap[mgr->heap_size];
    mgr->heap[idx]->heap_idx = idx;
    /* 可能需要向上或向下调整 */
    st_heap_sift_up_locked(mgr, idx);
    st_heap_sift_down_locked(mgr, idx);
}

/**
 * @brief 查看堆顶（不弹出），堆空返回 NULL
 */
static T_SoftTimer *st_heap_peek_top_locked(T_SoftTimerMgr *mgr)
{
    if(mgr->heap_size <= 0)
    {
        return NULL;
    }
    return mgr->heap[0];
}

/* ========================== live_set 实现（持锁） ========================== */

/**
 * @brief 头插一个 handle
 */
static void st_liveset_add_locked(T_SoftTimerMgr *mgr, T_SoftTimerHandle *h)
{
    h->next_in_set   = mgr->live_head;
    mgr->live_head   = h;
    mgr->live_count += 1;
}

/**
 * @brief 判断 h 是否在 live_set（指针身份匹配即可）
 * @retval 1 存在，0 不存在
 */
static int st_liveset_contains_locked(T_SoftTimerMgr *mgr, T_SoftTimerHandle *h)
{
    T_SoftTimerHandle *cur = mgr->live_head;
    while(NULL != cur)
    {
        if(cur == h)
        {
            return 1;
        }
        cur = cur->next_in_set;
    }
    return 0;
}

/**
 * @brief 从 live_set 摘除 handle（不释放内存）
 */
static void st_liveset_remove_locked(T_SoftTimerMgr *mgr, T_SoftTimerHandle *h)
{
    T_SoftTimerHandle **pp = &mgr->live_head;
    while(NULL != *pp)
    {
        if(*pp == h)
        {
            *pp             = h->next_in_set;
            h->next_in_set  = NULL;
            mgr->live_count -= 1;
            return;
        }
        pp = &(*pp)->next_in_set;
    }
}

/* ========================== 管理器默认值 ========================== */

/**
 * @brief 集中设置管理器非零初值（配合 calloc）
 * @details 把散落在 Init/SetAlarm 里的 next_id / heap_capacity 等初值集中在此，
 *          便于将来调整默认策略（例如把 next_id 从 1 改成随机种子）时单点修改。
 */
static void st_mgr_init_defaults(T_SoftTimerMgr *mgr, const T_SoftTimerConfig *cfg)
{
    if(NULL == mgr || NULL == cfg)
    {
        return;
    }
    st_set_name(mgr->name, cfg->sMgrName);
    mgr->next_id       = 1;               /* 从 1 开始，0 保留作"未分配" */
    mgr->heap_capacity = ST_HEAP_INIT_CAPACITY;
    /* heap 数组由调用方 calloc 分配；其余字段（size/counts/flags）均已 calloc 清零 */
}

/* ========================== 生命周期 ========================== */

/**
 * @func         SoftTimerAPI_Init
 * @brief        创建软件定时器管理器
 * @details      按 t_Config 分配管理器；condattr 绑 CLOCK_MONOTONIC；创建线程池；
 *               再由 ThreadAPI_ThreadCreate 创建调度线程。任一环节失败均完整回滚。
 *               失败清理采用 do{...}while(0) + break 模式（避免 goto）。
 * @param[in]    ppt_Mgr:  管理器指针的指针，*ppt_Mgr 必须为 NULL
 * @param[in]    t_Config: 管理器配置（按值传入）
 * @retval       0/-1/-2
 * @author       zlzksrl
 * @date         2026-07-13
 */
int SoftTimerAPI_Init(T_SoftTimerMgr **ppt_Mgr, T_SoftTimerConfig t_Config)
{
    int  iRet             = 0;
    int  iMuxInit         = 0;
    int  iCvSchedInit     = 0;
    int  iCvDeleteInit    = 0;
    int  iCondAttrInit    = 0;
    pthread_condattr_t t_CondAttr;
    T_SoftTimerMgr *pt_Mgr = NULL;

    if(NULL == ppt_Mgr || NULL != *ppt_Mgr)
    {
        ST_LOGW("Init param invalid\n");
        return -1;
    }
    if(t_Config.iMinNum <= 0 || t_Config.iMaxNum < t_Config.iMinNum
       || t_Config.iQueueMaxSize <= 0 || t_Config.iIdleTimeoutMs < 0)
    {
        ST_LOGW("Init cfg invalid: min=%d max=%d qsz=%d idle=%d\n",
                t_Config.iMinNum, t_Config.iMaxNum,
                t_Config.iQueueMaxSize, t_Config.iIdleTimeoutMs);
        return -1;
    }
    if(t_Config.iSchedPriority < 0 || t_Config.iSchedPriority > ST_SCHED_PRIO_MAX)
    {
        ST_LOGW("Init cfg invalid: iSchedPriority=%d (allowed 0..%d)\n",
                t_Config.iSchedPriority, ST_SCHED_PRIO_MAX);
        return -1;
    }

    /* 失败集中回滚：do{...}while(0) + break 替代 goto */
    do
    {
        pt_Mgr = (T_SoftTimerMgr *)calloc(1, sizeof(T_SoftTimerMgr));
        if(NULL == pt_Mgr)
        {
            ST_LOGE("Init calloc mgr fail\n");
            iRet = -2;
            break;
        }
        st_mgr_init_defaults(pt_Mgr, &t_Config);
        pt_Mgr->heap = (T_SoftTimer **)calloc((size_t)pt_Mgr->heap_capacity,
                                              sizeof(T_SoftTimer *));
        if(NULL == pt_Mgr->heap)
        {
            ST_LOGE("[%s] Init calloc heap fail\n", pt_Mgr->name);
            iRet = -2;
            break;
        }

        /* 互斥锁 */
        if(pthread_mutex_init(&pt_Mgr->mux, NULL) != 0)
        {
            ST_LOGE("[%s] mutex_init fail\n", pt_Mgr->name);
            iRet = -2;
            break;
        }
        iMuxInit = 1;

        /* condattr 绑 CLOCK_MONOTONIC */
        if(pthread_condattr_init(&t_CondAttr) != 0)
        {
            ST_LOGE("[%s] condattr_init fail\n", pt_Mgr->name);
            iRet = -2;
            break;
        }
        iCondAttrInit = 1;
        if(pthread_condattr_setclock(&t_CondAttr, CLOCK_MONOTONIC) != 0)
        {
            ST_LOGE("[%s] condattr_setclock fail\n", pt_Mgr->name);
            iRet = -2;
            break;
        }
        if(pthread_cond_init(&pt_Mgr->cv_sched, &t_CondAttr) != 0)
        {
            ST_LOGE("[%s] cv_sched init fail\n", pt_Mgr->name);
            iRet = -2;
            break;
        }
        iCvSchedInit = 1;
        if(pthread_cond_init(&pt_Mgr->cv_delete, &t_CondAttr) != 0)
        {
            ST_LOGE("[%s] cv_delete init fail\n", pt_Mgr->name);
            iRet = -2;
            break;
        }
        iCvDeleteInit = 1;
        pthread_condattr_destroy(&t_CondAttr);
        iCondAttrInit = 0;

        /* 线程池 */
        T_ThreadPoolConfig t_PoolCfg;
        memset(&t_PoolCfg, 0, sizeof(t_PoolCfg));
        t_PoolCfg.iMinNum        = t_Config.iMinNum;
        t_PoolCfg.iMaxNum        = t_Config.iMaxNum;
        t_PoolCfg.iQueueMaxSize  = t_Config.iQueueMaxSize;
        t_PoolCfg.iIdleTimeoutMs = t_Config.iIdleTimeoutMs;
        pt_Mgr->pool = ThreadAPI_ThreadPoolCreate(t_PoolCfg);
        if(NULL == pt_Mgr->pool)
        {
            ST_LOGE("[%s] ThreadPoolCreate fail\n", pt_Mgr->name);
            iRet = -2;
            break;
        }

        /* 调度线程：先按 SCHED_FIFO/优先级 尝试，失败降级默认调度 */
        T_ThreadCreateConfig t_ThCfg;
        memset(&t_ThCfg, 0, sizeof(t_ThCfg));
        t_ThCfg.pThreadFunc        = st_sched_thread;
        t_ThCfg.pThreadFuncUserArg = pt_Mgr;
        t_ThCfg.sThreadName        = pt_Mgr->name;
        if(t_Config.iSchedPriority > 0)
        {
            t_ThCfg.eSetAttr       = 2;
            t_ThCfg.istacksize_MB  = 2;
            t_ThCfg.eDetachState   = PTHREAD_CREATE_JOINABLE;
            t_ThCfg.einheritsched  = PTHREAD_EXPLICIT_SCHED;
            t_ThCfg.eSchedPolicy   = SCHED_FIFO;
            t_ThCfg.iSchedPriority = t_Config.iSchedPriority;
        }
        else
        {
            t_ThCfg.eSetAttr       = 1;
            t_ThCfg.istacksize_MB  = 2;
        }
        if(ThreadAPI_ThreadCreate(&t_ThCfg) < 0)
        {
            ST_LOGW("[%s] sched thread create fail (prio path), fallback default\n",
                    pt_Mgr->name);
            memset(&t_ThCfg, 0, sizeof(t_ThCfg));
            t_ThCfg.pThreadFunc        = st_sched_thread;
            t_ThCfg.pThreadFuncUserArg = pt_Mgr;
            t_ThCfg.sThreadName        = pt_Mgr->name;
            t_ThCfg.eSetAttr           = 1;
            t_ThCfg.istacksize_MB      = 2;
            if(ThreadAPI_ThreadCreate(&t_ThCfg) < 0)
            {
                ST_LOGE("[%s] sched thread create fail\n", pt_Mgr->name);
                iRet = -2;
                break;
            }
        }
        pt_Mgr->sched_tid     = t_ThCfg.tThreadPid;
        pt_Mgr->sched_created = 1;

        *ppt_Mgr = pt_Mgr;
        ST_LOGI("[%s] Init OK (min=%d max=%d qsz=%d idle=%d prio=%d)\n",
                pt_Mgr->name, t_Config.iMinNum, t_Config.iMaxNum,
                t_Config.iQueueMaxSize, t_Config.iIdleTimeoutMs,
                t_Config.iSchedPriority);
        return 0;
    } while(0);

    /* 失败回滚（按 init 顺序反向清理） */
    if(NULL != pt_Mgr)
    {
        if(NULL != pt_Mgr->pool)
        {
            ThreadAPI_ThreadPoolDestroy(pt_Mgr->pool);
            pt_Mgr->pool = NULL;
        }
        if(iCvDeleteInit)
        {
            pthread_cond_destroy(&pt_Mgr->cv_delete);
        }
        if(iCvSchedInit)
        {
            pthread_cond_destroy(&pt_Mgr->cv_sched);
        }
        if(iCondAttrInit)
        {
            pthread_condattr_destroy(&t_CondAttr);
        }
        if(iMuxInit)
        {
            pthread_mutex_destroy(&pt_Mgr->mux);
        }
        if(NULL != pt_Mgr->heap)
        {
            free(pt_Mgr->heap);
        }
        free(pt_Mgr);
    }
    *ppt_Mgr = NULL;
    return iRet;
}
/**
 * @func         SoftTimerAPI_QuickInit
 * @brief        创建软件定时器管理器-内部调用SoftTimerAPI_Init,默认一些常规使用的配置
 * @details      同 SoftTimerAPI_Init
 T_SoftTimerConfig  t_Config = {.sMgrName=sMgrName, .iMinNum=2, .iMaxNum=5,
                                .iQueueMaxSize=32, .iIdleTimeoutMs=5000,.iSchedPriority=0 };
 * @param[in]    sMgrName:  定时器的名称
 * @param[out]   无
 * @return       T_SoftTimerMgr *pt_SoftTimerMgr
 * @retval       NULL:   初始化失败
 * @retval       非NULL:  初始化成功，并且是定时器管理器的句柄
 * @author       zlzksrl
 * @date         2026-07-13
 * @Version      V1.0.0
 */

T_SoftTimerMgr *SoftTimerAPI_QuickInit(char sMgrName[32])
{
    int ret = 0;
    T_SoftTimerMgr *pt_SoftTimerMgr = NULL;
    T_SoftTimerConfig t_SoftTimerConfig;
    memset(&t_SoftTimerConfig,0,sizeof(T_SoftTimerConfig));
    strncpy(t_SoftTimerConfig.sMgrName,sMgrName,32);
    sMgrName[31] = '\0';
    t_SoftTimerConfig.iMinNum = 2;
    t_SoftTimerConfig.iMaxNum = 5;
    t_SoftTimerConfig.iQueueMaxSize = 32;
    t_SoftTimerConfig.iIdleTimeoutMs = 5*1000;
    t_SoftTimerConfig.iSchedPriority = 0;
    ret = SoftTimerAPI_Init(&pt_SoftTimerMgr, t_SoftTimerConfig);
    if(ret < 0 || NULL == pt_SoftTimerMgr)
    {
        return NULL;
    } 
    return pt_SoftTimerMgr;
}
/**
 * @func         SoftTimerAPI_Destroy
 * @brief        销毁管理器
 * @details      置 stop_flag + broadcast → join 调度线程 → 销毁线程池（等 in-flight 完成）
 *               → 释放堆残余节点 + live_set 所有 handle → 销毁同步原语 → free mgr
 * @param[in]    ppt_Mgr
 * @retval       0/-1
 * @author       zlzksrl
 * @date         2026-07-13
 */
int SoftTimerAPI_Destroy(T_SoftTimerMgr **ppt_Mgr)
{
    if(NULL == ppt_Mgr || NULL == *ppt_Mgr)
    {
        ST_LOGW("Destroy param invalid\n");
        return -1;
    }
    T_SoftTimerMgr *pt_Mgr = *ppt_Mgr;
    char sMgrName[MAX_SOFTTIMERNAME_LEN + 1];
    st_set_name(sMgrName, pt_Mgr->name);

    /* 1) 请求调度线程退出 */
    pthread_mutex_lock(&pt_Mgr->mux);
    pt_Mgr->stop_flag = 1;
    pthread_cond_broadcast(&pt_Mgr->cv_sched);
    pthread_cond_broadcast(&pt_Mgr->cv_delete);
    pthread_mutex_unlock(&pt_Mgr->mux);

    /* 2) join 调度线程 */
    if(pt_Mgr->sched_created)
    {
        pthread_join(pt_Mgr->sched_tid, NULL);
        pt_Mgr->sched_created = 0;
    }

    /* 3) 销毁线程池（内部等 in-flight 任务完成） */
    if(NULL != pt_Mgr->pool)
    {
        ThreadAPI_ThreadPoolDestroy(pt_Mgr->pool);
        pt_Mgr->pool = NULL;
    }

    /* 4) 扫尾：释放堆残余节点 + live_set 所有 handle */
    pthread_mutex_lock(&pt_Mgr->mux);
    while(pt_Mgr->heap_size > 0)
    {
        T_SoftTimer *node = st_heap_pop_top_locked(pt_Mgr);
        if(NULL != node)
        {
            /* handle 由 live_set 统一释放 */
            if(NULL != node->handle)
            {
                node->handle->node = NULL;
            }
            free(node);
        }
    }
    T_SoftTimerHandle *cur = pt_Mgr->live_head;
    while(NULL != cur)
    {
        T_SoftTimerHandle *nxt = cur->next_in_set;
        free(cur);
        cur = nxt;
    }
    pt_Mgr->live_head  = NULL;
    pt_Mgr->live_count = 0;
    pthread_mutex_unlock(&pt_Mgr->mux);

    /* 5) 销毁同步原语 */
    pthread_cond_destroy(&pt_Mgr->cv_delete);
    pthread_cond_destroy(&pt_Mgr->cv_sched);
    pthread_mutex_destroy(&pt_Mgr->mux);

    /* 6) 释放堆数组 + mgr 本体 */
    if(NULL != pt_Mgr->heap)
    {
        free(pt_Mgr->heap);
    }
    free(pt_Mgr);
    *ppt_Mgr = NULL;

    ST_LOGI("[%s] Destroy OK\n", sMgrName);
    return 0;
}

/**
 * @func         SoftTimerAPI_StatsGet
 * @brief        获取运行统计快照
 */
int SoftTimerAPI_StatsGet(T_SoftTimerMgr *pt_Mgr, T_SoftTimerStats *pt_Stats)
{
    if(NULL == pt_Mgr || NULL == pt_Stats)
    {
        ST_LOGW("StatsGet param invalid\n");
        return -1;
    }
    pthread_mutex_lock(&pt_Mgr->mux);
    pt_Stats->ulTotalSet      = pt_Mgr->ulTotalSet;
    pt_Stats->ulTotalFired    = pt_Mgr->ulTotalFired;
    pt_Stats->ulTotalCanceled = pt_Mgr->ulTotalCanceled;
    pt_Stats->ulTotalOverrun  = pt_Mgr->ulTotalOverrun;
    pt_Stats->iActiveCount    = pt_Mgr->iActiveCount;
    pt_Stats->iPeakActive     = pt_Mgr->iPeakActive;
    pthread_mutex_unlock(&pt_Mgr->mux);
    return 0;
}

/* ========================== SetAlarm ========================== */

/**
 * @func         SoftTimerAPI_SetAlarm
 * @brief        注册一个定时器
 * @details      分配 node + handle；deadline = now + iFirstDelayMs；push heap；
 *               若 node 成为堆顶则 signal(cv_sched) 唤醒调度线程。
 * @retval       0 / -1(参数无效) / -2(管理器已销毁) / -4(资源不足)
 */
int SoftTimerAPI_SetAlarm(T_SoftTimerMgr *pt_Mgr,
                          T_SoftTimerAlarm t_Alarm,
                          T_SoftTimerHandle **ppt_Handle)
{
    if(NULL == pt_Mgr || NULL == ppt_Handle || NULL != *ppt_Handle)
    {
        ST_LOGW("SetAlarm param invalid\n");
        return -1;
    }
    if(NULL == t_Alarm.cb || t_Alarm.iFirstDelayMs < 0 || t_Alarm.iPeriodMs < 0)
    {
        ST_LOGW("[%s] SetAlarm alarm cfg invalid (cb_null=%d first=%d period=%d)\n",
                pt_Mgr->name, (NULL == t_Alarm.cb ? 1 : 0),
                t_Alarm.iFirstDelayMs, t_Alarm.iPeriodMs);
        return -1;
    }

    T_SoftTimer       *node   = (T_SoftTimer *)calloc(1, sizeof(T_SoftTimer));
    T_SoftTimerHandle *handle = (T_SoftTimerHandle *)calloc(1, sizeof(T_SoftTimerHandle));
    if(NULL == node || NULL == handle)
    {
        ST_LOGE("[%s] SetAlarm calloc fail\n", pt_Mgr->name);
        if(NULL != node)
        {
            free(node);
        }
        if(NULL != handle)
        {
            free(handle);
        }
        return -4;
    }

    node->mgr          = pt_Mgr;
    node->handle       = handle;
    node->cb           = t_Alarm.cb;
    node->user_ctx     = t_Alarm.user_ctx;
    node->iPeriodMs    = t_Alarm.iPeriodMs;
    node->ePeriodMode  = t_Alarm.ePeriodMode;
    node->state        = ST_IN_HEAP;
    node->heap_idx     = -1;
    st_set_name(node->timer_name, t_Alarm.sTimerName);

    pthread_mutex_lock(&pt_Mgr->mux);
    if(pt_Mgr->stop_flag)
    {
        pthread_mutex_unlock(&pt_Mgr->mux);
        free(node);
        free(handle);
        ST_LOGW("[%s] SetAlarm rejected: destroying\n", pt_Mgr->name);
        return -2;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    st_timespec_add_ms(&node->deadline, &now, (long long)t_Alarm.iFirstDelayMs);
    node->last_scheduled = node->deadline;

    if(st_heap_push_locked(pt_Mgr, node) < 0)
    {
        pthread_mutex_unlock(&pt_Mgr->mux);
        free(node);
        free(handle);
        ST_LOGE("[%s] SetAlarm heap_push fail\n", pt_Mgr->name);
        return -4;
    }

    handle->ullId       = pt_Mgr->next_id++;
    handle->node        = node;
    handle->is_consumed = 0;
    st_liveset_add_locked(pt_Mgr, handle);

    pt_Mgr->ulTotalSet   += 1;
    pt_Mgr->iActiveCount += 1;
    if(pt_Mgr->iActiveCount > pt_Mgr->iPeakActive)
    {
        pt_Mgr->iPeakActive = pt_Mgr->iActiveCount;
    }

    /* cv_sched 仅 1 waiter（调度线程），signal 足矣 */
    if(node->heap_idx == 0)
    {
        pthread_cond_signal(&pt_Mgr->cv_sched);
    }
    /* 在锁内赋值 *ppt_Handle：闭合 iFirstDelayMs=0 场景下调度线程可能在解锁后
     * 立即 pop+dispatch 的理论窗口。用户变量赋值先于任何 cb 可能启动的时刻。 */
    *ppt_Handle = handle;
    pthread_mutex_unlock(&pt_Mgr->mux);
    return 0;
}
/**
 * @func         SoftTimerAPI_QuickSetAlarm
 * @brief        注册一个定时器-内部调用 SoftTimerAPI_SetAlarm,默认一些常规使用的配置
 * @details      同 SoftTimerAPI_SetAlarm
 T_SoftTimerAlarm   t_SoftTimerAlarm   = {   .cb=cb, 
                                             .user_ctx=user_ctx
                                             .iFirstDelayMs=iFirstDelayMs,
                                             .iPeriodMs=iPeriodMs,
                                             .ePeriodMode=SOFTTIMER_PERIOD_FROM_END,
                                             .sTimerName="QuickSetAlarm" };

 * @param[in]        pt_Mgr:      管理器句柄
                     cb             < 用户回调，不能为 NULL
                    *user_ctx       < 传给 cb 的上下文，可为 NULL
                     iFirstDelayMs  < 首次触发延迟(ms)，>=0 
                     iPeriodMs      < 周期(ms)，0=单次；>0=周期
                     char         sTimerName[32]
 * @param[out]   无
 * @return       T_SoftTimerHandle *pt_SoftTimerHandle
 * @retval       NULL:   初始化失败
 * @retval       非NULL:  初始化成功，并且是定时器的句柄
 * @author       zlzksrl
 * @date         2026-07-13
 * @Version      V1.0.0
 */
T_SoftTimerHandle *SoftTimerAPI_QuickSetAlarm(T_SoftTimerMgr *pt_Mgr
                                                ,SoftTimerCb  cb             /**< 用户回调，不能为 NULL */
                                                ,void        *user_ctx       /**< 传给 cb 的上下文，可为 NULL */
                                                ,int          iFirstDelayMs  /**< 首次触发延迟(ms)，>=0 */
                                                ,int          iPeriodMs      /**< 周期(ms)，0=单次；>0=周期 */
                                                ,char         sTimerName[32]
                                                )
{
    int ret = -1;
    T_SoftTimerHandle *pt_SoftTimerHandle = NULL;
    T_SoftTimerAlarm t_SoftTimerAlarm;
    memset(&t_SoftTimerAlarm,0,sizeof(T_SoftTimerAlarm));
    t_SoftTimerAlarm.cb            = cb;
    t_SoftTimerAlarm.user_ctx      = user_ctx;
    t_SoftTimerAlarm.iFirstDelayMs = iFirstDelayMs;
    t_SoftTimerAlarm.iPeriodMs     = iPeriodMs;
    strncpy(t_SoftTimerAlarm.sTimerName,sTimerName,32);
    t_SoftTimerAlarm.sTimerName[31] = '\0';    
    ret = SoftTimerAPI_SetAlarm(pt_Mgr, t_SoftTimerAlarm, &pt_SoftTimerHandle);
    if(ret < 0 || NULL == pt_SoftTimerHandle)
    {
        return NULL;
    }
    return pt_SoftTimerHandle;
}

/* ========================== 调度线程 ========================== */

/**
 * @brief 调度线程主循环：cond_timedwait 睡到堆顶 deadline；到点投递线程池
 *
 * 主循环逻辑（伪代码）:
 *   lock(mux)
 *   while(!stop_flag) {
 *     top = peek_heap()
 *     if(top == NULL)     cond_wait(cv_sched)               // 空堆:等 SetAlarm 唤醒
 *     elif(top->deadline > now)
 *                         cond_timedwait(cv_sched, deadline) // 未到点:睡到 deadline
 *     else {
 *       node = pop_heap();  node->state = DISPATCHED
 *       unlock; addret = ThreadPool_AddTaskTry(...); lock
 *       if(addret >= 0)   ulTotalFired++;                    // 投递成功
 *       elif(is_canceled) free(node)                          // Path C 竞态
 *       elif(stop_flag)   free(node)                          // 正在销毁
 *       else              requeue(node, +10ms); overrun++    // 池满退避
 *     }
 *   }
 *   unlock(mux)
 *
 * 统计口径:
 *   - AddTaskTry 成功 → ulTotalFired += 1（含 Path C 早退投递）
 *   - 池满 requeue   → ulTotalOverrun += 1（不区分模式，FROM_END 可能与 wrapper 处双计）
 *
 * 优先级:
 *   - iSchedPriority > 0 时以 SCHED_FIFO 创建；失败降级 SCHED_OTHER。
 *   - 优先级只影响"到点响应速度"，不影响 worker 执行 cb 的速度（详见 设计探讨.md §2）。
 */
static void *st_sched_thread(void *arg)
{
    T_SoftTimerMgr *mgr = (T_SoftTimerMgr *)arg;
    if(NULL == mgr)
    {
        return NULL;
    }
    ST_LOGI("[%s] sched thread start\n", mgr->name);

    pthread_mutex_lock(&mgr->mux);
    while(!mgr->stop_flag)
    {
        T_SoftTimer *top = st_heap_peek_top_locked(mgr);
        if(NULL == top)
        {
            /* 空堆：无期限等待，等 SetAlarm/Destroy 唤醒 */
            pthread_cond_wait(&mgr->cv_sched, &mgr->mux);
            continue;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if(st_timespec_cmp(&top->deadline, &now) > 0)
        {
            /* 未到点：睡到 deadline，或被 signal/broadcast 提前唤醒 */
            pthread_cond_timedwait(&mgr->cv_sched, &mgr->mux, &top->deadline);
            continue;
        }

        /* 到点：从堆摘出，尝试投池 */
        T_SoftTimer *node = st_heap_pop_top_locked(mgr);
        if(NULL == node)
        {
            continue;
        }
        node->state = ST_DISPATCHED;

        pthread_mutex_unlock(&mgr->mux);
        int addret = ThreadAPI_ThreadPoolAddTaskTry(mgr->pool,
                                                    st_wrapper_task, node);
        pthread_mutex_lock(&mgr->mux);

        if(addret >= 0)
        {
            /* 投递成功 —— 语义：任务已入池排队 */
            mgr->ulTotalFired += 1;
            continue;
        }

        /* 池满 —— overrun +1 */
        mgr->ulTotalOverrun += 1;

        /* Path C 竞态：若 pop 后 Delete 标 canceled，直接 free 短路 */
        if(node->is_canceled)
        {
            free(node);
            continue;
        }

        if(mgr->stop_flag)
        {
            /* 正在销毁：直接释放 node，handle 由 Destroy 扫尾处理 */
            if(NULL != node->handle)
            {
                node->handle->node = NULL;
            }
            free(node);
            continue;
        }

        /* 正常 requeue：过 ST_POOL_ADDTASK_RETRY_MS 再试 */
        struct timespec retry_at;
        clock_gettime(CLOCK_MONOTONIC, &retry_at);
        st_timespec_add_ms(&retry_at, &retry_at, ST_POOL_ADDTASK_RETRY_MS);
        node->deadline = retry_at;
        node->state    = ST_IN_HEAP;
        (void)st_heap_push_locked(mgr, node);
        ST_LOGW("[%s][%s] pool full, requeue in %dms\n",
                mgr->name, node->timer_name, ST_POOL_ADDTASK_RETRY_MS);
    }
    pthread_mutex_unlock(&mgr->mux);

    ST_LOGI("[%s] sched thread exit\n", mgr->name);
    return NULL;
}

/* ========================== 漂移计算 ========================== */

/**
 * @brief 按 ePeriodMode 计算周期定时器下一次 deadline，并累加 overrun 计数
 * @param[in]  cb_end  本次 cb 结束时刻
 * @details
 *  - FROM_END:       next = cb_end + P；若 cb_end 晚于本次 last_scheduled + P，overrun +1
 *  - FROM_SCHEDULED: next = last_scheduled + P；若 next <= cb_end，snap 到下一未来栅格，
 *                    统计 overrun += 跳过的栅格数
 */
static void st_compute_next_deadline_locked(T_SoftTimer *node,
                                            const struct timespec *cb_end)
{
    if(node->iPeriodMs <= 0)
    {
        return;
    }
    if(SOFTTIMER_PERIOD_FROM_END == node->ePeriodMode)
    {
        /* overrun 判定：cb 实际结束时刻 vs 本次预定 deadline + period */
        long long over_ms = st_timespec_diff_ms(cb_end, &node->last_scheduled);
        if(over_ms > (long long)node->iPeriodMs)
        {
            node->mgr->ulTotalOverrun += 1;
        }
        st_timespec_add_ms(&node->deadline, cb_end, (long long)node->iPeriodMs);
        node->last_scheduled = node->deadline;
        return;
    }
    /* FROM_SCHEDULED */
    struct timespec next;
    st_timespec_add_ms(&next, &node->last_scheduled, (long long)node->iPeriodMs);
    /* 严格 '<'：栅格恰好落在 cb_end 时视为"未跳过"，与 FROM_END 的 '>' 判定对齐 */
    while(st_timespec_cmp(&next, cb_end) < 0)
    {
        st_timespec_add_ms(&next, &next, (long long)node->iPeriodMs);
        node->mgr->ulTotalOverrun += 1;
    }
    node->deadline       = next;
    node->last_scheduled = next;
}

/* ========================== wrapper（线程池任务） ========================== */

/**
 * @brief 线程池 wrapper：执行 user cb + 收尾（周期续期 / 单次墓碑 / 各删除路径清理）
 *
 * 四阶段流程:
 *   1) 入口检查 canceled  ── Path C 竞态早退（只 free(node)，handle 由 Delete 释放）
 *   2) 转 ST_RUNNING，snapshot handle 指针，unlock 执行 cb（不持锁）
 *   3) 记录 cb_end 时刻
 *   4) 收尾分派（持锁）:
 *        ┌ is_canceled + ST_ORPHANED   → free(node)；若 node->handle 非 NULL 一并 free(handle)
 *        │                               （区分 D1 已释放 handle 与 D2 -2 延迟释放）
 *        ├ is_canceled + ST_RUNNING    → broadcast(cv_delete)，Delete D2 主路径接管
 *        ├ 周期正常                    → 计算 next deadline → push heap（失败则墓碑化）
 *        └ 单次正常                    → 标墓碑（node==NULL, is_consumed=1）；free(node)
 *
 * 内存所有权口径（详见文件头"内存所有权"块）:
 *   - Path C 早退  : Delete 释放 handle，wrapper 释放 node
 *   - D1 自删     : Delete 释放 handle（并置 node->handle=NULL），wrapper 释放 node
 *   - D2 主路径   : wrapper 广播，Delete 醒来后释放 node+handle
 *   - D2 -2 分支  : Delete 只摘 live_set 不释放；wrapper 通过 node->handle 非 NULL 释放二者
 *   - 单次完成   : wrapper 释放 node，handle 打墓碑等 Delete 回收（Path A）
 *
 * @note  入口的 ST_DEBUG_DISPATCH_DELAY_MS 睡眠仅用于测试构造 Path C 窗口；生产为 0（no-op）
 */
static void *st_wrapper_task(void *arg)
{
    T_SoftTimer    *node = (T_SoftTimer *)arg;
    if(NULL == node)
    {
        return NULL;
    }
    T_SoftTimerMgr *mgr  = node->mgr;

#if ST_DEBUG_DISPATCH_DELAY_MS > 0
    /* 拉长 DISPATCHED → RUNNING 的时间窗，供 Path C 测试用 */
    usleep((useconds_t)ST_DEBUG_DISPATCH_DELAY_MS * 1000U);
#endif

    /* 1) 进入 RUNNING 前检查 canceled（DISPATCHED 期间被 Delete 标记为早退） */
    pthread_mutex_lock(&mgr->mux);
    if(node->is_canceled)
    {
        /* Path C: DISPATCHED 早退，wrapper 释放 node；handle 由 Delete 释放 */
        pthread_mutex_unlock(&mgr->mux);
        free(node);
        return NULL;
    }
    node->state         = ST_RUNNING;
    node->running_count = 1;
    node->running_tid   = pthread_self();
    T_SoftTimerHandle *handle_snapshot = node->handle;
    pthread_mutex_unlock(&mgr->mux);

    /* 2) 执行用户回调（不持锁） */
    node->cb(handle_snapshot, node->user_ctx);

    /* 3) cb 结束时刻 */
    struct timespec cb_end;
    clock_gettime(CLOCK_MONOTONIC, &cb_end);

    /* 4) 收尾（持锁） */
    pthread_mutex_lock(&mgr->mux);
    node->running_count = 0;

    int is_canceled = node->is_canceled;
    int is_periodic = (node->iPeriodMs > 0);

    if(is_canceled)
    {
        /* Delete 已介入。按 state 分派：
         *   ST_ORPHANED  -> D1(自删) 或 D2(stop_flag -2 分支) 已放弃 handle 所有权，wrapper 释放 node；
         *                   若 node->handle 非 NULL 表示 D2 -2 分支延迟释放（防 UAF），wrapper 一并释放 handle。
         *                   D1 路径 Delete 已 free(handle) 并置 node->handle=NULL，此处不再触碰。
         *   ST_RUNNING   -> D2 正在 cv_delete 等待，wrapper broadcast 后由 Delete 释放 node+handle
         */
        if(ST_ORPHANED == node->state)
        {
            T_SoftTimerHandle *h_to_free = node->handle;
            pthread_mutex_unlock(&mgr->mux);
            free(node);
            if(NULL != h_to_free)
            {
                free(h_to_free);
            }
            return NULL;
        }
        pthread_cond_broadcast(&mgr->cv_delete);
        pthread_mutex_unlock(&mgr->mux);
        return NULL;
    }

    if(is_periodic)
    {
        /* 周期正常轮转：计算下次 deadline，重回堆 */
        st_compute_next_deadline_locked(node, &cb_end);
        node->state = ST_IN_HEAP;
        if(st_heap_push_locked(mgr, node) < 0)
        {
            /* 极端：realloc 失败，只能就地释放（handle 打墓碑等 Delete 回收） */
            ST_LOGE("[%s][%s] periodic requeue fail\n",
                    mgr->name, node->timer_name);
            if(NULL != node->handle)
            {
                node->handle->node        = NULL;
                node->handle->is_consumed = 1;
            }
            mgr->iActiveCount -= 1;
            free(node);
            pthread_mutex_unlock(&mgr->mux);
            return NULL;
        }
        int is_top = (node->heap_idx == 0);
        if(is_top)
        {
            pthread_cond_signal(&mgr->cv_sched);
        }
        pthread_mutex_unlock(&mgr->mux);
        return NULL;
    }

    /* 单次正常完成：打墓碑（handle 保留，等 Delete 回收） */
    if(NULL != node->handle)
    {
        node->handle->node        = NULL;
        node->handle->is_consumed = 1;
    }
    mgr->iActiveCount -= 1;
    free(node);
    pthread_mutex_unlock(&mgr->mux);
    return NULL;
}

/* ========================== Delete 四路径 ========================== */

/**
 * @func         SoftTimerAPI_Delete
 * @brief        删除定时器（支持任意状态、含自删除、含运行中他线程删）
 *
 * 分派决策表:
 *   ┌──────────────────────────────┬───────┬─────────────────────────────────────────┐
 *   │ 命中条件                     │ Path  │ 动作                                    │
 *   ├──────────────────────────────┼───────┼─────────────────────────────────────────┤
 *   │ handle->is_consumed          │  A    │ 摘 live_set → free(handle)              │
 *   │ 或 handle->node == NULL      │       │ 不改 iActiveCount / ulTotalCanceled     │
 *   ├──────────────────────────────┼───────┼─────────────────────────────────────────┤
 *   │ state == IN_HEAP             │  B    │ heap_remove_at → free(node,handle)      │
 *   │                              │       │ 堆顶被删时 signal cv_sched              │
 *   ├──────────────────────────────┼───────┼─────────────────────────────────────────┤
 *   │ state == DISPATCHED          │  C    │ 标 canceled + node->handle=NULL        │
 *   │                              │       │ wrapper 早退释放 node；本函数释放 handle│
 *   ├──────────────────────────────┼───────┼─────────────────────────────────────────┤
 *   │ state == RUNNING & self_tid  │  D1   │ 标 canceled + state=ORPHANED           │
 *   │ (cb 内调用 Delete 自己)      │       │ node->handle=NULL；立即 free(handle)   │
 *   │                              │       │ wrapper 收尾释放 node                   │
 *   ├──────────────────────────────┼───────┼─────────────────────────────────────────┤
 *   │ state == RUNNING & other_tid │  D2   │ 标 canceled → cv_delete_wait            │
 *   │ (他线程等 cb 结束)           │       │ 唤醒后 free(node,handle)，返回 0        │
 *   ├──────────────────────────────┼───────┼─────────────────────────────────────────┤
 *   │ D2 等待期间 stop_flag 置位   │D2 -2  │ node 转 ORPHANED (保留 node->handle)    │
 *   │ (Destroy 抢先，cb 仍持       │       │ 摘 live_set；*ppt_Handle=NULL；返回 -2  │
 *   │  handle_snapshot 在锁外)     │       │ node+handle 交 wrapper 收尾释放 (防UAF) │
 *   └──────────────────────────────┴───────┴─────────────────────────────────────────┘
 *
 * 返回值:
 *   0   成功；*ppt_Handle 已置 NULL
 *  -1   参数无效（NULL 指针）
 *  -2   管理器正在销毁（入口早退保留 *ppt_Handle；D2 -2 分支置 NULL 但 handle 由 wrapper 释放）
 *  -3   句柄非本管理器 / 已失效；*ppt_Handle 保持原值
 *
 * 关键不变量:
 *   - Path A 不累加 ulTotalCanceled 也不递减 iActiveCount（wrapper 单次收尾时已 -1）
 *   - Path B/C/D1/D2/D2-2 均累加 ulTotalCanceled 且 iActiveCount -= 1
 *   - D1 通过 node->handle == NULL 标识 handle 已释放；wrapper ORPHANED 分支据此判断
 */
int SoftTimerAPI_Delete(T_SoftTimerMgr *pt_Mgr, T_SoftTimerHandle **ppt_Handle)
{
    if(NULL == pt_Mgr || NULL == ppt_Handle || NULL == *ppt_Handle)
    {
        ST_LOGW("Delete param invalid\n");
        return -1;
    }
    T_SoftTimerHandle *handle = *ppt_Handle;

    pthread_mutex_lock(&pt_Mgr->mux);
    if(pt_Mgr->stop_flag)
    {
        pthread_mutex_unlock(&pt_Mgr->mux);
        ST_LOGW("[%s] Delete rejected: destroying\n", pt_Mgr->name);
        return -2;
    }
    if(!st_liveset_contains_locked(pt_Mgr, handle))
    {
        pthread_mutex_unlock(&pt_Mgr->mux);
        ST_LOGW("[%s] Delete: handle not owned/valid\n", pt_Mgr->name);
        return -3;
    }

    /* Path A: 墓碑 —— 单次已 fire，node 已由 wrapper 释放。
     * 语义上是"回收已完成定时器"，不属于"取消"：
     *   - iActiveCount 不递减：wrapper 单次收尾时已 -1，本次仅回收 handle 内存；
     *   - ulTotalCanceled 不累加：与文件头统计口径一致（Path A 排除）。 */
    if(handle->is_consumed || NULL == handle->node)
    {
        st_liveset_remove_locked(pt_Mgr, handle);
        pthread_mutex_unlock(&pt_Mgr->mux);
        free(handle);
        *ppt_Handle = NULL;
        return 0;
    }

    T_SoftTimer *node = handle->node;

    /* Path B: 在堆中 —— node 直接摘除释放，不需要标 canceled */
    if(ST_IN_HEAP == node->state)
    {
        /* 仅当被删的正是堆顶时才 signal 调度线程（与 SetAlarm 的堆顶判定一致） */
        int was_top = (0 == node->heap_idx);
        st_heap_remove_at_locked(pt_Mgr, node->heap_idx);
        st_liveset_remove_locked(pt_Mgr, handle);
        pt_Mgr->ulTotalCanceled += 1;
        pt_Mgr->iActiveCount    -= 1;
        if(was_top)
        {
            pthread_cond_signal(&pt_Mgr->cv_sched);
        }
        pthread_mutex_unlock(&pt_Mgr->mux);
        free(node);
        free(handle);
        *ppt_Handle = NULL;
        return 0;
    }

    /* Path C: DISPATCHED（已投池未开始） */
    if(ST_DISPATCHED == node->state)
    {
        /* 让 wrapper 早退释放 node；分离 handle 与 node */
        node->is_canceled = 1;
        node->handle      = NULL;
        st_liveset_remove_locked(pt_Mgr, handle);
        pt_Mgr->ulTotalCanceled += 1;
        pt_Mgr->iActiveCount    -= 1;
        pthread_mutex_unlock(&pt_Mgr->mux);
        free(handle);
        *ppt_Handle = NULL;
        return 0;
    }

    /* Path D: RUNNING —— 分自删除 / 他线程 */
    if(ST_RUNNING == node->state)
    {
        if(pthread_equal(pthread_self(), node->running_tid))
        {
            /* D1: 自删除 —— 不能等 cv_delete（会死锁）
             *     node 变孤儿，wrapper 收尾释放 node；Delete 立即释放 handle */
            node->is_canceled = 1;
            node->state       = ST_ORPHANED;
            node->handle      = NULL;
            st_liveset_remove_locked(pt_Mgr, handle);
            pt_Mgr->ulTotalCanceled += 1;
            pt_Mgr->iActiveCount    -= 1;
            pthread_mutex_unlock(&pt_Mgr->mux);
            free(handle);
            *ppt_Handle = NULL;
            return 0;
        }
        /* D2: 他线程删 —— 等 wrapper 收尾（running_count 归 0） */
        node->is_canceled = 1;
        while(1 == node->running_count && !pt_Mgr->stop_flag)
        {
            pthread_cond_wait(&pt_Mgr->cv_delete, &pt_Mgr->mux);
        }
        if(pt_Mgr->stop_flag && 1 == node->running_count)
        {
            /* Destroy 抢在 wrapper 收尾之前推进：cb 仍在锁外执行，其 handle_snapshot
             * 指向 handle —— 本函数**不能立即释放** handle（会 UAF：cb 若访问
             * pt_Handle->ullId 触发用崩溃）。改由 wrapper 在 cb 返回后的 ORPHANED
             * 分支通过 node->handle 判非 NULL 释放。
             *   - state 转 ST_ORPHANED 让 wrapper 知道走"孤儿"收尾路径；
             *   - node->handle 保持有效，作为 wrapper 释放 handle 的信号（区别于 D1）；
             *   - 但 live_set 立即摘除，防止 Destroy 扫尾时二次释放 handle；
             *   - *ppt_Handle 置 NULL，让用户变量在函数返回后即失效。 */
            node->state  = ST_ORPHANED;
            st_liveset_remove_locked(pt_Mgr, handle);
            pt_Mgr->ulTotalCanceled += 1;
            pt_Mgr->iActiveCount    -= 1;
            pthread_mutex_unlock(&pt_Mgr->mux);
            /* 不 free(handle) —— 由 wrapper 收尾释放 */
            *ppt_Handle = NULL;
            return -2;
        }
        /* wrapper 已完成本轮：node 未被 wrapper 释放（is_canceled 分支且 state==RUNNING） */
        st_liveset_remove_locked(pt_Mgr, handle);
        pt_Mgr->ulTotalCanceled += 1;
        pt_Mgr->iActiveCount    -= 1;
        pthread_mutex_unlock(&pt_Mgr->mux);
        free(node);
        free(handle);
        *ppt_Handle = NULL;
        return 0;
    }

    /* 兜底：ST_ORPHANED 不可能被 Delete 命中（handle 已在 D1 中分离） */
    pthread_mutex_unlock(&pt_Mgr->mux);
    ST_LOGE("[%s] Delete: unexpected state %d\n",
            pt_Mgr->name, (int)node->state);
    return -3;
}

/* ========================== END SoftTimer.c ========================== */
