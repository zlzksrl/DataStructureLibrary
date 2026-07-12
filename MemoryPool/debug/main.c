/**
 * @file        main.c
 * @brief       MemoryPool 内存池 - 测试/演示程序
 * @details     Part 1-3:  单线程三模式基础（DROP/GROW/BLOCK 基本功能）
 *              Part 4-6:  多线程 + ThreadQueue 三模式压测 10×1000
 *                         （BLOCK 阻塞 / GROW 扩容 / DROP 丢弃，都走 alloc→PutMsg→GetMsg→Free）
 *              Part 7-8:  init_count 扫描
 *              Part 9:    max_count 上限：到限后 GROW 自动退化 DROP
 *              Part 10:   max_count < init_count：无上限扩容
 *              Part 11:   max_count 非整倍数：尾轮按剩余额度扩容
 *              Part 12:   Init 参数边界（mode/block_timeo/超大 init_count）
 *              Part 13:   BLOCK 超时与 Free 并发（stolen wakeup 回归保护）
 *              Part 14:   Destroy 唤醒等待者（BLOCK 无限等 → shutting_down 立即 NULL）
 *
 * @author      zlzksrl
 * @Version     V1.1.0
 * @date        2026-07-12
 * @copyright   copyright (C) 2026
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <inttypes.h>    /* PRIu64 */
#include <limits.h>      /* INT_MAX */

#include "../include/MemoryPool.h"
#include <ThreadQueue.h>


/* ========================== 调试宏 ========================== */

