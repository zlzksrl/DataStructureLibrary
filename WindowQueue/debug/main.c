/**
 * @file        main.c
 * @brief       WindowQueue 滑动窗口队列 - 测试/演示程序
 * @details     本程序演示如何用 WindowQueue 实现传感器数据采集 + 滑动窗口滤波：
 *
 *              Part 1: 中值滤波（ForEach 累积法）★
 *              回答核心问题："ForEach 每次回调只给 1 条 data，怎么做中值滤波？"
 *              答：用 user_ctx 指向一个数值数组，回调里把每条数据的数值存进
 *              vals[index]，ForEach 返回后即得到整个窗口的数值数组，再排序取中值。
 *              该方式在互斥锁内零拷贝收集，不额外分配内存。
 *
 *              Part 2: 移动平均（Snapshot 法）
 *              用 Snapshot 把最新 N 条结构体拷贝出来，在锁外提取字段求平均。
 *              适合需要锁外做较重处理的场景。
 *
 *              Part 3: 采集→处理 解耦架构
 *              采集线程高频 Put（永不阻塞），处理线程按自己的周期滤波，
 *              两者通过队列解耦。演示中值滤波对随机尖刺噪声的抑制效果。
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
#include <math.h>
#include <pthread.h>

#include "../include/WindowQueue.h"


/* ========================== 调试宏 ========================== */

#if 1
/**
 * @def   Debug_printx
 * @brief 调试打印宏，输出格式: [Debug]-[#####]-[用户信息##@line:[行号]@func:[函数名]]
 *        将 #if 1 改为 #if 0 可关闭所有调试输出
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


/* ========================== 数据定义 ========================== */

/** 窗口容量（滤波窗口大小） */
#define WIN_CAPACITY      8

/** 采集基准值（真值），采集数据在其附近加噪声/尖刺 */
#define BASE_VALUE        10.0f

/** 模拟传感器数据结构体 */
typedef struct
{
    int   ts;        /**< 时间戳（采样序号） */
    float value;     /**< 传感器读数（待滤波字段） */
} Sensor;


/* ========================== 基础算法实现 ========================== */
/*                                                                     */
/*  以下为纯数值算法，操作 double 数组，与队列解耦。                     */
/*  处理线程通过 ForEach/Snapshot 取出窗口数值后调用这些函数。           */
/*                                                                     */
/* ================================================================== */

/**
 * @func         cmp_double
 * @brief        qsort 的 double 比较函数
 */
static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);   /* 返回 -1/0/1，避免直接相减的精度溢出 */
}

/**
 * @func         median_double
 * @brief        中值滤波：对数组排序后取中值
 * @details      注意：本函数会原地排序（破坏 a[] 原顺序）。
 *               调用者若需保留原序，应先拷贝一份再传入。
 *               对 ForEach 收集的临时 scratch 数组排序无副作用。
 * @param[in,out] a:  数组（会被排序）
 * @param[in]     n:  元素个数
 * @return        中值；n<=0 时返回 0
 */
static double median_double(double *a, int n)
{
    if(n <= 0)
    {
        return 0.0;
    }
    qsort(a, (size_t)n, sizeof(double), cmp_double);
    return a[n / 2];
}

/**
 * @func         mean_double
 * @brief        移动平均：算术平均（不修改输入数组）
 * @param[in]    a:  数组
 * @param[in]    n:  元素个数
 * @return       平均值；n<=0 时返回 0
 */
static double mean_double(const double *a, int n)
{
    if(n <= 0)
    {
        return 0.0;
    }
    double sum = 0.0;
    int i;
    for(i = 0; i < n; i++)
    {
        sum += a[i];
    }
    return sum / n;
}


/* ================================================================== */
/*                                                                    */
/*     Part 1: 中值滤波（ForEach 累积法）★                            */
/*                                                                    */
/*  核心思想：                                                         */
/*    ForEach 对窗口每条数据调一次回调，回调每次只拿到 1 条 data。      */
/*    用 user_ctx 指向一个"累积器"，回调里把数值存进 vals[index]，       */
/*    ForEach 返回后 vals[0..n-1] 即整个窗口的数值（老→新）。           */
/*                                                                    */
/* ================================================================== */

/**
 * @struct ValCollector
 * @brief 数值累积器，作为 ForEach 的 user_ctx
 */
typedef struct
{
    double *vals;   /**< 数值数组（容量 >= 窗口容量） */
    int     cnt;    /*  实际收集到的条数（ForEach 后 = 窗口条数 n） */
} ValCollector;

/**
 * @func         collect_value
 * @brief        ForEach 回调：取出本条数据的 value 字段存入累积器
 * @details      在互斥锁内执行（零拷贝，data 指向队列内部条目）。
 *               注意：回调内禁止调用本队列任何 API。
 */
