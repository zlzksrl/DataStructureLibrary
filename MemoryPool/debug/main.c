/**
 * @file        main.c
 * @brief       MemoryPool 内存池 - 测试/演示程序
 * @details     演示三种池满策略：
 *              Part 1: DROP  —— 池满返回 NULL（统计丢弃）
 *              Part 2: GROW  —— 池满动态扩容（capacity 增长）
 *              Part 3: BLOCK —— 池满阻塞等待（Free 唤醒）
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
/**
 * @def   Debug_printx
 * @brief 调试打印宏（将 #if 1 改为 0 可关闭）
 */
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
/*     Part 1: DROP（池满返回 NULL）                                   */
/*                                                                    */
/* ================================================================== */

static void test_drop(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 8, MEMPOOL_MODE_DROP, 0, 0 };
    T_MemPool *p = NULL;
    void *slots[12];
    int i;
    T_MemPoolStats st;

    MemPoolAPI_Init(&p, &cfg, "drop");

    /* 申请 12 个，容量 8 → 前 8 成功、后 4 返回 NULL */
    for(i = 0; i < 12; i++)
    {
        slots[i] = MemPoolAPI_Alloc(p);
        if(slots[i])
        {
            *(int *)slots[i] = i;
        }
    }
    MemPoolAPI_StatsGet(p, &st);
    Debug_printx("DROP: alloc=%lu drop=%lu capacity=%d (expect alloc=8 drop=4)",
                 st.ulTotalAlloc, st.ulTotalDrop, st.iCapacity);

    /* 归还全部 */
    for(i = 0; i < 12; i++)
    {
        if(slots[i])
        {
            MemPoolAPI_Free(p, slots[i]);
        }
    }
    MemPoolAPI_Destroy(&p);
}


/* ================================================================== */
/*                                                                    */
/*     Part 2: GROW（池满动态扩容）                                    */
/*                                                                    */
/* ================================================================== */

static void test_grow(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 4, MEMPOOL_MODE_GROW, 4, 0 };
    T_MemPool *p = NULL;
    void *slots[12];
    int i;
    T_MemPoolStats st;

    MemPoolAPI_Init(&p, &cfg, "grow");

    /* 申请 12 个，初始 4 + 每次扩容 4 → 全部成功，capacity 增长到 12 */
    for(i = 0; i < 12; i++)
    {
        slots[i] = MemPoolAPI_AllocGrow(p);
        if(slots[i])
        {
            *(int *)slots[i] = i;
        }
    }
    MemPoolAPI_StatsGet(p, &st);
    Debug_printx("GROW: alloc=%lu capacity=%d grow=%lu (expect alloc=12 capacity=12)",
                 st.ulTotalAlloc, st.iCapacity, st.ulTotalGrow);

    for(i = 0; i < 12; i++)
    {
        if(slots[i])
        {
            MemPoolAPI_Free(p, slots[i]);
        }
    }
    MemPoolAPI_Destroy(&p);
}


/* ================================================================== */
/*                                                                    */
/*     Part 3: BLOCK（池满阻塞等待，Free 唤醒）                        */
/*                                                                    */
/* ================================================================== */

static T_MemPool *g_pool = NULL;

/**
 * @func         blocker_thread
 * @brief        阻塞线程：在池满时 AllocBlock(0) 无限等待，被 Free 唤醒后返回
 */
static void *blocker_thread(void *arg)
{
    void *s;
    (void)arg;
    s = MemPoolAPI_AllocBlock(g_pool, 0);   /* 池满，无限阻塞等 */
    Debug_printx("blocker: got slot=%p after woken", s);
    if(s)
    {
        MemPoolAPI_Free(g_pool, s);
    }
    return NULL;
}

static void test_block(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 4, MEMPOOL_MODE_BLOCK, 0, 0 };
    void *slots[4];
    pthread_t tid;
    int i;

    MemPoolAPI_Init(&g_pool, &cfg, "block");

    /* 先占满 4 个 */
    for(i = 0; i < 4; i++)
    {
        slots[i] = MemPoolAPI_Alloc(g_pool);
    }
    Debug_printx("BLOCK: pool full (4/4), start blocker thread");

    /* 启动阻塞线程：它会 AllocBlock 无限等 */
    pthread_create(&tid, NULL, blocker_thread, NULL);
    usleep(100 * 1000);   /* 等 blocker 进入阻塞 */

    /* Free 一个 → signal 唤醒 blocker */
    Debug_printx("BLOCK: free one slot to wake blocker");
    MemPoolAPI_Free(g_pool, slots[0]);

    pthread_join(tid, NULL);

    /* 归还剩余 */
    for(i = 1; i < 4; i++)
    {
        MemPoolAPI_Free(g_pool, slots[i]);
    }
    MemPoolAPI_Destroy(&g_pool);
}


/* ================================================================== */
/*                                                                    */
/*     Part 4: 配合 ThreadQueue（Alloc→PutMsg→GetMsg→Free 循环复用）  */
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

/* 生产者：Alloc(池取) → 填数据 → PutMsg(入队) */
static void *q_producer(void *arg)
{
    int N = *(int *)arg;
    int i;
    for(i = 0; i < N; i++)
    {
        T_QMsg *m = (T_QMsg *)MemPoolAPI_AllocBlock(g_qpool, 0);   /* 池满则阻塞等 */
        m->id  = i;
        m->val = i * 10;
        ThreadQueueAPI_PutMsg(g_q, m);
    }
    return NULL;
}

