/**
 * @file        main.c
 * @brief       SoftTimer 软件定时器 - 功能演示与自检程序
 * @details     覆盖需求文档"典型应用"及 API 全表面，分 9 段用例：
 *
 *              Part 1:  单次定时器          —— iPeriodMs=0，验证 fire-once + 墓碑
 *              Part 2:  周期 FROM_END       —— 相邻触发间隔严格 >= P（不含 cb 时长）
 *              Part 3:  周期 FROM_SCHEDULED —— 栅格对齐 + cb 超时 snap（overrun 统计）
 *              Part 3b: 恰好耗时 P 边界     —— 验证 C6 修复：next==cb_end 不算 overrun
 *              Part 4:  Delete 五路径       —— A 墓碑 / B IN_HEAP / C DISPATCHED /
 *                                              D1 自删除 / D2 他线程删运行中
 *              Part 5:  并发压力            —— 多定时器并发触发 + 混合 Set/Delete
 *                                              + 统计一致性断言（L13）
 *              Part 6:  参数校验            —— 各种非法入参 & Destroy 幂等
 *              Part 7:  跨管理器 -3         —— A mgr 的 handle 传给 B mgr Delete
 *              Part 8:  Path C 独立复现     —— 依赖 -DST_DEBUG_DISPATCH_DELAY_MS>0 编译
 *
 * @author      zlzksrl

 * @Version     V1.0.0
 * @date        2026-07-13
 * @copyright   copyright (C) 2026
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "../include/SoftTimer.h"
#include "../src/SoftTimer_Main.h"    /* 仅测试用：读 ST_DEBUG_DISPATCH_DELAY_MS */
#include "../src/SoftTimer_Maketime.h"


/* ========================== 调试宏 ========================== */
#if 1
#define Debug_printx(format,...)\
                do\
                {\
                    printf("[Debug]-[#####]-["format" | line:[%d] func:[%s]]\n",##__VA_ARGS__,__LINE__,__FUNCTION__);\
                }while(0)
#else
#define Debug_printx(format,...)  do{}while(0)
#endif


/* ========================== 通用工具 ========================== */

/**
 * @brief 返回启动到现在的相对毫秒数（用于打印相对时间）
 */
static long long ms_since(struct timespec *base)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long ds = (long long)now.tv_sec  - (long long)base->tv_sec;
    long long dn = (long long)now.tv_nsec - (long long)base->tv_nsec;
    return ds * 1000LL + dn / 1000000LL;
}

static void print_stats(T_SoftTimerMgr *mgr, const char *tag)
{
    T_SoftTimerStats s;
    memset(&s, 0, sizeof(s));
    SoftTimerAPI_StatsGet(mgr, &s);
    printf("[Stats][%s] set=%lu fired=%lu canceled=%lu overrun=%lu active=%d peak=%d\n",
           tag, s.ulTotalSet, s.ulTotalFired, s.ulTotalCanceled,
           s.ulTotalOverrun, s.iActiveCount, s.iPeakActive);
}


/* ================================================================== */
/*                                                                    */
/*     Part 1: 单次定时器                                              */
/*                                                                    */
/* ================================================================== */

struct oneshot_ctx {
    struct timespec t0;
    long long       fired_at_ms;
    int             fired;
};

static void oneshot_cb(T_SoftTimerHandle *h, void *arg)
{
    (void)h;
    if(NULL == arg)
    {
        Debug_printx("oneshot_cb: NULL ctx, skip");
        return;
    }
    struct oneshot_ctx *c = (struct oneshot_ctx *)arg;
    c->fired_at_ms = ms_since(&c->t0);
    c->fired       = 1;
    Debug_printx("oneshot fired at %lldms", c->fired_at_ms);
}

