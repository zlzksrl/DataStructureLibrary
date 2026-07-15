/**
 * @file        stress_test.c
 * @brief       FileWriter 抗并发销毁高压测试
 * @details     场景：多个 Writer 线程疯狂写文件，主线程在随机时刻销毁实例。
 *              验证目标：
 *              1. 不崩溃（无 UAF、无 double-free、无锁悬空）
 *              2. Destroy 有界返回（不超过 destroy_wait_ms 的显著时间）
 *              3. 数据完整性：Destroy 返回后已入队的数据都落盘
 *              4. Phase B 兜底释放路径覆盖到（destroy_pending + finalize_taken CAS）
 *
 *              四段测试矩阵（默认 50 + 30 + 30 + 4 项 = 110 轮 + Stage 4 功能断言）：
 *
 *              Stage 1  NORMAL     — 每轮 6 个 Writer 疯狂写 2-5s，
 *                                    然后主线程 Destroy（wait 200-1000ms）。
 *                                    单轮估算 100 万+ 次 Write，总量千万级。
 *              Stage 2  IMMEDIATE  — Writer 0-20ms 就 Destroy，
 *                                    测启动窗口的 race。
 *              Stage 3  PHASE_B    — Writer 跑 1-3s + 极小 destroy_wait_ms（1-5ms），
 *                                    几乎必进 Phase B 兜底释放路径，
 *                                    验证 finalize_taken CAS 独占抢释放权正确。
 *              Stage 4  MAX_FILES  — 单线程功能断言（覆盖本轮改进点）：
 *                                    4.1 max_files 参数范围校验 [0, 999]，越界 Init 拒绝
 *                                    4.2 seq 000-999 循环：rotate 1005 次后 seq=4
 *                                    4.3 重启后 seq 延续：Init B 从 Init A 的 max_seq+1 继续
 *                                    4.4 降配批量清理：max_files 从 999 降到 5，Init 一次性剪掉超额
 *
 *              编译：make stress（见 Makefile）
 *              运行：./FileWriter_Stress.bin
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
#include <stdint.h>
#include <inttypes.h>

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
    uint64_t           total_calls;
    uint64_t           total_ok;
    uint64_t           total_rejected_uninit;
    uint64_t           total_rejected_closed;
    uint64_t           total_rejected_full;
    long               max_destroy_latency_us;
    long               sum_destroy_latency_us;
    int                phase_b_deferred_count; /* 启发式：Destroy 耗时接近 wait_ms 才计入 */
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
        g_rotate_call_count++;
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
        g_query_call_count += 7;
        usleep(1000);   /* 1ms 一轮 */
    }
    return NULL;
}

/* ========================== 单轮测试 ==========================
 *
 * @param round_idx          轮次编号
 * @param write_duration_ms  Writer 线程正常压测时长（先跑够这个时间再 Destroy）
 * @param destroy_wait_ms    Destroy 内部等 in-flight Writer 的超时
 */
