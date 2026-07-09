/**
 * @file        ThreadQueue_Main.c
 * @brief       LinuxARM-PublicLib-线程通讯队列-核心实现文件
 * @details     IMX6ULL平台
 *              本文件实现了两种线程安全队列:
 *              1. ThreadQueue - 环形缓冲区队列（FIFO）
 *              2. LatestQueue - 最新数据队列（只保留最新一条）
 *
 * @author      zlzksrl
 * @Version     V1.1.0
 * @date        2026-05-07
 * @copyright   copyright (C) 2024
 */

/**
 * @date        2024-08-15
 * @Version     V1.0.0
 * @brief       创建文件
 * @author      zlzksrl
 *
 * @date        2026-05-07
 * @Version     V1.1.0
 * @brief       新增 Close/Reopen/IsClosed/GetLength/Flush 队列管理函数;
 *              新增 LatestQueue 最新数据队列全套实现;
 *              修复 ThreadQueueAPI_DestroyMsg 等待线程退出时的锁安全问题;
 *              添加详细内联注释
 * @author      zlzksrl
 */
#include "../include/ThreadQueue.h"
#include "ThreadQueue_Main.h"
#include "ThreadQueue_Maketime.h"


/* ================================================================== */
/*                                                                    */
/*     ThreadQueue - 环形缓冲区线程安全队列 实现                        */
/*                                                                    */
/* ================================================================== */

/**
 * @func         ThreadQueueAPI_InitMsg
 * @brief        初始化线程队列
 * @details      分配 T_ThreadQueueMsg 结构体和 void* 环形缓冲区，
 *               初始化互斥锁和条件变量，设置队列名称，注册数据释放回调。
 *               调用成功后队列处于"已初始化、未关闭"状态。
 *
 * @param[in]    ppt_QueueMsg      二级指针，用于返回创建的队列对象
 * @param[in]    iQueueLen         环形缓冲区容量（void* 元素个数）
 * @param[in]    sQueueName        队列名称，用于日志输出
 * @param[in]    release_callback  数据释放回调函数，Destroy 时自动释放残留数据，可为 NULL
 * @return       0成功, -1失败
 */