static void demo_oneshot(T_SoftTimerMgr *mgr)
{
    Debug_printx("======== Part 1: ONESHOT ========");

    struct oneshot_ctx c;
    memset(&c, 0, sizeof(c));
    clock_gettime(CLOCK_MONOTONIC, &c.t0);

    T_SoftTimerAlarm a;
    memset(&a, 0, sizeof(a));
    a.cb              = oneshot_cb;
    a.user_ctx        = &c;
    a.iFirstDelayMs   = 200;
    a.iPeriodMs       = 0;
    strncpy(a.sTimerName, "oneshot_200ms", sizeof(a.sTimerName) - 1);

    T_SoftTimerHandle *h = NULL;
    int r = SoftTimerAPI_SetAlarm(mgr, a, &h);
    Debug_printx("SetAlarm ret=%d h=%p", r, (void *)h);

    usleep(400 * 1000);
    Debug_printx("after 400ms: fired=%d at=%lldms (expect ~200)",
                 c.fired, c.fired_at_ms);

    /* 单次已 fire, handle 变墓碑, Delete 走 Path A */
    r = SoftTimerAPI_Delete(mgr, &h);
    Debug_printx("Delete tombstone ret=%d h=%p (expect 0, NULL)", r, (void *)h);

    /* 二次 Delete 应失败（NULL） */
    r = SoftTimerAPI_Delete(mgr, &h);
    Debug_printx("Delete twice ret=%d (expect -1)", r);

    print_stats(mgr, "after Part 1");
}


/* ================================================================== */
/*                                                                    */
/*     Part 2: 周期 FROM_END                                           */
/*                                                                    */
/* ================================================================== */

struct period_ctx {
    struct timespec t0;
    long long       last_fired_ms;
    long long       intervals[16];
    int             fired_count;
};

static void period_from_end_cb(T_SoftTimerHandle *h, void *arg)
{
    (void)h;
    struct period_ctx *c = (struct period_ctx *)arg;
    long long now_ms = ms_since(&c->t0);
    if(c->fired_count > 0
       && c->fired_count < (int)(sizeof(c->intervals) / sizeof(c->intervals[0])))
    {
        c->intervals[c->fired_count] = now_ms - c->last_fired_ms;
    }
    c->last_fired_ms = now_ms;
    c->fired_count  += 1;
    /* 模拟 cb 中等耗时（30ms） */
    usleep(30 * 1000);
}

static void demo_period_from_end(T_SoftTimerMgr *mgr)
{
    Debug_printx("======== Part 2: PERIOD FROM_END ========");

    struct period_ctx c;
    memset(&c, 0, sizeof(c));
    clock_gettime(CLOCK_MONOTONIC, &c.t0);

    T_SoftTimerAlarm a;
    memset(&a, 0, sizeof(a));
    a.cb            = period_from_end_cb;
    a.user_ctx      = &c;
    a.iFirstDelayMs = 100;
    a.iPeriodMs     = 100;
    a.ePeriodMode   = SOFTTIMER_PERIOD_FROM_END;
    strncpy(a.sTimerName, "period_end_100", sizeof(a.sTimerName) - 1);

    T_SoftTimerHandle *h = NULL;
    SoftTimerAPI_SetAlarm(mgr, a, &h);

    usleep(700 * 1000);   /* 约 5-6 次触发 */
    SoftTimerAPI_Delete(mgr, &h);

    Debug_printx("fired=%d (expect ~5-6, gap ~130ms=P+cb)", c.fired_count);
    for(int i = 1; i < c.fired_count && i < 6; i++)
    {
        Debug_printx("  gap[%d] = %lldms", i, c.intervals[i]);
    }
    print_stats(mgr, "after Part 2");
}


/* ================================================================== */
/*                                                                    */
/*     Part 3: 周期 FROM_SCHEDULED + snap                              */
/*                                                                    */
/* ================================================================== */

static void period_from_scheduled_cb(T_SoftTimerHandle *h, void *arg)
{
    (void)h;
    struct period_ctx *c = (struct period_ctx *)arg;
    long long now_ms = ms_since(&c->t0);
    if(c->fired_count > 0
       && c->fired_count < (int)(sizeof(c->intervals) / sizeof(c->intervals[0])))
    {
        c->intervals[c->fired_count] = now_ms - c->last_fired_ms;
    }
    c->last_fired_ms = now_ms;
    c->fired_count  += 1;
    /* 第 2 次触发时故意超时（150ms 长于 100ms 周期）验证 snap */
    if(2 == c->fired_count)
    {
        usleep(150 * 1000);
    }
}