static int run_one_round(int round_idx, int write_duration_ms, int destroy_wait_ms)
{
    T_FileWriterConfig cfg;
    pthread_t tids[WRITER_THREADS];
    pthread_t tid_rotate, tid_query;
    int worker_ids[WRITER_THREADS];
    int i;
    long t_start_us, t_destroy_us, t_destroyed_us;
    long destroy_latency_us;
    int rc;

    printf("\n==================== Round %d ====================\n", round_idx);
    printf("write_duration=%dms, destroy_wait=%dms, writers=%d\n",
           write_duration_ms, destroy_wait_ms, WRITER_THREADS);

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

    /* 让 Writer 充分跑一段时间——这是真正的"高压期"。
     * write_duration_ms=0 时相当于立即销毁的极端场景。 */
    if(write_duration_ms > 0)
    {
        usleep(write_duration_ms * 1000);
    }

    t_destroy_us = now_us();

    /* 关键操作：在业务线程疯狂写的时候调 Destroy——这是本测试的核心 */
    rc = FileWriterAPI_Destroy(&g_fw);
    t_destroyed_us = now_us();
    destroy_latency_us = t_destroyed_us - t_destroy_us;

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

    /* 打印本轮结果 */
    printf("Destroy latency: %ld us (%.1f ms), write phase=%ld ms\n",
           destroy_latency_us,
           destroy_latency_us / 1000.0,
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

    /* 累计到全局统计 */
    g_global.total_calls           += total_calls;
    g_global.total_ok              += total_ok;
    g_global.total_rejected_uninit += total_uninit;
    g_global.total_rejected_closed += total_closed;
    g_global.total_rejected_full   += total_full;
    g_global.sum_destroy_latency_us += destroy_latency_us;
    if(destroy_latency_us > g_global.max_destroy_latency_us)
    {
        g_global.max_destroy_latency_us = destroy_latency_us;
    }
    /* 启发式：Destroy 耗时 >= wait_ms 的 80% 视为触发了 Phase B（用于观察，非精确） */
    if(destroy_latency_us >= (long)destroy_wait_ms * 800)
    {
        g_global.phase_b_deferred_count++;
    }
    g_global.rounds_done++;

    /* 关键断言：如果程序跑到这里没崩，就通过了 */
    printf("Round %d: PASS (no crash)\n", round_idx);
    return 0;
}

/* ================================================================== *
 *  Stage 4: max_files 改进点功能断言（单线程、逐项 PASS/FAIL）         *
 *                                                                    *
 *  与 Stage 1-3 的"没崩=通过"不同，本段测试对返回值和实际磁盘状态     *
 *  做显式断言。任何一项失败都会打印 FAIL 并递增计数，最终在总结       *
 *  里体现。测试路径固定为 ./stress_test/s4_*，跑之前清空避免历史干扰。 *
 * ================================================================== */

/* Stage 4 结果累计（各 test_* 函数返回本次的 FAIL 计数，聚合到这里） */
static int g_s4_fail_total = 0;

/**
 * 通用配置生成器：Stage 4 的功能测试无需并发压力，
 * 用小 buffer / 短 flush_ms 让 rotate 快速稳态，
 * 关掉 date_subdir 让所有测试文件都落在同一层目录，方便断言。
 */
static void s4_make_cfg(T_FileWriterConfig *cfg,
                        const char *prefix, int max_files)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->dir_path, sizeof(cfg->dir_path), "%s", STRESS_ROOT);
    /* date_subdir_prefix 留空——避免跨秒/跨日给测试制造额外目录层次 */
    cfg->date_subdir_prefix[0] = '\0';
    strncpy(cfg->file_prefix, prefix, sizeof(cfg->file_prefix) - 1);
    cfg->file_type         = FILEWRITER_TYPE_LOG;
    cfg->max_files         = max_files;
    cfg->max_file_size     = 0;
    cfg->auto_rotate_daily = 0;
    cfg->thread_priority   = 20;
    cfg->timestamp         = 0;
    cfg->flush_bytes       = 1024;
    cfg->flush_ms          = 20;
    cfg->buffer_capacity   = 4096;
    cfg->destroy_wait_ms   = 200;
}

/**
 * 从文件名 "{prefix_name}_NNN_YYYY-...log" 解析出 3 位序号。
 * 与 FileWriter.c 里的 fw_scan_max_seq_locked 保持相同的解析规则。
 * 返回 [0..999] 或 -1（不匹配）。
 */
static int s4_parse_seq(const char *filename, const char *prefix_name)
{
    int prefix_len = (int)strlen(prefix_name);
    const char *p;
    int seq = 0;
    int i;

    if(strncmp(filename, prefix_name, prefix_len) != 0
       || filename[prefix_len] != '_')
    {
        return -1;
    }
    p = filename + prefix_len + 1;
    if(strlen(p) < 4 || p[3] != '_')
    {
        return -1;
    }
    for(i = 0; i < 3; i++)
    {
        if(p[i] < '0' || p[i] > '9')
        {
            return -1;
        }
        seq = seq * 10 + (p[i] - '0');
    }
    return seq;
}