#if 1
#define Debug_printx(format,...)\
                do\
                {\
                    printf("[Debug]-[#####]-["format"##@line:[%d]@func:[%s]]\n",##__VA_ARGS__,__LINE__,__FUNCTION__);\
                }while(0)
#else
#define Debug_printx(format,...)\
                do\
                {\
                }while(0)
#endif

/* 简易断言：不中止程序，只打印 PASS/FAIL */
static int g_test_pass = 0;
static int g_test_fail = 0;
#define TEST_ASSERT(cond, fmt, ...) do { \
    if(cond) { g_test_pass++; Debug_printx("[PASS] "fmt, ##__VA_ARGS__); } \
    else     { g_test_fail++; Debug_printx("[FAIL] "fmt, ##__VA_ARGS__); } \
} while(0)


/* ================================================================== */
/*                                                                    */
/*     Part 1-3: 单线程三模式基础                                      */
/*                                                                    */
/* ================================================================== */
/* ---- Part 1: DROP（池满返回 NULL） ---- */
static void test_drop(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 8, MEMPOOL_MODE_DROP, 0, 0, 0 };
    T_MemPool *p = NULL;
    void *slots[12];
    int i;
    T_MemPoolStats st;

    MemPoolAPI_Init(&p, &cfg, "drop");
    for(i = 0; i < 12; i++)
    {
        slots[i] = MemPoolAPI_Alloc(p);
        if(slots[i]) *(int *)slots[i] = i;
    }
    MemPoolAPI_StatsGet(p, &st);
    Debug_printx("DROP: alloc=%"PRIu64" drop=%"PRIu64" capacity=%d (expect alloc=8 drop=4)",
                 st.ulTotalAlloc, st.ulTotalDrop, st.iCapacity);
    for(i = 0; i < 12; i++) if(slots[i]) MemPoolAPI_Free(p, slots[i]);
    MemPoolAPI_Destroy(&p);
}

/* ---- Part 2: GROW（池满动态扩容） ---- */
static void test_grow(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 4, MEMPOOL_MODE_GROW, 4, 0, 0 };
    T_MemPool *p = NULL;
    void *slots[12];
    int i;
    T_MemPoolStats st;

    MemPoolAPI_Init(&p, &cfg, "grow");
    for(i = 0; i < 12; i++)
    {
        slots[i] = MemPoolAPI_Alloc(p);
        if(slots[i]) *(int *)slots[i] = i;
    }
    MemPoolAPI_StatsGet(p, &st);
    Debug_printx("GROW: alloc=%"PRIu64" capacity=%d grow=%"PRIu64" (expect alloc=12 capacity=12)",
                 st.ulTotalAlloc, st.iCapacity, st.ulTotalGrow);
    for(i = 0; i < 12; i++) if(slots[i]) MemPoolAPI_Free(p, slots[i]);
    MemPoolAPI_Destroy(&p);
}

/* ---- Part 3: BLOCK（池满阻塞 + Free 唤醒） ---- */
static T_MemPool *g_pool = NULL;
static void *blocker_thread(void *arg)
{
    void *s;
    (void)arg;
    s = MemPoolAPI_AllocBlock(g_pool, 0);   /* 无限阻塞等 */
    Debug_printx("blocker: got slot=%p after woken", s);
    if(s) MemPoolAPI_Free(g_pool, s);
    return NULL;
}
static void test_block(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 4, MEMPOOL_MODE_BLOCK, 0, 0, 0 };
    void *slots[4];
    pthread_t tid;
    int i;

    MemPoolAPI_Init(&g_pool, &cfg, "block");
    for(i = 0; i < 4; i++) slots[i] = MemPoolAPI_Alloc(g_pool);
    Debug_printx("BLOCK: pool full (4/4), start blocker thread");
    pthread_create(&tid, NULL, blocker_thread, NULL);
    usleep(100 * 1000);
    Debug_printx("BLOCK: free one slot to wake blocker");
    MemPoolAPI_Free(g_pool, slots[0]);
    pthread_join(tid, NULL);
    for(i = 1; i < 4; i++) MemPoolAPI_Free(g_pool, slots[i]);
    MemPoolAPI_Destroy(&g_pool);
}


/* ================================================================== */
/*                                                                    */
/*     Part 4-6: 多线程 + ThreadQueue 三模式压测 10×1000              */
/*                                                                    */
/* ================================================================== */

typedef struct { int id; int val; } T_QMsg;

static T_MemPool        *g_qpool;
static T_ThreadQueueMsg *g_q;

static void q_release(void *data)
{
    MemPoolAPI_Free(g_qpool, data);
}

static volatile int g_prod_done = 0;

static void *q_producer(void *arg)
{
    int N = *(int *)arg;
    int i;
    for(i = 0; i < N; i++)
    {
        T_QMsg *m = (T_QMsg *)MemPoolAPI_Alloc(g_qpool);
        if(m != NULL)
        {
            m->id  = i;
            m->val = i * 10;
            ThreadQueueAPI_PutMsg(g_q, m);
        }
    }
    g_prod_done = 1;
    return NULL;
}

static void *q_consumer(void *arg)
{
    (void)arg;
    while(1)
    {
        T_QMsg *m = (T_QMsg *)ThreadQueueAPI_GetMsg(g_q, 100);
        if(m != NULL)
        {
            MemPoolAPI_Free(g_qpool, m);
        }
        else if(g_prod_done)
        {
            break;
        }
    }
    return NULL;
}

static void test_pool_queue_stress(MemPoolMode mode, const char *name)
{
    T_MemPoolConfig pcfg = { (int)sizeof(T_QMsg), 8, mode, 4, 0, 0 };
    int N = 1000;
    int ROUNDS = 10;
    int r;
    pthread_t tp, tc;
    T_MemPoolStats st;

    MemPoolAPI_Init(&g_qpool, &pcfg, name);
    ThreadQueueAPI_InitMsg(&g_q, 100, name, q_release);

    for(r = 0; r < ROUNDS; r++)
    {
        g_prod_done = 0;
        pthread_create(&tp, NULL, q_producer, &N);
        pthread_create(&tc, NULL, q_consumer, NULL);
        pthread_join(tp, NULL);
        pthread_join(tc, NULL);
        MemPoolAPI_StatsGet(g_qpool, &st);
        Debug_printx("%s round %d/%d: alloc=%"PRIu64" free=%"PRIu64" drop=%"PRIu64" grow=%"PRIu64" cap=%d peak=%d",
                     name, r + 1, ROUNDS, st.ulTotalAlloc, st.ulTotalFree,
                     st.ulTotalDrop, st.ulTotalGrow, st.iCapacity, st.iPeakUsed);
    }

    MemPoolAPI_StatsGet(g_qpool, &st);
    Debug_printx("%s total: alloc=%"PRIu64" free=%"PRIu64" drop=%"PRIu64" grow=%"PRIu64" cap=%d peak=%d",
                 name, st.ulTotalAlloc, st.ulTotalFree,
                 st.ulTotalDrop, st.ulTotalGrow, st.iCapacity, st.iPeakUsed);

    ThreadQueueAPI_CloseMsg(g_q);
    ThreadQueueAPI_FlushMsg(g_q, q_release);
    ThreadQueueAPI_DestroyMsg(&g_q);
    MemPoolAPI_Destroy(&g_qpool);
}


/* ================================================================== */
/*                                                                    */
/*     Part 7-8: init_count 扫描                                       */
/*                                                                    */
/* ================================================================== */
static void test_init_scan(MemPoolMode mode, const char *name)
{
    int inits[] = {8, 16, 32, 64, 128, 256, 512, 1024};
    int n = (int)(sizeof(inits) / sizeof(inits[0]));
    int N = 1000;
    int k;

    Debug_printx("=== %s init_count scan (N=%d per init) ===", name, N);
    for(k = 0; k < n; k++)
    {
        T_MemPoolConfig pcfg = { (int)sizeof(T_QMsg), inits[k], mode, 4, 0, 0 };
        T_MemPoolStats st;
        pthread_t tp, tc;
        const char *tag;

        MemPoolAPI_Init(&g_qpool, &pcfg, "scan");
        ThreadQueueAPI_InitMsg(&g_q, 2000, "scanq", q_release);

        g_prod_done = 0;
        pthread_create(&tp, NULL, q_producer, &N);
        pthread_create(&tc, NULL, q_consumer, NULL);
        pthread_join(tp, NULL);
        pthread_join(tc, NULL);

        MemPoolAPI_StatsGet(g_qpool, &st);
        tag = (mode == MEMPOOL_MODE_DROP)
              ? (st.ulTotalDrop == 0 ? "[OK no-drop]" : "")
              : (st.ulTotalGrow == 0 ? "[OK no-grow]" : "");
        Debug_printx("%s init=%-5d: alloc=%"PRIu64" free=%"PRIu64" drop=%"PRIu64" grow=%"PRIu64" cap=%d peak=%d %s",
                     name, inits[k], st.ulTotalAlloc, st.ulTotalFree,
                     st.ulTotalDrop, st.ulTotalGrow, st.iCapacity, st.iPeakUsed, tag);

        ThreadQueueAPI_CloseMsg(g_q);
        ThreadQueueAPI_FlushMsg(g_q, q_release);
        ThreadQueueAPI_DestroyMsg(&g_q);
        MemPoolAPI_Destroy(&g_qpool);
    }
}


/* ================================================================== */
/*                                                                    */
/*     Part 9: max_count 上限（到限后 GROW 自动 DROP）                */
/*                                                                    */
/* ================================================================== */
static void test_max_count_limit(void)
{
    /* init=4, grow=4, max=12：期望 alloc 前 12 成功、后 8 drop；cap=12 不再扩 */
    T_MemPoolConfig cfg = { (int)sizeof(int), 4, MEMPOOL_MODE_GROW, 4, 0, 12 };
    T_MemPool *p = NULL;
    void *slots[20] = {0};
    int i, got = 0;
    T_MemPoolStats st;

    if(MemPoolAPI_Init(&p, &cfg, "max_limit") != 0)
    {
        TEST_ASSERT(0, "max_limit Init should succeed");
        return;
    }
    for(i = 0; i < 20; i++)
    {
        slots[i] = MemPoolAPI_Alloc(p);
        if(slots[i]) got++;
    }
    MemPoolAPI_StatsGet(p, &st);
    Debug_printx("max_limit: alloc=%"PRIu64" drop=%"PRIu64" grow=%"PRIu64" cap=%d peak=%d",
                 st.ulTotalAlloc, st.ulTotalDrop, st.ulTotalGrow, st.iCapacity, st.iPeakUsed);
    TEST_ASSERT(got == 12,             "max_limit success count=12, got=%d", got);
    TEST_ASSERT(st.ulTotalAlloc == 12, "max_limit total_alloc=12, got=%"PRIu64, st.ulTotalAlloc);
    TEST_ASSERT(st.ulTotalDrop  == 8,  "max_limit total_drop=8, got=%"PRIu64, st.ulTotalDrop);
    TEST_ASSERT(st.iCapacity    == 12, "max_limit capacity=12, got=%d", st.iCapacity);
    TEST_ASSERT(st.ulTotalGrow  == 8,  "max_limit total_grow=8, got=%"PRIu64, st.ulTotalGrow);
    for(i = 0; i < 20; i++) if(slots[i]) MemPoolAPI_Free(p, slots[i]);
    MemPoolAPI_Destroy(&p);
}


/* ================================================================== */
/*                                                                    */
/*     Part 10: max_count < init_count = 无上限扩容                   */
/*                                                                    */
/* ================================================================== */
static void test_max_count_unlimited(void)
{
    /* init=8, grow=4, max=1 (<init) → 无上限，20 次分配全部成功 */
    T_MemPoolConfig cfg = { (int)sizeof(int), 8, MEMPOOL_MODE_GROW, 4, 0, 1 };
    T_MemPool *p = NULL;
    void *slots[20] = {0};
    int i, got = 0;
    T_MemPoolStats st;

    if(MemPoolAPI_Init(&p, &cfg, "max_unlimited") != 0)
    {
        TEST_ASSERT(0, "max_unlimited Init should succeed");
        return;
    }
    for(i = 0; i < 20; i++)
    {
        slots[i] = MemPoolAPI_Alloc(p);
        if(slots[i]) got++;
    }
    MemPoolAPI_StatsGet(p, &st);
    Debug_printx("max_unlimited: alloc=%"PRIu64" drop=%"PRIu64" grow=%"PRIu64" cap=%d",
                 st.ulTotalAlloc, st.ulTotalDrop, st.ulTotalGrow, st.iCapacity);
    TEST_ASSERT(got == 20,             "max_unlimited success count=20, got=%d", got);
    TEST_ASSERT(st.ulTotalDrop == 0,   "max_unlimited no drop, got=%"PRIu64, st.ulTotalDrop);
    TEST_ASSERT(st.iCapacity >= 20,    "max_unlimited capacity>=20, got=%d", st.iCapacity);
    for(i = 0; i < 20; i++) if(slots[i]) MemPoolAPI_Free(p, slots[i]);
    MemPoolAPI_Destroy(&p);
}


/* ================================================================== */
/*                                                                    */
/*     Part 11: max_count 非整倍数（尾轮按剩余额度扩容）              */
/*                                                                    */
/* ================================================================== */
static void test_max_count_partial(void)
{
    /* init=4, grow=5, max=10：
       - 前 4 用初始容量；第 5 次触发第一次扩容，remain=6, grow=5 → 加 5，cap=9；
       - 第 10 次触发第二次扩容，remain=1 < grow=5 → 只加 1，cap=10；
       - 第 11 次开始 GROW 自动 DROP 到 15。 */
    T_MemPoolConfig cfg = { (int)sizeof(int), 4, MEMPOOL_MODE_GROW, 5, 0, 10 };
    T_MemPool *p = NULL;
    void *slots[15] = {0};
    int i, got = 0;
    T_MemPoolStats st;

    if(MemPoolAPI_Init(&p, &cfg, "max_partial") != 0)
    {
        TEST_ASSERT(0, "max_partial Init should succeed");
        return;
    }
    for(i = 0; i < 15; i++)
    {
        slots[i] = MemPoolAPI_Alloc(p);
        if(slots[i]) got++;
    }
    MemPoolAPI_StatsGet(p, &st);
    Debug_printx("max_partial: alloc=%"PRIu64" drop=%"PRIu64" grow=%"PRIu64" cap=%d",
                 st.ulTotalAlloc, st.ulTotalDrop, st.ulTotalGrow, st.iCapacity);
    TEST_ASSERT(got == 10,             "max_partial success=10, got=%d", got);
    TEST_ASSERT(st.iCapacity == 10,    "max_partial capacity=10, got=%d", st.iCapacity);
    TEST_ASSERT(st.ulTotalDrop == 5,   "max_partial drop=5, got=%"PRIu64, st.ulTotalDrop);
    for(i = 0; i < 15; i++) if(slots[i]) MemPoolAPI_Free(p, slots[i]);
    MemPoolAPI_Destroy(&p);
}


/* ================================================================== */
/*                                                                    */
/*     Part 12: Init 参数边界（mode/block_timeo/超大 init_count）    */
/*                                                                    */
/* ================================================================== */
static void test_init_bounds(void)
{
    T_MemPool *p = NULL;
    T_MemPool *pt = NULL;
    T_MemPool *pt2;
    T_MemPool *pb = NULL;
    T_MemPoolConfig cfg;

    /* 12.1 非法 mode */
    memset(&cfg, 0, sizeof(cfg));
    cfg.element_size = 8; cfg.init_count = 4; cfg.mode = (MemPoolMode)99;
    TEST_ASSERT(MemPoolAPI_Init(&p, &cfg, "bad_mode") == -1, "reject mode=99");
    TEST_ASSERT(p == NULL, "reject mode leaves *pp NULL");

    /* 12.2 GROW + grow_count=0 */
    memset(&cfg, 0, sizeof(cfg));
    cfg.element_size = 8; cfg.init_count = 4; cfg.mode = MEMPOOL_MODE_GROW; cfg.grow_count = 0;
    TEST_ASSERT(MemPoolAPI_Init(&p, &cfg, "grow_zero") == -1, "reject GROW+grow_count=0");

    /* 12.3 负 block_timeo */
    memset(&cfg, 0, sizeof(cfg));
    cfg.element_size = 8; cfg.init_count = 4; cfg.mode = MEMPOOL_MODE_BLOCK; cfg.block_timeo = -1;
    TEST_ASSERT(MemPoolAPI_Init(&p, &cfg, "neg_timeo") == -1, "reject block_timeo=-1");

    /* 12.4 超大 init_count（乘法溢出） */
    memset(&cfg, 0, sizeof(cfg));
    cfg.element_size = 1024; cfg.init_count = INT_MAX; cfg.mode = MEMPOOL_MODE_DROP;
    TEST_ASSERT(MemPoolAPI_Init(&p, &cfg, "huge_count") == -1, "reject count*size overflow");

    /* 12.5 element_size 上界 */
    memset(&cfg, 0, sizeof(cfg));
    cfg.element_size = INT_MAX; cfg.init_count = 1; cfg.mode = MEMPOOL_MODE_DROP;
    TEST_ASSERT(MemPoolAPI_Init(&p, &cfg, "huge_elem") == -1, "reject element_size=INT_MAX");

    /* 12.6 *pp 已非 NULL */
    memset(&cfg, 0, sizeof(cfg));
    cfg.element_size = 8; cfg.init_count = 4; cfg.mode = MEMPOOL_MODE_DROP;
    MemPoolAPI_Init(&pt, &cfg, "ok");
    TEST_ASSERT(pt != NULL, "first Init ok");
    pt2 = pt;
    TEST_ASSERT(MemPoolAPI_Init(&pt2, &cfg, "already") == -1, "reject *pp not NULL");
    MemPoolAPI_Destroy(&pt);

    /* 12.7 AllocBlock(-1) 应返回 NULL */
    memset(&cfg, 0, sizeof(cfg));
    cfg.element_size = 8; cfg.init_count = 2; cfg.mode = MEMPOOL_MODE_BLOCK; cfg.block_timeo = 0;
    MemPoolAPI_Init(&pb, &cfg, "block_neg");
    TEST_ASSERT(MemPoolAPI_AllocBlock(pb, -1) == NULL, "AllocBlock(-1) returns NULL");
    MemPoolAPI_Destroy(&pb);

    /* 12.8 非 GROW 模式下 grow_count 负数 Init 加严拒绝（P3-5） */
    {
        T_MemPool *pn = NULL;
        memset(&cfg, 0, sizeof(cfg));
        cfg.element_size = 8; cfg.init_count = 4; cfg.mode = MEMPOOL_MODE_DROP; cfg.grow_count = -100;
        TEST_ASSERT(MemPoolAPI_Init(&pn, &cfg, "neg_grow") == -1, "reject grow_count<0 in non-GROW");
    }
}


/* ================================================================== */
/*                                                                    */
/*     Part 13: BLOCK 超时与 Free 并发（stolen wakeup 回归保护）      */
/*                                                                    */
/* ================================================================== */
#define STL_WAITERS 8
#define STL_ROUNDS  10

static T_MemPool  *g_stl_pool;
static int         g_stl_got[STL_WAITERS];

static void *stl_waiter(void *arg)
{
    int idx = *(int *)arg;
    void *s = MemPoolAPI_AllocBlock(g_stl_pool, 100);
    g_stl_got[idx] = (s != NULL) ? 1 : 0;
    if(s) MemPoolAPI_Free(g_stl_pool, s);
    return NULL;
}

static void test_stolen_wakeup(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), STL_WAITERS, MEMPOOL_MODE_BLOCK, 0, 0, 0 };
    int r, i;
    int total_success = 0, total_fail = 0;
    T_MemPoolStats st;

    MemPoolAPI_Init(&g_stl_pool, &cfg, "stl");
    for(r = 0; r < STL_ROUNDS; r++)
    {
        void *pre[STL_WAITERS];
        pthread_t tid[STL_WAITERS];
        int idx[STL_WAITERS];

        for(i = 0; i < STL_WAITERS; i++) pre[i] = MemPoolAPI_Alloc(g_stl_pool);
        memset(g_stl_got, 0, sizeof(g_stl_got));

        for(i = 0; i < STL_WAITERS; i++)
        {
            idx[i] = i;
            pthread_create(&tid[i], NULL, stl_waiter, &idx[i]);
        }
        usleep(20 * 1000);
        for(i = 0; i < STL_WAITERS; i++)
        {
            usleep((70 / STL_WAITERS) * 1000);
            MemPoolAPI_Free(g_stl_pool, pre[i]);
        }
        for(i = 0; i < STL_WAITERS; i++) pthread_join(tid[i], NULL);
        for(i = 0; i < STL_WAITERS; i++)
        {
            if(g_stl_got[i]) total_success++;
            else             total_fail++;
        }
    }
    MemPoolAPI_StatsGet(g_stl_pool, &st);
    Debug_printx("stolen_wakeup: success=%d fail=%d (alloc=%"PRIu64" drop=%"PRIu64")",
                 total_success, total_fail, st.ulTotalAlloc, st.ulTotalDrop);
    /* 每轮先 Alloc 抽干池(STL_WAITERS 次)再起等待者(STL_WAITERS 次)：
       total_alloc = drain(ROUNDS*WAITERS) + 等待者成功数(total_success) */
    {
        int drain = STL_ROUNDS * STL_WAITERS;
        TEST_ASSERT(total_success + total_fail == drain,
                    "stolen: total waiters=%d", drain);
        TEST_ASSERT((int)st.ulTotalAlloc == total_success + drain,
                    "stolen: total_alloc(%"PRIu64") == success(%d) + drain(%d)",
                    st.ulTotalAlloc, total_success, drain);
        TEST_ASSERT((int)st.ulTotalDrop  == total_fail,
                    "stolen: total_drop=%"PRIu64" match fail=%d", st.ulTotalDrop, total_fail);
        TEST_ASSERT(total_success >= drain / 2,
                    "stolen: success rate >=50%% (Free 全部到位)");
    }
    MemPoolAPI_Destroy(&g_stl_pool);
}