/* 消费者：GetMsg(取) → 用数据 → Free(归还池) */
static void *q_consumer(void *arg)
{
    int N = *(int *)arg;
    int i;
    for(i = 0; i < N; i++)
    {
        T_QMsg *m = (T_QMsg *)ThreadQueueAPI_GetMsg(g_q, 1000);
        if(m != NULL)
        {
            MemPoolAPI_Free(g_qpool, m);   /* 归还池，循环复用 */
        }
    }
    return NULL;
}

static void test_with_threadqueue(void)
{
    T_MemPoolConfig pcfg = { (int)sizeof(T_QMsg), 8, MEMPOOL_MODE_BLOCK, 0, 0 };
    int N = 1000;        /* 每轮入队/出队条数 */
    int ROUNDS = 10;     /* 频繁测试轮数（共 N×ROUNDS 次流转） */
    int r;
    pthread_t tp, tc;
    T_MemPoolStats st;

    MemPoolAPI_Init(&g_qpool, &pcfg, "qpool");
    ThreadQueueAPI_InitMsg(&g_q, 100, "q", q_release);   /* 队列残留→归还池 */

    /* 多轮频繁测试：每轮 N 条入队/出队，验证池循环复用长期稳定 */
    for(r = 0; r < ROUNDS; r++)
    {
        pthread_create(&tp, NULL, q_producer, &N);
        pthread_create(&tc, NULL, q_consumer, &N);
        pthread_join(tp, NULL);
        pthread_join(tc, NULL);
        MemPoolAPI_StatsGet(g_qpool, &st);
        Debug_printx("POOL+QUEUE round %d/%d: alloc=%lu free=%lu capacity=%d peak=%d",
                     r + 1, ROUNDS, st.ulTotalAlloc, st.ulTotalFree, st.iCapacity, st.iPeakUsed);
    }

    /* 最终校验：累计 alloc==free==N*ROUNDS，capacity 恒定 8（无泄漏、无扩容） */
    MemPoolAPI_StatsGet(g_qpool, &st);
    Debug_printx("POOL+QUEUE total: alloc=%lu free=%lu capacity=%d peak=%d (expect alloc=free=%d capacity=8)",
                 st.ulTotalAlloc, st.ulTotalFree, st.iCapacity, st.iPeakUsed, N * ROUNDS);

    /* 优雅关闭队列：Close → Flush(残留归还池) → Destroy（避免 Destroy 警告） */
    ThreadQueueAPI_CloseMsg(g_q);
    ThreadQueueAPI_FlushMsg(g_q, q_release);
    ThreadQueueAPI_DestroyMsg(&g_q);
    MemPoolAPI_Destroy(&g_qpool);
}


/* ================================================================== */
/*                                                                    */
/*     Part 5: GROW 扩容压测（10000 次，打印 peak）                    */
/*                                                                    */
/* ================================================================== */

static void test_grow_stress(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 4, MEMPOOL_MODE_GROW, 4, 0 };
    T_MemPool *p = NULL;
    int N = 10000;
    void **slots;
    int i;
    T_MemPoolStats st;

    slots = (void **)malloc((size_t)N * sizeof(void *));
    if(slots == NULL) return;
    MemPoolAPI_Init(&p, &cfg, "grow_stress");
    for(i = 0; i < N; i++)
    {
        slots[i] = MemPoolAPI_AllocGrow(p);   /* 满则扩容 */
    }
    MemPoolAPI_StatsGet(p, &st);
    Debug_printx("GROW stress N=%d: alloc=%lu capacity=%d grow=%lu peak=%d",
                 N, st.ulTotalAlloc, st.iCapacity, st.ulTotalGrow, st.iPeakUsed);
    for(i = 0; i < N; i++)
    {
        if(slots[i]) MemPoolAPI_Free(p, slots[i]);
    }
    MemPoolAPI_Destroy(&p);
    free(slots);
}


/* ================================================================== */
/*                                                                    */
/*     Part 6: DROP 丢弃压测（10000 次，打印 peak）                    */
/*                                                                    */
/* ================================================================== */

static void test_drop_stress(void)
{
    T_MemPoolConfig cfg = { (int)sizeof(int), 4, MEMPOOL_MODE_DROP, 0, 0 };
    T_MemPool *p = NULL;
    int N = 10000;
    void **slots;
    int i, got = 0;
    T_MemPoolStats st;

    slots = (void **)malloc((size_t)N * sizeof(void *));
    if(slots == NULL) return;
    MemPoolAPI_Init(&p, &cfg, "drop_stress");
    for(i = 0; i < N; i++)
    {
        slots[i] = MemPoolAPI_AllocDrop(p);   /* 满则 NULL（丢弃） */
        if(slots[i]) got++;
    }
    MemPoolAPI_StatsGet(p, &st);
    Debug_printx("DROP stress N=%d: alloc=%lu drop=%lu capacity=%d peak=%d (got=%d)",
                 N, st.ulTotalAlloc, st.ulTotalDrop, st.iCapacity, st.iPeakUsed, got);
    for(i = 0; i < N; i++)
    {
        if(slots[i]) MemPoolAPI_Free(p, slots[i]);
    }
    MemPoolAPI_Destroy(&p);
    free(slots);
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

    Debug_printx("========== Part 1: DROP Start ==========");
    test_drop();

    Debug_printx("========== Part 2: GROW Start ==========");
    test_grow();

    Debug_printx("========== Part 3: BLOCK Start ==========");
    test_block();

    Debug_printx("========== Part 4: with ThreadQueue Start ==========");
    test_with_threadqueue();

    Debug_printx("========== Part 5: GROW stress Start ==========");
    test_grow_stress();

    Debug_printx("========== Part 6: DROP stress Start ==========");
    test_drop_stress();

    Debug_printx("Program exit");
    return 0;
}
