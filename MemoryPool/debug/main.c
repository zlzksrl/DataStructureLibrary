/**
 * @file        main.c
 * @brief       MemoryPool 内存池 - 测试/演示程序
 * @details     Part 1-3: 单线程三模式基础（DROP/GROW/BLOCK 基本功能）
 *              Part 4-6: 多线程 + ThreadQueue 三模式压测 10×1000
 *                        （BLOCK 阻塞 / GROW 扩容 / DROP 丢弃，都走 alloc→PutMsg→GetMsg→Free）
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-10
 * @copyright   copyright (C) 2026
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

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


/* ================================================================== */
/*                                                                    */
/*     Part 1-3: 单线程三模式基础                                      */
/*                                                                    */
/* ================================================================== */

/* ---- Part 1: DROP（池满返回 NULL） ---- */
static void test_drop(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 8, MEMPOOL_MODE_DROP, 0, 0 };
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
    Debug_printx("DROP: alloc=%lu drop=%lu capacity=%d (expect alloc=8 drop=4)",
                 st.ulTotalAlloc, st.ulTotalDrop, st.iCapacity);
    for(i = 0; i < 12; i++) if(slots[i]) MemPoolAPI_Free(p, slots[i]);
    MemPoolAPI_Destroy(&p);
}

/* ---- Part 2: GROW（池满动态扩容） ---- */
static void test_grow(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 4, MEMPOOL_MODE_GROW, 4, 0 };
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
    Debug_printx("GROW: alloc=%lu capacity=%d grow=%lu (expect alloc=12 capacity=12)",
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
    T_MemPoolConfig cfg = { (int)sizeof(int), 4, MEMPOOL_MODE_BLOCK, 0, 0 };
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
/*  producer: Alloc(按cfg.mode) → 填数据 → PutMsg                     */
/*  consumer: GetMsg → 用数据 → Free(归还池)                          */
/*  三模式：BLOCK(满则阻塞等) / GROW(满则扩容) / DROP(满则丢弃)       */
/*                                                                    */
/* ================================================================== */

typedef struct { int id; int val; } T_QMsg;

static T_MemPool        *g_qpool;
static T_ThreadQueueMsg *g_q;

/* 队列残留数据释放回调：归还到池 */
static void q_release(void *data)
{
    MemPoolAPI_Free(g_qpool, data);
}

static volatile int g_prod_done = 0;   /* 生产者产完标志 */

/* 生产者：Alloc(按 cfg.mode) → 填数据 → PutMsg */
static void *q_producer(void *arg)
{
    int N = *(int *)arg;
    int i;
    for(i = 0; i < N; i++)
    {
        T_QMsg *m = (T_QMsg *)MemPoolAPI_Alloc(g_qpool);   /* BLOCK/GROW/DROP 按 cfg.mode */
        if(m != NULL)              /* DROP 模式可能返回 NULL（丢弃，不入队） */
        {
            m->id  = i;
            m->val = i * 10;
            ThreadQueueAPI_PutMsg(g_q, m);
        }
    }
    g_prod_done = 1;               /* 产完，通知消费者 */
    return NULL;
}

/* 消费者：GetMsg → Free(归还池)；产完且超时无数据则退出 */
static void *q_consumer(void *arg)
{
    (void)arg;
    while(1)
    {
        T_QMsg *m = (T_QMsg *)ThreadQueueAPI_GetMsg(g_q, 100);  /* 100ms 超时 */
        if(m != NULL)
        {
            MemPoolAPI_Free(g_qpool, m);
        }
        else if(g_prod_done)
        {
            break;   /* 生产者已产完 + 超时无数据 → 退出 */
        }
    }
    return NULL;
}

/**
 * @func         test_pool_queue_stress
 * @brief        多线程+队列压测：指定模式跑 10 轮 × 1000 条
 */
static void test_pool_queue_stress(MemPoolMode mode, const char *name)
{
    T_MemPoolConfig pcfg = { (int)sizeof(T_QMsg), 8, mode, 4, 0 };  /* init 8, grow 4, block_timeo 0 */
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
        Debug_printx("%s round %d/%d: alloc=%lu free=%lu drop=%lu grow=%lu cap=%d peak=%d",
                     name, r + 1, ROUNDS, st.ulTotalAlloc, st.ulTotalFree,
                     st.ulTotalDrop, st.ulTotalGrow, st.iCapacity, st.iPeakUsed);
    }

    MemPoolAPI_StatsGet(g_qpool, &st);
    Debug_printx("%s total: alloc=%lu free=%lu drop=%lu grow=%lu cap=%d peak=%d",
                 name, st.ulTotalAlloc, st.ulTotalFree,
                 st.ulTotalDrop, st.ulTotalGrow, st.iCapacity, st.iPeakUsed);

    /* 优雅关闭队列 */
    ThreadQueueAPI_CloseMsg(g_q);
    ThreadQueueAPI_FlushMsg(g_q, q_release);
    ThreadQueueAPI_DestroyMsg(&g_q);
    MemPoolAPI_Destroy(&g_qpool);
}


/* ================================================================== */
/*                                                                    */
/*     Part 7-8: init_count 扫描（找"不丢/不扩"的最小 init）          */
/*                                                                    */
/* ================================================================== */

/**
 * @func         test_init_scan
 * @brief        扫描不同 init_count，找 drop=0(DROP) 或 grow=0(GROW) 的最小 init
 */
static void test_init_scan(MemPoolMode mode, const char *name)
{
    int inits[] = {8, 16, 32, 64, 128, 256, 512, 1024};
    int n = (int)(sizeof(inits) / sizeof(inits[0]));
    int N = 1000;
    int k;

    Debug_printx("=== %s init_count scan (N=%d per init) ===", name, N);
    for(k = 0; k < n; k++)
    {
        T_MemPoolConfig pcfg = { (int)sizeof(T_QMsg), inits[k], mode, 4, 0 };
        T_MemPoolStats st;
        pthread_t tp, tc;
        const char *tag;

        MemPoolAPI_Init(&g_qpool, &pcfg, "scan");
        ThreadQueueAPI_InitMsg(&g_q, 2000, "scanq", q_release);   /* 队列大，不限制 */

        g_prod_done = 0;
        pthread_create(&tp, NULL, q_producer, &N);
        pthread_create(&tc, NULL, q_consumer, NULL);
        pthread_join(tp, NULL);
        pthread_join(tc, NULL);

        MemPoolAPI_StatsGet(g_qpool, &st);
        tag = (mode == MEMPOOL_MODE_DROP)
              ? (st.ulTotalDrop == 0 ? "[OK no-drop]" : "")
              : (st.ulTotalGrow == 0 ? "[OK no-grow]" : "");
        Debug_printx("%s init=%-5d: alloc=%lu free=%lu drop=%lu grow=%lu cap=%d peak=%d %s",
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
/*     main                                                          */
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

    Debug_printx("Program exit");
    return 0;
}