/* ================================================================== */
/*                                                                    */
/*     Part 14: Destroy 唤醒 BLOCK 等待者（shutting_down）            */
/*                                                                    */
/* ================================================================== */
static T_MemPool *g_dst_pool;
static volatile int g_dst_ret_null_count = 0;

static void *dst_waiter(void *arg)
{
    (void)arg;
    void *s = MemPoolAPI_AllocBlock(g_dst_pool, 0);
    if(s == NULL) __sync_fetch_and_add(&g_dst_ret_null_count, 1);
    if(s) MemPoolAPI_Free(g_dst_pool, s);
    return NULL;
}

static void test_destroy_wakes_waiters(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 2, MEMPOOL_MODE_BLOCK, 0, 0, 0 };
    void *s1, *s2;
    pthread_t t1, t2, t3;

    MemPoolAPI_Init(&g_dst_pool, &cfg, "dst");
    s1 = MemPoolAPI_Alloc(g_dst_pool);
    s2 = MemPoolAPI_Alloc(g_dst_pool);
    (void)s1; (void)s2;

    g_dst_ret_null_count = 0;
    pthread_create(&t1, NULL, dst_waiter, NULL);
    pthread_create(&t2, NULL, dst_waiter, NULL);
    pthread_create(&t3, NULL, dst_waiter, NULL);
    usleep(150 * 1000);   /* 让 3 个等待者都进入 cond_wait */

    /* 不 Free，直接 Destroy：应 broadcast + 等 waiter_count 归零后释放 */
    MemPoolAPI_Destroy(&g_dst_pool);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    TEST_ASSERT(g_dst_ret_null_count == 3, "Destroy wakes all 3 waiters -> return NULL, got=%d",
                g_dst_ret_null_count);
    TEST_ASSERT(g_dst_pool == NULL,        "Destroy sets *pp = NULL");
}