static void demo_period_from_scheduled(T_SoftTimerMgr *mgr)
{
    Debug_printx("======== Part 3: PERIOD FROM_SCHEDULED ========");

    struct period_ctx c;
    memset(&c, 0, sizeof(c));
    clock_gettime(CLOCK_MONOTONIC, &c.t0);

    T_SoftTimerAlarm a;
    memset(&a, 0, sizeof(a));
    a.cb            = period_from_scheduled_cb;
    a.user_ctx      = &c;
    a.iFirstDelayMs = 100;
    a.iPeriodMs     = 100;
    a.ePeriodMode   = SOFTTIMER_PERIOD_FROM_SCHEDULED;
    strncpy(a.sTimerName, "period_sched_100", sizeof(a.sTimerName) - 1);

    T_SoftTimerHandle *h = NULL;
    SoftTimerAPI_SetAlarm(mgr, a, &h);

    usleep(700 * 1000);   /* 约 5-6 次栅格 */
    SoftTimerAPI_Delete(mgr, &h);

    Debug_printx("fired=%d (2nd cb sleeps 150 -> 1 snap overrun expected)",
                 c.fired_count);
    for(int i = 1; i < c.fired_count && i < 7; i++)
    {
        Debug_printx("  gap[%d] = %lldms", i, c.intervals[i]);
    }
    print_stats(mgr, "after Part 3");
}


/* ================================================================== */
/*                                                                    */
/*     Part 3b: FROM_SCHEDULED 恰好耗时 P（栅格边界回归）              */
/*                                                                    */
/*  背景：C6 修复将 snap 循环判定改为严格 '<'。当 cb 耗时正好等于       */
/*  一个周期时，下一栅格 next 恰好等于 cb_end，语义上未跳过 grid，      */
/*  不应触发 overrun 计数。本用例验证该边界。                          */
/*                                                                    */
/* ================================================================== */

static void period_exact_grid_cb(T_SoftTimerHandle *h, void *arg)
{
    (void)h;
    struct period_ctx *c = (struct period_ctx *)arg;
    long long now_ms = ms_since(&c->t0);
    if(c->fired_count > 0
       && c->fired_count < (int)(sizeof(c->intervals) / sizeof(c->intervals[0])))
    {
        c->intervals[c->fired_count] = now_ms - c->last_fired_ms;
    }
    c->last_fired_ms = now_ms;
    c->fired_count  += 1;
    /* cb 耗时 = 周期，理想情况下 next == cb_end，严格 '<' 不 snap */
    usleep(100 * 1000);
}

static void demo_period_exact_grid(T_SoftTimerMgr *mgr)
{
    Debug_printx("======== Part 3b: FROM_SCHEDULED EXACT GRID ========");

    struct period_ctx c;
    memset(&c, 0, sizeof(c));
    clock_gettime(CLOCK_MONOTONIC, &c.t0);

    T_SoftTimerStats s0;
    memset(&s0, 0, sizeof(s0));
    SoftTimerAPI_StatsGet(mgr, &s0);

    T_SoftTimerAlarm a;
    memset(&a, 0, sizeof(a));
    a.cb            = period_exact_grid_cb;
    a.user_ctx      = &c;
    a.iFirstDelayMs = 50;
    a.iPeriodMs     = 100;
    a.ePeriodMode   = SOFTTIMER_PERIOD_FROM_SCHEDULED;
    strncpy(a.sTimerName, "sched_exact", sizeof(a.sTimerName) - 1);

    T_SoftTimerHandle *h = NULL;
    SoftTimerAPI_SetAlarm(mgr, a, &h);

    usleep(700 * 1000);
    SoftTimerAPI_Delete(mgr, &h);

    T_SoftTimerStats s1;
    memset(&s1, 0, sizeof(s1));
    SoftTimerAPI_StatsGet(mgr, &s1);
    unsigned long overrun_delta = s1.ulTotalOverrun - s0.ulTotalOverrun;

    Debug_printx("fired=%d overrun_delta=%lu (expect fired ~5-6, overrun_delta small: <=1 抖动)",
                 c.fired_count, overrun_delta);
    for(int i = 1; i < c.fired_count && i < 6; i++)
    {
        Debug_printx("  gap[%d] = %lldms", i, c.intervals[i]);
    }
    print_stats(mgr, "after Part 3b");
}


