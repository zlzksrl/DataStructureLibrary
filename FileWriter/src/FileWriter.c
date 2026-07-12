/**
 * @file        FileWriter.c
 * @brief       LinuxARM-PublicLib-异步文件写入-核心实现文件
 * @details     IMX6ULL平台
 *              基于 StreamBuffer（攒批）+ ThreadManage（消费线程）。
 *              内置消费线程：Wait→GetData→fwrite→fflush，按大小/跨日/手动轮转。
 *
 *              线程模型:
 *              - 生产者(业务线程) 调 Write/WriteBin → vsnprintf → StreamBuffer_PutData
 *              - 消费者(内置线程) 由 ThreadManage 创建，SCHED_RR + 可配优先级；
 *                Wait(flush_ms) → GetData → fwrite → fflush → 大小/跨日轮转
 *              - 关闭: StreamBuffer.Close 阻止写入 → Flush 唤醒 → 消费线程排空 → fclose → 线程退出
 *
 *              并发保护:
 *              - fp / current_* / file_seq / file_written / current_date / stat_* 由 file_lock 保护
 *              - thread_running / shutting_down 为 volatile int（跨线程标志位）
 *              - StreamBuffer 内部线程安全
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-11
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-11
 * @Version     V1.0.0
 * @brief       创建文件，实现异步文件写入全套API
 * @author      zlzksrl
 */
#include "FileWriter_Main.h"
#include "FileWriter_Maketime.h"


/* ========================== 本文件内部前向声明 ========================== */

/* ---- 路径与文件管理 ---- */
static int  fw_make_dirs(const char *path);
static int  fw_build_paths_locked(T_FileWriter *fw);
static int  fw_open_new_file_locked(T_FileWriter *fw);
static int  fw_rotate_locked(T_FileWriter *fw);
static int  fw_check_file_size_rotate_locked(T_FileWriter *fw);
static int  fw_check_daily_rotate_locked(T_FileWriter *fw);
static int  fw_delete_oldest_locked(T_FileWriter *fw);
static int  fw_get_ext_from_type(FileWriterType type, char *out, int out_len);
static int  fw_drain_sb_locked(T_FileWriter *fw);

/* ---- 时间工具 ---- */
static void fw_get_date_str(char *out, int out_len);
static void fw_get_datetime_str(char *out, int out_len);
static void fw_get_timestamp_str(char *out, int out_len);
static int  fw_date_changed_locked(T_FileWriter *fw);

/* ---- 消费线程 ---- */
static void *fw_consumer_thread(void *arg);


/* ========================== 内部辅助函数 ========================== */

/**
 * @func         fw_make_dirs
 * @brief        递归创建多级目录（mkdir -p 语义）
 * @details      支持绝对路径和相对路径；忽略末尾 /；EEXIST 视为成功。
 */
static int fw_make_dirs(const char *path)
{
    char tmp[FW_PATH_MAX];
    size_t len;
    size_t i;

    if(NULL == path || path[0] == '\0')
    {
        return -1;
    }

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);
    if(0 == len)
    {
        return -1;
    }

    /* 去掉末尾多余的 / */
    while(len > 1 && tmp[len - 1] == '/')
    {
        tmp[len - 1] = '\0';
        len--;
    }

    /* 逐级创建 */
    for(i = 1; i < len; i++)
    {
        if(tmp[i] == '/')
        {
            tmp[i] = '\0';
            if(tmp[0] != '\0' && mkdir(tmp, 0755) != 0 && errno != EEXIST)
            {
                printf("mkdir [%s] fail: %s ##%s->%d\n",
                       tmp, strerror(errno), __FUNCTION__, __LINE__);
                return -1;
            }
            tmp[i] = '/';
        }
    }
    if(mkdir(tmp, 0755) != 0 && errno != EEXIST)
    {
        printf("mkdir [%s] fail: %s ##%s->%d\n",
               tmp, strerror(errno), __FUNCTION__, __LINE__);
        return -1;
    }
    return 0;
}

/**
 * @func         fw_get_ext_from_type
 * @brief        根据 file_type 枚举获取默认扩展名
 */
static int fw_get_ext_from_type(FileWriterType type, char *out, int out_len)
{
    const char *ext;

    if(NULL == out || out_len <= 0)
    {
        return -1;
    }

    switch(type)
    {
        case FILEWRITER_TYPE_LOG:
        {
            ext = ".log";
            break;
        }
        case FILEWRITER_TYPE_CSV:
        {
            ext = ".csv";
            break;
        }
        case FILEWRITER_TYPE_BIN:
        {
            ext = ".bin";
            break;
        }
        case FILEWRITER_TYPE_TXT:
        default:
        {
            ext = ".txt";
            break;
        }
    }
    strncpy(out, ext, out_len - 1);
    out[out_len - 1] = '\0';
    return 0;
}

/**
 * @func         fw_get_date_str
 * @brief        获取当前日期字符串 "2026_07_11"
 */
