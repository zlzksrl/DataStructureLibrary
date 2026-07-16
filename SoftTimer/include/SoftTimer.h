/**
 * @file        SoftTimer.h
 * @brief       LinuxARM-PublicLib-软件定时器-公共API头文件
 * @details     IMX6ULL平台
 *              本文件提供 SoftTimer 软件定时器模块的公共API。
 *              SoftTimer 基于 ThreadManage（线程池 + 调度线程）实现：
 *              内部一个调度线程用 pthread_cond_timedwait(CLOCK_MONOTONIC) 睡眠到堆顶
 *              deadline；到点后从 min-heap 摘出节点，投递给线程池执行用户回调。
 *
 *              核心特性:
 *              - 非轮询计时:      调度线程 cond_timedwait(CLOCK_MONOTONIC) 精准唤醒
 *              - 单次/周期定时:   iPeriodMs=0 单次；>0 周期
 *              - 两种漂移策略:    FROM_END(间隔精确) / FROM_SCHEDULED(栅格精确 + snap)
 *              - 用户回调并发:    回调在线程池执行，多个回调可并行（用户配置线程池大小）
 *              - 安全删除:        Delete 支持任意状态（含 cb 内自删除，含 cb 运行中他线程删）
 *              - 二级指针句柄:    Delete 后自动置 NULL，重复删返回 -1
 *              - 运行统计:        StatsGet 查累计 set/fired/canceled/overrun、当前活跃/峰值
 *              - 线程池分离:      调度线程用 ThreadAPI_ThreadCreate，回调分发用 ThreadPool
 *
 *              使用流程:
 *                SoftTimerAPI_Init(&mgr, cfg)
 *                → SoftTimerAPI_SetAlarm(mgr, alarm, &h)
 *                → (定时器自动触发用户 cb 直至 Delete 或单次结束)
 *                → SoftTimerAPI_Delete(mgr, &h)
 *                → SoftTimerAPI_Destroy(&mgr)
 *
 *              内部架构（一图看懂）:
 *
 *                   用户线程                    调度线程                 线程池 worker
 *                 ┌──────────┐    push      ┌────────────┐   dispatch  ┌──────────┐
 *                 │ SetAlarm ├─────────────►│ min-heap   ├────────────►│ wrapper  │
 *                 │  Delete  │  signal      │ (deadline) │  AddTaskTry │  → cb()  │
 *                 └────┬─────┘              │            │             │  → 续期  │
 *                      │  live_set          │ cond_time  │             └────┬─────┘
 *                      └───────────► ◄──────┤ dwait()    │◄─────────────────┘
 *                       (身份验证)          │            │   周期 push 回堆
 *                                           └────────────┘   / 单次墓碑
 *
 *                计时（sched 线程）与执行（线程池）分离：cb 可并行、可安全取消。
 *
 *              最小 quick-start 示例:
 *                static void on_tick(T_SoftTimerHandle *h, void *ctx) { (void)h; (void)ctx; }
 *                T_SoftTimerMgr    *mgr = NULL;
 *                T_SoftTimerHandle *h   = NULL;
 *                T_SoftTimerConfig  cfg = { .sMgrName="demo", .iMinNum=2, .iMaxNum=4,
 *                                           .iQueueMaxSize=32, .iIdleTimeoutMs=5000 };
 *                SoftTimerAPI_Init(&mgr, cfg);
 *                T_SoftTimerAlarm   a   = { .cb=on_tick, .iFirstDelayMs=100,
 *                                           .iPeriodMs=1000,
 *                                           .ePeriodMode=SOFTTIMER_PERIOD_FROM_END,
 *                                           .sTimerName="tick" };
 *                SoftTimerAPI_SetAlarm(mgr, a, &h);
 *                //... 业务运行 ... 
 *                SoftTimerAPI_Delete(mgr, &h);       //h 被置 NULL 
 *                SoftTimerAPI_Destroy(&mgr);         //mgr 被置 NULL 
 *
 *              极端资源不足行为:
 *                周期定时器在 wrapper 收尾时需将 node 重新压入堆（heap_ensure_capacity
 *                内部走 realloc）。若 realloc 失败（内存耗尽等），本轮周期定时器**提前
 *                终止**：node 释放、handle 打墓碑（is_consumed=1）；用户 cb 不再被触发，
 *                须由用户 Delete 或 Destroy 扫尾回收 handle。此路径仅记录 ST_LOGE 日志，
 *                统计字段不体现（既非取消、也非 overrun 语义）。生产环境应通过监控进程
 *                RSS/OOM 预防。
 *
 *              错误码约定:
 *                 0  : 成功
 *                -1  : 参数无效或未初始化
 *                -2  : 管理器正在销毁 / 已销毁
 *                -3  : 句柄非本管理器 / 已失效
 *                -4  : 资源不足（内存分配失败等，仅 SetAlarm 返回）
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-13
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-13
 * @Version     V1.0.0
 * @brief       创建文件，提供软件定时器全套API
 * @author      zlzksrl
 */