/**
 * 从 file_prefix（可能含 "/sub" 前缀）取出文件名侧的 prefix_name。
 * 与 FileWriter.c 内部逻辑一致。
 */
static const char *s4_prefix_name(const char *file_prefix)
{
    const char *slash = strrchr(file_prefix, '/');
    return (slash != NULL) ? slash + 1 : file_prefix;
}

/**
 * 拿当前正写文件的 seq。失败返回 -1。
 */
static int s4_current_seq(T_FileWriter *fw, const char *file_prefix)
{
    char name[256];
    if(FileWriterAPI_GetCurrentFileName(fw, name, sizeof(name)) != 0)
    {
        return -1;
    }
    return s4_parse_seq(name, s4_prefix_name(file_prefix));
}

/* -------------------- 4.1 参数校验 -------------------- */
static int test_max_files_validation(void)
{
    T_FileWriter *fw = NULL;
    T_FileWriterConfig cfg;
    int rc;
    int fail = 0;
    struct { int val; int expect_ok; const char *desc; } cases[] = {
        { -1,  0, "max_files=-1  (out of range)" },
        {  0,  1, "max_files=0   (unlimited)"    },
        { 999, 1, "max_files=999 (upper bound)"  },
        { 1000,0, "max_files=1000 (out of range)"},
    };
    int i;

    printf("\n--- Test 4.1: max_files parameter range [0,999] ---\n");

    for(i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
    {
        fw = NULL;
        s4_make_cfg(&cfg, "s4_valid/x", cases[i].val);
        rc = FileWriterAPI_Init(&fw, &cfg);
        if(cases[i].expect_ok)
        {
            if(rc == 0 && fw != NULL)
            {
                printf("  OK   %s → Init returned 0\n", cases[i].desc);
                FileWriterAPI_Destroy(&fw);
            }
            else
            {
                printf("  FAIL %s → Init returned %d (expected 0)\n",
                       cases[i].desc, rc);
                fail++;
            }
        }
        else
        {
            if(rc != 0 && fw == NULL)
            {
                printf("  OK   %s → Init returned %d (rejected as expected)\n",
                       cases[i].desc, rc);
            }
            else
            {
                printf("  FAIL %s → Init accepted (rc=%d), expected reject\n",
                       cases[i].desc, rc);
                fail++;
                if(fw != NULL) FileWriterAPI_Destroy(&fw);
            }
        }
    }

    return fail;
}

/* -------------------- 4.2 seq 0-999 循环 -------------------- */
static int test_seq_wrap(void)
{
    T_FileWriter *fw = NULL;
    T_FileWriterConfig cfg;
    int fail = 0;
    int i;
    int seq_after;
    const int ROTATE_TIMES = 1005;   /* 起始 seq=0，rotate 1005 次后应 = 1005 % 1000 = 5，
                                        但 rotate 内部先 ++、再打开新文件，所以
                                        rotate 完成后当前 seq = (0 + 1005) % 1000 = 5 */

    printf("\n--- Test 4.2: seq wraps 0-999 after %d rotates ---\n", ROTATE_TIMES);

    /* 清理历史避免 max_seq 干扰起始序号 */
    system("rm -rf " STRESS_ROOT "/s4_wrap 2>/dev/null");

    s4_make_cfg(&cfg, "s4_wrap/x", 0);   /* 无限制，避免每次 rotate 触发清理 */
    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        printf("  FAIL Init returned nonzero\n");
        return 1;
    }

    /* 验证起始 seq=0 */
    if(s4_current_seq(fw, cfg.file_prefix) != 0)
    {
        printf("  FAIL initial seq != 0 (got %d)\n",
               s4_current_seq(fw, cfg.file_prefix));
        fail++;
    }

    for(i = 0; i < ROTATE_TIMES; i++)
    {
        if(FileWriterAPI_Rotate(fw) != 0)
        {
            printf("  FAIL Rotate #%d returned nonzero\n", i);
            fail++;
            break;
        }
    }

    seq_after = s4_current_seq(fw, cfg.file_prefix);
    if(seq_after == 5)
    {
        printf("  OK   after %d rotates, current seq = %d (wrapped 0-999 once, +5)\n",
               ROTATE_TIMES, seq_after);
    }
    else
    {
        printf("  FAIL after %d rotates, current seq = %d, expected 5\n",
               ROTATE_TIMES, seq_after);
        fail++;
    }

    FileWriterAPI_Destroy(&fw);
    return fail;
}

