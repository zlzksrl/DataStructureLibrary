/**
 * @file        main.c
 * @brief       FileWriter 异步文件写入 - 测试/演示程序
 * @details     覆盖需求文档"典型应用"的四类场景：
 *
 *              Part 1: 异步日志 (LOG) —— printf 式格式化 + 时间戳前缀
 *              Part 2: CSV 记录     —— 表头 + 多列数据（无时间戳）
 *              Part 3: 二进制 BIN   —— WriteBin 原样入队（无时间戳、无格式化）
 *              Part 4: 多实例       —— 一个进程 Init 多个 FileWriter，独立线程写盘
 *              Part 5: 手动轮转     —— Rotate() 生成新文件，max_files 限制生效
 *              Part 6: 查询接口     —— GetCurrentFileName/Path/DirPath/FileCount
 *              Part 7: 工具函数     —— GetTimeString / MakeDirs
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-11
 * @copyright   copyright (C) 2026
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include "../include/FileWriter.h"


/* ========================== 调试宏 ========================== */

#if 1
#define Debug_printx(format,...)\
                do\
                {\
                    printf("[Debug]-[#####]-["format"##@line:[%d]@func:[%s]]\n",##__VA_ARGS__,__LINE__,__FUNCTION__);\
                }while(0)
#else
#define Debug_printx(format,...)  do{}while(0)
#endif


/* ========================== 测试根目录 ========================== */
/* 现场跑起来会在此目录下创建结构：
 *   ./fw_test/
 *     ├── log/X2026_07_11/                  (Part 1 log)
 *     │   └── demo_log_000_...log
 *     ├── log/X2026_07_11/sensor/           (Part 2 csv)
 *     │   └── sensor1_000_...csv
 *     ├── bin/                              (Part 3 bin, 无日期子目录)
 *     │   └── frame_000_...bin
 *     └── multi/X2026_07_11/                (Part 4 多实例)
 *         ├── inst_a_000_...log
 *         └── inst_b_000_...log
 */
#define TEST_ROOT   "./fw_test"


/* ================================================================== */
/*                                                                    */
/*     Part 1: 异步日志 LOG                                            */
/*                                                                    */
/* ================================================================== */

static void demo_log(void)
{
    T_FileWriter *fw = NULL;
    T_FileWriterConfig cfg;
    int i;
    char name[128];
    char path[512];

    Debug_printx("======== Part 1: LOG ========");

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.dir_path, sizeof(cfg.dir_path), "%s/log", TEST_ROOT);
    strncpy(cfg.date_subdir_prefix, "X", sizeof(cfg.date_subdir_prefix) - 1);
    strncpy(cfg.file_prefix,        "demo_log", sizeof(cfg.file_prefix) - 1);
    cfg.file_type         = FILEWRITER_TYPE_LOG;
    cfg.max_files         = 5;
    cfg.max_file_size     = 0;              /* 不按大小轮转 */
    cfg.auto_rotate_daily = 1;
    cfg.thread_priority   = 20;
    cfg.timestamp         = 1;              /* 加时间戳前缀 */
    cfg.flush_bytes       = 512;
    cfg.flush_ms          = 100;
    cfg.buffer_capacity   = 65536;

    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        Debug_printx("Init fail");
        return;
    }

    FileWriterAPI_GetCurrentFileName(fw, name, sizeof(name));
    FileWriterAPI_GetCurrentFilePath(fw, path, sizeof(path));
    Debug_printx("current file = [%s]", name);
    Debug_printx("current path = [%s]", path);

    /* 写 20 条日志 */
    for(i = 0; i < 20; i++)
    {
        FileWriterAPI_Write(fw, "[moduleA] iter=%d ret=%d msg=%s\n", i, i * 2, "hello");
        usleep(20 * 1000);   /* 20ms */
    }

    /* 手动刷盘（等一会让消费线程处理完） */
    FileWriterAPI_Flush(fw);
    usleep(200 * 1000);

    Debug_printx("prefix file count = %d", FileWriterAPI_GetFileCount(fw));
    Debug_printx("total  file count = %d", FileWriterAPI_GetTotalFileCount(fw));

    /* 优雅关闭 */
    FileWriterAPI_Destroy(&fw);
    Debug_printx("Part 1 done, fw=%p", (void *)fw);
}


