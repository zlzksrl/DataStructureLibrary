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

    Debug_printx("Program exit");
    return 0;
}
