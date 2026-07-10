/**
 * @file        WindowQueue.c
 * @brief       LinuxARM-PublicLib-滑动窗口队列-核心实现文件
 * @details     IMX6ULL平台
 *              本文件实现滑动窗口队列（WindowQueue）的全部公共API。
 *              特性：有界环形缓冲区、值拷贝预分配、满则丢弃最老（零拷贝 O(1)）、
 *              只读窗口访问（Snapshot/ForEach）、入队回调（Push）、动态 Resize、运行统计。
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-09
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-09
 * @Version     V1.0.0
 * @brief       创建文件，实现滑动窗口队列全套API
 * @author      zlzksrl
 */
#include "../include/WindowQueue.h"
#include "WindowQueue_Main.h"
#include "WindowQueue_Maketime.h"


/* ========================== 内部辅助函数 ========================== */

/**
 * @func         wq_set_name
 * @brief        拷贝队列名称到固定缓冲区（带截断保护）
 * @details      缓冲区大小为 MAX_WINDOWQUEUENAME_LEN+1，可存 MAX 字符 + '\0'。
 */
static void wq_set_name(char *dst, const char *src)
{
    size_t len = strlen(src);
    if(len < MAX_WINDOWQUEUENAME_LEN)
    {
        strncpy(dst, src, len);
        dst[len] = '\0';
    }
    else
    {
        strncpy(dst, src, MAX_WINDOWQUEUENAME_LEN);
        dst[MAX_WINDOWQUEUENAME_LEN] = '\0';
    }
}


/* ========================== 生命周期管理 ========================== */

/**
 * @func         WindowQueueAPI_Init
 * @brief        初始化滑动窗口队列
 */