#ifndef __SoftTimer_H__
#define __SoftTimer_H__

#ifdef __cplusplus
 extern "C" {
#endif


/* ================================================================== */
/*                                                                    */
/*     类型定义                                                       */
/*                                                                    */
/* ================================================================== */

/** @brief SoftTimer 管理器句柄前向声明（隐藏内部实现，opaque pointer） */
typedef struct T_SOFTTIMERMGR    T_SoftTimerMgr;

/** @brief SoftTimer 单个定时器句柄前向声明（隐藏内部实现，opaque pointer） */
typedef struct T_SOFTTIMERHANDLE T_SoftTimerHandle;

/**
 * @brief 用户定时器回调原型
 * @param pt_Handle 触发本次回调的定时器句柄（回调内可用于查询/自删除）
 * @param user_ctx  SetAlarm 时传入的用户上下文
 */
typedef void (*SoftTimerCb)(T_SoftTimerHandle *pt_Handle, void *user_ctx);

/**
 * @enum         E_SoftTimerPeriodMode
 * @brief        周期定时器漂移策略
 * @details      仅对 iPeriodMs > 0 的周期定时器生效。
 */
typedef enum
{
    /**
     * FROM_END：下次 deadline = 本次 cb 结束时刻 + iPeriodMs。
     * 特性：间隔严格 = iPeriodMs（不会打爆线程池），但触发时刻不对齐栅格。
     * 适用：cb 执行时间不定但用户要求"相邻两次之间至少 iPeriodMs"的场景。
     */
    SOFTTIMER_PERIOD_FROM_END       = 0,

    /**
     * FROM_SCHEDULED：下次 deadline = 本次预定时刻 + iPeriodMs。
     * 特性：触发时刻严格对齐 t0/t0+P/t0+2P 栅格（精确定时），
     *       但若 cb 耗时 > P，本轮完成时下次栅格已过期 → 自动 snap 到下一未来栅格
     *       （跳过 N-1 个 grid），累计 overrun += N-1；避免线程池堆积。
     * 适用：控制环、采样等需要严格对齐时间栅格的场景。
     *       前置条件：正常运行时 cb 执行时间 < 周期。
     * 注意：调度线程若遇线程池满触发 requeue 时，仅推迟本次 deadline（并 overrun+1），
     *       last_scheduled 栅格基点不动 —— 池满恢复后 wrapper 内 snap 循环会一次性
     *       跳过累计过期的 grid，栅格对齐性得以保持。用户表现：池压导致的抖动会被
     *       归并进 ulTotalOverrun，不会打爆线程池。
     */
    SOFTTIMER_PERIOD_FROM_SCHEDULED = 1,
} E_SoftTimerPeriodMode;

/**
 * @struct       T_SoftTimerAlarm
 * @brief        单个定时器的配置（SetAlarm 传入）
 * @details      支持 C99 designated initializer 一行到位构造。
 */
typedef struct T_SOFTTIMERALARM
{
    SoftTimerCb           cb;             /**< 用户回调，不能为 NULL */
    void                 *user_ctx;       /**< 传给 cb 的上下文，可为 NULL */
    int                   iFirstDelayMs;  /**< 首次触发延迟(ms)，>=0 */
    int                   iPeriodMs;      /**< 周期(ms)，0=单次；>0=周期 */
    E_SoftTimerPeriodMode ePeriodMode;    /**< 周期漂移策略（iPeriodMs=0 时忽略） */
    char                  sTimerName[32]; /**< 定时器名（内联定长，含末尾'\0'，超长截断） */
} T_SoftTimerAlarm;

/**
 * @struct       T_SoftTimerConfig
 * @brief        管理器创建配置（Init 传入）
 * @details      线程池由内部按此配置创建，用户不直接持有 pool handle。
 */
typedef struct T_SOFTTIMERCONFIG
{
    char sMgrName[32];       /**< 管理器名，内联定长，含末尾'\0'，超长截断 */
    int  iMinNum;            /**< 线程池最小线程数，>0 */
    int  iMaxNum;            /**< 线程池最大线程数，>=iMinNum */
    int  iQueueMaxSize;      /**< 线程池任务队列最大长度，>0 */
    int  iIdleTimeoutMs;     /**< 线程池空闲缩容超时(ms)，0=禁用 */
    int  iSchedPriority;     /**< 调度线程 SCHED_FIFO 优先级(1-99)，0=用默认调度 */
} T_SoftTimerConfig;

/**
 * @struct       T_SoftTimerStats
 * @brief        运行统计信息
 */
typedef struct T_SOFTTIMERSTATS
{
    unsigned long ulTotalSet;       /**< 累计 SetAlarm 成功次数 */
    unsigned long ulTotalFired;     /**< 累计成功投递到线程池次数（不代表 cb 完成） */
    unsigned long ulTotalCanceled;  /**< 累计 Delete 取消次数（含 D2 -2 分支） */
    unsigned long ulTotalOverrun;   /**< overrun 累计计数：包含
                                     *   (a) FROM_END：cb 完成时刻晚于本次预定 deadline + iPeriodMs 时 +1
                                     *   (b) FROM_SCHEDULED：snap 时跳过的 grid 数
                                     *   (c) 池满 requeue：+1
                                     *   三者共用一个字段，用于粗粒度背压监控。
                                     *   注：FROM_END 模式下一次池满事件可能贡献 2 计数（
                                     *   一次自 (c) requeue，一次自 (a) wrapper 收尾时 delta 判定
                                     *   —— 因 last_scheduled 未随 requeue 前推）；
                                     *   此为粗粒度指标的已知行为，趋势可信、绝对值偏高。
                                     *   FROM_SCHEDULED 模式下 requeue 不参与 snap 计数，无此叠加。 */
    int           iActiveCount;     /**< 当前活跃定时器数（含墓碑之外的所有已 Set 未 Delete） */
    int           iPeakActive;      /**< 历史峰值活跃数 */
} T_SoftTimerStats;


/* ================================================================== */
/*                                                                    */
/*     生命周期                                                       */
/*                                                                    */
/* ================================================================== */

/**
 * @func         SoftTimerAPI_Init
 * @brief        创建软件定时器管理器
 * @details      内部按 t_Config 创建：一把互斥锁 + 两个 cond 变量（绑 CLOCK_MONOTONIC）
 *               + 一个调度线程（ThreadAPI_ThreadCreate）+ 一个线程池（ThreadAPI_ThreadPoolCreate）。
 *               调用成功后 *ppt_Mgr 指向有效管理器。
 * @param[in]    ppt_Mgr:   管理器指针的指针；调用前 *ppt_Mgr 必须为 NULL
 * @param[in]    t_Config:  管理器配置（按值传入，内部保存副本）
 * @return       int ret
 * @retval       0:   初始化成功
 * @retval       -1:  参数无效
 * @retval       -2:  资源分配失败（malloc/线程池/线程创建失败）
 * @warning      *ppt_Mgr 必须为 NULL，否则拒绝（防止重复 Init 泄漏）
 * @author       zlzksrl
 * @date         2026-07-13
 * @Version      V1.0.0
 */
int SoftTimerAPI_Init(T_SoftTimerMgr **ppt_Mgr, T_SoftTimerConfig t_Config);



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

T_SoftTimerMgr *SoftTimerAPI_QuickInit(char sMgrName[32]);

/**
 * @func         SoftTimerAPI_Destroy
 * @brief        销毁管理器，释放所有资源
 * @details      流程：
 *               1) 置 stop_flag，broadcast 唤醒调度线程；
 *               2) join 调度线程；
 *               3) 销毁线程池（等待所有 in-flight 回调完成）；
 *               4) 扫尾所有残留节点与句柄（含墓碑）；
 *               5) 销毁互斥锁与 cond；
 *               6) *ppt_Mgr 置 NULL。
 * @param[in]    ppt_Mgr: 管理器指针的指针
 * @return       int ret
 * @retval       0:   销毁成功
 * @retval       -1:  参数无效
 * @warning      调用者应确保销毁前不再有新的 SetAlarm/Delete 调用（除自回调内退出）
 * @author       zlzksrl
 * @date         2026-07-13
 * @Version      V1.0.0
 */
int SoftTimerAPI_Destroy(T_SoftTimerMgr **ppt_Mgr);


/* ================================================================== */
/*                                                                    */
/*     定时器操作                                                     */
/*                                                                    */
/* ================================================================== */

/**
 * @func         SoftTimerAPI_SetAlarm
 * @brief        注册一个定时器
 * @details      内部分配 node + handle，插入 min-heap，若成为堆顶则唤醒调度线程。
 *               对单次定时器（iPeriodMs==0）：cb 执行一次后 node 被释放，handle 变成"墓碑"，
 *               直到用户 Delete 才释放 handle 内存。
 *               对周期定时器：cb 执行完后按 ePeriodMode 计算下次 deadline，重回堆。
 *               极端情况下（wrapper 重回堆时 realloc 失败），周期定时器可能提前终止（墓碑化），
 *               不再触发；详见文件级说明"极端资源不足行为"。
 * @param[in]    pt_Mgr:      管理器句柄
 * @param[in]    t_Alarm:     定时器配置（按值传入）
 * @param[out]   ppt_Handle:  返回句柄；调用前 *ppt_Handle 必须为 NULL
 * @return       int ret
 * @retval       0:   成功
 * @retval       -1:  参数无效（cb=NULL / iFirstDelayMs<0 / iPeriodMs<0 / *ppt_Handle!=NULL）
 * @retval       -2:  管理器正在销毁
 * @retval       -4:  资源不足（node/handle 内存分配失败或 heap 扩容失败）
 * @warning      *ppt_Handle 必须为 NULL，否则拒绝
 * @author       zlzksrl
 * @date         2026-07-13
 * @Version      V1.0.0
 */
int SoftTimerAPI_SetAlarm(T_SoftTimerMgr *pt_Mgr,
                          T_SoftTimerAlarm t_Alarm,
                          T_SoftTimerHandle **ppt_Handle);

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
                                                );

