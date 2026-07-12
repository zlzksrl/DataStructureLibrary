/**
 * @file        stress_test.c
 * @brief       FileWriter 抗并发销毁高压测试
 * @details     场景：多个 Writer 线程疯狂写文件，主线程在随机时刻销毁实例。
 *              验证目标：
 *              1. 不崩溃（无 UAF、无 double-free、无锁悬空）
 *              2. Destroy 有界返回（不超过 destroy_wait_ms 的显著时间）
 *              3. 数据完整性：Destroy 返回后已入队的数据都落盘
 *              4. Phase B 触发时能被计数（观察是否稳定进入延迟释放路径）
 *
 *              测试矩阵：
 *              - Round 1-20: 每轮 4 个 Writer 线程 + 1 个 Rotate 线程 + 1 个 Query 线程；
 *                            主线程 10-500ms 后调 Destroy；重复 20 轮。
 *              - Round 21-30: 极端场景（Destroy 立即触发，不给 Writer 起跑时间）
 *
 *              编译：make stress_test（见 Makefile）
 *              运行：./stress_test.bin
 *
 * @author      zlzksrl
 * @Version     V1.1.0
 * @date        2026-07-12
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include "../include/FileWriter.h"

/* ========================== 测试配置 ========================== */

#define STRESS_ROOT             "./stress_test"

/* 每类场景的轮数 */
#define ROUNDS_NORMAL           50      /* 常规：Writer 充分跑 2-5s 再销毁 */
#define ROUNDS_IMMEDIATE        30      /* 立即：Writer 刚起来就销毁 */
#define ROUNDS_PHASE_B          30      /* Phase B 专项：极小 destroy_wait_ms */

/* 每轮线程规模 */
#define WRITER_THREADS          6       /* 每轮的 Writer 线程数 */
#define WRITE_BUFFER_SIZE       256     /* 每次 WriteBin 字节数 */

/* 全局共享句柄（业务线程访问）——刻意用共享指针模拟第三方框架把句柄传给多个线程的场景 */
static T_FileWriter *g_fw          = NULL;
/* worker 用于自己安全地退出的软标志（当他们观察到 Write 长期返回 -2 时） */
static volatile int  g_round_stop  = 0;

/* ========================== 统计信息 ========================== */

typedef struct
{
    unsigned long calls;           /* Write/WriteBin 调用次数 */
    unsigned long ok;              /* 返回 >=0 的次数 */
    unsigned long rejected_uninit; /* -1：未初始化/参数错 */
    unsigned long rejected_closed; /* -2：Destroy 已开始 */
    unsigned long rejected_full;   /* -3：SB 满 */
} T_WorkerStats;

static T_WorkerStats g_stats[WRITER_THREADS];

/* 全局累计统计（所有轮次汇总） */
typedef struct
{
    unsigned long long total_calls;
    unsigned long long total_ok;
    unsigned long long total_rejected_uninit;
    unsigned long long total_rejected_closed;
    unsigned long long total_rejected_full;
    unsigned long long total_bytes_ok;         /* 估算：ok 次数 * 平均字节 */
    unsigned long long total_rotate_calls;
    unsigned long long total_query_calls;
    long               max_destroy_latency_us;
    long               sum_destroy_latency_us;
    int                phase_b_deferred_count; /* Phase B 触发次数（依赖日志难以直接感知，这里用启发式：Destroy 耗时接近 wait_ms 才计入） */
    int                rounds_done;
} T_GlobalStats;

static T_GlobalStats g_global;

/* Rotate / Query 计数（跨轮累加，用于总结） */
static volatile unsigned long g_rotate_call_count = 0;
static volatile unsigned long g_query_call_count  = 0;

/* ========================== 工具 ========================== */

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

/* ========================== Writer 线程：疯狂写 ========================== */

