/**
 * @file        FileWriter.c
 * @brief       LinuxARM-PublicLib-异步文件写入-核心实现文件
 * @details     IMX6ULL平台
 *              基于 StreamBuffer（攒批）+ MemoryPool（格式化buffer）+ ThreadManage（线程）。
 *              内置消费线程：Wait→GetData→fwrite→fflush，按大小/跨日/手动轮转。
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


/* ========================== 内部辅助函数 ========================== */

/**
 * @func         fw_make_dirs
 * @brief        递归创建多级目录（mkdir -p 语义）
 */
static int fw_make_dirs(const char *path)
{
    char tmp[FW_PATH_MAX];
    size_t len;
    size_t i;

    if(NULL == path) return -1;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);
    if(len == 0) return -1;
    if(tmp[len - 1] == '/') tmp[len - 1] = '\0';  /* 去掉末尾 / */

    for(i = 1; i < len; i++)
    {
        if(tmp[i] == '/')
        {
            tmp[i] = '\0';
            if(mkdir(tmp, 0755) != 0 && errno != EEXIST)
            {
                printf("mkdir [%s] fail: %s ##%s->%d\n", tmp, strerror(errno), __FUNCTION__, __LINE__);
                return -1;
            }
            tmp[i] = '/';
        }
    }
    if(mkdir(tmp, 0755) != 0 && errno != EEXIST)
    {
        printf("mkdir [%s] fail: %s ##%s->%d\n", tmp, strerror(errno), __FUNCTION__, __LINE__);
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
    switch(type)
    {
        case FILEWRITER_TYPE_LOG: ext = ".log"; break;
        case FILEWRITER_TYPE_CSV: ext = ".csv"; break;
        case FILEWRITER_TYPE_BIN: ext = ".bin"; break;
        case FILEWRITER_TYPE_TXT:
        default:                  ext = ".txt"; break;
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
 * @func         fw_date_changed
 * @brief        检查日期是否变化（跨日检测）
 * @return       1=变化, 0=未变化
 */
static int fw_date_changed(T_FileWriter *fw)
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
 * @func         fw_build_paths
 * @brief        组装目录路径（不含文件名），并创建目录
 */
static int fw_build_paths(T_FileWriter *fw)
{
    char full_dir[FW_PATH_MAX];
    int offset = 0;

    /* dir_path */
    offset += snprintf(full_dir + offset, sizeof(full_dir) - offset, "%s", fw->config.dir_path);

    /* 日期子目录 */
    if(fw->config.date_subdir_prefix[0] != '\0')
    {
        offset += snprintf(full_dir + offset, sizeof(full_dir) - offset, "/%s%s",
                           fw->config.date_subdir_prefix, fw->current_date);
    }

    /* file_prefix 的路径部分（含 / 的前缀） */
    {
        const char *slash = strrchr(fw->config.file_prefix, '/');
        if(slash != NULL)
        {
            int subdir_len = (int)(slash - fw->config.file_prefix);
            offset += snprintf(full_dir + offset, sizeof(full_dir) - offset, "/%.*s",
                               subdir_len, fw->config.file_prefix);
        }
    }

    /* 去掉末尾可能的 // */
    /* 创建目录 */
    if(fw_make_dirs(full_dir) != 0)
    {
        return -1;
    }

    strncpy(fw->current_dirpath, full_dir, sizeof(fw->current_dirpath) - 1);
    fw->current_dirpath[sizeof(fw->current_dirpath) - 1] = '\0';
    return 0;
}

/**
 * @func         fw_create_file
 * @brief        按命名规则创建新文件
 */
static int fw_create_file(T_FileWriter *fw)
{
    char datetime[FW_DATETIME_STR_LEN];
    const char *prefix_name;  /* file_prefix 的文件名部分 */
    const char *slash;

    /* 找 file_prefix 的文件名部分 */
    slash = strrchr(fw->config.file_prefix, '/');
    prefix_name = (slash != NULL) ? slash + 1 : fw->config.file_prefix;

    /* 日期时间 */
    fw_get_datetime_str(datetime, sizeof(datetime));

    /* 文件名：{prefix_name}{seq3位}_{datetime}.{ext} */
    snprintf(fw->current_filename, sizeof(fw->current_filename), "%s%03d_%s%s",
             prefix_name, fw->file_seq, datetime, fw->ext);

    /* 完整路径 */
    snprintf(fw->current_filepath, sizeof(fw->current_filepath), "%s/%s",
             fw->current_dirpath, fw->current_filename);

    /* 打开文件 */
    if(fw->fp != NULL)
    {
        fflush(fw->fp);
        fclose(fw->fp);
    }
    fw->fp = fopen(fw->current_filepath, "wb");
    if(fw->fp == NULL)
    {
        printf("fopen [%s] fail: %s ##%s->%d\n", fw->current_filepath, strerror(errno), __FUNCTION__, __LINE__);
        return -1;
    }

    fw->file_written = 0;
    return 0;
}

/**
 * @func         fw_delete_oldest_if_needed
 * @brief        检查并删除超额的旧文件
 */
static int fw_delete_oldest_if_needed(T_FileWriter *fw)
{
    DIR *dir;
    struct dirent *ent;
    const char *prefix_name;
    const char *slash;
    char oldest_name[FW_FILENAME_MAX];
    char oldest_path[FW_PATH_MAX];
    time_t oldest_time = 0;
    int count = 0;
    int prefix_len;

    if(fw->config.max_files <= 0) return 0;

    slash = strrchr(fw->config.file_prefix, '/');
    prefix_name = (slash != NULL) ? slash + 1 : fw->config.file_prefix;
    prefix_len = (int)strlen(prefix_name);

    dir = opendir(fw->current_dirpath);
    if(dir == NULL) return -1;

    oldest_name[0] = '\0';
    while((ent = readdir(dir)) != NULL)
    {
        /* 跳过 . 和 .. */
        if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        /* 匹配前缀 + "_" */
        if(strncmp(ent->d_name, prefix_name, prefix_len) == 0 && ent->d_name[prefix_len] == '_')
        {
            count++;
            /* 用文件名排序找最老（文件名含日期时间，字典序=时间序） */
            if(oldest_name[0] == '\0' || strcmp(ent->d_name, oldest_name) < 0)
            {
                strncpy(oldest_name, ent->d_name, sizeof(oldest_name) - 1);
                oldest_name[sizeof(oldest_name) - 1] = '\0';
            }
        }
    }
    closedir(dir);

    /* 超额则删最老 */
    if(count > fw->config.max_files && oldest_name[0] != '\0')
    {
        snprintf(oldest_path, sizeof(oldest_path), "%s/%s", fw->current_dirpath, oldest_name);
        if(remove(oldest_path) == 0)
        {
            printf("FileWriter %s deleted old file: %s ##%s->%d\n",
                   fw->name, oldest_name, __FUNCTION__, __LINE__);
        }
    }
    return 0;
}

/**
 * @func         fw_rotate_internal
 * @brief        内部轮转：关闭当前文件，建新目录+新文件
 */
static int fw_rotate_internal(T_FileWriter *fw)
{
    /* 序号+1 */
    fw->file_seq++;

    /* 重新组装路径（日期可能变了） */
    if(fw_build_paths(fw) != 0) return -1;

    /* 创建新文件 */
    if(fw_create_file(fw) != 0) return -1;

    /* 检查并删除超额旧文件 */
    fw_delete_oldest_if_needed(fw);

    return 0;
}

/**
 * @func         fw_check_file_size_rotate
 * @brief        检查文件大小是否超限，超限则轮转
 */
static int fw_check_file_size_rotate(T_FileWriter *fw, int bytes_written)
{
    fw->file_written += bytes_written;
    if(fw->config.max_file_size > 0 && fw->file_written >= fw->config.max_file_size)
    {
        return fw_rotate_internal(fw);
    }
    return 0;
}

/**
 * @func         fw_check_daily_rotate
 * @brief        检查跨日，跨日则轮转（建新日期目录+新文件）
 */
static int fw_check_daily_rotate(T_FileWriter *fw)
{
    if(fw->config.auto_rotate_daily && fw_date_changed(fw))
    {
        return fw_rotate_internal(fw);
    }
    return 0;
}


/* ========================== 消费线程 ========================== */

/**
 * @func         fw_consumer_thread
 * @brief        消费线程：Wait→GetData→fwrite→fflush，处理轮转
 */
static void *fw_consumer_thread(void *arg)
{
    T_FileWriter *fw = (T_FileWriter *)arg;
    char buf[FW_FORMAT_BUF_SIZE * 2];
    int used;
    int r;

    while(fw->thread_running)
    {
        r = StreamBufferAPI_Wait(fw->sb, fw->config.flush_ms, &used);

        if(r > 0 || (r == 0 && used > 0))
        {
            /* 检查跨日轮转 */
            fw_check_daily_rotate(fw);

            /* 取数据写盘 */
            int n;
            while((n = StreamBufferAPI_GetData(fw->sb, buf, sizeof(buf))) > 0)
            {
                if(fw->fp != NULL)
                {
                    size_t w = fwrite(buf, 1, (size_t)n, fw->fp);
                    if(w > 0) fflush(fw->fp);
                    fw_check_file_size_rotate(fw, (int)w);
                }
            }
        }

        /* 关闭标志 */
        if(fw->shutting_down)
        {
            /* 排空剩余 */
            int n;
            while((n = StreamBufferAPI_GetData(fw->sb, buf, sizeof(buf))) > 0)
            {
                if(fw->fp != NULL)
                {
                    fwrite(buf, 1, (size_t)n, fw->fp);
                }
            }
            break;
        }
    }

    /* 关闭文件 */
    if(fw->fp != NULL)
    {
        fflush(fw->fp);
        fclose(fw->fp);
        fw->fp = NULL;
    }

    return NULL;
}


/* ========================== 公共 API ========================== */

/* ---- 生命周期 ---- */

int FileWriterAPI_Init(T_FileWriter **pp, const T_FileWriterConfig *cfg)
{
    T_FileWriter *pt;
    T_StreamBufferConfig sb_cfg;
    T_MemPoolConfig mp_cfg;
    pthread_condattr_t attr;

    if(NULL == pp) { printf("NULL == pp ##%s->%d\n", __FUNCTION__, __LINE__); return -1; }
    if(NULL != *pp) { printf("NULL != *pp fail ##%s->%d\n", __FUNCTION__, __LINE__); return -1; }
    if(NULL == cfg) { printf("NULL == cfg ##%s->%d\n", __FUNCTION__, __LINE__); return -1; }

    printf("FileWriterLibVision = [%s]\n", FileWriter_PROJECT_MAKETIME);

    pt = (T_FileWriter *)malloc(sizeof(T_FileWriter));
    if(NULL == pt) { printf("malloc fail ##%s->%d\n", __FUNCTION__, __LINE__); return -1; }
    memset(pt, 0, sizeof(T_FileWriter));

    /* ★ memcpy 配置（防篡改） */
    memcpy(&pt->config, cfg, sizeof(T_FileWriterConfig));

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

    /* 初始日期 */
    fw_get_date_str(pt->current_date, sizeof(pt->current_date));
    pt->file_seq = 0;

    /* 组装目录路径 + 创建目录 */
    if(fw_build_paths(pt) != 0)
    {
        free(pt);
        return -1;
    }

    /* 创建第一个文件 */
    if(fw_create_file(pt) != 0)
    {
        free(pt);
        return -1;
    }

    /* 初始化 MemoryPool（格式化 buffer） */
    mp_cfg.element_size = FW_FORMAT_BUF_SIZE;
    mp_cfg.init_count = FW_FORMAT_POOL_COUNT;
    mp_cfg.mode = MEMPOOL_MODE_DROP;
    mp_cfg.grow_count = 0;
    mp_cfg.block_timeo = 0;
    if(MemPoolAPI_Init(&pt->pool, &mp_cfg, "fw_pool") != 0)
    {
        printf("MemPool init fail ##%s->%d\n", __FUNCTION__, __LINE__);
        fclose(pt->fp);
        free(pt);
        return -1;
    }

    /* 初始化 StreamBuffer */
    sb_cfg.iCapacity = (pt->config.buffer_capacity > 0) ? pt->config.buffer_capacity : 65536;
    sb_cfg.iFlushBytes = (pt->config.flush_bytes > 0) ? pt->config.flush_bytes : 4096;
    sb_cfg.iFlushMs = 0;  /* 消费线程自己用 Wait(flush_ms) */
    if(StreamBufferAPI_Init(&pt->sb, &sb_cfg, "fw_sb") != 0)
    {
        printf("StreamBuffer init fail ##%s->%d\n", __FUNCTION__, __LINE__);
        MemPoolAPI_Destroy(&pt->pool);
        fclose(pt->fp);
        free(pt);
        return -1;
    }

    /* 启动消费线程 */
    pt->thread_running = 1;
    pt->shutting_down = 0;
    if(pthread_create(&pt->thread_id, NULL, fw_consumer_thread, pt) != 0)
    {
        printf("pthread_create fail ##%s->%d\n", __FUNCTION__, __LINE__);
        StreamBufferAPI_Destroy(&pt->sb);
        MemPoolAPI_Destroy(&pt->pool);
        fclose(pt->fp);
        free(pt);
        return -1;
    }

    pt->init_done = 1;
    *pp = pt;
    return 0;
}

int FileWriterAPI_Destroy(T_FileWriter **pp)
{
    T_FileWriter *pt;

    if(NULL == pp || NULL == *pp) return -1;
    if(1 != (*pp)->init_done) return -1;

    pt = *pp;

    /* 1. 阻止新写入 */
    StreamBufferAPI_Close(pt->sb);

    /* 2. 通知消费线程排空后退出 */
    pt->shutting_down = 1;
    StreamBufferAPI_Flush(pt->sb);  /* 唤醒线程 */

    /* 3. 等线程退出（排空+fclose 在线程内完成） */
    pthread_join(pt->thread_id, NULL);

    /* 4. 销毁 StreamBuffer + MemoryPool */
    StreamBufferAPI_Destroy(&pt->sb);
    MemPoolAPI_Destroy(&pt->pool);

    /* 5. 安全：文件已在线程内关闭，这里兜底 */
    if(pt->fp != NULL) fclose(pt->fp);

    pt->init_done = 0;
    free(pt);
    *pp = NULL;
    return 0;
}


/* ---- 写入 ---- */

int FileWriterAPI_Write(T_FileWriter *fw, const char *fmt, ...)
{
    char *buf;
    int offset = 0;
    int fmt_len;
    va_list ap;

    if(NULL == fw || NULL == fmt || !fw->init_done) return -1;

    /* 从 MemoryPool 取 buffer */
    buf = (char *)MemPoolAPI_Alloc(fw->pool);
    if(buf == NULL)
    {
        /* 池满，用栈兜底 */
        char stack_buf[FW_FORMAT_BUF_SIZE];
        va_start(ap, fmt);
        vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
        va_end(ap);
        return StreamBufferAPI_PutData(fw->sb, stack_buf, (int)strlen(stack_buf));
    }

    /* 时间戳前缀 */
    if(fw->config.timestamp)
    {
        fw_get_timestamp_str(buf, FW_TIMESTAMP_STR_LEN);
        offset = (int)strlen(buf);
    }

    /* 格式化 */
    va_start(ap, fmt);
    vsnprintf(buf + offset, FW_FORMAT_BUF_SIZE - offset, fmt, ap);
    va_end(ap);

    fmt_len = (int)strlen(buf);

    /* 入队 StreamBuffer */
    {
        int ret = StreamBufferAPI_PutData(fw->sb, buf, fmt_len);
        MemPoolAPI_Free(fw->pool, buf);
        return ret;
    }
}

int FileWriterAPI_WriteBin(T_FileWriter *fw, const void *data, int len)
{
    if(NULL == fw || NULL == data || !fw->init_done) return -1;
    if(len <= 0) return -1;
    return StreamBufferAPI_PutData(fw->sb, (const char *)data, len);
}


/* ---- 轮转与刷新 ---- */

int FileWriterAPI_Rotate(T_FileWriter *fw)
{
    if(NULL == fw || !fw->init_done) return -1;
    return fw_rotate_internal(fw);
}

int FileWriterAPI_Flush(T_FileWriter *fw)
{
    if(NULL == fw || !fw->init_done) return -1;
    return StreamBufferAPI_Flush(fw->sb);
}


/* ---- 查询 ---- */

int FileWriterAPI_GetCurrentFileName(T_FileWriter *fw, char *out, int out_len)
{
    if(NULL == fw || NULL == out || !fw->init_done) return -1;
    strncpy(out, fw->current_filename, out_len - 1);
    out[out_len - 1] = '\0';
    return 0;
}

int FileWriterAPI_GetCurrentFilePath(T_FileWriter *fw, char *out, int out_len)
{
    if(NULL == fw || NULL == out || !fw->init_done) return -1;
    strncpy(out, fw->current_filepath, out_len - 1);
    out[out_len - 1] = '\0';
    return 0;
}

int FileWriterAPI_GetCurrentDirPath(T_FileWriter *fw, char *out, int out_len)
{
    if(NULL == fw || NULL == out || !fw->init_done) return -1;
    strncpy(out, fw->current_dirpath, out_len - 1);
    out[out_len - 1] = '\0';
    return 0;
}

int FileWriterAPI_GetFileCount(T_FileWriter *fw)
{
    DIR *dir;
    struct dirent *ent;
    const char *prefix_name;
    const char *slash;
    int count = 0;
    int prefix_len;

    if(NULL == fw || !fw->init_done) return -1;

    slash = strrchr(fw->config.file_prefix, '/');
    prefix_name = (slash != NULL) ? slash + 1 : fw->config.file_prefix;
    prefix_len = (int)strlen(prefix_name);

    dir = opendir(fw->current_dirpath);
    if(dir == NULL) return -1;

    while((ent = readdir(dir)) != NULL)
    {
        if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if(strncmp(ent->d_name, prefix_name, prefix_len) == 0 && ent->d_name[prefix_len] == '_')
            count++;
    }
    closedir(dir);
    return count;
}

int FileWriterAPI_GetTotalFileCount(T_FileWriter *fw)
{
    DIR *dir;
    struct dirent *ent;
    int count = 0;

    if(NULL == fw || !fw->init_done) return -1;

    dir = opendir(fw->current_dirpath);
    if(dir == NULL) return -1;

    while((ent = readdir(dir)) != NULL)
    {
        if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        count++;
    }
    closedir(dir);
    return count;
}


/* ---- 工具函数 ---- */

int FileWriterAPI_GetTimeString(char *out, int out_len, const char *fmt)
{
    if(NULL == out || NULL == fmt) return -1;

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
        return -1;  /* 未知格式 */
    }
    return 0;
}

int FileWriterAPI_MakeDirs(const char *path)
{
    return fw_make_dirs(path);
}