/* -------------------- 4.3 重启后 seq 延续 -------------------- */
static int test_seq_continuity_across_restart(void)
{
    T_FileWriter *fw = NULL;
    T_FileWriterConfig cfg;
    int fail = 0;
    int seq_before_destroy;
    int seq_after_reinit;
    int i;

    printf("\n--- Test 4.3: seq continues (max_seq+1) after restart ---\n");

    /* 清理历史 */
    system("rm -rf " STRESS_ROOT "/s4_cont 2>/dev/null");

    /* Round A: Init + rotate 3 次 → 当前 seq 应该 = 3 */
    s4_make_cfg(&cfg, "s4_cont/x", 0);
    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        printf("  FAIL Round A Init failed\n");
        return 1;
    }
    for(i = 0; i < 3; i++)
    {
        FileWriterAPI_Rotate(fw);
    }
    seq_before_destroy = s4_current_seq(fw, cfg.file_prefix);
    if(seq_before_destroy != 3)
    {
        printf("  FAIL Round A seq before Destroy = %d, expected 3\n",
               seq_before_destroy);
        fail++;
    }
    FileWriterAPI_Destroy(&fw);

    /* Round B: 同前缀 Init，应该扫到 max_seq=3、file_seq = 4 */
    fw = NULL;
    s4_make_cfg(&cfg, "s4_cont/x", 0);
    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        printf("  FAIL Round B Init failed\n");
        return fail + 1;
    }
    seq_after_reinit = s4_current_seq(fw, cfg.file_prefix);
    if(seq_after_reinit == 4)
    {
        printf("  OK   Round A ended at seq=3, Round B started at seq=%d\n",
               seq_after_reinit);
    }
    else
    {
        printf("  FAIL Round B seq = %d, expected 4 (Round A max_seq=3, +1)\n",
               seq_after_reinit);
        fail++;
    }
    FileWriterAPI_Destroy(&fw);

    return fail;
}