/* ================================================================== */
/*                                                                    */
/*     Part 4: Delete 五路径                                           */
/*                                                                    */
/* ================================================================== */

/* --- Path B: 在堆中未触发即删除 --- */
static void never_fire_cb(T_SoftTimerHandle *h, void *arg)
{
    (void)h; (void)arg;
    Debug_printx("never_fire_cb: SHOULD NOT be called");
}

/* --- Path D1: 自删除 --- */
struct self_del_ctx {
    T_SoftTimerMgr    *mgr;
    T_SoftTimerHandle *self_h;   /* cb 用这个 handle 自删 */
    int                self_del_ret;
    int                fired_count;
};

static void self_delete_cb(T_SoftTimerHandle *h, void *arg)
{
    struct self_del_ctx *c = (struct self_del_ctx *)arg;
    c->fired_count += 1;
    if(1 == c->fired_count)
    {
        Debug_printx("self_delete_cb: calling Delete on self (h=%p)", (void *)h);
        c->self_del_ret = SoftTimerAPI_Delete(c->mgr, &c->self_h);
        Debug_printx("self_delete_cb: Delete ret=%d h=%p (expect 0, NULL)",
                     c->self_del_ret, (void *)c->self_h);
    }
}

/* --- Path D2: 他线程删运行中 --- */
struct running_ctx {
    int                busy_ms;
    int                fired_count;
    struct timespec    started_at;
    struct timespec    finished_at;
};

static void long_running_cb(T_SoftTimerHandle *h, void *arg)
{
    (void)h;
    struct running_ctx *c = (struct running_ctx *)arg;
    c->fired_count += 1;
    clock_gettime(CLOCK_MONOTONIC, &c->started_at);
    Debug_printx("long_running_cb: busy %dms start", c->busy_ms);
    usleep(c->busy_ms * 1000);
    clock_gettime(CLOCK_MONOTONIC, &c->finished_at);
    Debug_printx("long_running_cb: busy end");
}

static void demo_delete_paths(T_SoftTimerMgr *mgr)
{
    Debug_printx("======== Part 4: DELETE PATHS ========");

    /* ---------- Path B: 在堆中未触发即删除 ---------- */
    {
        T_SoftTimerAlarm a;
        memset(&a, 0, sizeof(a));
        a.cb            = never_fire_cb;
        a.iFirstDelayMs = 5000;   /* 5s 后才触发 */
        strncpy(a.sTimerName, "PathB_in_heap", sizeof(a.sTimerName) - 1);

        T_SoftTimerHandle *h = NULL;
        SoftTimerAPI_SetAlarm(mgr, a, &h);
        usleep(50 * 1000);
        int r = SoftTimerAPI_Delete(mgr, &h);
        Debug_printx("PathB Delete ret=%d h=%p (expect 0, NULL)",
                     r, (void *)h);
    }

    /* ---------- Path D1: 自删除 ---------- */
    {
        struct self_del_ctx c;
        memset(&c, 0, sizeof(c));
        c.mgr = mgr;

        T_SoftTimerAlarm a;
        memset(&a, 0, sizeof(a));
        a.cb            = self_delete_cb;
        a.user_ctx      = &c;
        a.iFirstDelayMs = 100;
        a.iPeriodMs     = 100;
        strncpy(a.sTimerName, "PathD1_self_del", sizeof(a.sTimerName) - 1);

        SoftTimerAPI_SetAlarm(mgr, a, &c.self_h);
        usleep(400 * 1000);
        Debug_printx("PathD1 fired=%d (expect 1) self_del_ret=%d h_after=%p (expect NULL)",
                     c.fired_count, c.self_del_ret, (void *)c.self_h);
    }

    /* ---------- Path D2: 他线程 Delete 运行中 ---------- */
    {
        struct running_ctx c;
        memset(&c, 0, sizeof(c));
        c.busy_ms = 300;

        T_SoftTimerAlarm a;
        memset(&a, 0, sizeof(a));
        a.cb            = long_running_cb;
        a.user_ctx      = &c;
        a.iFirstDelayMs = 50;
        a.iPeriodMs     = 500;
        strncpy(a.sTimerName, "PathD2_running", sizeof(a.sTimerName) - 1);

        T_SoftTimerHandle *h = NULL;
        SoftTimerAPI_SetAlarm(mgr, a, &h);
        usleep(100 * 1000);   /* 100ms 后：cb 正在忙 */
        Debug_printx("PathD2: calling Delete while cb busy");
        struct timespec t_del0, t_del1;
        clock_gettime(CLOCK_MONOTONIC, &t_del0);
        int r = SoftTimerAPI_Delete(mgr, &h);
        clock_gettime(CLOCK_MONOTONIC, &t_del1);
        long long block_ms = ((long long)t_del1.tv_sec - t_del0.tv_sec) * 1000LL
                             + (t_del1.tv_nsec - t_del0.tv_nsec) / 1000000LL;
        Debug_printx("PathD2 Delete ret=%d blocked ~%lldms h=%p (expect 0, ~250ms, NULL)",
                     r, block_ms, (void *)h);
    }
    print_stats(mgr, "after Part 4");
}