/* ================================================================== */
/*                                                                    */
/*     Part 2: CSV 记录（含子路径 file_prefix）                        */
/*                                                                    */
/* ================================================================== */

static void demo_csv(void)
{
    T_FileWriter *fw = NULL;
    T_FileWriterConfig cfg;
    int i;
    char dir[512];

    Debug_printx("======== Part 2: CSV ========");

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.dir_path, sizeof(cfg.dir_path), "%s/log", TEST_ROOT);
    strncpy(cfg.date_subdir_prefix, "X", sizeof(cfg.date_subdir_prefix) - 1);
    strncpy(cfg.file_prefix,        "sensor/sensor1", sizeof(cfg.file_prefix) - 1);
    cfg.file_type         = FILEWRITER_TYPE_CSV;
    cfg.max_files         = 3;
    cfg.max_file_size     = 0;
    cfg.auto_rotate_daily = 0;
    cfg.thread_priority   = 20;
    cfg.timestamp         = 0;              /* CSV 不加时间戳前缀 */
    cfg.flush_bytes       = 512;
    cfg.flush_ms          = 100;
    cfg.buffer_capacity   = 65536;

    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        Debug_printx("Init fail");
        return;
    }

    /* 写 CSV 表头 */
    FileWriterAPI_Write(fw, "timestamp,channel,voltage\n");

    /* 写数据行 */
    for(i = 0; i < 10; i++)
    {
        FileWriterAPI_Write(fw, "%d,%d,%.3f\n", 1000 + i, i % 4, 3.3 * i / 10);
        usleep(10 * 1000);
    }

    FileWriterAPI_Flush(fw);
    usleep(200 * 1000);

    FileWriterAPI_GetCurrentDirPath(fw, dir, sizeof(dir));
    Debug_printx("csv dir = [%s]", dir);

    FileWriterAPI_Destroy(&fw);
    Debug_printx("Part 2 done");
}


/* ================================================================== */
/*                                                                    */
/*     Part 3: 二进制 BIN                                              */
/*                                                                    */
/* ================================================================== */

typedef struct
{
    unsigned int  magic;
    unsigned int  seq;
    unsigned char payload[16];
} Frame;

static void demo_bin(void)
{
    T_FileWriter *fw = NULL;
    T_FileWriterConfig cfg;
    Frame frame;
    int i;
    int k;

    Debug_printx("======== Part 3: BIN ========");

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.dir_path, sizeof(cfg.dir_path), "%s/bin", TEST_ROOT);
    cfg.date_subdir_prefix[0] = '\0';   /* 不分日期目录 */
    strncpy(cfg.file_prefix,        "frame", sizeof(cfg.file_prefix) - 1);
    cfg.file_type         = FILEWRITER_TYPE_BIN;
    cfg.max_files         = 0;          /* 不限文件数 */
    cfg.max_file_size     = 512;        /* 512 字节触发轮转，便于观察 */
    cfg.auto_rotate_daily = 0;
    cfg.thread_priority   = 20;
    cfg.timestamp         = 0;
    cfg.flush_bytes       = 128;
    cfg.flush_ms          = 50;
    cfg.buffer_capacity   = 8192;

    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        Debug_printx("Init fail");
        return;
    }

    /* 写 40 帧，每帧 24 字节，总 960 字节，会触发多次大小轮转 */
    for(i = 0; i < 40; i++)
    {
        frame.magic = 0xDEADBEEF;
        frame.seq   = (unsigned int)i;
        for(k = 0; k < (int)sizeof(frame.payload); k++)
        {
            frame.payload[k] = (unsigned char)((i + k) & 0xFF);
        }

        FileWriterAPI_WriteBin(fw, &frame, (int)sizeof(frame));
        usleep(5 * 1000);
    }

    FileWriterAPI_Flush(fw);
    usleep(300 * 1000);

    Debug_printx("bin file count = %d", FileWriterAPI_GetFileCount(fw));

    FileWriterAPI_Destroy(&fw);
    Debug_printx("Part 3 done");
}