int ThreadQueueAPI_InitMsg(T_ThreadQueueMsg **ppt_QueueMsg, int iQueueLen, const char *sQueueName, void (*release_callback)(void* data))
{
    /* ---- 参数合法性检查 ---- */
    if(NULL == ppt_QueueMsg)
    {
        printf("NULL == ppt_QueueMsg fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(NULL != *ppt_QueueMsg)  /* 防止重复初始化或覆盖已有队列 */
    {
        printf("NULL != *ppt_QueueMsg fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(NULL == sQueueName)
    {
        printf("NULL == sQueueName fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(iQueueLen <= 0)
    {
        printf("iQueueLen <= 0 fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }

    /* ---- 打印库版本信息 ---- */
    printf("ThreadQueueLibVision  = [%s]\r\n",ThreadQueue_PROJECT_MAKETIME);

    /* ---- 分配结构体内存 ---- */
    T_ThreadQueueMsg *pt_QueueMsg = (T_ThreadQueueMsg *)malloc(sizeof(T_ThreadQueueMsg));
    if(NULL == pt_QueueMsg)
    {
        printf("malloc fail Msg! ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    memset(pt_QueueMsg, 0, sizeof(T_ThreadQueueMsg));  /* 全部字段清零 */

    /* ---- 分配环形缓冲区 ---- */
    pt_QueueMsg->buffer = malloc(iQueueLen * sizeof(void*));
    if(NULL == pt_QueueMsg->buffer)
    {
        printf("malloc fail buffer! ##%s->%d\r\n",__FUNCTION__,__LINE__);
        free(pt_QueueMsg);  /* 释放已分配的结构体 */
        return -1;
    }

    /* ---- 初始化队列参数 ---- */
    pt_QueueMsg->size = iQueueLen;   /* 环形缓冲区容量 */
    pthread_mutex_init(&pt_QueueMsg->mux, 0);       /* 初始化互斥锁 */
    pthread_cond_init(&pt_QueueMsg->cond_get, 0);   /* 初始化消费者条件变量 */
    pthread_cond_init(&pt_QueueMsg->cond_put, 0);   /* 初始化生产者条件变量 */
    pt_QueueMsg->release_callback = release_callback;  /* 注册数据释放回调 */
    pt_QueueMsg->init_done = 1;      /* 标记初始化完成 */
    /* is_closed 已被 memset 清零为 0（未关闭） */

    /* ---- 设置队列名称（截断保护） ---- */
    size_t iNameLen = strlen(sQueueName);
    if(iNameLen < MAX_THREADQUEUENAME_LEN)
    {
        strncpy(pt_QueueMsg->name, sQueueName, iNameLen);
        pt_QueueMsg->name[iNameLen] = '\0';
    }
    else
    {
        strncpy(pt_QueueMsg->name, sQueueName, MAX_THREADQUEUENAME_LEN - 1);
        pt_QueueMsg->name[MAX_THREADQUEUENAME_LEN - 1] = '\0';
    }

    /* ---- 返回队列对象 ---- */
    *ppt_QueueMsg = pt_QueueMsg;
    return 0;
}

/**
 * @func         ThreadQueueAPI_PutMsg
 * @brief        向线程队列发送消息（生产者调用）
 * @details      加锁后检查关闭状态，若队列满则阻塞在 cond_put 等待消费者取走数据。
 *               被唤醒后再次检查关闭状态。写入成功后唤醒一个等待的消费者。
 *
 * @param[in]    pt_QueueMsg  队列结构体指针
 * @param[in]    data         待发送的数据指针
 * @return       0成功, -1参数无效, -2队列已关闭
 */
int ThreadQueueAPI_PutMsg(T_ThreadQueueMsg *pt_QueueMsg, void *data)
{
    /* ---- 参数合法性检查（无需加锁） ---- */
    if(NULL == pt_QueueMsg)
    {
        printf("NULL == pt_QueueMsg fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(NULL == data)
    {
        printf("NULL == data fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        printf("WARNNING!!!!!%p Not Init ##%s->%d\r\n",pt_QueueMsg,__FUNCTION__,__LINE__);
        return -1;
    }

    /* ---- 加锁进入临界区 ---- */
    pthread_mutex_lock(&pt_QueueMsg->mux);

    /* 检查队列是否已关闭（在锁内，保证与 Close 的互斥） */
    if(pt_QueueMsg->is_closed)
    {
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        printf("Queue %s is closed, cannot put data ##%s->%d\r\n",
               pt_QueueMsg->name, __FUNCTION__, __LINE__);
        return -2;  /* 特定错误码: 队列已关闭 */
    }

    /* 队列满判断: lget==lput 且 nData>0 表示环形缓冲区已满 */
    while(pt_QueueMsg->lget == pt_QueueMsg->lput && pt_QueueMsg->nData)
    {
        pt_QueueMsg->nFullThread++;  /* 增加等待计数（在锁内，原子操作） */
        /* pthread_cond_wait 会原子性地释放 mux 并进入等待，
           被唤醒时重新获取 mux 后返回 */
        pthread_cond_wait(&pt_QueueMsg->cond_put, &pt_QueueMsg->mux);
        pt_QueueMsg->nFullThread--;  /* 被唤醒，减少等待计数 */

        /* 被唤醒后再次检查是否被关闭（Close 会 broadcast 唤醒） */
        if(pt_QueueMsg->is_closed)
        {
            pthread_mutex_unlock(&pt_QueueMsg->mux);
            return -2;
        }
    }

    /* ---- 写入数据到环形缓冲区 ---- */
    (pt_QueueMsg->buffer)[pt_QueueMsg->lput++] = data;
    /* 环形回绕: 写入位置到达末尾时回到 0 */
    if(pt_QueueMsg->lput == pt_QueueMsg->size)
    {
        pt_QueueMsg->lput = 0;
    }
    pt_QueueMsg->nData++;  /* 数据计数+1 */

    /* 如果有消费者在等待数据，唤醒其中一个 */
    if(pt_QueueMsg->nEmptyThread)
    {
        pthread_cond_signal(&pt_QueueMsg->cond_get);
    }

    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return 0;
}


/**
 * @func         ThreadQueueAPI_PutMsgTimeout
 * @brief        向线程队列发送消息（带超时，生产者调用）
 * @details      与 ThreadQueueAPI_PutMsg 类似，但队列满时不会无限阻塞，
 *               而是使用 pthread_cond_timedwait 带超时等待。
 *               超时后返回 -3，数据未被存入。
 *
 * @param[in]    pt_QueueMsg  队列结构体指针
 * @param[in]    data         待发送的数据指针
 * @param[in]    timeo        超时时间(ms)，队列满时最长等待时间
 * @return       0成功, -1参数无效, -2队列已关闭, -3超时
 */
int ThreadQueueAPI_PutMsgTimeout(T_ThreadQueueMsg *pt_QueueMsg, void *data, int timeo)
{
    /* ---- 参数合法性检查（无需加锁） ---- */
    if(NULL == pt_QueueMsg)
    {
        printf("NULL == pt_QueueMsg fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(NULL == data)
    {
        printf("NULL == data fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        printf("WARNNING!!!!!%p Not Init ##%s->%d\r\n",pt_QueueMsg,__FUNCTION__,__LINE__);
        return -1;
    }
    if(timeo < 0)
    {
        printf("timeo < 0 fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }

    struct timeval now;
    struct timespec outtime;
    long total_us;
    int ret;

    /* ---- 加锁进入临界区 ---- */
    pthread_mutex_lock(&pt_QueueMsg->mux);

    /* 检查队列是否已关闭（在锁内，保证与 Close 的互斥） */
    if(pt_QueueMsg->is_closed)
    {
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        printf("Queue %s is closed, cannot put data ##%s->%d\r\n",
               pt_QueueMsg->name, __FUNCTION__, __LINE__);
        return -2;  /* 特定错误码: 队列已关闭 */
    }

    /* 队列满判断: lget==lput 且 nData>0 表示环形缓冲区已满 */
    while(pt_QueueMsg->lget == pt_QueueMsg->lput && pt_QueueMsg->nData)
    {
        /* 计算绝对超时时间（从当前时间 + timeo 毫秒） */
        pt_QueueMsg->nFullThread++;
        gettimeofday(&now, NULL);
        /* 处理微秒进位: total_us 可能超过 1000000，需要进位到秒 */
        total_us = now.tv_usec + (timeo % 1000) * 1000;
        outtime.tv_sec = now.tv_sec + timeo / 1000 + total_us / 1000000;
        outtime.tv_nsec = (total_us % 1000000) * 1000;

        /* 带超时等待条件变量:
           返回 0: 被唤醒（有空间或被 broadcast）
           返回非0: 超时 */
        ret = pthread_cond_timedwait(&pt_QueueMsg->cond_put, &pt_QueueMsg->mux, &outtime);
        pt_QueueMsg->nFullThread--;

        if(ret)  /* 超时，队列仍然满 */
        {
            pthread_mutex_unlock(&pt_QueueMsg->mux);
            return -3;  /* 特定错误码: 超时 */
        }

        /* 被唤醒后再次检查是否被关闭（Close 会 broadcast 唤醒） */
        if(pt_QueueMsg->is_closed)
        {
            pthread_mutex_unlock(&pt_QueueMsg->mux);
            return -2;
        }
    }

    /* ---- 写入数据到环形缓冲区 ---- */
    (pt_QueueMsg->buffer)[pt_QueueMsg->lput++] = data;
    /* 环形回绕: 写入位置到达末尾时回到 0 */
    if(pt_QueueMsg->lput == pt_QueueMsg->size)
    {
        pt_QueueMsg->lput = 0;
    }
    pt_QueueMsg->nData++;  /* 数据计数+1 */

    /* 如果有消费者在等待数据，唤醒其中一个 */
    if(pt_QueueMsg->nEmptyThread)
    {
        pthread_cond_signal(&pt_QueueMsg->cond_get);
    }

    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return 0;
}


/**
 * @func         ThreadQueueAPI_GetMsg
 * @brief        从线程队列获取消息（消费者调用）
 * @details      加锁后检查队列空状态。若队列为空:
 *               - 已关闭: 直接返回 NULL
 *               - 未关闭: 阻塞在 cond_get 等待生产者写入或超时
 *               取出数据后唤醒一个等待的生产者。
 *
 * @param[in]    pt_QueueMsg  队列结构体指针
 * @param[in]    timeo        超时时间(ms)
 * @return       数据指针或 NULL
 */
void* ThreadQueueAPI_GetMsg(T_ThreadQueueMsg *pt_QueueMsg, int timeo)
{
    /* ---- 参数合法性检查 ---- */
    if(NULL == pt_QueueMsg)
    {
        printf("NULL == pt_QueueMsg fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return NULL;
    }
    if(timeo < 0)
    {
        printf("timeo < 0 fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return NULL;
    }
    if(!pt_QueueMsg->init_done)
    {
        printf("WARNNING!!!!!%p Not Init ##%s->%d\r\n",pt_QueueMsg,__FUNCTION__,__LINE__);
        return NULL;
    }

    void* data = NULL;
    struct timeval now;
    struct timespec outtime;
    int ret;
    long total_us;

    /* ---- 加锁进入临界区 ---- */
    pthread_mutex_lock(&pt_QueueMsg->mux);

    /* 队列空判断: lget==lput 且 nData==0 */
    while(pt_QueueMsg->lget == pt_QueueMsg->lput && 0 == pt_QueueMsg->nData)
    {
        /* 队列已关闭且为空，消费者无需再等，直接返回 */
        if(pt_QueueMsg->is_closed)
        {
            pthread_mutex_unlock(&pt_QueueMsg->mux);
            return NULL;
        }

        /* 计算绝对超时时间（从当前时间 + timeo 毫秒） */
        pt_QueueMsg->nEmptyThread++;
        gettimeofday(&now, NULL);
        /* 处理微秒进位: total_us 可能超过 1000000，需要进位到秒 */
        total_us = now.tv_usec + (timeo % 1000) * 1000;
        outtime.tv_sec = now.tv_sec + timeo / 1000 + total_us / 1000000;
        outtime.tv_nsec = (total_us % 1000000) * 1000;

        /* 带超时等待条件变量:
           返回 0: 被唤醒（有数据或被 broadcast）
           返回非0: 超时 */
        ret = pthread_cond_timedwait(&pt_QueueMsg->cond_get, &pt_QueueMsg->mux, &outtime);
        pt_QueueMsg->nEmptyThread--;

        if(ret)  /* 超时，无数据 */
        {
            pthread_mutex_unlock(&pt_QueueMsg->mux);
            return NULL;
        }
        /* 被唤醒后循环回到 while 条件重新检查队列是否为空 */
    }

    /* ---- 从环形缓冲区取出最早的数据 ---- */
    data = (pt_QueueMsg->buffer)[pt_QueueMsg->lget++];
    if(pt_QueueMsg->lget == pt_QueueMsg->size)
    {
        pt_QueueMsg->lget = 0;
    }
    pt_QueueMsg->nData--;

    /* 如果有生产者在等待空间，唤醒其中一个 */
    if(pt_QueueMsg->nFullThread)
    {
        pthread_cond_signal(&pt_QueueMsg->cond_put);
    }

    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return data;
}


/**
 * @func         ThreadQueueAPI_IsClosed
 * @brief        查询队列是否已关闭（线程安全读取）
 */
int ThreadQueueAPI_IsClosed(T_ThreadQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg)
    {
        return -1;
    }
    /* 直接读取 is_closed，对于 int 类型的原子读取在大多数平台上是安全的 */
    return pt_QueueMsg->is_closed ? 1 : 0;
}

/**
 * @func         ThreadQueueAPI_GetLength
 * @brief        查询队列当前数据长度（互斥锁保护下读取）
 */
int ThreadQueueAPI_GetLength(T_ThreadQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg)
    {
        return -1;
    }
    int length = 0;
    pthread_mutex_lock(&pt_QueueMsg->mux);
    length = pt_QueueMsg->nData;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return length;
}

/**
 * @func         ThreadQueueAPI_CloseMsg
 * @brief        关闭线程队列
 * @details      在锁内设置 is_closed=1，然后 broadcast 唤醒所有阻塞的
 *               生产者(cond_put)和消费者(cond_get)，让它们检测关闭状态并退出。
 */
int ThreadQueueAPI_CloseMsg(T_ThreadQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg)
    {
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);
    pt_QueueMsg->is_closed = 1;
    /* 广播唤醒所有等待线程（生产者和消费者） */
    pthread_cond_broadcast(&pt_QueueMsg->cond_get);
    pthread_cond_broadcast(&pt_QueueMsg->cond_put);
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return 0;
}

/**
 * @func         ThreadQueueAPI_ReopenMsg
 * @brief        重新打开已关闭的线程队列
 * @details      在锁内检查前置条件，通过后重置 is_closed、lget、lput。
 *               注意: nData 不需要重置，因为前置条件要求 nData==0。
 */
int ThreadQueueAPI_ReopenMsg(T_ThreadQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg)
    {
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);

    /* 前置条件1: 队列必须是已关闭状态 */
    if(!pt_QueueMsg->is_closed)
    {
        printf("WARNING: Queue %s is not closed, no need to reopen ##%s->%d\r\n",
               pt_QueueMsg->name, __FUNCTION__, __LINE__);
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        return -1;
    }
    /* 前置条件2: 队列中不能有剩余数据 */
    if(pt_QueueMsg->nData > 0)
    {
        printf("WARNING: Queue %s still has %d data, flush first ##%s->%d\r\n",
               pt_QueueMsg->name, pt_QueueMsg->nData, __FUNCTION__, __LINE__);
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        return -1;
    }
    /* 前置条件3: 不能有等待中的线程 */
    if(pt_QueueMsg->nFullThread > 0 || pt_QueueMsg->nEmptyThread > 0)
    {
        printf("WARNING: Queue %s has waiting threads, cannot reopen ##%s->%d\r\n",
               pt_QueueMsg->name, __FUNCTION__, __LINE__);
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        return -1;
    }

    /* 重置队列状态 */
    pt_QueueMsg->is_closed = 0;
    pt_QueueMsg->lget = 0;
    pt_QueueMsg->lput = 0;

    pthread_mutex_unlock(&pt_QueueMsg->mux);
    printf("Queue %s reopened ##%s->%d\r\n",
           pt_QueueMsg->name, __FUNCTION__, __LINE__);
    return 0;
}

/**
 * @func         ThreadQueueAPI_FlushMsg
 * @brief        刷新线程队列，通过回调处理所有剩余数据
 * @details      在锁内逐条取出数据，取出后短暂释放锁执行回调，
 *               然后重新获取锁继续取下一条。这样设计是为了在回调执行期间
 *               不阻塞其他线程（如正在退出的消费者线程）。
 */
int ThreadQueueAPI_FlushMsg(T_ThreadQueueMsg *pt_QueueMsg, void (*callback)(void* data))
{
    if(NULL == pt_QueueMsg || NULL == callback)
    {
        return -1;
    }
    int flush_count = 0;
    void* data = NULL;

    pthread_mutex_lock(&pt_QueueMsg->mux);
    while(pt_QueueMsg->nData > 0)
    {
        /* 取出一条数据 */
        data = (pt_QueueMsg->buffer)[pt_QueueMsg->lget++];
        if(pt_QueueMsg->lget == pt_QueueMsg->size)
        {
            pt_QueueMsg->lget = 0;
        }
        pt_QueueMsg->nData--;
        flush_count++;

        /* 短暂释放锁，执行回调处理数据 */
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        callback(data);
        pthread_mutex_lock(&pt_QueueMsg->mux);
    }
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return flush_count;
}

/**
 * @func         ThreadQueueAPI_DestroyMsg
 * @brief        销毁线程队列，释放所有资源
 * @details      1. 如果未关闭，自动关闭队列
 *               2. 忙等待所有阻塞线程退出（在锁内读取计数）
 *               3. 释放残留数据（通过 release_callback 逐条释放）
 *               4. 释放环形缓冲区、销毁锁和条件变量、释放结构体
 *               5. 将 *ppt_QueueMsg 置 NULL
 *
 * @note         如果注册了 release_callback，队列中的残留数据会被自动释放；
 *               否则仅打印警告（可能内存泄漏）。建议先调用 ThreadQueueAPI_FlushMsg
 *               以便在销毁前自定义处理残留数据。
 */
int ThreadQueueAPI_DestroyMsg(T_ThreadQueueMsg **ppt_QueueMsg)
{
    if(NULL == ppt_QueueMsg)
    {
        return -1;
    }
    if(NULL == (*ppt_QueueMsg))
    {
        return -1;
    }
    if(1 != (*ppt_QueueMsg)->init_done)
    {
        return -1;
    }

    /* 步骤1: 确保队列已关闭（如果未关闭则自动关闭） */
    if(!(*ppt_QueueMsg)->is_closed)
    {
        printf("WARNING: Destroying queue without closing first ##%s->%d\r\n",
               __FUNCTION__, __LINE__);
        ThreadQueueAPI_CloseMsg(*ppt_QueueMsg);
    }

    /* 步骤2: 等待所有阻塞线程退出
       在锁内读取计数，确保与 cond_wait 中的计数修改同步 */
    {
        pthread_mutex_lock(&(*ppt_QueueMsg)->mux);
        int waiting = (*ppt_QueueMsg)->nFullThread + (*ppt_QueueMsg)->nEmptyThread;
        pthread_mutex_unlock(&(*ppt_QueueMsg)->mux);
        while(waiting > 0)
        {
            usleep(1000);  /* 等待1ms后重新检查 */
            pthread_mutex_lock(&(*ppt_QueueMsg)->mux);
            waiting = (*ppt_QueueMsg)->nFullThread + (*ppt_QueueMsg)->nEmptyThread;
            pthread_mutex_unlock(&(*ppt_QueueMsg)->mux);
        }
    }

    /* 步骤3: 释放残留数据（与 ThreadQueueAPI_Latest_DestroyMsg 行为一致） */
    if((*ppt_QueueMsg)->nData > 0)
    {
        if((*ppt_QueueMsg)->release_callback != NULL)
        {
            /* 通过回调逐条释放残留数据 */
            int i;
            for(i = 0; i < (*ppt_QueueMsg)->nData; i++)
            {
                int idx = ((*ppt_QueueMsg)->lget + i) % (*ppt_QueueMsg)->size;
                (*ppt_QueueMsg)->release_callback((*ppt_QueueMsg)->buffer[idx]);
            }
        }
        else
        {
            printf("WARNING: Destroying queue %s with %d unreleased data items, "
                   "no release_callback registered ##%s->%d\r\n",
                   (*ppt_QueueMsg)->name, (*ppt_QueueMsg)->nData,
                   __FUNCTION__, __LINE__);
        }
    }

    /* 步骤4: 释放所有资源（此时已无线程在等待） */
    free((*ppt_QueueMsg)->buffer);                          /* 释放环形缓冲区 */
    pthread_mutex_destroy(&((*ppt_QueueMsg)->mux));          /* 销毁互斥锁 */
    pthread_cond_destroy(&((*ppt_QueueMsg)->cond_get));      /* 销毁消费者条件变量 */
    pthread_cond_destroy(&((*ppt_QueueMsg)->cond_put));      /* 销毁生产者条件变量 */
    (*ppt_QueueMsg)->init_done = 0;                          /* 标记未初始化 */
    free(*ppt_QueueMsg);                                     /* 释放结构体 */
    *ppt_QueueMsg = NULL;                                    /* 置空指针，防止悬空引用 */
    return 0;
}


/* ================================================================== */
/*                                                                    */
/*     LatestQueue - 最新数据队列 实现                                 */
/*                                                                    */
/* ================================================================== */

/**
 * @func         ThreadQueueAPI_Latest_InitMsg
 * @brief        初始化最新数据队列
 * @details      分配 T_LatestQueueMsg 结构体，初始化互斥锁和条件变量，
 *               注册数据释放回调。不分配缓冲区（仅保存一个 void* 指针）。
 */
int ThreadQueueAPI_Latest_InitMsg(T_LatestQueueMsg **ppt_QueueMsg, const char *sQueueName, void (*release_callback)(void* data))
{
    /* ---- 参数合法性检查 ---- */
    if(NULL == ppt_QueueMsg)
    {
        printf("NULL == ppt_QueueMsg fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(NULL != *ppt_QueueMsg)
    {
        printf("NULL != *ppt_QueueMsg fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(NULL == sQueueName)
    {
        printf("NULL == sQueueName fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }

    /* ---- 分配并初始化结构体 ---- */
    T_LatestQueueMsg *pt_QueueMsg = (T_LatestQueueMsg *)malloc(sizeof(T_LatestQueueMsg));
    if(NULL == pt_QueueMsg)
    {
        printf("malloc fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    memset(pt_QueueMsg, 0, sizeof(T_LatestQueueMsg));
    pthread_mutex_init(&pt_QueueMsg->mux, 0);
    pthread_cond_init(&pt_QueueMsg->cond_get, 0);
    pt_QueueMsg->release_callback = release_callback;  /* 注册数据释放回调 */
    pt_QueueMsg->init_done = 1;

    /* ---- 设置队列名称 ---- */
    size_t iNameLen = strlen(sQueueName);
    if(iNameLen < MAX_THREADQUEUENAME_LEN)
    {
        strncpy(pt_QueueMsg->name, sQueueName, iNameLen);
        pt_QueueMsg->name[iNameLen] = '\0';
    }
    else
    {
        strncpy(pt_QueueMsg->name, sQueueName, MAX_THREADQUEUENAME_LEN - 1);
        pt_QueueMsg->name[MAX_THREADQUEUENAME_LEN - 1] = '\0';
    }

    *ppt_QueueMsg = pt_QueueMsg;
    return 0;
}

/**
 * @func         ThreadQueueAPI_Latest_PutMsg
 * @brief        向最新数据队列写入数据
 * @details      如果存在未读旧数据:
 *               - 有 release_callback: 调用 callback(旧数据) 释放
 *               - 无 release_callback: 打印警告，旧数据内存泄漏
 *               然后存入新数据并唤醒等待的消费者。
 */
int ThreadQueueAPI_Latest_PutMsg(T_LatestQueueMsg *pt_QueueMsg, void *data)
{
    /* ---- 参数合法性检查 ---- */
    if(NULL == pt_QueueMsg)
    {
        printf("NULL == pt_QueueMsg fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(NULL == data)
    {
        printf("NULL == data fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        printf("WARNING! %p Not Init ##%s->%d\r\n",pt_QueueMsg,__FUNCTION__,__LINE__);
        return -1;
    }

    pthread_mutex_lock(&pt_QueueMsg->mux);

    /* 检查队列是否已关闭 */
    if(pt_QueueMsg->is_closed)
    {
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        printf("LatestQueue %s is closed, cannot put data ##%s->%d\r\n",
               pt_QueueMsg->name, __FUNCTION__, __LINE__);
        return -2;
    }

    /* 如果有未读旧数据，自动丢弃 */
    if(pt_QueueMsg->has_data && pt_QueueMsg->data != NULL)
    {
        if(pt_QueueMsg->release_callback != NULL)
        {
            /* 通过回调释放旧数据，避免内存泄漏 */
            pt_QueueMsg->release_callback(pt_QueueMsg->data);
        }
        else
        {
            /* 没有注册回调，旧数据被丢弃但未释放，可能内存泄漏 */
            printf("WARNING: LatestQueue %s discarding data %p without release_callback ##%s->%d\r\n",
                   pt_QueueMsg->name, pt_QueueMsg->data, __FUNCTION__, __LINE__);
        }
    }

    /* 存入最新数据 */
    pt_QueueMsg->data = data;
    pt_QueueMsg->has_data = 1;

    /* 唤醒一个等待的消费者 */
    if(pt_QueueMsg->nEmptyThread > 0)
    {
        pthread_cond_signal(&pt_QueueMsg->cond_get);
    }

    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return 0;
}

/**
 * @func         ThreadQueueAPI_Latest_GetMsg
 * @brief        从最新数据队列获取最新数据
 * @details      如果没有数据(has_data==0)，阻塞等待直到:
 *               - 生产者写入新数据（被 signal 唤醒）
 *               - 超时
 *               - 队列被关闭（被 broadcast 唤醒）
 */
void* ThreadQueueAPI_Latest_GetMsg(T_LatestQueueMsg *pt_QueueMsg, int timeo)
{
    /* ---- 参数合法性检查 ---- */
    if(NULL == pt_QueueMsg)
    {
        printf("NULL == pt_QueueMsg fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return NULL;
    }
    if(timeo < 0)
    {
        printf("timeo < 0 fail ##%s->%d\r\n",__FUNCTION__,__LINE__);
        return NULL;
    }
    if(!pt_QueueMsg->init_done)
    {
        printf("WARNING! %p Not Init ##%s->%d\r\n",pt_QueueMsg,__FUNCTION__,__LINE__);
        return NULL;
    }

    void* data = NULL;
    struct timeval now;
    struct timespec outtime;
    int ret;
    long total_us;

    pthread_mutex_lock(&pt_QueueMsg->mux);

    /* 等待数据可用 */
    while(!pt_QueueMsg->has_data)
    {
        /* 队列已关闭且无数据，直接返回 */
        if(pt_QueueMsg->is_closed)
        {
            pthread_mutex_unlock(&pt_QueueMsg->mux);
            return NULL;
        }

        /* 计算绝对超时时间 */
        pt_QueueMsg->nEmptyThread++;
        gettimeofday(&now, NULL);
        total_us = now.tv_usec + (timeo % 1000) * 1000;
        outtime.tv_sec = now.tv_sec + timeo / 1000 + total_us / 1000000;
        outtime.tv_nsec = (total_us % 1000000) * 1000;

        ret = pthread_cond_timedwait(&pt_QueueMsg->cond_get, &pt_QueueMsg->mux, &outtime);
        pt_QueueMsg->nEmptyThread--;

        if(ret)  /* 超时 */
        {
            pthread_mutex_unlock(&pt_QueueMsg->mux);
            return NULL;
        }
    }

    /* 取出最新数据并清空标志 */
    data = pt_QueueMsg->data;
    pt_QueueMsg->data = NULL;
    pt_QueueMsg->has_data = 0;

    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return data;
}

/**
 * @func         ThreadQueueAPI_Latest_CloseMsg
 * @brief        关闭最新数据队列
 */
int ThreadQueueAPI_Latest_CloseMsg(T_LatestQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg)
    {
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);
    pt_QueueMsg->is_closed = 1;
    pthread_cond_broadcast(&pt_QueueMsg->cond_get);  /* 唤醒所有等待的消费者 */
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return 0;
}

/**
 * @func         ThreadQueueAPI_Latest_IsClosed
 * @brief        查询最新数据队列是否已关闭
 */
int ThreadQueueAPI_Latest_IsClosed(T_LatestQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg)
    {
        return -1;
    }
    return pt_QueueMsg->is_closed ? 1 : 0;
}

/**
 * @func         ThreadQueueAPI_Latest_FlushMsg
 * @brief        刷新最新数据队列
 * @details      LatestQueue 最多只有一条数据，所以回调最多执行一次。
 */
int ThreadQueueAPI_Latest_FlushMsg(T_LatestQueueMsg *pt_QueueMsg, void (*callback)(void* data))
{
    if(NULL == pt_QueueMsg || NULL == callback)
    {
        return -1;
    }
    int flush_count = 0;
    pthread_mutex_lock(&pt_QueueMsg->mux);
    if(pt_QueueMsg->has_data && pt_QueueMsg->data != NULL)
    {
        void* data = pt_QueueMsg->data;
        pt_QueueMsg->data = NULL;
        pt_QueueMsg->has_data = 0;
        flush_count = 1;
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        callback(data);  /* 在锁外执行回调 */
        return flush_count;
    }
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return flush_count;
}

/**
 * @func         ThreadQueueAPI_Latest_ReopenMsg
 * @brief        重新打开已关闭的最新数据队列
 */
int ThreadQueueAPI_Latest_ReopenMsg(T_LatestQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg)
    {
        return -1;
    }
    if(!pt_QueueMsg->init_done)
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);

    /* 前置条件检查 */
    if(!pt_QueueMsg->is_closed)
    {
        printf("WARNING: LatestQueue %s is not closed, no need to reopen ##%s->%d\r\n",
               pt_QueueMsg->name, __FUNCTION__, __LINE__);
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        return -1;
    }
    if(pt_QueueMsg->has_data)
    {
        printf("WARNING: LatestQueue %s still has data, flush first ##%s->%d\r\n",
               pt_QueueMsg->name, __FUNCTION__, __LINE__);
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        return -1;
    }
    if(pt_QueueMsg->nEmptyThread > 0)
    {
        printf("WARNING: LatestQueue %s has waiting threads, cannot reopen ##%s->%d\r\n",
               pt_QueueMsg->name, __FUNCTION__, __LINE__);
        pthread_mutex_unlock(&pt_QueueMsg->mux);
        return -1;
    }

    pt_QueueMsg->is_closed = 0;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    printf("LatestQueue %s reopened ##%s->%d\r\n",
           pt_QueueMsg->name, __FUNCTION__, __LINE__);
    return 0;
}

/**
 * @func         ThreadQueueAPI_Latest_DestroyMsg
 * @brief        销毁最新数据队列，释放所有资源
 * @details      1. 如果未关闭，自动关闭
 *               2. 在锁内读取等待线程计数，忙等待所有线程退出
 *               3. 释放残留数据（通过 release_callback）
 *               4. 销毁锁和条件变量，释放结构体
 */
int ThreadQueueAPI_Latest_DestroyMsg(T_LatestQueueMsg **ppt_QueueMsg)
{
    if(NULL == ppt_QueueMsg)
    {
        return -1;
    }
    if(NULL == (*ppt_QueueMsg))
    {
        return -1;
    }
    if(1 != (*ppt_QueueMsg)->init_done)
    {
        return -1;
    }

    /* 步骤1: 确保队列已关闭 */
    if(!(*ppt_QueueMsg)->is_closed)
    {
        printf("WARNING: Destroying LatestQueue without closing first ##%s->%d\r\n",
               __FUNCTION__, __LINE__);
        ThreadQueueAPI_Latest_CloseMsg(*ppt_QueueMsg);
    }

    /* 步骤2: 在锁内读取等待线程计数，忙等待退出 */
    {
        pthread_mutex_lock(&(*ppt_QueueMsg)->mux);
        int waiting = (*ppt_QueueMsg)->nEmptyThread;
        pthread_mutex_unlock(&(*ppt_QueueMsg)->mux);
        while(waiting > 0)
        {
            usleep(1000);
            pthread_mutex_lock(&(*ppt_QueueMsg)->mux);
            waiting = (*ppt_QueueMsg)->nEmptyThread;
            pthread_mutex_unlock(&(*ppt_QueueMsg)->mux);
        }
    }

    /* 步骤3: 释放残留数据 */
    if((*ppt_QueueMsg)->has_data && (*ppt_QueueMsg)->data != NULL)
    {
        if((*ppt_QueueMsg)->release_callback != NULL)
        {
            (*ppt_QueueMsg)->release_callback((*ppt_QueueMsg)->data);
        }
        else
        {
            printf("WARNING: Destroying LatestQueue %s with unreleased data %p ##%s->%d\r\n",
                   (*ppt_QueueMsg)->name, (*ppt_QueueMsg)->data, __FUNCTION__, __LINE__);
        }
    }

    /* 步骤4: 销毁同步原语并释放内存 */
    pthread_mutex_destroy(&((*ppt_QueueMsg)->mux));
    pthread_cond_destroy(&((*ppt_QueueMsg)->cond_get));
    (*ppt_QueueMsg)->init_done = 0;
    free(*ppt_QueueMsg);
    *ppt_QueueMsg = NULL;
    return 0;
}