/* ================================================================== */
/*                                                                    */
/*     Part 5: 并发压力                                                */
/*                                                                    */
/* ================================================================== */

static void tiny_cb(T_SoftTimerHandle *h, void *arg)
{
    (void)h;
    int *cnt = (int *)arg;
    (*cnt)++;
    usleep(1 * 1000);
}

static void demo_stress(T_SoftTimerMgr *mgr)
{
    Debug_printx("======== Part 5: STRESS ========");

    enum { N = 40 };
    T_SoftTimerHandle *handles[N] = {NULL};
    int                counters[N] = {0};

    /* 快照：本段开头统计基线，用于末尾的一致性断言 */
    T_SoftTimerStats s_begin;
    memset(&s_begin, 0, sizeof(s_begin));
    SoftTimerAPI_StatsGet(mgr, &s_begin);

    for(int i = 0; i < N; i++)
    {
        T_SoftTimerAlarm a;
        memset(&a, 0, sizeof(a));
        a.cb            = tiny_cb;
        a.user_ctx      = &counters[i];
        a.iFirstDelayMs = 50 + (i % 10) * 5;
        a.iPeriodMs     = 20 + (i % 8);
        a.ePeriodMode   = (i & 1) ? SOFTTIMER_PERIOD_FROM_SCHEDULED
                                  : SOFTTIMER_PERIOD_FROM_END;
        snprintf(a.sTimerName, sizeof(a.sTimerName), "stress_%d", i);
        SoftTimerAPI_SetAlarm(mgr, a, &handles[i]);
    }

    usleep(500 * 1000);
    print_stats(mgr, "stress mid");

    /* 中途删一半（各种状态混合命中：IN_HEAP/DISPATCHED/RUNNING/墓碑均有可能） */
    for(int i = 0; i < N; i += 2)
    {
        SoftTimerAPI_Delete(mgr, &handles[i]);
    }
    usleep(300 * 1000);
    print_stats(mgr, "stress after half-delete");

    /* 收尾清理剩余 */
    for(int i = 1; i < N; i += 2)
    {
        if(NULL != handles[i])
        {
            SoftTimerAPI_Delete(mgr, &handles[i]);
        }
    }
    print_stats(mgr, "stress end");

    int total = 0;
    for(int i = 0; i < N; i++)
    {
        total += counters[i];
    }
    Debug_printx("total fires observed by user cb = %d", total);

    /* ---- L13：统计一致性断言 ----
     * 本段全部注册的是周期定时器（不会自然消亡），且末尾对每个 handle 均调过 Delete。
     * 期望不变式（相对基线）：
     *   delta(ulTotalSet)      == N
     *   delta(ulTotalCanceled) == N   （每个 handle 都被成功 Delete 一次）
     *   delta(iActiveCount)    == 0   （全部退出活跃集合）
     * 注：不含"墓碑滞留"影响，因为周期定时器不会成为墓碑。
     */
    T_SoftTimerStats s_end;
    memset(&s_end, 0, sizeof(s_end));
    SoftTimerAPI_StatsGet(mgr, &s_end);
    unsigned long d_set    = s_end.ulTotalSet      - s_begin.ulTotalSet;
    unsigned long d_cancel = s_end.ulTotalCanceled - s_begin.ulTotalCanceled;
    int           d_active = s_end.iActiveCount    - s_begin.iActiveCount;
    Debug_printx("[L13] invariants: dSet=%lu dCanceled=%lu dActive=%d (expect %d/%d/0)",
                 d_set, d_cancel, d_active, N, N);
    if((int)d_set != N || (int)d_cancel != N || 0 != d_active)
    {
        Debug_printx("[L13] INVARIANT FAIL");
    }
    else
    {
        Debug_printx("[L13] invariants OK");
    }
}