/* ================================================================== */
/*                                                                    */
/*     Part 4: 多实例（可重入）                                        */
/*                                                                    */
/* ================================================================== */

static void demo_multi_instance(void)
{
    T_FileWriter *fw_a = NULL;
    T_FileWriter *fw_b = NULL;
    T_FileWriterConfig cfg;
    int i;

    Debug_printx("======== Part 4: MULTI INSTANCE ========");

    /* 实例 A */
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.dir_path, sizeof(cfg.dir_path), "%s/multi", TEST_ROOT);
    strncpy(cfg.date_subdir_prefix, "X", sizeof(cfg.date_subdir_prefix) - 1);
    strncpy(cfg.file_prefix, "inst_a", sizeof(cfg.file_prefix) - 1);
    cfg.file_type       = FILEWRITER_TYPE_LOG;
    cfg.max_files       = 2;
    cfg.thread_priority = 15;
    cfg.timestamp       = 1;
    cfg.flush_bytes     = 256;
    cfg.flush_ms        = 100;
    cfg.buffer_capacity = 16384;
    if(FileWriterAPI_Init(&fw_a, &cfg) != 0)
    {
        Debug_printx("inst_a init fail");
        return;
    }

    /* 实例 B（不同前缀、不同优先级） */
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.dir_path, sizeof(cfg.dir_path), "%s/multi", TEST_ROOT);
    strncpy(cfg.date_subdir_prefix, "X", sizeof(cfg.date_subdir_prefix) - 1);
    strncpy(cfg.file_prefix, "inst_b", sizeof(cfg.file_prefix) - 1);
    cfg.file_type       = FILEWRITER_TYPE_LOG;
    cfg.max_files       = 2;
    cfg.thread_priority = 25;
    cfg.timestamp       = 1;
    cfg.flush_bytes     = 256;
    cfg.flush_ms        = 100;
    cfg.buffer_capacity = 16384;
    if(FileWriterAPI_Init(&fw_b, &cfg) != 0)
    {
        FileWriterAPI_Destroy(&fw_a);
        Debug_printx("inst_b init fail");
        return;
    }

    for(i = 0; i < 15; i++)
    {
        FileWriterAPI_Write(fw_a, "A - iter=%d\n", i);
        FileWriterAPI_Write(fw_b, "B - iter=%d\n", i);
        usleep(20 * 1000);
    }

    FileWriterAPI_Flush(fw_a);
    FileWriterAPI_Flush(fw_b);
    usleep(200 * 1000);

    FileWriterAPI_Destroy(&fw_a);
    FileWriterAPI_Destroy(&fw_b);
    Debug_printx("Part 4 done");
}


/* ================================================================== */
/*                                                                    */
/*     Part 5: 手动轮转 + max_files 限制                               */
/*                                                                    */
/* ================================================================== */

static void demo_rotate(void)
{
    T_FileWriter *fw = NULL;
    T_FileWriterConfig cfg;
    int i;
    int r;
    char name[128];

    Debug_printx("======== Part 5: ROTATE ========");

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.dir_path, sizeof(cfg.dir_path), "%s/rotate", TEST_ROOT);
    cfg.date_subdir_prefix[0] = '\0';
    strncpy(cfg.file_prefix, "rot", sizeof(cfg.file_prefix) - 1);
    cfg.file_type       = FILEWRITER_TYPE_LOG;
    cfg.max_files       = 3;                /* 保留最新 3 个 */
    cfg.thread_priority = 20;
    cfg.timestamp       = 0;
    cfg.flush_bytes     = 128;
    cfg.flush_ms        = 100;
    cfg.buffer_capacity = 8192;
    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        Debug_printx("Init fail");
        return;
    }

    /* 写 → 手动 Rotate 6 次，观察 max_files=3 是否生效 */
    for(i = 0; i < 6; i++)
    {
        FileWriterAPI_Write(fw, "content of file #%d\n", i);
        FileWriterAPI_Flush(fw);
        usleep(50 * 1000);   /* 让消费线程落盘 */

        FileWriterAPI_GetCurrentFileName(fw, name, sizeof(name));
        Debug_printx("before rotate #%d: %s", i, name);

        /* 每个文件名要含不同时间戳，间隔至少 1s */
        sleep(1);

        r = FileWriterAPI_Rotate(fw);
        Debug_printx("rotate ret=%d, count=%d", r, FileWriterAPI_GetFileCount(fw));
    }

    Debug_printx("final file count = %d (should <= 3)", FileWriterAPI_GetFileCount(fw));

    FileWriterAPI_Destroy(&fw);
    Debug_printx("Part 5 done");
}