static void fw_get_date_str(char *out, int out_len)
{
    struct timespec ts;
    struct tm tm_val;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_val);
    snprintf(out, out_len, "%04d_%02d_%02d",
             tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday);
}

/**
 * @func         fw_get_datetime_str
 * @brief        获取当前日期时间字符串 "2026-07-11-12-41-30"
 */
static void fw_get_datetime_str(char *out, int out_len)
{
    struct timespec ts;
    struct tm tm_val;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_val);
    snprintf(out, out_len, "%04d-%02d-%02d-%02d-%02d-%02d",
             tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
             tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
}

/**
 * @func         fw_get_timestamp_str
 * @brief        获取时间戳字符串 "[16:50:44.789550] "
 */
static void fw_get_timestamp_str(char *out, int out_len)
{
    struct timespec ts;
    struct tm tm_val;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_val);
    snprintf(out, out_len, "[%02d:%02d:%02d.%06ld] ",
             tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec, ts.tv_nsec / 1000);
}

/**
 * @func         fw_date_changed_locked
 * @brief        检查日期是否变化（跨日检测），已变则更新 current_date
 * @return       1=变化，0=未变化
 * @warning      调用前须持有 fw->file_lock
 */
static int fw_date_changed_locked(T_FileWriter *fw)
{
    char today[FW_DATE_STR_LEN];

    fw_get_date_str(today, sizeof(today));
    if(strcmp(today, fw->current_date) != 0)
    {
        strncpy(fw->current_date, today, sizeof(fw->current_date) - 1);
        fw->current_date[sizeof(fw->current_date) - 1] = '\0';
        return 1;
    }
    return 0;
}

/**
 * @func         fw_build_paths_locked
 * @brief        组装目录路径（不含文件名），并递归创建目录
 * @details      路径组成: {dir_path}[/{date_subdir_prefix}{YYYY_MM_DD}][/{file_prefix 路径部分}]
 *               每段 snprintf 后检查返回值和累加边界，防截断/溢出。
 * @warning      调用前须持有 fw->file_lock
 */