/* ================================================================== */
/*                                                                    */
/*     Part 6: 参数校验                                                */
/*                                                                    */
/* ================================================================== */

static void demo_param_check(T_SoftTimerMgr *mgr)
{
    Debug_printx("======== Part 6: PARAM CHECK ========");

    int r;

    /* SetAlarm 非法入参：用 never_fire_cb + 长延迟，避免 100ms 内被误触发导致 NULL ctx 段错误 */
    T_SoftTimerAlarm a;
    memset(&a, 0, sizeof(a));
    a.cb            = never_fire_cb;
    a.iFirstDelayMs = 10 * 1000;   /* 10s，本 Part 内绝不会 fire */
    strncpy(a.sTimerName, "bad_param", sizeof(a.sTimerName) - 1);

    T_SoftTimerHandle *h = NULL;
    r = SoftTimerAPI_SetAlarm(NULL, a, &h);
    Debug_printx("SetAlarm(NULL,..)          ret=%d (expect -1)", r);

    r = SoftTimerAPI_SetAlarm(mgr, a, NULL);
    Debug_printx("SetAlarm(..,NULL)          ret=%d (expect -1)", r);

    T_SoftTimerAlarm bad = a;
    bad.cb = NULL;
    r = SoftTimerAPI_SetAlarm(mgr, bad, &h);
    Debug_printx("SetAlarm cb=NULL           ret=%d (expect -1)", r);

    bad = a;
    bad.iFirstDelayMs = -1;
    r = SoftTimerAPI_SetAlarm(mgr, bad, &h);
    Debug_printx("SetAlarm firstDelay<0      ret=%d (expect -1)", r);

    /* SetAlarm 成功再传入非 NULL 句柄应被拒 */
    r = SoftTimerAPI_SetAlarm(mgr, a, &h);
    Debug_printx("SetAlarm ok                ret=%d h=%p", r, (void *)h);
    T_SoftTimerHandle *h_alias = h;
    r = SoftTimerAPI_SetAlarm(mgr, a, &h_alias);
    Debug_printx("SetAlarm reuse handle      ret=%d (expect -1)", r);
    SoftTimerAPI_Delete(mgr, &h);

    /* Delete 非法入参 */
    r = SoftTimerAPI_Delete(NULL, &h);
    Debug_printx("Delete(NULL,..)            ret=%d (expect -1)", r);
    r = SoftTimerAPI_Delete(mgr, NULL);
    Debug_printx("Delete(..,NULL)            ret=%d (expect -1)", r);

    /* StatsGet 非法入参 */
    r = SoftTimerAPI_StatsGet(NULL, NULL);
    Debug_printx("StatsGet(NULL,NULL)        ret=%d (expect -1)", r);
}


/* ================================================================== */
/*                                                                    */
/*     Part 7: 跨管理器 -3 校验                                        */
/*                                                                    */
/* ================================================================== */