/**
 * @func         SoftTimerAPI_Delete
 * @brief        删除定时器
 * @details      按 node 当前状态走四条路径：
 *               A) 单次已 fire（墓碑）：仅摘除 handle 并释放；
 *               B) 在堆中未运行：从堆摘除，释放 node + handle；
 *               C) 已投递未运行（DISPATCHED）：标记 canceled，wrapper 早退释放 node；Delete 释放 handle；
 *               D) 运行中：
 *                  D1) 自删除（cb 内 Delete 自己）：立即返回，wrapper 收尾释放 node；
 *                  D2) 他线程：等 cv_delete 唤醒（wrapper 完成），wrapper 负责释放 node。
 *               成功后 *ppt_Handle 置 NULL。
 * @param[in]    pt_Mgr:      管理器句柄
 * @param[in,out] ppt_Handle: 定时器句柄的指针；成功后被置 NULL
 * @return       int ret
 * @retval       0:   删除成功；*ppt_Handle 已置 NULL
 * @retval       -1:  参数无效（*ppt_Handle == NULL）
 * @retval       -2:  管理器正在销毁。分两种子情况：
 *                    (a) 入口检查即被拒（尚未做归属判定）：*ppt_Handle 保持原值不变，
 *                        句柄由 Destroy 扫尾释放，用户不应再解引用它；
 *                    (b) D2 等待期间被 stop_flag 抢先：*ppt_Handle 已置 NULL；
 *                        handle 与 node 均由 wrapper 收尾释放（cb 仍持 handle 快照，本函数
 *                        延迟释放以避免 UAF）。用户视角同样"无需再处理该句柄"。
 * @retval       -3:  句柄非本管理器 / 已失效；*ppt_Handle 保持原值不变。
 * @author       zlzksrl
 * @date         2026-07-13
 * @Version      V1.0.0
 */
int SoftTimerAPI_Delete(T_SoftTimerMgr *pt_Mgr, T_SoftTimerHandle **ppt_Handle);


/* ================================================================== */
/*                                                                    */
/*     查询                                                           */
/*                                                                    */
/* ================================================================== */

/**
 * @func         SoftTimerAPI_StatsGet
 * @brief        获取运行统计
 * @param[in]    pt_Mgr:   管理器句柄
 * @param[out]   pt_Stats: 统计输出结构体指针
 * @return       int ret
 * @retval       0:  成功
 * @retval       -1: 参数无效
 * @author       zlzksrl
 * @date         2026-07-13
 * @Version      V1.0.0
 */
int SoftTimerAPI_StatsGet(T_SoftTimerMgr *pt_Mgr, T_SoftTimerStats *pt_Stats);


#ifdef __cplusplus
 }
#endif

#endif