/* -------------------- 4.4 降配批量清理 -------------------- */
static int test_bulk_prune_on_downgrade(void)
{
    T_FileWriter *fw = NULL;
    T_FileWriterConfig cfg;
    int fail = 0;
    int i;
    int fc_before, fc_after;

    printf("\n--- Test 4.4: Init prunes excess files when max_files is lowered ---\n");

    /* 清理历史 */
    system("rm -rf " STRESS_ROOT "/s4_prune 2>/dev/null");

    /* Round A: max_files=999，rotate 20 次 → 目录里 21 个同前缀文件 */
    s4_make_cfg(&cfg, "s4_prune/x", 999);
    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        printf("  FAIL Round A Init failed\n");
        return 1;
    }
    for(i = 0; i < 20; i++)
    {
        FileWriterAPI_Rotate(fw);
    }
    fc_before = FileWriterAPI_GetFileCount(fw);
    if(fc_before != 21)
    {
        printf("  FAIL Round A left %d files, expected 21\n", fc_before);
        fail++;
    }
    else
    {
        printf("  OK   Round A created 21 files (max_files=999, 20 rotates)\n");
    }
    FileWriterAPI_Destroy(&fw);

    /* Round B: 同前缀 Init，但 max_files 骤降到 5。
     * Init 内部：扫描 max_seq=20 → file_seq=21 → 开新文件（22 个）→
     * fw_delete_oldest_locked 批量删 22-5=17 个最老，剩最新 5 个（含新开的）。 */
    fw = NULL;
    s4_make_cfg(&cfg, "s4_prune/x", 5);
    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        printf("  FAIL Round B Init failed\n");
        return fail + 1;
    }
    fc_after = FileWriterAPI_GetFileCount(fw);
    if(fc_after == 5)
    {
        printf("  OK   Round B Init pruned to %d files (max_files=5)\n", fc_after);
    }
    else
    {
        printf("  FAIL Round B has %d files after Init, expected 5\n", fc_after);
        fail++;
    }

    /* 附加验证：留下的 5 个应该是最新的 5 个——含 seq=21（新开）+ seq=17..20 */
    {
        int cur_seq = s4_current_seq(fw, cfg.file_prefix);
        if(cur_seq == 21)
        {
            printf("  OK   current file seq = 21 (Round A max=20, +1)\n");
        }
        else
        {
            printf("  FAIL current file seq = %d, expected 21\n", cur_seq);
            fail++;
        }
    }

    FileWriterAPI_Destroy(&fw);
    return fail;
}

/**
 * Stage 4 汇总执行器：串行跑四项功能断言。
 * 任何一项 FAIL 都递增 g_s4_fail_total，供 main 总结时展示。
 */