/* ================================================================== */
/*                                                                    */
/*     Part 6: 查询接口                                                */
/*                                                                    */
/* ================================================================== */

static void demo_query(void)
{
    T_FileWriter *fw = NULL;
    T_FileWriterConfig cfg;
    char buf[512];

    Debug_printx("======== Part 6: QUERY ========");

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.dir_path, sizeof(cfg.dir_path), "%s/query", TEST_ROOT);
    strncpy(cfg.date_subdir_prefix, "X", sizeof(cfg.date_subdir_prefix) - 1);
    strncpy(cfg.file_prefix, "q1", sizeof(cfg.file_prefix) - 1);
    cfg.file_type       = FILEWRITER_TYPE_TXT;
    cfg.max_files       = 0;
    cfg.thread_priority = 20;
    cfg.timestamp       = 1;
    cfg.flush_bytes     = 128;
    cfg.flush_ms        = 100;
    cfg.buffer_capacity = 8192;
    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        Debug_printx("Init fail");
        return;
    }

    FileWriterAPI_Write(fw, "query test\n");
    FileWriterAPI_Flush(fw);
    usleep(150 * 1000);

    FileWriterAPI_GetCurrentFileName(fw, buf, sizeof(buf));
    Debug_printx("FileName  = [%s]", buf);
    FileWriterAPI_GetCurrentFilePath(fw, buf, sizeof(buf));
    Debug_printx("FilePath  = [%s]", buf);
    FileWriterAPI_GetCurrentDirPath (fw, buf, sizeof(buf));
    Debug_printx("DirPath   = [%s]", buf);
    Debug_printx("prefix count = %d, total count = %d",
                 FileWriterAPI_GetFileCount(fw), FileWriterAPI_GetTotalFileCount(fw));

    FileWriterAPI_Destroy(&fw);
    Debug_printx("Part 6 done");
}


/* ================================================================== */
/*                                                                    */
/*     Part 7: 工具函数                                                */
/*                                                                    */
/* ================================================================== */

static void demo_utils(void)
{
    char buf[64];

    Debug_printx("======== Part 7: UTILS ========");

    FileWriterAPI_GetTimeString(buf, sizeof(buf), "datetime");
    Debug_printx("datetime    = [%s]", buf);
    FileWriterAPI_GetTimeString(buf, sizeof(buf), "date");
    Debug_printx("date        = [%s]", buf);
    FileWriterAPI_GetTimeString(buf, sizeof(buf), "log");
    Debug_printx("log         = [%s]", buf);
    FileWriterAPI_GetTimeString(buf, sizeof(buf), "datetime_ms");
    Debug_printx("datetime_ms = [%s]", buf);

    FileWriterAPI_MakeDirs(TEST_ROOT "/utils/a/b/c");
    Debug_printx("MakeDirs [%s/utils/a/b/c] done", TEST_ROOT);
}


/* ================================================================== */
/*                                                                    */
/*     Main                                                            */
/*                                                                    */
/* ================================================================== */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    Debug_printx("FileWriter demo start");

    /* 先建根目录，避免污染用户当前路径 */
    FileWriterAPI_MakeDirs(TEST_ROOT);

    demo_log();
    demo_csv();
    demo_bin();
    demo_multi_instance();
    demo_rotate();
    demo_query();
    demo_utils();

    Debug_printx("FileWriter demo end");
    return 0;
}
