/**
 * @file        StreamBuffer.c
 * @brief       LinuxARM-PublicLib-流缓冲区-核心实现文件
 * @details     IMX6ULL平台
 *              本文件实现流缓冲区（StreamBuffer）的全部公共API。
 *              字节流环形缓冲区：预分配连续内存、变长紧凑、环形复用、零 malloc；
 *              满则丢新（不阻塞）；Close broadcast/Reopen 可逆；
 *              消费三种方式：GetData(拷贝)/GetDataAddress(零拷贝逐段)/回调(零拷贝自动)。
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-09
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-09
 * @Version     V1.0.0
 * @brief       创建文件，实现流缓冲区全套API
 * @author      zlzksrl
 */
#include "../include/StreamBuffer.h"
#include "StreamBuffer_Main.h"
#include "StreamBuffer_Maketime.h"

#include <errno.h>   /* ETIMEDOUT */
#include <time.h>    /* clock_gettime / CLOCK_MONOTONIC */


/* ========================== 内部辅助函数 ========================== */

/**
 * @func         sb_is_pow2
 * @brief        判断 n 是否为 2 的幂（且 > 0）
 */
static int sb_is_pow2(int n)
{
    return (n > 0) && ((n & (n - 1)) == 0);
}

/**
 * @func         sb_set_name
 * @brief        拷贝队列名称到固定缓冲（截断保护）
 */
static void sb_set_name(char *dst, const char *src)
{
    size_t len = strlen(src);
    if(len < MAX_STREAMBUFFERNAME_LEN)
    {
        strncpy(dst, src, len);
        dst[len] = '\0';
    }
    else
    {
        strncpy(dst, src, MAX_STREAMBUFFERNAME_LEN);
        dst[MAX_STREAMBUFFERNAME_LEN] = '\0';
    }
}

/**
 * @func         sb_clamp_ret
 * @brief        回调返回值钳位到 [0, len]，越界打印警告
 */
static int sb_clamp_ret(int c, int len, const char *name)
{
    if(c < 0)
    {
        printf("WARNING: StreamBuffer %s consume callback return %d < 0, clamp to 0 ##%s->%d\n",
               name, c, __FUNCTION__, __LINE__);
        return 0;
    }
    if(c > len)
    {
        printf("WARNING: StreamBuffer %s consume callback return %d > len %d, clamp to len ##%s->%d\n",
               name, c, len, __FUNCTION__, __LINE__);
        return len;
    }
    return c;
}

/**
 * @func         sb_calc_deadline
 * @brief        计算条件变量绝对超时（当前时间 + timeo_ms 毫秒，CLOCK_REALTIME）
 */
static void sb_calc_deadline(struct timespec *ts, int timeo_ms)
{
    long add_ns;
    long nsec;
    clock_gettime(CLOCK_MONOTONIC, ts);     /* 单调时钟，不受 NTP/手动改时影响 */
    add_ns = (long)(timeo_ms % 1000) * 1000000L;
    nsec   = ts->tv_nsec + add_ns;
    ts->tv_sec  += timeo_ms / 1000 + nsec / 1000000000L;
    ts->tv_nsec  = nsec % 1000000000L;
}


/* ========================== 生命周期 ========================== */

/**
 * @func         StreamBufferAPI_Init
 * @brief        初始化流缓冲区
 */