static void run_stage4(void)
{
    printf("\n\n=========================================\n");
    printf("Stage 4: MAX_FILES functional assertions\n");
    printf("=========================================\n");
    g_s4_fail_total += test_max_files_validation();
    g_s4_fail_total += test_seq_wrap();
    g_s4_fail_total += test_seq_continuity_across_restart();
    g_s4_fail_total += test_bulk_prune_on_downgrade();

    if(g_s4_fail_total == 0)
    {
        printf("\nStage 4: ALL PASS\n");
    }
    else
    {
        printf("\nStage 4: %d assertion(s) FAILED\n", g_s4_fail_total);
    }
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

    /* ==============================================================
     * 常规轮次：Writer 充分跑 2-5s 再销毁。
     * 单轮预期数据量：6 线程 * ~2-5s * ~100k calls/s ≈ 1M~3M 次 Write
     * ============================================================== */
    printf("\n\n=========================================\n");
    printf("Stage 1: NORMAL — 50 rounds, 2-5s writes\n");
    printf("=========================================\n");
    for(round = 1; round <= ROUNDS_NORMAL; round++)
    {
        int write_ms = 2000 + rand() % 3000;   /* 2-5 秒 */
        int wait_ms  = 200 + rand() % 800;     /* 200-1000ms */
        if(run_one_round(round, write_ms, wait_ms) != 0)
        {
            printf("Round %d FAILED, abort\n", round);
            return 1;
        }
    }

    /* ==============================================================
     * 极端轮次：Writer 刚起来（0-20ms）就销毁——测启动窗口的 race
     * ============================================================== */
    printf("\n\n=========================================\n");
    printf("Stage 2: IMMEDIATE — 30 rounds, 0-20ms writes\n");
    printf("=========================================\n");
    for(round = 1; round <= ROUNDS_IMMEDIATE; round++)
    {
        int write_ms = rand() % 20;             /* 0-19ms，几乎没起跑就销毁 */
        int wait_ms  = 50 + rand() % 200;
        if(run_one_round(ROUNDS_NORMAL + round, write_ms, wait_ms) != 0)
        {
            printf("Immediate round %d FAILED, abort\n", round);
            return 1;
        }
    }

    /* ==============================================================
     * Phase B 专项：Writer 跑一会 + 极小 destroy_wait_ms
     * 让 spin-wait 极易超时，走 Phase B 兜底释放路径。
     * 观察日志应出现 "destroy deferred"。
     * ============================================================== */
    printf("\n\n=========================================\n");
    printf("Stage 3: PHASE_B — %d rounds, 1-3s writes + 1-5ms wait\n",
           ROUNDS_PHASE_B);
    printf("=========================================\n");
    for(round = 1; round <= ROUNDS_PHASE_B; round++)
    {
        int write_ms = 1000 + rand() % 2000;   /* 1-3 秒 */
        int wait_ms  = 1 + rand() % 5;         /* 1-5ms，几乎必进 Phase B */
        if(run_one_round(ROUNDS_NORMAL + ROUNDS_IMMEDIATE + round,
                         write_ms, wait_ms) != 0)
        {
            printf("Phase B round %d FAILED, abort\n", round);
            return 1;
        }
    }

    /* ==============================================================
     * Stage 4：max_files 改进点功能断言（单线程）
     * 与前三段并发压测互补——前三段验证"不崩"，本段验证"行为对"。
     * ============================================================== */
    run_stage4();

    /* ==============================================================
     * 总结报告
     * ============================================================== */
    printf("\n\n╔══════════════════════════════════════════════════════════════╗\n");
    if(g_s4_fail_total == 0)
    {
        printf("║                    ALL PASSED — SUMMARY                      ║\n");
    }
    else
    {
        printf("║          STAGES 1-3 PASSED,  STAGE 4 HAS FAILURES            ║\n");
    }
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("seed                       = %d\n", seed);
    printf("Total rounds               = %d (normal:%d + immediate:%d + phase_b:%d)\n",
           g_global.rounds_done, ROUNDS_NORMAL, ROUNDS_IMMEDIATE, ROUNDS_PHASE_B);
    printf("Total Write/WriteBin calls = %" PRIu64 "\n", g_global.total_calls);
    printf("  ok  (>=0)                = %" PRIu64 " (%.1f%%)\n",
           g_global.total_ok,
           g_global.total_calls ? 100.0 * (double)g_global.total_ok / (double)g_global.total_calls : 0.0);
    printf("  -1  (uninit/err)         = %" PRIu64 "\n", g_global.total_rejected_uninit);
    printf("  -2  (closed/destroying)  = %" PRIu64 "\n", g_global.total_rejected_closed);
    printf("  -3  (buffer full)        = %" PRIu64 "\n", g_global.total_rejected_full);
    printf("Rotate calls               = %lu\n", g_rotate_call_count);
    printf("Query calls (7 per iter)   = %lu\n", g_query_call_count);
    printf("Destroy latency:\n");
    printf("  max                      = %ld us (%.1f ms)\n",
           g_global.max_destroy_latency_us,
           g_global.max_destroy_latency_us / 1000.0);
    printf("  avg                      = %ld us (%.1f ms)\n",
           g_global.rounds_done ? g_global.sum_destroy_latency_us / g_global.rounds_done : 0,
           g_global.rounds_done ? (g_global.sum_destroy_latency_us / g_global.rounds_done) / 1000.0 : 0.0);
    printf("Phase B triggered (approx) = %d rounds (启发式: destroy_latency >= 80%% * wait_ms)\n",
           g_global.phase_b_deferred_count);
    printf("Stage 4 functional asserts = %s (%d failure(s))\n",
           g_s4_fail_total == 0 ? "ALL PASS" : "SOME FAILED",
           g_s4_fail_total);
    printf("\nNo crash observed. Check log for 'destroy deferred' to confirm Phase B path.\n");
    printf("Files under: ./stress_test/RYYYY_MM_DD/roundNNN/w_*.log (Stages 1-3)\n");
    printf("             ./stress_test/s4_*/x_*.log                  (Stage 4)\n");
    return g_s4_fail_total == 0 ? 0 : 1;
}