static int fw_build_paths_locked(T_FileWriter *fw)
{
    char full_dir[FW_PATH_MAX];
    int offset = 0;
    int remain;
    int n;
    int dir_len;

    if(NULL == fw)
    {
        return -1;
    }

    /* dir_path（去尾 /） */
    dir_len = (int)strlen(fw->config.dir_path);
    while(dir_len > 1 && fw->config.dir_path[dir_len - 1] == '/')
    {
        dir_len--;
    }

    remain = (int)sizeof(full_dir) - offset;
    n = snprintf(full_dir + offset, (size_t)remain, "%.*s",
                 dir_len, fw->config.dir_path);
    if(n < 0 || n >= remain)
    {
        printf("path truncated at dir_path ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    offset += n;

    /* 日期子目录 */
    if(fw->config.date_subdir_prefix[0] != '\0')
    {
        remain = (int)sizeof(full_dir) - offset;
        n = snprintf(full_dir + offset, (size_t)remain, "/%s%s",
                     fw->config.date_subdir_prefix, fw->current_date);
        if(n < 0 || n >= remain)
        {
            printf("path truncated at date_subdir ##%s->%d\n", __FUNCTION__, __LINE__);
            return -1;
        }
        offset += n;
    }

    /* file_prefix 的路径部分（含 / 时才有子目录） */
    {
        const char *slash = strrchr(fw->config.file_prefix, '/');
        if(slash != NULL && slash != fw->config.file_prefix)
        {
            int subdir_len = (int)(slash - fw->config.file_prefix);
            remain = (int)sizeof(full_dir) - offset;
            n = snprintf(full_dir + offset, (size_t)remain, "/%.*s",
                         subdir_len, fw->config.file_prefix);
            if(n < 0 || n >= remain)
            {
                printf("path truncated at file_prefix ##%s->%d\n", __FUNCTION__, __LINE__);
                return -1;
            }
            offset += n;
        }
    }

    /* 递归创建 */
    if(fw_make_dirs(full_dir) != 0)
    {
        return -1;
    }

    strncpy(fw->current_dirpath, full_dir, sizeof(fw->current_dirpath) - 1);
    fw->current_dirpath[sizeof(fw->current_dirpath) - 1] = '\0';
    return 0;
}

/**
 * @func         fw_open_new_file_locked
 * @brief        按命名规则打开新文件（若成功则关闭旧文件，事务性切换）
 * @details      文件名: {prefix_name}_{seq3位}_{YYYY-MM-DD-HH-MM-SS}.{ext}
 *               先 fopen 新文件成功后，再 fclose 旧文件；失败保持旧文件可写。
 * @warning      调用前须持有 fw->file_lock
 */
static int fw_open_new_file_locked(T_FileWriter *fw)
{
    char datetime[FW_DATETIME_STR_LEN];
    char new_filename[FW_FILENAME_MAX];
    char new_filepath[FW_PATH_MAX];
    const char *prefix_name;
    const char *slash;
    FILE *new_fp;
    int n;

    /* file_prefix 的文件名部分 */
    slash = strrchr(fw->config.file_prefix, '/');
    prefix_name = (slash != NULL) ? slash + 1 : fw->config.file_prefix;

    /* 日期时间 */
    fw_get_datetime_str(datetime, sizeof(datetime));

    /* 文件名 */
    n = snprintf(new_filename, sizeof(new_filename),
                 "%s_%03d_%s%s", prefix_name, fw->file_seq, datetime, fw->ext);
    if(n < 0 || n >= (int)sizeof(new_filename))
    {
        printf("filename truncated ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }

    /* 完整路径 */
    n = snprintf(new_filepath, sizeof(new_filepath),
                 "%s/%s", fw->current_dirpath, new_filename);
    if(n < 0 || n >= (int)sizeof(new_filepath))
    {
        printf("filepath truncated ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }

    /* 打开新文件（wb 覆盖，文件名含时间戳一般不冲突） */
    new_fp = fopen(new_filepath, "wb");
    if(NULL == new_fp)
    {
        printf("fopen [%s] fail: %s ##%s->%d\n",
               new_filepath, strerror(errno), __FUNCTION__, __LINE__);
        return -1;
    }

    /* 新文件已打开：关旧、切换 */
    if(fw->fp != NULL)
    {
        fflush(fw->fp);
        fclose(fw->fp);
    }
    fw->fp = new_fp;
    fw->file_written = 0;

    strncpy(fw->current_filename, new_filename, sizeof(fw->current_filename) - 1);
    fw->current_filename[sizeof(fw->current_filename) - 1] = '\0';
    strncpy(fw->current_filepath, new_filepath, sizeof(fw->current_filepath) - 1);
    fw->current_filepath[sizeof(fw->current_filepath) - 1] = '\0';
    return 0;
}

/**
 * @func         fw_drain_sb_locked
 * @brief        把 StreamBuffer 里剩余数据全部写入当前 fp（不 Rotate 前调用，防跨文件错位）
 * @return       写入字节数（>=0）
 * @warning      调用前须持有 fw->file_lock
 */
static int fw_drain_sb_locked(T_FileWriter *fw)
{
    char buf[FW_FORMAT_BUF_SIZE * 2];
    int total = 0;
    int n;

    while((n = StreamBufferAPI_GetData(fw->sb, buf, sizeof(buf))) > 0)
    {
        if(fw->fp != NULL)
        {
            size_t w = fwrite(buf, 1, (size_t)n, fw->fp);
            if(w > 0)
            {
                fw->file_written += (long)w;
                fw->stat_bytes_written += (unsigned long)w;
                total += (int)w;
            }
            if((int)w < n)
            {
                fw->stat_bytes_lost += (unsigned long)(n - (int)w);
                printf("drain fwrite short: %d/%d, lost=%d ##%s->%d\n",
                       (int)w, n, n - (int)w, __FUNCTION__, __LINE__);
                break;
            }
        }
        else
        {
            /* fp==NULL 数据无处落，计入丢失 */
            fw->stat_bytes_lost += (unsigned long)n;
        }
    }
    if(fw->fp != NULL)
    {
        fflush(fw->fp);
    }
    return total;
}

/**
 * @func         fw_delete_oldest_locked
 * @brief        循环删最老文件（跳过当前正在写的），直到数量 <= max_files
 * @details      匹配当前目录下前缀为 "{prefix_name}_" 的文件；按文件名字典序 = 时间序。
 * @warning      调用前须持有 fw->file_lock
 */
static int fw_delete_oldest_locked(T_FileWriter *fw)
{
    if(fw->config.max_files <= 0)
    {
        return 0;
    }

    while(1)
    {
        DIR *dir;
        struct dirent *ent;
        const char *prefix_name;
        const char *slash;
        char oldest_name[FW_FILENAME_MAX];
        char oldest_path[FW_PATH_MAX];
        int count = 0;
        int prefix_len;

        slash = strrchr(fw->config.file_prefix, '/');
        prefix_name = (slash != NULL) ? slash + 1 : fw->config.file_prefix;
        prefix_len = (int)strlen(prefix_name);

        dir = opendir(fw->current_dirpath);
        if(NULL == dir)
        {
            return -1;
        }

        oldest_name[0] = '\0';
        while((ent = readdir(dir)) != NULL)
        {
            if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            {
                continue;
            }
            /* 匹配 prefix_name + "_" */
            if(strncmp(ent->d_name, prefix_name, prefix_len) == 0
               && ent->d_name[prefix_len] == '_')
            {
                count++;
                /* 跳过当前正在写的文件，只在非当前文件中挑最老 */
                if(strcmp(ent->d_name, fw->current_filename) == 0)
                {
                    continue;
                }
                if(oldest_name[0] == '\0' || strcmp(ent->d_name, oldest_name) < 0)
                {
                    strncpy(oldest_name, ent->d_name, sizeof(oldest_name) - 1);
                    oldest_name[sizeof(oldest_name) - 1] = '\0';
                }
            }
        }
        closedir(dir);

        /* 未超额则退出 */
        if(count <= fw->config.max_files)
        {
            break;
        }
        /* 超额但候选为空（除了当前文件没有其它同前缀文件） → 无法再删 */
        if(oldest_name[0] == '\0')
        {
            break;
        }

        snprintf(oldest_path, sizeof(oldest_path), "%s/%s",
                 fw->current_dirpath, oldest_name);
        if(0 == remove(oldest_path))
        {
            printf("FileWriter [%s] deleted old: %s ##%s->%d\n",
                   fw->name, oldest_name, __FUNCTION__, __LINE__);
        }
        else
        {
            printf("remove [%s] fail: %s ##%s->%d\n",
                   oldest_path, strerror(errno), __FUNCTION__, __LINE__);
            break;
        }
    }
    return 0;
}

/**
 * @func         fw_rotate_locked
 * @brief        内部轮转：排空 SB → 序号+1 → 建目录 → 开新文件 → 删超额
 * @details      失败时回滚 file_seq，保持原文件可写。
 * @warning      调用前须持有 fw->file_lock
 */
static int fw_rotate_locked(T_FileWriter *fw)
{
    int saved_seq;
    int rc;

    /* 1. 先把 SB 里已入队的数据落到当前文件（避免这些数据被写入新文件） */
    fw_drain_sb_locked(fw);

    /* 2. 保存现场以便回滚 */
    saved_seq = fw->file_seq;

    /* 3. 序号 +1（如失败会回滚） */
    fw->file_seq++;

    /* 4. 重新组装路径（日期可能变了） */
    rc = fw_build_paths_locked(fw);
    if(0 != rc)
    {
        fw->file_seq = saved_seq;
        fw->stat_rotate_fail++;
        return -1;
    }

    /* 5. 开新文件（成功后自动关旧、切换） */
    rc = fw_open_new_file_locked(fw);
    if(0 != rc)
    {
        fw->file_seq = saved_seq;
        fw->stat_rotate_fail++;
        return -1;
    }

    /* 6. 检查并循环删超额旧文件（失败不影响主流程） */
    fw_delete_oldest_locked(fw);

    fw->stat_rotate_count++;
    return 0;
}

/**
 * @func         fw_check_file_size_rotate_locked
 * @brief        按 max_file_size 触发轮转（file_written 由调用者在 fwrite 成功后累加）
 * @warning      调用前须持有 fw->file_lock
 */
static int fw_check_file_size_rotate_locked(T_FileWriter *fw)
{
    if(fw->config.max_file_size > 0 && fw->file_written >= fw->config.max_file_size)
    {
        return fw_rotate_locked(fw);
    }
    return 0;
}

/**
 * @func         fw_check_daily_rotate_locked
 * @brief        跨日检测：若跨日且开启 auto_rotate_daily 则轮转
 * @warning      调用前须持有 fw->file_lock
 */
static int fw_check_daily_rotate_locked(T_FileWriter *fw)
{
    if(fw->config.auto_rotate_daily && fw_date_changed_locked(fw))
    {
        /* 跨日：序号继续递增（文件名含日期，同一日期目录下按 seq 顺序增长） */
        return fw_rotate_locked(fw);
    }
    return 0;
}


/* ========================== 消费线程 ========================== */

/**
 * @func         fw_consumer_thread
 * @brief        消费线程：Wait→GetData→fwrite→fflush，处理轮转/关闭
 * @details      主循环:
 *               1. Wait(flush_ms)   —— 等待触发条件(阈值/超时/Flush/Close)
 *               2. 若 used>0 或 r>0（有数据）：
 *                    加锁 → 跨日检查 → 循环 GetData→fwrite→fflush → 大小轮转 → 释放锁
 *               3. Wait 返回 CLOSE_EMPTY（关闭且缓冲空）→ 退出
 *               4. shutting_down 且 used==0 时兜底退出（防 CLOSE_EMPTY 被吞的情况）
 *
 *               退出后再执行一次锁内兜底排空 + fclose，保证优雅关闭数据完整。
 *
 *               类型转换说明：
 *               - StreamBufferAPI_GetData 返回 int（>=0 且 <=max），转 size_t 传 fwrite 安全；
 *               - fwrite 返回 size_t，本函数用 (int)w 与 n 比较：因 n<=sizeof(buf)=2KB
 *                 远小于 INT_MAX，(int)w 转换无溢出风险。
 */
static void *fw_consumer_thread(void *arg)
{
    T_FileWriter *fw = (T_FileWriter *)arg;
    char buf[FW_FORMAT_BUF_SIZE * 2];
    int used = 0;
    int r;
    int n;

    while(fw->thread_running)
    {
        r = StreamBufferAPI_Wait(fw->sb, fw->config.flush_ms, &used);

        /* 只要 used>0 就取数据（含 Wait 返回 CLOSE_DATA / TRIGGER / TIMEOUT_DATA 的所有情况） */
        if(used > 0 || r > 0)
        {
            pthread_mutex_lock(&fw->file_lock);

            /* 跨日检查（每批开始时检一次；不需要在每条数据前检） */
            fw_check_daily_rotate_locked(fw);

            /* 取数据写盘 */
            while((n = StreamBufferAPI_GetData(fw->sb, buf, sizeof(buf))) > 0)
            {
                if(NULL == fw->fp)
                {
                    /* 无 fp 可写（Rotate 失败极端情况），数据只能计入丢失 */
                    fw->stat_bytes_lost += (unsigned long)n;
                    continue;
                }

                size_t w = fwrite(buf, 1, (size_t)n, fw->fp);
                if(w > 0)
                {
                    fflush(fw->fp);
                    fw->file_written        += (long)w;
                    fw->stat_bytes_written  += (unsigned long)w;
                }
                if((int)w < n)
                {
                    /* fwrite 短写/失败：剩余字节丢失并计入统计 */
                    int lost = n - (int)w;
                    fw->stat_bytes_lost += (unsigned long)lost;
                    printf("fwrite short: %d/%d, lost=%d, errno=%s ##%s->%d\n",
                           (int)w, n, lost, strerror(errno), __FUNCTION__, __LINE__);
                    break;  /* 本轮放弃，等下轮 */
                }

                /* 大小触发的轮转（用累计 file_written 判断） */
                fw_check_file_size_rotate_locked(fw);
            }

            pthread_mutex_unlock(&fw->file_lock);
        }

        /* 关闭标志 + 无数据 → 退出 */
        if(STREAMBUFFER_STATUS_CLOSE_EMPTY == r)
        {
            break;
        }
        if(fw->shutting_down && 0 == used)
        {
            break;
        }
    }

    /* 兜底：最后再排空一次（防 Close 之后 Wait 已退出但仍有残余） */
    pthread_mutex_lock(&fw->file_lock);
    while((n = StreamBufferAPI_GetData(fw->sb, buf, sizeof(buf))) > 0)
    {
        if(fw->fp != NULL)
        {
            size_t w = fwrite(buf, 1, (size_t)n, fw->fp);
            if(w > 0)
            {
                fw->stat_bytes_written += (unsigned long)w;
            }
            if((int)w < n)
            {
                fw->stat_bytes_lost += (unsigned long)(n - (int)w);
            }
        }
        else
        {
            fw->stat_bytes_lost += (unsigned long)n;
        }
    }

    /* 关闭文件 */
    if(fw->fp != NULL)
    {
        fflush(fw->fp);
        fclose(fw->fp);
        fw->fp = NULL;
    }
    pthread_mutex_unlock(&fw->file_lock);

    return NULL;
}


/* ========================== 公共 API ========================== */

/* ---- 生命周期 ---- */

int FileWriterAPI_Init(T_FileWriter **pp, const T_FileWriterConfig *cfg)
{
    T_FileWriter *pt;
    T_StreamBufferConfig sb_cfg;
    T_ThreadCreateConfig th_cfg;
    int prio;
    int rc;

    if(NULL == pp)
    {
        printf("NULL == pp ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(NULL != *pp)
    {
        printf("NULL != *pp ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(NULL == cfg)
    {
        printf("NULL == cfg ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(cfg->dir_path[0] == '\0' || cfg->file_prefix[0] == '\0')
    {
        printf("dir_path/file_prefix empty ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    
    printf("FileWriterLibVision = [%s]\n", FileWriter_PROJECT_MAKETIME);

    pt = (T_FileWriter *)malloc(sizeof(T_FileWriter));
    if(NULL == pt)
    {
        printf("malloc fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    memset(pt, 0, sizeof(T_FileWriter));

    /* 拷贝配置 */
    memcpy(&pt->config, cfg, sizeof(T_FileWriterConfig));

    /* 实例名（借 file_prefix 的文件名部分） */
    {
        const char *slash = strrchr(cfg->file_prefix, '/');
        const char *nm    = (slash != NULL) ? slash + 1 : cfg->file_prefix;
        strncpy(pt->name, nm, MAX_FILEWRITERNAME_LEN);
        pt->name[MAX_FILEWRITERNAME_LEN] = '\0';
    }

    /* 扩展名：file_ext 非空用 file_ext，否则按 file_type 选 */
    if(pt->config.file_ext[0] != '\0')
    {
        strncpy(pt->ext, pt->config.file_ext, sizeof(pt->ext) - 1);
        pt->ext[sizeof(pt->ext) - 1] = '\0';
    }
    else
    {
        fw_get_ext_from_type(pt->config.file_type, pt->ext, sizeof(pt->ext));
    }

    /* file_lock */
    if(pthread_mutex_init(&pt->file_lock, NULL) != 0)
    {
        printf("mutex_init fail ##%s->%d\n", __FUNCTION__, __LINE__);
        free(pt);
        return -1;
    }

    /* 初始日期 & 序号 */
    fw_get_date_str(pt->current_date, sizeof(pt->current_date));
    pt->file_seq = 0;

    /* StreamBuffer（名字拼实例名，多实例日志可辨识） */
    memset(&sb_cfg, 0, sizeof(sb_cfg));
    sb_cfg.iCapacity   = (pt->config.buffer_capacity > 0) ? pt->config.buffer_capacity : FW_DEFAULT_BUFFER_CAPACITY;
    sb_cfg.iFlushBytes = (pt->config.flush_bytes     > 0) ? pt->config.flush_bytes     : FW_DEFAULT_FLUSH_BYTES;
    {
        char sb_name[MAX_FILEWRITERNAME_LEN + 8];
        snprintf(sb_name, sizeof(sb_name), "fw_%s_sb", pt->name);
        if(StreamBufferAPI_Init(&pt->sb, &sb_cfg, sb_name) != 0)
        {
            printf("StreamBuffer init fail ##%s->%d\n", __FUNCTION__, __LINE__);
            pthread_mutex_destroy(&pt->file_lock);
            free(pt);
            return -1;
        }
    }

    /* flush_ms 未配置则用默认 */
    if(pt->config.flush_ms <= 0)
    {
        pt->config.flush_ms = FW_DEFAULT_FLUSH_MS;
    }

    /* 组装目录 + 创建第一个文件（此时其它线程尚未启动，但仍加锁以对齐函数约定） */
    pthread_mutex_lock(&pt->file_lock);
    rc = fw_build_paths_locked(pt);
    if(0 == rc)
    {
        rc = fw_open_new_file_locked(pt);
    }
    pthread_mutex_unlock(&pt->file_lock);
    if(0 != rc)
    {
        StreamBufferAPI_Destroy(&pt->sb);
        pthread_mutex_destroy(&pt->file_lock);
        free(pt);
        return -1;
    }

    /* 启动消费线程（用 ThreadManage：SCHED_RR + 可配优先级） */
    prio = (pt->config.thread_priority > 0 && pt->config.thread_priority <= 99)
           ? pt->config.thread_priority : FW_DEFAULT_THREAD_PRIORITY;

    memset(&th_cfg, 0, sizeof(th_cfg));
    th_cfg.pThreadFunc        = fw_consumer_thread;
    th_cfg.pThreadFuncUserArg = pt;
    th_cfg.sThreadName        = pt->name;
    th_cfg.eSetAttr           = 2;                         /* 配置全部属性 */
    th_cfg.istacksize_MB      = 2;
    th_cfg.eDetachState       = PTHREAD_CREATE_JOINABLE;
    th_cfg.einheritsched      = PTHREAD_EXPLICIT_SCHED;    /* 显式使用下面的策略/优先级 */
    th_cfg.eSchedPolicy       = SCHED_RR;                  /* 需求 D2：SCHED_RR 轮转 */
    th_cfg.iSchedPriority     = prio;

    pt->thread_running = 1;
    pt->shutting_down  = 0;

    if(ThreadAPI_ThreadCreate(&th_cfg) < 0)
    {
        /* 非 root 时 SCHED_RR 常因权限失败，降级到默认调度策略（保留栈大小配置） */
        printf("ThreadCreate(SCHED_RR pri=%d) fail, fallback to default ##%s->%d\n",
               prio, __FUNCTION__, __LINE__);
        memset(&th_cfg, 0, sizeof(th_cfg));
        th_cfg.pThreadFunc        = fw_consumer_thread;
        th_cfg.pThreadFuncUserArg = pt;
        th_cfg.sThreadName        = pt->name;
        th_cfg.eSetAttr           = 1;                     /* 仅配置栈大小，调度用继承 */
        th_cfg.istacksize_MB      = 2;
        th_cfg.eDetachState       = PTHREAD_CREATE_JOINABLE;

        if(ThreadAPI_ThreadCreate(&th_cfg) < 0)
        {
            printf("ThreadCreate fail ##%s->%d\n", __FUNCTION__, __LINE__);
            pt->thread_running = 0;
            pthread_mutex_lock(&pt->file_lock);
            if(pt->fp != NULL)
            {
                fclose(pt->fp);
                pt->fp = NULL;
            }
            pthread_mutex_unlock(&pt->file_lock);
            StreamBufferAPI_Destroy(&pt->sb);
            pthread_mutex_destroy(&pt->file_lock);
            free(pt);
            return -1;
        }
    }
    pt->thread_id = th_cfg.tThreadPid;

    pt->init_done = 1;
    *pp = pt;
    return 0;
}

int FileWriterAPI_Destroy(T_FileWriter **pp)
{
    T_FileWriter *pt;

    if(NULL == pp || NULL == *pp)
    {
        return -1;
    }
    if(1 != (*pp)->init_done)
    {
        return -1;
    }

    pt = *pp;

    /* 1. 阻止新写入 */
    StreamBufferAPI_Close(pt->sb);

    /* 2. 通知消费线程排空后退出（thread_running 只是软标志，实际靠 CLOSE_EMPTY） */
    pt->shutting_down  = 1;
    pt->thread_running = 0;
    StreamBufferAPI_Flush(pt->sb);  /* 唤醒 Wait */

    /* 3. 等线程退出（排空 + fclose 在线程内完成） */
    pthread_join(pt->thread_id, NULL);

    /* 4. 兜底：线程内已 fclose，这里再检查一次 */
    pthread_mutex_lock(&pt->file_lock);
    if(pt->fp != NULL)
    {
        fflush(pt->fp);
        fclose(pt->fp);
        pt->fp = NULL;
    }
    pthread_mutex_unlock(&pt->file_lock);

    /* 5. 销毁 StreamBuffer */
    StreamBufferAPI_Destroy(&pt->sb);

    /* 6. 销毁锁、释放结构体 */
    pthread_mutex_destroy(&pt->file_lock);
    pt->init_done = 0;
    free(pt);
    *pp = NULL;
    return 0;
}


/* ---- 写入 ---- */

int FileWriterAPI_Write(T_FileWriter *fw, const char *fmt, ...)
{
    /* 栈 buffer：单条日志上限 FW_FORMAT_BUF_SIZE(1KB)。
     * 由于 StreamBuffer.PutData 内部立即 memcpy，返回后 buf 可复用，
     * 不需要 MemoryPool 之类跨线程持有 buffer 的机制。 */
    char buf[FW_FORMAT_BUF_SIZE];
    int offset = 0;    /* 时间戳前缀长度（若开启） */
    int n;             /* vsnprintf 返回值 */
    int fmt_len;
    va_list ap;

    if(NULL == fw || NULL == fmt || !fw->init_done)
    {
        return -1;
    }

    /* 时间戳前缀 [HH:MM:SS.mmmmmm] （config.timestamp==1 时开启） */
    if(fw->config.timestamp)
    {
        fw_get_timestamp_str(buf, FW_TIMESTAMP_STR_LEN);
        offset = (int)strlen(buf);
    }

    /* 格式化：vsnprintf 返回"若空间够会写入的字节数"（不含 '\0'），
     * 直接用它算 fmt_len，省一次 strlen。 */
    va_start(ap, fmt);
    n = vsnprintf(buf + offset, sizeof(buf) - offset, fmt, ap);
    va_end(ap);
    if(n < 0)
    {
        /* vsnprintf 遇编码错误极少发生；出现即视为参数无效 */
        return -1;
    }
    /* 超长时截断到 buffer 末尾（不含末尾 '\0'），仅丢内容不越界 */
    if(n > (int)sizeof(buf) - offset - 1)
    {
        n = (int)sizeof(buf) - offset - 1;
    }
    fmt_len = offset + n;

    /* 入队（StreamBuffer 内部 memcpy，返回后 buf 可复用/释放）。
     * PutData 返回值：>=0 入队字节数；-1 参数无效；-2 已关闭；-3 缓冲满丢弃。 */
    return StreamBufferAPI_PutData(fw->sb, buf, fmt_len);
}

int FileWriterAPI_WriteBin(T_FileWriter *fw, const void *data, int len)
{
    if(NULL == fw || NULL == data || !fw->init_done)
    {
        return -1;
    }
    if(len <= 0)
    {
        return -1;
    }
    return StreamBufferAPI_PutData(fw->sb, (const char *)data, len);
}


/* ---- 轮转与刷新 ---- */

int FileWriterAPI_Rotate(T_FileWriter *fw)
{
    int ret;

    if(NULL == fw || !fw->init_done)
    {
        return -1;
    }

    /* Rotate 需要与消费线程互斥（消费线程写盘可能同时进行） */
    pthread_mutex_lock(&fw->file_lock);
    ret = fw_rotate_locked(fw);
    pthread_mutex_unlock(&fw->file_lock);
    return ret;
}

int FileWriterAPI_Flush(T_FileWriter *fw)
{
    if(NULL == fw || !fw->init_done)
    {
        return -1;
    }
    return StreamBufferAPI_Flush(fw->sb);
}


/* ---- 查询 ---- */

int FileWriterAPI_GetCurrentFileName(T_FileWriter *fw, char *out, int out_len)
{
    if(NULL == fw || NULL == out || out_len <= 0 || !fw->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&fw->file_lock);
    strncpy(out, fw->current_filename, out_len - 1);
    out[out_len - 1] = '\0';
    pthread_mutex_unlock(&fw->file_lock);
    return 0;
}

int FileWriterAPI_GetCurrentFilePath(T_FileWriter *fw, char *out, int out_len)
{
    if(NULL == fw || NULL == out || out_len <= 0 || !fw->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&fw->file_lock);
    strncpy(out, fw->current_filepath, out_len - 1);
    out[out_len - 1] = '\0';
    pthread_mutex_unlock(&fw->file_lock);
    return 0;
}

int FileWriterAPI_GetCurrentDirPath(T_FileWriter *fw, char *out, int out_len)
{
    if(NULL == fw || NULL == out || out_len <= 0 || !fw->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&fw->file_lock);
    strncpy(out, fw->current_dirpath, out_len - 1);
    out[out_len - 1] = '\0';
    pthread_mutex_unlock(&fw->file_lock);
    return 0;
}

int FileWriterAPI_GetFileCount(T_FileWriter *fw)
{
    DIR *dir;
    struct dirent *ent;
    const char *prefix_name;
    const char *slash;
    char dirpath[FW_PATH_MAX];
    int count = 0;
    int prefix_len;

    if(NULL == fw || !fw->init_done)
    {
        return -1;
    }

    /* 快照当前目录 */
    pthread_mutex_lock(&fw->file_lock);
    strncpy(dirpath, fw->current_dirpath, sizeof(dirpath) - 1);
    dirpath[sizeof(dirpath) - 1] = '\0';
    pthread_mutex_unlock(&fw->file_lock);

    slash = strrchr(fw->config.file_prefix, '/');
    prefix_name = (slash != NULL) ? slash + 1 : fw->config.file_prefix;
    prefix_len = (int)strlen(prefix_name);

    dir = opendir(dirpath);
    if(NULL == dir)
    {
        return -1;
    }

    while((ent = readdir(dir)) != NULL)
    {
        if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
            continue;
        }
        if(strncmp(ent->d_name, prefix_name, prefix_len) == 0
           && ent->d_name[prefix_len] == '_')
        {
            count++;
        }
    }
    closedir(dir);
    return count;
}

int FileWriterAPI_GetTotalFileCount(T_FileWriter *fw)
{
    DIR *dir;
    struct dirent *ent;
    char dirpath[FW_PATH_MAX];
    int count = 0;

    if(NULL == fw || !fw->init_done)
    {
        return -1;
    }

    pthread_mutex_lock(&fw->file_lock);
    strncpy(dirpath, fw->current_dirpath, sizeof(dirpath) - 1);
    dirpath[sizeof(dirpath) - 1] = '\0';
    pthread_mutex_unlock(&fw->file_lock);

    dir = opendir(dirpath);
    if(NULL == dir)
    {
        return -1;
    }

    while((ent = readdir(dir)) != NULL)
    {
        if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
            continue;
        }
        count++;
    }
    closedir(dir);
    return count;
}

int FileWriterAPI_StatsGet(T_FileWriter *fw, T_FileWriterStats *out)
{
    int sb_used;
    int file_count;

    if(NULL == fw || NULL == out || !fw->init_done)
    {
        return -1;
    }

    /* SB used 和 file_count 不持 file_lock 也能拿；且必须在拿 file_lock 前取，
     * 避免锁嵌套：
     * - StreamBufferAPI_GetLength 内部持 SB 自己的锁；
     * - FileWriterAPI_GetFileCount 内部会拿 file_lock 快照目录路径。
     * 若这里已持 file_lock 再调 GetFileCount，会自锁死锁。 */
    sb_used    = StreamBufferAPI_GetLength(fw->sb);
    file_count = FileWriterAPI_GetFileCount(fw);

    /* 累计计数器由 file_lock 保护 */
    pthread_mutex_lock(&fw->file_lock);
    out->bytes_written = fw->stat_bytes_written;
    out->bytes_lost    = fw->stat_bytes_lost;
    out->rotate_count  = fw->stat_rotate_count;
    out->rotate_fail   = fw->stat_rotate_fail;
    pthread_mutex_unlock(&fw->file_lock);

    /* 负值兜底：SB/文件系统 API 返回 -1 表示查询失败，对用户呈 0 更合理 */
    out->sb_used    = (sb_used    >= 0) ? sb_used    : 0;
    out->file_count = (file_count >= 0) ? file_count : 0;
    return 0;
}


/* ---- 工具函数 ---- */

int FileWriterAPI_GetTimeString(char *out, int out_len, const char *fmt)
{
    if(NULL == out || out_len <= 0 || NULL == fmt)
    {
        return -1;
    }

    if(strcmp(fmt, "datetime") == 0)
    {
        fw_get_datetime_str(out, out_len);
    }
    else if(strcmp(fmt, "date") == 0)
    {
        fw_get_date_str(out, out_len);
    }
    else if(strcmp(fmt, "log") == 0)
    {
        fw_get_timestamp_str(out, out_len);
    }
    else if(strcmp(fmt, "datetime_ms") == 0)
    {
        struct timespec ts;
        struct tm tm_val;
        clock_gettime(CLOCK_REALTIME, &ts);
        localtime_r(&ts.tv_sec, &tm_val);
        snprintf(out, out_len, "%04d-%02d-%02d-%02d-%02d-%02d.%06ld",
                 tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
                 tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec, ts.tv_nsec / 1000);
    }
    else
    {
        return -1;
    }
    return 0;
}

int FileWriterAPI_MakeDirs(const char *path)
{
    return fw_make_dirs(path);
}