int StreamBufferAPI_Init(T_StreamBuffer **pp, const T_StreamBufferConfig *cfg, const char *name)
{
    int capacity;
    int flush_bytes;

    /* ---- 参数合法性检查 ---- */
    if(NULL == pp)
    {
        printf("NULL == pp fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(NULL != *pp)
    {
        printf("NULL != *pp fail (already init?) ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(NULL == name)
    {
        printf("NULL == name fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }

    /* ---- 解析配置（NULL 或 0 用默认） ---- */
    capacity    = (cfg && cfg->iCapacity   > 0) ? cfg->iCapacity   : 65536;
    flush_bytes = (cfg && cfg->iFlushBytes > 0) ? cfg->iFlushBytes : 4096;

    /* ---- 容量必须为 2 的幂 ---- */
    if(!sb_is_pow2(capacity))
    {
        printf("capacity %d not power of 2 fail ##%s->%d\n", capacity, __FUNCTION__, __LINE__);
        return -1;
    }
    /* ---- 阈值必须 <= 容量 ---- */
    if(flush_bytes > capacity)
    {
        printf("flush_bytes %d > capacity %d fail ##%s->%d\n",
               flush_bytes, capacity, __FUNCTION__, __LINE__);
        return -1;
    }

    /* ---- 打印库版本 ---- */
    printf("StreamBufferLibVision = [%s]\n", StreamBuffer_PROJECT_MAKETIME);

    /* ---- 分配结构体 ---- */
    T_StreamBuffer *pt = (T_StreamBuffer *)malloc(sizeof(T_StreamBuffer));
    if(NULL == pt)
    {
        printf("malloc fail struct ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    memset(pt, 0, sizeof(T_StreamBuffer));

    /* ---- 预分配环形缓冲 ---- */
    pt->buffer = (unsigned char *)malloc((size_t)capacity);
    if(NULL == pt->buffer)
    {
        printf("malloc fail buffer ##%s->%d\n", __FUNCTION__, __LINE__);
        free(pt);
        return -1;
    }

    /* ---- 初始化字段 ---- */
    pt->capacity    = capacity;
    pt->mask        = capacity - 1;
    pt->flush_bytes = flush_bytes;
    pthread_mutex_init(&pt->mux, NULL);
    {
        /* cond 使用 CLOCK_MONOTONIC，超时不受系统时间跳变影响 */
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&pt->cond, &attr);
        pthread_condattr_destroy(&attr);
    }
    pt->init_done   = 1;
    /* read/write/used/is_closed/统计/consume_cb 均被 memset 清零 */

    sb_set_name(pt->name, name);

    *pp = pt;
    return 0;
}

/**
 * @func         StreamBufferAPI_Destroy
 * @brief        销毁流缓冲区，释放所有资源
 * @details      不负责停止用户线程（调用前应 Close + join）。幂等：*pp==NULL 返回 -1。
 */
int StreamBufferAPI_Destroy(T_StreamBuffer **pp)
{
    if(NULL == pp || NULL == *pp)
    {
        return -1;
    }
    if(1 != (*pp)->init_done)
    {
        return -1;
    }

    T_StreamBuffer *pt = *pp;
    free(pt->buffer);
    pthread_mutex_destroy(&pt->mux);
    pthread_cond_destroy(&pt->cond);
    free(pt);
    *pp = NULL;
    return 0;
}

/**
 * @func         StreamBufferAPI_Close
 * @brief        关闭（阻止写入 + broadcast 唤醒所有等待者）；幂等
 */
int StreamBufferAPI_Close(T_StreamBuffer *p)
{
    if(NULL == p || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    p->is_closed = 1;
    pthread_cond_broadcast(&p->cond);   /* 唤醒所有阻塞在 Wait 的线程 */
    pthread_mutex_unlock(&p->mux);
    return 0;
}

/**
 * @func         StreamBufferAPI_Reopen
 * @brief        重新打开已关闭的队列（Close 可逆），不清空缓冲
 */
int StreamBufferAPI_Reopen(T_StreamBuffer *p)
{
    if(NULL == p || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    if(!p->is_closed)
    {
        printf("WARNING: StreamBuffer %s is not closed, no need to reopen ##%s->%d\n",
               p->name, __FUNCTION__, __LINE__);
        pthread_mutex_unlock(&p->mux);
        return -1;
    }
    p->is_closed = 0;
    pthread_mutex_unlock(&p->mux);
    printf("StreamBuffer %s reopened ##%s->%d\n", p->name, __FUNCTION__, __LINE__);
    return 0;
}

/**
 * @func         StreamBufferAPI_IsClosed
 * @brief        查询是否已关闭（加锁读，保证多核可见性）
 */
int StreamBufferAPI_IsClosed(T_StreamBuffer *p)
{
    int c;
    if(NULL == p || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    c = p->is_closed;
    pthread_mutex_unlock(&p->mux);
    return c ? 1 : 0;
}


/* ========================== 写入 ========================== */

/**
 * @func         StreamBufferAPI_PutData
 * @brief        写入一段字节流（不阻塞，满则丢新，跨回绕两段 memcpy）
 */
int StreamBufferAPI_PutData(T_StreamBuffer *p, const char *buf, int len)
{
    int free_space;
    int first;

    /* ---- 参数合法性检查 ---- */
    if(NULL == p)
    {
        printf("NULL == p fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(NULL == buf)
    {
        printf("NULL == buf fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(!p->init_done)
    {
        printf("WARNING!! %p Not Init ##%s->%d\n", (void *)p, __FUNCTION__, __LINE__);
        return -1;
    }
    if(len <= 0)
    {
        printf("len <= 0 fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }

    pthread_mutex_lock(&p->mux);

    /* 关闭检查 */
    if(p->is_closed)
    {
        pthread_mutex_unlock(&p->mux);
        printf("StreamBuffer %s is closed, cannot put ##%s->%d\n",
               p->name, __FUNCTION__, __LINE__);
        return -2;
    }

    /* 超容量截断 */
    {
        int orig_len = len;
        if(len > p->capacity)
        {
            len = p->capacity;
        }

        /* 满则丢新：dropped 记原始长度（用户实际想写的量，非截断后） */
        free_space = p->capacity - p->used;
        if(free_space < len)
        {
            p->dropped += (unsigned long)orig_len;
            pthread_mutex_unlock(&p->mux);
            return -3;
        }
    }

    /* 写入：跨回绕分两段 memcpy */
    first = len;
    if(p->write + len > p->capacity)
    {
        first = p->capacity - p->write;     /* 第一段：write 到尾 */
    }
    memcpy(p->buffer + p->write, buf, (size_t)first);
    if(first < len)
    {
        memcpy(p->buffer + 0, buf + first, (size_t)(len - first));  /* 第二段：从头 */
    }
    p->write = (p->write + len) & p->mask;   /* & mask 回绕 */
    p->used += len;
    p->total_put += (unsigned long)len;
    if(p->used > p->peak_used)
    {
        p->peak_used = p->used;
    }

    /* 达阈值 signal 唤醒一个等待者 */
    if(p->used >= p->flush_bytes)
    {
        pthread_cond_signal(&p->cond);
    }

    pthread_mutex_unlock(&p->mux);
    return len;
}


/* ========================== 消费 ========================== */

/**
 * @func         StreamBufferAPI_Wait
 * @brief        阻塞等待出队触发条件（含零拷贝回调消费 + 状态重算）
 */
int StreamBufferAPI_Wait(T_StreamBuffer *p, int timeo, int *used_out)
{
    int timed_out = 0;
    int status;

    if(NULL == p)
    {
        return STREAMBUFFER_STATUS_INVALID;     /* -3 */
    }
    if(!p->init_done)
    {
        return STREAMBUFFER_STATUS_NOINIT;      /* -4 */
    }
    if(timeo < 0)
    {
        return STREAMBUFFER_STATUS_INVALID;     /* -3 */
    }

    pthread_mutex_lock(&p->mux);

    /* ---- 阻塞等待（timeo>0）：used≥阈值 或 关闭 或 超时 ---- */
    if(timeo > 0)
    {
        struct timespec ts;
        sb_calc_deadline(&ts, timeo);     /* 循环外算一次绝对 deadline，循环内复用（避免反复刷新致永不超时） */
        while(p->used < p->flush_bytes && !p->is_closed)
        {
            int r = pthread_cond_timedwait(&p->cond, &p->mux, &ts);
            if(r == ETIMEDOUT)
            {
                timed_out = 1;
                break;
            }
            /* r==0: 被 signal(PutData/Flush)/broadcast(Close) 唤醒，循环回到 while 重新检查 */
        }
    }
    /* timeo==0: 不等待，直接计算当前状态 */

    /* ---- 计算触发状态码 ---- */
    if(p->is_closed)
    {
        status = (p->used > 0) ? STREAMBUFFER_STATUS_CLOSE_DATA : STREAMBUFFER_STATUS_CLOSE_EMPTY;
    }
    else if(p->used >= p->flush_bytes)
    {
        status = STREAMBUFFER_STATUS_TRIGGER;       /* 达阈值 */
    }
    else if(timeo == 0)
    {
        /* 立即返回当前状态 */
        status = (p->used > 0) ? STREAMBUFFER_STATUS_TRIGGER : STREAMBUFFER_STATUS_TIMEOUT_EMPTY;
    }
    else if(timed_out)
    {
        status = (p->used > 0) ? STREAMBUFFER_STATUS_TIMEOUT_DATA : STREAMBUFFER_STATUS_TIMEOUT_EMPTY;
    }
    else
    {
        /* 被 signal 唤醒但未达阈值、未关闭、未超时 → Flush 唤醒 */
        status = (p->used > 0) ? STREAMBUFFER_STATUS_TRIGGER : STREAMBUFFER_STATUS_FLUSH_EMPTY;
    }

    /* ---- 零拷贝回调消费（注册了回调 且 本次有数据 status>0 且 used>0）---- */
    if(p->consume_cb != NULL && status > 0 && p->used > 0)
    {
        int total = 0;
        /* 第一段：read 到回绕点或 write */
        int seg1 = p->used;
        if(p->read + seg1 > p->capacity)
        {
            seg1 = p->capacity - p->read;
        }
        if(seg1 > 0)
        {
            int c1 = p->consume_cb((StreamBufferStatus)status,
                                   (const char *)(p->buffer + p->read),
                                   seg1, p->cb_ctx);
            c1 = sb_clamp_ret(c1, seg1, p->name);
            total += c1;
            /* 仅当第一段被全消费，才回调第二段（从头） */
            if(c1 == seg1 && (p->used - total) > 0)
            {
                int seg2 = p->used - total;
                int c2 = p->consume_cb((StreamBufferStatus)status,
                                       (const char *)(p->buffer + 0),
                                       seg2, p->cb_ctx);
                c2 = sb_clamp_ret(c2, seg2, p->name);
                total += c2;
            }
        }
        /* 偏移推进 read */
        if(total > 0)
        {
            p->read = (p->read + total) & p->mask;
            p->used -= total;
            p->consumed += (unsigned long)total;
        }
        /* 按剩余 used + 关闭状态，重新计算返回码 */
        if(p->is_closed)
        {
            status = (p->used > 0) ? STREAMBUFFER_STATUS_CLOSE_DATA : STREAMBUFFER_STATUS_CLOSE_EMPTY;
        }
        else
        {
            status = (p->used > 0) ? STREAMBUFFER_STATUS_TRIGGER : STREAMBUFFER_STATUS_TIMEOUT_EMPTY;
        }
    }

    if(used_out != NULL)
    {
        *used_out = p->used;
    }
    pthread_mutex_unlock(&p->mux);
    return status;
}

/**
 * @func         StreamBufferAPI_GetData
 * @brief        非阻塞出队（拷贝式，合并回绕两段为连续输出）
 */
int StreamBufferAPI_GetData(T_StreamBuffer *p, char *buf, int max)
{
    int n, seg1;

    if(NULL == p || NULL == buf || !p->init_done)
    {
        return -1;
    }
    if(max <= 0)
    {
        printf("max <= 0 fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }

    pthread_mutex_lock(&p->mux);

    n = p->used;
    if(n > max)
    {
        n = max;
    }
    if(n > 0)
    {
        seg1 = n;
        if(p->read + n > p->capacity)
        {
            seg1 = p->capacity - p->read;
        }
        memcpy(buf, p->buffer + p->read, (size_t)seg1);
        if(seg1 < n)
        {
            memcpy(buf + seg1, p->buffer + 0, (size_t)(n - seg1));
        }
        p->read = (p->read + n) & p->mask;
        p->used -= n;
        p->consumed += (unsigned long)n;
    }

    pthread_mutex_unlock(&p->mux);
    return n;
}

/**
 * @func         StreamBufferAPI_GetDataAddress
 * @brief        非阻塞零拷贝出队（输出本段地址，不合并回绕）
 */
int StreamBufferAPI_GetDataAddress(T_StreamBuffer *p, char **out_buf, int max)
{
    int seg, consume;

    if(NULL == p || NULL == out_buf || !p->init_done)
    {
        return -1;
    }
    if(max <= 0)
    {
        printf("max <= 0 fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }

    pthread_mutex_lock(&p->mux);

    if(p->used <= 0)
    {
        *out_buf = NULL;
        pthread_mutex_unlock(&p->mux);
        return 0;
    }

    /* 本段连续长度：read 到回绕点 */
    seg = p->used;
    if(p->read + seg > p->capacity)
    {
        seg = p->capacity - p->read;
    }
    consume = (seg > max) ? max : seg;

    *out_buf = (char *)(p->buffer + p->read);
    p->read = (p->read + consume) & p->mask;
    p->used -= consume;
    p->consumed += (unsigned long)consume;

    pthread_mutex_unlock(&p->mux);
    return consume;
}

/**
 * @func         StreamBufferAPI_Flush
 * @brief        signal 唤醒一个等待者（不等阈值/超时）
 */
int StreamBufferAPI_Flush(T_StreamBuffer *p)
{
    if(NULL == p || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mux);
    return 0;
}

/**
 * @func         StreamBufferAPI_SetConsumeCallback
 * @brief        注册/取消零拷贝消费回调
 */
int StreamBufferAPI_SetConsumeCallback(T_StreamBuffer *p, StreamBufferConsumeCb cb, void *user_ctx)
{
    if(NULL == p || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    p->consume_cb = cb;
    p->cb_ctx     = user_ctx;
    pthread_mutex_unlock(&p->mux);
    return 0;
}


/* ========================== 查询与统计 ========================== */

/**
 * @func         StreamBufferAPI_GetLength
 * @brief        获取当前未消费字节数 used
 */
int StreamBufferAPI_GetLength(T_StreamBuffer *p)
{
    int n;
    if(NULL == p || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    n = p->used;
    pthread_mutex_unlock(&p->mux);
    return n;
}

/**
 * @func         StreamBufferAPI_StatsGet
 * @brief        获取运行统计信息
 */
int StreamBufferAPI_StatsGet(T_StreamBuffer *p, T_StreamBufferStats *st)
{
    if(NULL == p || NULL == st || !p->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&p->mux);
    st->ulTotalPut  = p->total_put;
    st->ulDropped   = p->dropped;
    st->ulConsumed  = p->consumed;
    st->iPeakUsed   = p->peak_used;
    pthread_mutex_unlock(&p->mux);
    return 0;
}