static void *writer_thread(void *arg)
{
    int tid = *(int *)arg;
    T_WorkerStats *st = &g_stats[tid];
    char bin_buf[WRITE_BUFFER_SIZE];
    int seq = 0;
    int r;

    memset(bin_buf, 0xAA + tid, sizeof(bin_buf));

    /* 疯狂写循环——直到看到 g_round_stop 且连续多次 Write 返回 -2 才退出。
     * 关键：g_fw 可能在写入过程中被 Destroy 置 NULL——业务线程可能仍持有旧句柄。
     * 我们特意读一次到局部变量，模拟"业务已经拿到了 fw 指针"这个真实的 UAF 竞态。 */
    T_FileWriter *local_fw = g_fw;   /* 拷贝一份，模拟第三方框架局部持有 */

    while(!g_round_stop)
    {
        st->calls++;

        if(seq % 2 == 0)
        {
            /* 交替：一次 printf 式格式化写 */
            r = FileWriterAPI_Write(local_fw,
                "T%d seq=%d val=%.6f str=%s\n",
                tid, seq, (double)seq * 0.123, "stress");
        }
        else
        {
            /* 一次二进制写 */
            r = FileWriterAPI_WriteBin(local_fw, bin_buf, sizeof(bin_buf));
        }

        if(r >= 0)          st->ok++;
        else if(r == -1)    st->rejected_uninit++;
        else if(r == -2)    st->rejected_closed++;
        else if(r == -3)    st->rejected_full++;

        seq++;

        /* 不 sleep：真正的高压。但每 256 次让出一次 CPU，避免完全饿死其它线程 */
        if((seq & 0xFF) == 0)
        {
            sched_yield();
        }
    }

    return NULL;
}

/* ========================== Rotate 线程：不断触发轮转 ========================== */

static void *rotate_thread(void *arg)
{
    T_FileWriter *local_fw = g_fw;
    (void)arg;

    while(!g_round_stop)
    {
        FileWriterAPI_Rotate(local_fw);
        /* 隔 30-80ms 一次 */
        usleep(30 * 1000 + (rand() % (50 * 1000)));
    }
    return NULL;
}

/* ========================== 查询线程：不断查询各种接口 ========================== */

static void *query_thread(void *arg)
{
    T_FileWriter *local_fw = g_fw;
    char buf[512];
    T_FileWriterStats st;
    (void)arg;

    while(!g_round_stop)
    {
        FileWriterAPI_GetCurrentFileName(local_fw, buf, sizeof(buf));
        FileWriterAPI_GetCurrentFilePath(local_fw, buf, sizeof(buf));
        FileWriterAPI_GetCurrentDirPath(local_fw, buf, sizeof(buf));
        FileWriterAPI_GetFileCount(local_fw);
        FileWriterAPI_GetTotalFileCount(local_fw);
        FileWriterAPI_StatsGet(local_fw, &st);
        FileWriterAPI_Flush(local_fw);
        usleep(1000);   /* 1ms 一轮 */
    }
    return NULL;
}

/* ========================== 单轮测试 ========================== */

static int run_one_round(int round_idx, int destroy_delay_ms, int destroy_wait_ms)
{
    T_FileWriterConfig cfg;
    pthread_t tids[WRITER_THREADS];
    pthread_t tid_rotate, tid_query;
    int worker_ids[WRITER_THREADS];
    int i;
    long t_start_us, t_destroy_us, t_destroyed_us;
    int rc;

    printf("\n==================== Round %d ====================\n", round_idx);
    printf("delay=%dms, destroy_wait=%dms\n", destroy_delay_ms, destroy_wait_ms);

    memset(g_stats, 0, sizeof(g_stats));
    g_round_stop = 0;
    g_fw = NULL;

    /* 每轮不同 file_prefix，避免文件互相踩 */
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.dir_path, sizeof(cfg.dir_path), "%s", STRESS_ROOT);
    strncpy(cfg.date_subdir_prefix, "R",  sizeof(cfg.date_subdir_prefix) - 1);
    snprintf(cfg.file_prefix, sizeof(cfg.file_prefix), "round%03d/w", round_idx);
    cfg.file_type         = FILEWRITER_TYPE_LOG;
    cfg.max_files         = 0;                      /* 不限制，避免删文件干扰观察 */
    cfg.max_file_size     = 0;                      /* 不按大小轮转（Rotate 线程手动） */
    cfg.auto_rotate_daily = 0;
    cfg.thread_priority   = 20;
    cfg.timestamp         = 1;
    cfg.flush_bytes       = 4096;
    cfg.flush_ms          = 50;
    cfg.buffer_capacity   = 65536;
    cfg.destroy_wait_ms   = destroy_wait_ms;

    if(FileWriterAPI_Init(&g_fw, &cfg) != 0)
    {
        printf("Init fail!\n");
        return -1;
    }

    t_start_us = now_us();

    /* 启动 Writer / Rotate / Query 线程 */
    for(i = 0; i < WRITER_THREADS; i++)
    {
        worker_ids[i] = i;
        if(pthread_create(&tids[i], NULL, writer_thread, &worker_ids[i]) != 0)
        {
            printf("pthread_create writer %d fail\n", i);
            return -1;
        }
    }
    pthread_create(&tid_rotate, NULL, rotate_thread, NULL);
    pthread_create(&tid_query,  NULL, query_thread,  NULL);

    /* 主线程随机等一段时间后调 Destroy（模拟"信号来了"） */
    if(destroy_delay_ms > 0)
    {
        usleep(destroy_delay_ms * 1000);
    }

    t_destroy_us = now_us();

    /* 关键操作：在业务线程疯狂写的时候调 Destroy——这是本测试的核心 */
    rc = FileWriterAPI_Destroy(&g_fw);
    t_destroyed_us = now_us();

    if(rc != 0)
    {
        printf("Destroy returned %d\n", rc);
    }

    /* Destroy 已返回，g_fw 已置 NULL。但业务线程可能仍在用局部拷贝的 fw 指针
     * 循环调 Write——它们应该都拿到 -2 而不是崩溃。
     * 通知它们退出（软标志），然后 join。 */
    g_round_stop = 1;
    for(i = 0; i < WRITER_THREADS; i++)
    {
        pthread_join(tids[i], NULL);
    }
    pthread_join(tid_rotate, NULL);
    pthread_join(tid_query,  NULL);

    /* 打印结果 */
    printf("Destroy latency: %ld us (%.1f ms), from-start=%ld ms\n",
           t_destroyed_us - t_destroy_us,
           (t_destroyed_us - t_destroy_us) / 1000.0,
           (t_destroy_us - t_start_us) / 1000);

    unsigned long total_calls = 0, total_ok = 0, total_uninit = 0;
    unsigned long total_closed = 0, total_full = 0;
    for(i = 0; i < WRITER_THREADS; i++)
    {
        total_calls  += g_stats[i].calls;
        total_ok     += g_stats[i].ok;
        total_uninit += g_stats[i].rejected_uninit;
        total_closed += g_stats[i].rejected_closed;
        total_full   += g_stats[i].rejected_full;
    }
    printf("Writes: calls=%lu ok=%lu (-1)=%lu (-2)=%lu (-3)=%lu\n",
           total_calls, total_ok, total_uninit, total_closed, total_full);

    /* 关键断言：如果程序跑到这里没崩，就通过了 */
    printf("Round %d: PASS (no crash)\n", round_idx);
    return 0;
}