static void demo_cross_mgr(void)
{
    Debug_printx("======== Part 7: CROSS-MGR -3 ========");

    T_SoftTimerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.iMinNum        = 1;
    cfg.iMaxNum        = 2;
    cfg.iQueueMaxSize  = 8;
    cfg.iIdleTimeoutMs = 1000;

    T_SoftTimerMgr *mgr_A = NULL;
    T_SoftTimerMgr *mgr_B = NULL;
    strncpy(cfg.sMgrName, "mgr_A", sizeof(cfg.sMgrName) - 1);
    if(0 != SoftTimerAPI_Init(&mgr_A, cfg))
    {
        Debug_printx("mgr_A init fail");
        return;
    }
    strncpy(cfg.sMgrName, "mgr_B", sizeof(cfg.sMgrName) - 1);
    if(0 != SoftTimerAPI_Init(&mgr_B, cfg))
    {
        Debug_printx("mgr_B init fail");
        SoftTimerAPI_Destroy(&mgr_A);
        return;
    }

    T_SoftTimerAlarm a;
    memset(&a, 0, sizeof(a));
    a.cb            = never_fire_cb;
    a.iFirstDelayMs = 10 * 1000;
    strncpy(a.sTimerName, "cross_test", sizeof(a.sTimerName) - 1);

    T_SoftTimerHandle *h_A = NULL;
    if(0 != SoftTimerAPI_SetAlarm(mgr_A, a, &h_A))
    {
        Debug_printx("SetAlarm on mgr_A fail");
        SoftTimerAPI_Destroy(&mgr_A);
        SoftTimerAPI_Destroy(&mgr_B);
        return;
    }

    /* 用 mgr_B 去 Delete mgr_A 的 handle：应返回 -3 且不改动 h_A */
    T_SoftTimerHandle *h_alias = h_A;
    int r = SoftTimerAPI_Delete(mgr_B, &h_alias);
    Debug_printx("Delete(mgr_B, handle_of_A) ret=%d h_alias=%p (expect -3, unchanged=%p)",
                 r, (void *)h_alias, (void *)h_A);

    /* 用陌生指针（栈局部变量的地址）去 Delete：也应 -3 */
    T_SoftTimerHandle dummy_stack;
    T_SoftTimerHandle *h_fake = &dummy_stack;
    r = SoftTimerAPI_Delete(mgr_A, &h_fake);
    Debug_printx("Delete(mgr_A, &stack_var)  ret=%d (expect -3)", r);

    /* 正常清理 mgr_A 上真正的 handle */
    r = SoftTimerAPI_Delete(mgr_A, &h_A);
    Debug_printx("Delete(mgr_A, real_h)      ret=%d (expect 0)", r);

    SoftTimerAPI_Destroy(&mgr_A);
    SoftTimerAPI_Destroy(&mgr_B);
}


/* ================================================================== */
/*                                                                    */
/*     Part 8: Path C 独立复现（需编译期 hook）                        */
/*                                                                    */
/* ================================================================== */

/**
 * @brief Path C = DISPATCHED 期间被 Delete 取消。生产代码窗口极窄（微秒级），
 *        必须编译时打开 -DST_DEBUG_DISPATCH_DELAY_MS=200 让 wrapper 入口睡 200ms，
 *        Delete 才有机会在 DISPATCHED 阶段命中。
 */
static void demo_path_c(T_SoftTimerMgr *mgr)
{
    Debug_printx("======== Part 8: PATH C (DISPATCHED cancel) ========");
    Debug_printx("ST_DEBUG_DISPATCH_DELAY_MS = %d", ST_DEBUG_DISPATCH_DELAY_MS);

    if(ST_DEBUG_DISPATCH_DELAY_MS <= 0)
    {
        Debug_printx("hook disabled -> skip (rebuild with -DST_DEBUG_DISPATCH_DELAY_MS=200)");
        return;
    }

    T_SoftTimerAlarm a;
    memset(&a, 0, sizeof(a));
    a.cb            = never_fire_cb;
    a.iFirstDelayMs = 30;             /* 30ms 后到点，进 DISPATCHED */
    strncpy(a.sTimerName, "PathC_cancel", sizeof(a.sTimerName) - 1);

    T_SoftTimerHandle *h = NULL;
    SoftTimerAPI_SetAlarm(mgr, a, &h);

    /* 等 60ms：调度线程已 pop 并 AddTaskTry；wrapper 正在 dispatch delay 里睡 */
    usleep(60 * 1000);

    /* 此时 Delete 应命中 DISPATCHED 分支，never_fire_cb 保证之后也不会被调用 */
    int r = SoftTimerAPI_Delete(mgr, &h);
    Debug_printx("PathC Delete ret=%d h=%p (expect 0, NULL)", r, (void *)h);

    /* 再睡够 DISPATCH_DELAY 保证 wrapper 早退 */
    usleep((ST_DEBUG_DISPATCH_DELAY_MS + 50) * 1000);
    print_stats(mgr, "after Part 8");
}