static void collect_value(const void *data, int index, void *ctx)
{
    ValCollector *c = (ValCollector *)ctx;
    c->vals[index] = ((const Sensor *)data)->value;
    c->cnt = index + 1;   /* 最后一次 index = n-1，故 cnt = n */
}

/**
 * @func         median_filter_foreach
 * @brief        用 ForEach 累积法做中值滤波
 * @details      ForEach 锁内零拷贝收集窗口数值到 scratch，返回后排序取中值，
 *               同时附带返回最新一条原始值（vals[cnt-1]，排序前）。
 * @param[in]    q:        队列句柄
 * @param[out]   scratch:  临时数组（容量 >= 窗口容量），会被排序
 * @param[out]   p_latest: 输出最新一条原始值（可为 NULL）
 * @param[out]   p_n:      输出本次参与滤波的条数（可为 NULL）
 * @return       中值；窗口为空时返回 NAN
 */
static double median_filter_foreach(T_WindowQueueMsg *q, double *scratch,
                                    double *p_latest, int *p_n)
{
    ValCollector c = { scratch, 0 };
    int n = WindowQueueAPI_ForEach(q, collect_value, &c);   /* 锁内收集 */
    if(n <= 0)
    {
        if(p_n)      *p_n = 0;
        if(p_latest) *p_latest = NAN;
        return NAN;
    }
    double latest = scratch[n - 1];   /* 排序前先取最新原始值 */
    double med = median_double(scratch, n);   /* 排序取中值 */
    if(p_latest) *p_latest = latest;
    if(p_n)      *p_n = n;
    return med;
}


/* ================================================================== */
/*                                                                    */
/*     Part 2: 移动平均（Snapshot 法）                                 */
/*                                                                    */
/*  用 Snapshot 把最新 N 条结构体拷贝到本地数组（锁内拷贝，锁外处理），   */
/*  再提取数值求平均。适合需要在锁外做较重处理的场景。                    */
/*                                                                    */
/* ================================================================== */

/**
 * @func         mean_filter_snapshot
 * @brief        用 Snapshot 法做移动平均
 * @param[in]    q:       队列句柄
 * @param[out]   p_buf:   临时结构体数组（容量 >= WIN_CAPACITY）
 * @param[out]   p_n:     输出本次参与平均的条数（可为 NULL）
 * @return       平均值；窗口为空时返回 NAN
 */
static double mean_filter_snapshot(T_WindowQueueMsg *q, Sensor *p_buf, int *p_n)
{
    int n = WindowQueueAPI_Snapshot(q, p_buf, WIN_CAPACITY);   /* 拷贝最新N条 */
    if(n <= 0)
    {
        if(p_n) *p_n = 0;
        return NAN;
    }
    /* 提取 value 字段到 double 数组，调用通用均值算法 */
    double vals[WIN_CAPACITY];
    int i;
    for(i = 0; i < n; i++)
    {
        vals[i] = p_buf[i].value;
    }
    if(p_n) *p_n = n;
    return mean_double(vals, n);
}


/* ================================================================== */
/*                                                                    */
/*     Part 3: 采集→处理 解耦架构                                       */
/*                                                                    */
/* ================================================================== */

/** 运行标志，0=通知线程退出 */
static volatile int g_running = 1;

/** Push 入队回调实时计算的窗口均值/条数（与 Pull 的 ForEach 结果对比验证） */
static volatile double g_push_mean  = 0.0;
static volatile int    g_push_count = 0;

/**
 * @func         put_cb
 * @brief        入队回调：每次 Put 后锁内零拷贝算窗口均值（轻量 O(n) 运算）
 * @details      演示 Push 模型：每条数据入队即更新窗口均值，无需处理线程干预。
 *               约束：锁内执行、快速返回、禁调本队列 API、entries 仅回调内有效。
 *               entries[i] 指向第 i 条数据（老->新），直接读队列内部，零拷贝。
 */
static void put_cb(const void * const *entries, int count, void *ctx)
{
    (void)ctx;
    if(count <= 0)
    {
        g_push_mean  = 0.0;
        g_push_count = 0;
        return;
    }
    double sum = 0.0;
    int i;
    for(i = 0; i < count; i++)
    {
        sum += ((const Sensor *)entries[i])->value;   /* 零拷贝读队列内部 */
    }
    g_push_mean  = sum / count;   /* 简单均值，O(count) */
    g_push_count = count;
}

/**
 * @func         acq_thread
 * @brief        采集线程：模拟传感器，高频 Put（永不阻塞）
 * @details      value = BASE + 高斯噪声，并随机插入尖刺（脉冲噪声），
 *               用于观察中值滤波对尖刺的抑制效果。
 */