int WindowQueueAPI_Init(T_WindowQueueMsg **ppt_QueueMsg,
                        int iQueueLen, int iElementSize, const char *sQueueName)
{
    /* ---- 参数合法性检查 ---- */
    if(NULL == ppt_QueueMsg)
    {
        printf("NULL == ppt_QueueMsg fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(NULL != *ppt_QueueMsg)
    {
        printf("NULL != *ppt_QueueMsg fail (already init?) ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(NULL == sQueueName)
    {
        printf("NULL == sQueueName fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(iQueueLen <= 0)
    {
        printf("iQueueLen <= 0 fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(iElementSize <= 0)
    {
        printf("iElementSize <= 0 fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }

    /* ---- 打印库版本信息 ---- */
    printf("WindowQueueLibVision = [%s]\n", WindowQueue_PROJECT_MAKETIME);

    /* ---- 分配结构体 ---- */
    T_WindowQueueMsg *pt = (T_WindowQueueMsg *)malloc(sizeof(T_WindowQueueMsg));
    if(NULL == pt)
    {
        printf("malloc fail struct ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    memset(pt, 0, sizeof(T_WindowQueueMsg));

    /* ---- 32位平台乘法溢出守卫 ---- */
    if((size_t)iElementSize > SIZE_MAX / (size_t)iQueueLen ||
       sizeof(const void *) > SIZE_MAX / (size_t)iQueueLen)
    {
        printf("size overflow (queueLen*elementSize) fail ##%s->%d\n", __FUNCTION__, __LINE__);
        free(pt);
        return -1;
    }

    /* ---- 预分配环形缓冲区（值拷贝连续内存）---- */
    pt->buffer = (unsigned char *)malloc((size_t)iQueueLen * (size_t)iElementSize);
    if(NULL == pt->buffer)
    {
        printf("malloc fail buffer ##%s->%d\n", __FUNCTION__, __LINE__);
        free(pt);
        return -1;
    }

    /* ---- 预分配入队回调视图（指针数组）---- */
    pt->view = (const void **)malloc((size_t)iQueueLen * sizeof(const void *));
    if(NULL == pt->view)
    {
        printf("malloc fail view ##%s->%d\n", __FUNCTION__, __LINE__);
        free(pt->buffer);
        free(pt);
        return -1;
    }

    /* ---- 初始化参数 ---- */
    pt->size         = iQueueLen;
    pt->element_size = iElementSize;
    pthread_mutex_init(&pt->mux, NULL);
    pt->init_done    = 1;
    /* is_closed/nData/lget/lput/统计 均已被 memset 清零 */

    wq_set_name(pt->name, sQueueName);

    *ppt_QueueMsg = pt;
    return 0;
}

/**
 * @func         WindowQueueAPI_Destroy
 * @brief        销毁队列，释放所有资源
 * @details      值拷贝模式下数据在预分配内存中，整体 free 即可。
 *               调用者需确保无其他线程正在访问（建议先 Close 并 join 相关线程）。
 */
int WindowQueueAPI_Destroy(T_WindowQueueMsg **ppt_QueueMsg)
{
    if(NULL == ppt_QueueMsg || NULL == *ppt_QueueMsg)
    {
        return -1;
    }
    if(1 != (*ppt_QueueMsg)->init_done)
    {
        return -1;
    }

    T_WindowQueueMsg *pt = *ppt_QueueMsg;
    free(pt->buffer);
    free(pt->view);
    pthread_mutex_destroy(&pt->mux);
    pt->init_done = 0;
    free(pt);
    *ppt_QueueMsg = NULL;
    return 0;
}

/**
 * @func         WindowQueueAPI_Close
 * @brief        关闭队列，阻止新数据写入
 * @details      本队列无条件变量，仅设置标志。关闭后 Snapshot/ForEach 等只读操作仍可用。
 */
int WindowQueueAPI_Close(T_WindowQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg || !pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);
    pt_QueueMsg->is_closed = 1;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return 0;
}

/**
 * @func         WindowQueueAPI_Reopen
 * @brief        重新打开已关闭的队列
 * @details      仅重置 is_closed。窗口中残留数据保持不变（不清空）。
 */
int WindowQueueAPI_Reopen(T_WindowQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg || !pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);
    if(!pt_QueueMsg->is_closed)
    {
        printf("WARNING: WindowQueue %s is not closed, no need to reopen ##%s->%d\n",
               pt_QueueMsg->name, __FUNCTION__, __LINE__);
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        return -1;
    }
    pt_QueueMsg->is_closed = 0;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    printf("WindowQueue %s reopened ##%s->%d\n",
           pt_QueueMsg->name, __FUNCTION__, __LINE__);
    return 0;
}

/**
 * @func         WindowQueueAPI_IsClosed
 * @brief        查询队列是否已关闭（互斥锁保护读取，保证多核可见性）
 */
int WindowQueueAPI_IsClosed(T_WindowQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg || !pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);
    int closed = pt_QueueMsg->is_closed;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return closed ? 1 : 0;
}


/* ========================== 写入与窗口访问 ========================== */

/**
 * @func         WindowQueueAPI_Put
 * @brief        写入一条数据（永不阻塞，满则丢最老）
 */
int WindowQueueAPI_Put(T_WindowQueueMsg *pt_QueueMsg, const void *data)
{
    /* ---- 参数合法性检查 ---- */
    if(NULL == pt_QueueMsg)
    {
        printf("NULL == pt_QueueMsg fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(NULL == data)
    {
        printf("NULL == data fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        printf("WARNING!! %p Not Init ##%s->%d\n", (void *)pt_QueueMsg, __FUNCTION__, __LINE__);
        return -1;
    }

    pthread_mutex_lock(&pt_QueueMsg->mux);

    /* 关闭检查 */
    if(pt_QueueMsg->is_closed)
    {
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        printf("WindowQueue %s is closed, cannot put ##%s->%d\n",
               pt_QueueMsg->name, __FUNCTION__, __LINE__);
        return -2;
    }

    int discarded = 0;

    /* 队列满：丢弃最老一条（移动读指针，零拷贝） */
    if(pt_QueueMsg->nData >= pt_QueueMsg->size)
    {
        pt_QueueMsg->lget = (pt_QueueMsg->lget + 1) % pt_QueueMsg->size;
        pt_QueueMsg->nData--;
        discarded = 1;
        pt_QueueMsg->ulTotalDiscarded++;
    }

    /* 写入新数据到 lput 位置（值拷贝） */
    memcpy(pt_QueueMsg->buffer + (size_t)pt_QueueMsg->lput * pt_QueueMsg->element_size,
           data, (size_t)pt_QueueMsg->element_size);
    pt_QueueMsg->lput = (pt_QueueMsg->lput + 1) % pt_QueueMsg->size;
    pt_QueueMsg->nData++;
    pt_QueueMsg->ulTotalPut++;

    /* 更新峰值长度 */
    if(pt_QueueMsg->nData > pt_QueueMsg->iPeakLength)
    {
        pt_QueueMsg->iPeakLength = pt_QueueMsg->nData;
    }

    /* 入队回调（锁内零拷贝）：构建窗口指针视图后调用 */
    if(pt_QueueMsg->put_callback != NULL)
    {
        int i;
        for(i = 0; i < pt_QueueMsg->nData; i++)
        {
            int idx = (pt_QueueMsg->lget + i) % pt_QueueMsg->size;
            pt_QueueMsg->view[i] = pt_QueueMsg->buffer + (size_t)idx * pt_QueueMsg->element_size;
        }
        pt_QueueMsg->put_callback(pt_QueueMsg->view, pt_QueueMsg->nData, pt_QueueMsg->put_cb_ctx);
    }

    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return discarded;
}

/**
 * @func         WindowQueueAPI_Snapshot
 * @brief        快照拷贝最新 N 条数据（只读，不消费）
 * @details      取最新的 min(max_count, nData) 条，按 老->新 拷贝到 out_buf。
 */
int WindowQueueAPI_Snapshot(T_WindowQueueMsg *pt_QueueMsg,
                            void *out_buf, int max_count)
{
    if(NULL == pt_QueueMsg || NULL == out_buf)
    {
        return -1;
    }
    if(max_count <= 0)
    {
        printf("max_count <= 0 fail ##%s->%d\n", __FUNCTION__, __LINE__);
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        printf("WARNING!! %p Not Init ##%s->%d\n", (void *)pt_QueueMsg, __FUNCTION__, __LINE__);
        return -1;
    }

    pthread_mutex_lock(&pt_QueueMsg->mux);

    int m = max_count;
    if(m > pt_QueueMsg->nData)
    {
        m = pt_QueueMsg->nData;
    }
    /* 最新 m 条：窗口内偏移从 (nData - m) 开始 */
    int base = pt_QueueMsg->nData - m;
    int i;
    for(i = 0; i < m; i++)
    {
        int idx = (pt_QueueMsg->lget + base + i) % pt_QueueMsg->size;
        memcpy((unsigned char *)out_buf + (size_t)i * pt_QueueMsg->element_size,
               pt_QueueMsg->buffer + (size_t)idx * pt_QueueMsg->element_size,
               (size_t)pt_QueueMsg->element_size);
    }

    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return m;
}

/**
 * @func         WindowQueueAPI_ForEach
 * @brief        回调遍历整个窗口（只读，零拷贝）
 * @details      按 老->新 顺序，锁内逐条调用 callback。回调须快速返回，禁调本队列 API。
 */
int WindowQueueAPI_ForEach(T_WindowQueueMsg *pt_QueueMsg,
                           WindowQueueDataCb callback, void *user_ctx)
{
    if(NULL == pt_QueueMsg || NULL == callback)
    {
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        printf("WARNING!! %p Not Init ##%s->%d\n", (void *)pt_QueueMsg, __FUNCTION__, __LINE__);
        return -1;
    }

    pthread_mutex_lock(&pt_QueueMsg->mux);
    int i;
    for(i = 0; i < pt_QueueMsg->nData; i++)
    {
        int idx = (pt_QueueMsg->lget + i) % pt_QueueMsg->size;
        callback(pt_QueueMsg->buffer + (size_t)idx * pt_QueueMsg->element_size, i, user_ctx);
    }
    int n = pt_QueueMsg->nData;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return n;
}


/* ========================== 容量与查询 ========================== */

/**
 * @func         WindowQueueAPI_Resize
 * @brief        动态调整队列容量
 * @details      重新分配 new_size 缓冲区，按 FIFO 顺序搬移现有数据。
 *               变大全部保留；变小从最老端丢弃多余。view 同步扩缩。原子替换。
 */
int WindowQueueAPI_Resize(T_WindowQueueMsg *pt_QueueMsg, int new_size)
{
    if(NULL == pt_QueueMsg || new_size <= 0)
    {
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        return -1;
    }

    pthread_mutex_lock(&pt_QueueMsg->mux);

    int es = pt_QueueMsg->element_size;

    /* 先分配新 buffer 与新 view，成功后才替换（原子） */
    if((size_t)es > SIZE_MAX / (size_t)new_size ||
       sizeof(const void *) > SIZE_MAX / (size_t)new_size)
    {
        printf("size overflow in Resize ##%s->%d\n", __FUNCTION__, __LINE__);
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        return -1;
    }
    unsigned char *newbuf = (unsigned char *)malloc((size_t)new_size * (size_t)es);
    const void   **newview = (const void **)malloc((size_t)new_size * sizeof(const void *));
    if(NULL == newbuf || NULL == newview)
    {
        printf("malloc fail in Resize ##%s->%d\n", __FUNCTION__, __LINE__);
        free(newbuf);
        free(newview);
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        return -1;
    }

    /* 决定搬移条数：变大全搬，变小丢弃最老多余条 */
    int move = pt_QueueMsg->nData;
    if(move > new_size)
    {
        int drop = pt_QueueMsg->nData - new_size;
        /* 缩容丢弃不计入 ulTotalDiscarded（那是"满则丢老"丢包率，非用户主动缩容）*/
        pt_QueueMsg->lget = (pt_QueueMsg->lget + drop) % pt_QueueMsg->size;
        move = new_size;
    }

    /* 搬移 move 条（老->新）到 newbuf 开头（连续） */
    int i;
    for(i = 0; i < move; i++)
    {
        int idx = (pt_QueueMsg->lget + i) % pt_QueueMsg->size;
        memcpy(newbuf + (size_t)i * es,
               pt_QueueMsg->buffer + (size_t)idx * es,
               (size_t)es);
    }

    /* 原子替换 */
    free(pt_QueueMsg->buffer);
    free(pt_QueueMsg->view);
    pt_QueueMsg->buffer = newbuf;
    pt_QueueMsg->view   = newview;
    pt_QueueMsg->size   = new_size;
    pt_QueueMsg->lget   = 0;
    pt_QueueMsg->lput   = move % new_size;   /* move==new_size 时满态 lput=0=lget */
    pt_QueueMsg->nData  = move;

    pthread_mutex_unlock(&pt_QueueMsg->mux);
    printf("WindowQueue %s resized to %d (moved %d) ##%s->%d\n",
           pt_QueueMsg->name, new_size, move, __FUNCTION__, __LINE__);
    return 0;
}

/**
 * @func         WindowQueueAPI_GetLength
 * @brief        获取当前窗口数据条数
 */
int WindowQueueAPI_GetLength(T_WindowQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg || !pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);
    int n = pt_QueueMsg->nData;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return n;
}

/**
 * @func         WindowQueueAPI_GetCapacity
 * @brief        获取队列容量
 */
int WindowQueueAPI_GetCapacity(T_WindowQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg || !pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);
    int s = pt_QueueMsg->size;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return s;
}


/* ========================== 刷新 ========================== */

/**
 * @func         WindowQueueAPI_Flush
 * @brief        刷新队列：回调处理窗口数据并清空
 * @details      callback 为 NULL 时仅清空。回调在锁内执行。
 */
int WindowQueueAPI_Flush(T_WindowQueueMsg *pt_QueueMsg,
                         WindowQueueDataCb callback, void *user_ctx)
{
    if(NULL == pt_QueueMsg || !pt_QueueMsg->init_done)
    {
        return -1;
    }

    pthread_mutex_lock(&pt_QueueMsg->mux);
    int cnt = pt_QueueMsg->nData;
    if(callback != NULL)
    {
        int i;
        for(i = 0; i < cnt; i++)
        {
            int idx = (pt_QueueMsg->lget + i) % pt_QueueMsg->size;
            callback(pt_QueueMsg->buffer + (size_t)idx * pt_QueueMsg->element_size,
                     i, user_ctx);
        }
    }
    /* 清空 */
    pt_QueueMsg->lget  = 0;
    pt_QueueMsg->lput  = 0;
    pt_QueueMsg->nData = 0;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return cnt;
}


/* ========================== 运行统计 ========================== */

/**
 * @func         WindowQueueAPI_StatsGet
 * @brief        获取运行统计信息
 */
int WindowQueueAPI_StatsGet(T_WindowQueueMsg *pt_QueueMsg, T_WindowQueueStats *pt_Stats)
{
    if(NULL == pt_QueueMsg || NULL == pt_Stats)
    {
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);
    pt_Stats->ulTotalPut       = pt_QueueMsg->ulTotalPut;
    pt_Stats->ulTotalDiscarded = pt_QueueMsg->ulTotalDiscarded;
    pt_Stats->iPeakLength      = pt_QueueMsg->iPeakLength;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return 0;
}


/* ========================== 入队回调 ========================== */

/**
 * @func         WindowQueueAPI_SetPutCallback
 * @brief        注册/取消入队回调
 * @details      注册后每次成功 Put 在锁内构建窗口视图并调用 cb；传 NULL 取消。
 */
int WindowQueueAPI_SetPutCallback(T_WindowQueueMsg *pt_QueueMsg,
                                  WindowQueuePutCb cb, void *user_ctx)
{
    if(NULL == pt_QueueMsg || !pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);
    pt_QueueMsg->put_callback = cb;
    pt_QueueMsg->put_cb_ctx   = user_ctx;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return 0;
}
