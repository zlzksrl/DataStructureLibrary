/**
 * @file        main.c
 * @brief       StreamBuffer 流缓冲区 - 测试/演示程序
 * @details     演示三种消费方式 + 优雅关闭排空 + Reopen：
 *
 *              Part 1: GetData 拷贝式（生产者 PutData + 消费者 GetData 写文件 + 优雅关闭）
 *              Part 2: 零拷贝回调式（SetConsumeCallback + Wait 自动消费）
 *              Part 3: GetDataAddress 零拷贝逐段式
 *              Part 4: Reopen（Close 可逆）
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-09
 * @copyright   copyright (C) 2026
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include "../include/StreamBuffer.h"


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
/*     Part 1: GetData 拷贝式（生产者 + 消费者写文件 + 优雅关闭）       */
/*                                                                    */
/* ================================================================== */

static int  g_produce_count = 0;     /* 生产者累计条数 */
static FILE *g_fp          = NULL;   /* 消费者写入的文件 */

/**
 * @func         producer_thread
 * @brief        生产者：格式化日志行 PutData
 */
static void *producer_thread(void *arg)
{
    T_StreamBuffer *sb = (T_StreamBuffer *)arg;
    int i;
    char line[128];
    for(i = 0; i < 300; i++)
    {
        int n = snprintf(line, sizeof(line), "ts=%d,value=%d\r\n", i, i * 2);
        int r = StreamBufferAPI_PutData(sb, line, n);
        if(r >= 0)
        {
            g_produce_count++;
        }
        else if(r == -3)
        {
            Debug_printx("PutData dropped (full), i=%d", i);
        }
        usleep(1 * 1000);   /* 1ms 一条 */
    }
    return NULL;
}

/**
 * @func         consumer_thread
 * @brief        消费者：Wait + GetData 拷贝式写文件
 */
static void *consumer_thread(void *arg)
{
    T_StreamBuffer *sb = (T_StreamBuffer *)arg;
    char buf[8192];
    int used;
    while(1)
    {
        int r = StreamBufferAPI_Wait(sb, 1000, &used);   /* 等1s/阈值/关闭 */
        if(r > 0)
        {
            int n;
            while((n = StreamBufferAPI_GetData(sb, buf, sizeof(buf))) > 0)
            {
                if(g_fp)
                {
                    fwrite(buf, 1, (size_t)n, g_fp);
                }
            }
            if(g_fp)
            {
                fflush(g_fp);
            }
        }
        if(r <= -2)
        {
            Debug_printx("consumer exit, r=%d", r);
            break;   /* CLOSE_EMPTY(-2) 或错误 */
        }
        /* r==0 超时空 或 r==-1 Flush空：继续等 */
    }
    return NULL;
}


/* ================================================================== */
/*                                                                    */
/*     Part 2: 零拷贝回调式                                            */
/*                                                                    */
/* ================================================================== */

static unsigned long g_cb_consumed = 0;   /* 回调累计消费字节 */

/**
 * @func         my_consume_cb
 * @brief        零拷贝回调：直接对内部地址写文件
 */
static int my_consume_cb(StreamBufferStatus status, const char *data, int len, void *ctx)
{
    FILE *fp = (FILE *)ctx;
    (void)status;
    if(fp != NULL && len > 0)
    {
        fwrite(data, 1, (size_t)len, fp);   /* 零拷贝：直接 fwrite 内部地址 */
        g_cb_consumed += (unsigned long)len;
    }
    return len;   /* 全消费 */
}


/* ================================================================== */
/*                                                                    */
/*     main                                                          */
/*                                                                    */
/* ================================================================== */