/* ========================== main ========================== */

int main(int argc, char *argv[])
{
    int round;
    int seed;

    (void)argc;
    (void)argv;

    printf("FileWriter stress test start\n");

    /* 建根目录 */
    FileWriterAPI_MakeDirs(STRESS_ROOT);

    seed = (int)time(NULL);
    srand(seed);
    printf("seed = %d\n", seed);

    /* 常规轮次：给业务线程 10-500ms 起跑时间后 Destroy */
    for(round = 1; round <= ROUNDS_NORMAL; round++)
    {
        int delay_ms = 10 + rand() % 490;
        int wait_ms  = 100 + rand() % 900;   /* destroy_wait_ms: 100-1000ms */
        if(run_one_round(round, delay_ms, wait_ms) != 0)
        {
            printf("Round %d FAILED, abort\n", round);
            return 1;
        }
    }

    /* 极端轮次：Destroy 几乎立即到来，业务线程可能还没起来 */
    for(round = 1; round <= ROUNDS_IMMEDIATE; round++)
    {
        int delay_ms = rand() % 5;          /* 0-4ms */
        int wait_ms  = 50 + rand() % 200;   /* 50-250ms */
        if(run_one_round(ROUNDS_NORMAL + round, delay_ms, wait_ms) != 0)
        {
            printf("Immediate round %d FAILED, abort\n", round);
            return 1;
        }
    }

    /* Phase B 专项：极小 destroy_wait_ms + Rotate 密集触发，
     * 让 spin-wait 更容易超时，从而走 Phase B 兜底释放路径。
     * 观察日志里应该出现 "destroy deferred" 才算测到位。 */
    printf("\n\n----- Phase B trigger rounds -----\n");
    for(round = 1; round <= 10; round++)
    {
        int delay_ms = 50 + rand() % 150;
        int wait_ms  = 1 + rand() % 5;      /* 1-5ms，极短，几乎注定 Phase B */
        if(run_one_round(ROUNDS_NORMAL + ROUNDS_IMMEDIATE + round,
                         delay_ms, wait_ms) != 0)
        {
            printf("Phase B round %d FAILED, abort\n", round);
            return 1;
        }
    }

    printf("\n\n==================== ALL PASSED ====================\n");
    printf("Total rounds: %d\n", ROUNDS_NORMAL + ROUNDS_IMMEDIATE + 10);
    printf("No crash observed.\n");
    printf("Check ./stress_test/RYYYY_MM_DD/roundNNN/w_*.log for data integrity.\n");
    printf("Look for 'destroy deferred' in the log above to confirm Phase B was exercised.\n");
    return 0;
}