static void *acq_thread(void *arg)
{
    T_WindowQueueMsg *q = (T_WindowQueueMsg *)arg;
    int ts = 0;

    while(g_running)
    {
        Sensor s;
        s.ts = ts++;
        /* 伪高斯噪声：两个均匀随机数相加再减均值，近似三角分布 */
        double noise = ((double)(rand() % 1000) / 1000.0 - 0.5)
                     + ((double)(rand() % 1000) / 1000.0 - 0.5);
        s.value = BASE_VALUE + (float)noise;   /* 真值附近抖动 */
        /* 随机尖刺（约 5% 概率），模拟脉冲噪声 */
        if((rand() % 100) < 5)
        {
            s.value += 20.0f;
        }

        int ret = WindowQueueAPI_Put(q, &s);
        if(ret == -2)   /* 队列已关闭，退出 */
        {
            Debug_printx("acq: queue closed, exit");
            break;
        }
        usleep(10 * 1000);   /* 10ms 采集周期 = 100Hz */
    }
    return NULL;
}

/**
 * @func         proc_thread
 * @brief        处理线程：按自己的周期做滤波，与采集频率解耦
 * @details      每 100ms 做一次：
 *               - ForEach 法取中值（演示 Part 1）
 *               - Snapshot 法取均值（演示 Part 2）
 *               打印 窗口条数 / 最新原始值 / 中值 / 均值，观察中值对尖刺的抑制。
 */
static void *proc_thread(void *arg)
{
    T_WindowQueueMsg *q = (T_WindowQueueMsg *)arg;
    double scratch[WIN_CAPACITY];     /* ForEach 累积用临时数组 */
    Sensor  snap_buf[WIN_CAPACITY];   /* Snapshot 用临时数组 */

    while(g_running)
    {
        double latest = NAN;
        int n = 0;

        /* Part 1: 中值滤波（ForEach 累积法） */
        double med = median_filter_foreach(q, scratch, &latest, &n);

        /* Part 2: 移动平均（Snapshot 法） */
        int n2 = 0;
        double mean = mean_filter_snapshot(q, snap_buf, &n2);

        if(n > 0)
        {
            Debug_printx("window=%d latest=%6.3f median=%6.3f mean(pull)=%6.3f mean(push)=%6.3f",
                         n, latest, med, mean, g_push_mean);
        }
        usleep(100 * 1000);   /* 100ms 处理周期 = 10Hz（与采集100Hz解耦） */
    }
    return NULL;
}


/* ================================================================== */
/*                                                                    */
/*     main                                                           */
/*                                                                    */
/* ================================================================== */

/**
 * @func         main
 * @brief        演示入口
 */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int ret;
    T_WindowQueueMsg *q = NULL;
    pthread_t tid_acq, tid_proc;

    /* 步骤1: 初始化队列：容量 WIN_CAPACITY，元素大小 = sizeof(Sensor) */
    ret = WindowQueueAPI_Init(&q, WIN_CAPACITY, (int)sizeof(Sensor), "sensor");
    if(ret != 0)
    {
        Debug_printx("Init fail ret=%d", ret);
        return -1;
    }
    Debug_printx("Init OK, capacity=%d elemsize=%d", WIN_CAPACITY, (int)sizeof(Sensor));

    /* 注册入队回调（Push 轻量处理）：每次 Put 后零拷贝算窗口均值 */
    WindowQueueAPI_SetPutCallback(q, put_cb, NULL);
    Debug_printx("SetPutCallback OK (push mean)");

    /* 步骤2: 启动采集线程与处理线程 */
    pthread_create(&tid_acq,  NULL, acq_thread,  q);
    pthread_create(&tid_proc, NULL, proc_thread, q);

    /* 步骤3: 运行 3 秒 */
    sleep(3);
    g_running = 0;   /* 通知线程退出 */

    /* 步骤4: 关闭队列（阻止新 Put），等待线程退出 */
    WindowQueueAPI_Close(q);
    pthread_join(tid_acq,  NULL);
    pthread_join(tid_proc, NULL);
    Debug_printx("threads joined");

    /* 步骤5: 打印运行统计 */
    T_WindowQueueStats stats;
    if(WindowQueueAPI_StatsGet(q, &stats) == 0)
    {
        Debug_printx("stats: totalPut=%lu discarded=%lu peakLen=%d",
                     stats.ulTotalPut, stats.ulTotalDiscarded, stats.iPeakLength);
    }

    /* 步骤6: Flush 残留窗口数据并销毁 */
    WindowQueueAPI_Flush(q, NULL, NULL);   /* 无需处理残留（值拷贝），传NULL即可 */
    WindowQueueAPI_Destroy(&q);
    Debug_printx("Destroy OK, program exit");
    return 0;
}