int main(int argc, char **argv)
{
    T_StreamBufferConfig cfg = { 8192, 1024 };   /* 容量8K(2的幂), 阈值1K */
    T_StreamBuffer *sb = NULL;
    T_StreamBufferStats st;
    pthread_t pid, cid;
    int ret;

    (void)argc;
    (void)argv;

    /* ============================================================== */
    /* Part 1: GetData 拷贝式                                          */
    /* ============================================================== */
    Debug_printx("========== Part 1: GetData (copy) Start ==========");

    g_produce_count = 0;
    ret = StreamBufferAPI_Init(&sb, &cfg, "demo1");
    if(ret != 0)
    {
        Debug_printx("Init fail ret=%d", ret);
        return -1;
    }

    g_fp = fopen("demo1_output.csv", "w");
    if(g_fp == NULL)
    {
        Debug_printx("fopen demo1 fail");
        StreamBufferAPI_Destroy(&sb);
        return -1;
    }

    pthread_create(&pid, NULL, producer_thread, sb);
    pthread_create(&cid, NULL, consumer_thread, sb);

    pthread_join(pid, NULL);            /* 等生产者写完 300 条 */
    Debug_printx("producer joined, produced=%d", g_produce_count);

    /* 优雅关闭：阻止写入 + broadcast 唤醒；消费者取空剩余后退出 */
    StreamBufferAPI_Close(sb);
    pthread_join(cid, NULL);
    Debug_printx("consumer joined (drained)");

    fclose(g_fp);
    g_fp = NULL;

    StreamBufferAPI_StatsGet(sb, &st);
    Debug_printx("stats: put=%lu dropped=%lu consumed=%lu peak=%d",
                 st.ulTotalPut, st.ulDropped, st.ulConsumed, st.iPeakUsed);

    StreamBufferAPI_Destroy(&sb);
    Debug_printx("========== Part 1 End ==========");

    /* ============================================================== */
    /* Part 2: 零拷贝回调式                                            */
    /* ============================================================== */
    Debug_printx("========== Part 2: ConsumeCallback (zero-copy) Start ==========");

    g_cb_consumed = 0;
    ret = StreamBufferAPI_Init(&sb, &cfg, "demo2");
    if(ret != 0)
    {
        Debug_printx("Init2 fail ret=%d", ret);
        return -1;
    }

    g_fp = fopen("demo2_output.csv", "w");
    StreamBufferAPI_SetConsumeCallback(sb, my_consume_cb, g_fp);   /* 注册回调 */

    /* 生产者直接在主线程 PutData，消费者用回调（Wait 内自动消费） */
    {
        int i;
        char line[128];
        for(i = 0; i < 100; i++)
        {
            int n = snprintf(line, sizeof(line), "cb=%d\r\n", i);
            StreamBufferAPI_PutData(sb, line, n);
            if(i % 20 == 0)
            {
                /* 周期性 Wait：触发回调消费（阈值/超时） */
                int used;
                StreamBufferAPI_Wait(sb, 50, &used);
            }
            usleep(1 * 1000);
        }
        /* 排空：Close 后 Wait 触发回调消费剩余 */
        StreamBufferAPI_Close(sb);
        {
            int used;
            int r = StreamBufferAPI_Wait(sb, 100, &used);
            Debug_printx("Part2 final Wait r=%d used=%d", r, used);
        }
    }

    if(g_fp)
    {
        fflush(g_fp);
        fclose(g_fp);
        g_fp = NULL;
    }
    Debug_printx("callback consumed=%lu bytes", g_cb_consumed);

    StreamBufferAPI_Destroy(&sb);
    Debug_printx("========== Part 2 End ==========");

    /* ============================================================== */
    /* Part 3: GetDataAddress 零拷贝逐段式                             */
    /* ============================================================== */
    Debug_printx("========== Part 3: GetDataAddress (zero-copy segment) Start ==========");

    ret = StreamBufferAPI_Init(&sb, &cfg, "demo3");
    if(ret != 0)
    {
        Debug_printx("Init3 fail ret=%d", ret);
        return -1;
    }

    /* PutData 跨回绕（容量8K，写入使其回绕），再用 GetDataAddress 逐段取 */
    {
        char big[4096];
        int i;
        char *ptr = NULL;
        int total_got = 0;

        memset(big, 'A', sizeof(big));
        StreamBufferAPI_PutData(sb, big, 4096);   /* used=4096, write=4096 */
        StreamBufferAPI_PutData(sb, big, 4096);   /* used=8192(满), write 回绕到0 */

        /* GetDataAddress 逐段取（回绕时分两次，每次本段地址） */
        for(i = 0; i < 4; i++)
        {
            int n = StreamBufferAPI_GetDataAddress(sb, &ptr, 4096);
            Debug_printx("GetDataAddress seg[%d]: n=%d ptr=%p", i, n, (void *)ptr);
            if(n > 0 && ptr)
            {
                /* 此处可 fwrite(ptr, 1, n, fp) —— 零拷贝 */
                total_got += n;
            }
        }
        Debug_printx("GetDataAddress total got=%d (expect 8192)", total_got);
    }

    StreamBufferAPI_Destroy(&sb);
    Debug_printx("========== Part 3 End ==========");

    /* ============================================================== */
    /* Part 4: Reopen（Close 可逆）                                    */
    /* ============================================================== */
    Debug_printx("========== Part 4: Reopen Start ==========");

    ret = StreamBufferAPI_Init(&sb, &cfg, "demo4");
    if(ret != 0)
    {
        Debug_printx("Init4 fail ret=%d", ret);
        return -1;
    }

    {
        char line[64] = "hello\r\n";
        int r;

        r = StreamBufferAPI_PutData(sb, line, (int)strlen(line));
        Debug_printx("before close: PutData=%d", r);

        StreamBufferAPI_Close(sb);
        r = StreamBufferAPI_PutData(sb, line, (int)strlen(line));
        Debug_printx("after close: PutData=%d (expect -2)", r);

        StreamBufferAPI_Reopen(sb);
        r = StreamBufferAPI_PutData(sb, line, (int)strlen(line));
        Debug_printx("after reopen: PutData=%d (expect >=0)", r);
    }

    StreamBufferAPI_Destroy(&sb);
    Debug_printx("========== Part 4 End ==========");

    /* 校验：非 2 的幂容量应 Init 失败 */
    {
        T_StreamBufferConfig bad = { 5000, 1024 };   /* 5000 非 2 的幂 */
        T_StreamBuffer *badp = NULL;
        ret = StreamBufferAPI_Init(&badp, &bad, "bad");
        Debug_printx("non-power-of-2 Init ret=%d (expect -1)", ret);
        if(ret == 0)
        {
            StreamBufferAPI_Destroy(&badp);
        }
    }

    Debug_printx("Program exit");
    return 0;
}