/* ================================================================== */
/*                                                                    */
/*     main                                                           */
/*                                                                    */
/* ================================================================== */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("=========================================================\n");
    printf(" SoftTimer demo - build %s\n", SoftTimer_PROJECT_MAKETIME);
    printf("=========================================================\n");

    /* Init 前非法入参 */
    T_SoftTimerMgr *mgr = NULL;
    T_SoftTimerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    int r = SoftTimerAPI_Init(NULL, cfg);
    Debug_printx("Init(NULL,..) ret=%d (expect -1)", r);
    r = SoftTimerAPI_Init(&mgr, cfg);
    Debug_printx("Init default(0/0/0) ret=%d (expect -1)", r);

    /* 越界 SchedPriority */
    cfg.iMinNum        = 1;
    cfg.iMaxNum        = 2;
    cfg.iQueueMaxSize  = 8;
    cfg.iIdleTimeoutMs = 1000;
    cfg.iSchedPriority = 200;
    r = SoftTimerAPI_Init(&mgr, cfg);
    Debug_printx("Init prio=200 ret=%d (expect -1)", r);

    /* 正常 Init */
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.sMgrName, "st_demo", sizeof(cfg.sMgrName) - 1);
    cfg.iMinNum        = 2;
    cfg.iMaxNum        = 8;
    cfg.iQueueMaxSize  = 64;
    cfg.iIdleTimeoutMs = 2000;
    cfg.iSchedPriority = 0;   /* 0 = 默认调度，非 root 也能跑 */

    r = SoftTimerAPI_Init(&mgr, cfg);
    Debug_printx("Init OK ret=%d mgr=%p", r, (void *)mgr);
    if(0 != r || NULL == mgr)
    {
        printf("Init failed, abort\n");
        return -1;
    }

    demo_oneshot(mgr);
    demo_period_from_end(mgr);
    demo_period_from_scheduled(mgr);
    demo_period_exact_grid(mgr);
    demo_delete_paths(mgr);
    demo_stress(mgr);
    demo_param_check(mgr);
    demo_path_c(mgr);

    print_stats(mgr, "final");

    r = SoftTimerAPI_Destroy(&mgr);
    Debug_printx("Destroy ret=%d mgr=%p (expect 0, NULL)", r, (void *)mgr);

    /* --- Destroy 幂等 & 参数校验加严（L8） --- */

    /* (1) 同一变量二次 Destroy：已置 NULL，应稳定 -1 */
    r = SoftTimerAPI_Destroy(&mgr);
    Debug_printx("Destroy twice(same var)    ret=%d (expect -1)", r);

    /* (2) ppt_Mgr == NULL：应返回 -1，不崩溃 */
    r = SoftTimerAPI_Destroy(NULL);
    Debug_printx("Destroy(NULL ptr)          ret=%d (expect -1)", r);

    /* (3) 另一个未 Init 的栈变量（*ppt_Mgr==NULL）：应返回 -1 */
    T_SoftTimerMgr *mgr_uninit = NULL;
    r = SoftTimerAPI_Destroy(&mgr_uninit);
    Debug_printx("Destroy(uninit ptr)        ret=%d (expect -1)", r);

    /* (4) 三次 Destroy 同一变量：语义仍是 -1（无副作用） */
    r = SoftTimerAPI_Destroy(&mgr);
    Debug_printx("Destroy 3rd(same var)      ret=%d (expect -1)", r);

    /* 独立的跨 mgr 用例（自己 Init/Destroy 一对 mgr） */
    demo_cross_mgr();

    printf("=========================================================\n");
    printf(" SoftTimer demo done\n");
    printf("=========================================================\n");
    return 0;
}