/* ================================================================== */
/*                                                                    */
/*     main                                                           */
/*                                                                    */
/* ================================================================== */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    Debug_printx("========== Part 1: DROP (single) ==========");
    test_drop();

    Debug_printx("========== Part 2: GROW (single) ==========");
    test_grow();

    Debug_printx("========== Part 3: BLOCK (single) ==========");
    test_block();

    Debug_printx("========== Part 4: BLOCK + ThreadQueue (10x1000) ==========");
    test_pool_queue_stress(MEMPOOL_MODE_BLOCK, "BLOCK-Q");

    Debug_printx("========== Part 5: GROW + ThreadQueue (10x1000) ==========");
    test_pool_queue_stress(MEMPOOL_MODE_GROW, "GROW-Q");

    Debug_printx("========== Part 6: DROP + ThreadQueue (10x1000) ==========");
    test_pool_queue_stress(MEMPOOL_MODE_DROP, "DROP-Q");

    Debug_printx("========== Part 7: init_count scan (DROP) ==========");
    test_init_scan(MEMPOOL_MODE_DROP, "DROP-scan");

    Debug_printx("========== Part 8: init_count scan (GROW) ==========");
    test_init_scan(MEMPOOL_MODE_GROW, "GROW-scan");

    Debug_printx("========== Part 9: max_count limit (GROW->DROP) ==========");
    test_max_count_limit();

    Debug_printx("========== Part 10: max_count<init = unlimited ==========");
    test_max_count_unlimited();

    Debug_printx("========== Part 11: max_count partial grow ==========");
    test_max_count_partial();

    Debug_printx("========== Part 12: Init parameter bounds ==========");
    test_init_bounds();

    Debug_printx("========== Part 13: BLOCK stolen wakeup race ==========");
    test_stolen_wakeup();

    Debug_printx("========== Part 14: Destroy wakes BLOCK waiters ==========");
    test_destroy_wakes_waiters();

    Debug_printx("========== Test Summary: PASS=%d FAIL=%d ==========",
                 g_test_pass, g_test_fail);

    Debug_printx("Program exit");
    return (g_test_fail == 0) ? 0 : 1;
}
