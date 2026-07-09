/**
 * @file        main.c
 * @brief       ThreadQueue 线程通讯队列 - 测试程序
 * @details     本程序演示两种队列的完整使用流程:
 *
 *              Part 1: ThreadQueue（环形缓冲区队列）
 *              - 创建2个队列和2个消费者线程
 *              - 主线程作为生产者循环发送数据
 *              - 达到上限后按 Close → Join → Flush → Destroy 流程关闭
 *
 *              Part 2: LatestQueue（最新数据队列）
 *              - 创建1个队列和1个消费者线程
 *              - 生产者快速写入，消费者慢速读取
 *              - 演示旧数据自动丢弃机制
 *              - 按 Close → Join → Flush → Destroy 流程关闭
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
 * @brief       新增 LatestQueue 测试;
 *              使用 Close→Join→Flush→Destroy 安全销毁流程;
 *              添加 Init 返回值检查和 Put 返回-2时的内存释放
 * @author      zlzksrl
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#include "../include/ThreadQueue.h"


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
                    printf("[Debug]-[#####]-["format"##@line:[%d]@func:[%s]]\r\n",##__VA_ARGS__,__LINE__,__FUNCTION__);\
                }while(0)
#else
#define Debug_printx(format,...)\
                do\
                {\
                }while(0)
#endif


/* ================================================================== */
/*                                                                    */
/*     Part 1: ThreadQueue（环形缓冲区队列）测试代码                     */
/*                                                                    */
/* ================================================================== */

/**
 * @func         SetThread_GetQueueMsg1
 * @brief        ThreadQueue 消费者线程1
 * @details      循环调用 ThreadQueueAPI_GetMsg 获取数据。
 *               当 Get 返回 NULL 时，检查队列是否已关闭:
 *               - 已关闭: 退出循环，线程结束
 *               - 未关闭: 超时，继续等待
 *               获取到数据后打印并释放。
 * @param[in]    pUserArg  线程参数，类型为 T_ThreadQueueMsg*
 * @return       NULL
 */
void *SetThread_GetQueueMsg1(void *pUserArg)
{
    T_ThreadQueueMsg *pt_testQueueMsg = (T_ThreadQueueMsg *)pUserArg;
    int *pt_msg;
    while(1)
    {
        pt_msg = (int *)ThreadQueueAPI_GetMsg(pt_testQueueMsg, 10);  /* 10ms超时 */
        if(pt_msg == NULL)
        {
            /* Get返回NULL可能是超时或队列关闭，通过IsClosed区分 */
            if(ThreadQueueAPI_IsClosed(pt_testQueueMsg) > 0)
            {
                Debug_printx("Thread1 exit, queue is closed");
                break;  /* 队列已关闭，退出线程 */
            }
            continue;  /* 超时，继续等待 */
        }
        Debug_printx("GetMsg = [%d]", *pt_msg);
        free(pt_msg);  /* 释放从队列获取的数据（由生产者malloc分配） */
    }
    return NULL;
}

/**
 * @func         SetThread_GetQueueMsg2
 * @brief        ThreadQueue 消费者线程2（与线程1逻辑相同，使用不同队列）
 * @param[in]    pUserArg  线程参数，类型为 T_ThreadQueueMsg*
 * @return       NULL
 */
void *SetThread_GetQueueMsg2(void *pUserArg)
{
    T_ThreadQueueMsg *pt_testQueueMsg = (T_ThreadQueueMsg *)pUserArg;
    int *pt_msg;
    while(1)
    {
        pt_msg = (int *)ThreadQueueAPI_GetMsg(pt_testQueueMsg, 10);
        if(pt_msg == NULL)
        {
            if(ThreadQueueAPI_IsClosed(pt_testQueueMsg) > 0)
            {
                Debug_printx("Thread2 exit, queue is closed");
                break;
            }
            continue;
        }
        Debug_printx("GetMsg = [%d]", *pt_msg);
        free(pt_msg);
    }
    return NULL;
}

/**
 * @func         my_data_callback
 * @brief        ThreadQueue 数据处理回调，用于 ThreadQueueAPI_FlushMsg
 * @details      打印并释放剩余数据。在队列销毁前通过 Flush 调用，
 *               确保缓冲区中的数据被正确释放，避免内存泄漏。
 * @param[in]    data  队列中的剩余数据
 */
void my_data_callback(void* data)
{
    int *pt_msg = (int *)data;
    if(pt_msg != NULL)
    {
        Debug_printx("Flush remaining data: [%d]", *pt_msg);
        free(pt_msg);
    }
}


/* ================================================================== */
/*                                                                    */
/*     Part 2: LatestQueue（最新数据队列）测试代码                       */
/*                                                                    */
/* ================================================================== */

/**
 * @func         latest_release_callback
 * @brief        LatestQueue 数据释放回调
 * @details      当 ThreadQueueAPI_Latest_PutMsg 发现存在未读旧数据时自动调用，
 *               释放旧数据的内存，避免内存泄漏。
 * @param[in]    data  被自动丢弃的旧数据
 */
void latest_release_callback(void* data)
{
    int *pt_msg = (int *)data;
    if(pt_msg != NULL)
    {
        Debug_printx("LatestQueue auto discard data: [%d]", *pt_msg);
        free(pt_msg);
    }
}

/**
 * @func         SetThread_GetLatestQueueMsg
 * @brief        LatestQueue 消费者线程
 * @details      循环获取最新数据，每次获取后延迟50ms（模拟慢速处理）。
 *               由于生产者每10ms发一条，消费者50ms才处理一条，
 *               大部分数据会在 Put 时被自动丢弃，消费者只读到最新值。
 * @param[in]    pUserArg  线程参数，类型为 T_LatestQueueMsg*
 * @return       NULL
 */
void *SetThread_GetLatestQueueMsg(void *pUserArg)
{
    T_LatestQueueMsg *pt_testQueueMsg = (T_LatestQueueMsg *)pUserArg;
    int *pt_msg;
    while(1)
    {
        pt_msg = (int *)ThreadQueueAPI_Latest_GetMsg(pt_testQueueMsg, 100);  /* 100ms超时 */
        if(pt_msg == NULL)
        {
            if(ThreadQueueAPI_Latest_IsClosed(pt_testQueueMsg) > 0)
            {
                Debug_printx("LatestQueue consumer thread exit, queue is closed");
                break;
            }
            continue;
        }
        Debug_printx("LatestQueue GetMsg = [%d]", *pt_msg);
        free(pt_msg);
        usleep(50000);  /* 模拟消费者处理较慢（50ms），生产者10ms发一条 */
    }
    return NULL;
}

/**
 * @func         latest_flush_callback
 * @brief        LatestQueue 刷新回调，处理销毁前的剩余数据
 * @param[in]    data  队列中的剩余数据
 */
void latest_flush_callback(void* data)
{
    int *pt_msg = (int *)data;
    if(pt_msg != NULL)
    {
        Debug_printx("LatestQueue Flush remaining data: [%d]", *pt_msg);
        free(pt_msg);
    }
}


/* ================================================================== */
/*                                                                    */
/*     Part 3: ThreadQueueAPI_PutMsgTimeout（带超时的Put）测试代码          */
/*                                                                    */
/* ================================================================== */

/**
 * @func         SetThread_SlowConsumer
 * @brief        慢速消费者线程，用于测试 ThreadQueueAPI_PutMsgTimeout
 * @details      每次获取数据后延迟 200ms，使小容量队列容易满，
 *               从而触发生产者的超时等待。
 * @param[in]    pUserArg  线程参数，类型为 T_ThreadQueueMsg*
 * @return       NULL
 */
void *SetThread_SlowConsumer(void *pUserArg)
{
    T_ThreadQueueMsg *pt_testQueueMsg = (T_ThreadQueueMsg *)pUserArg;
    int *pt_msg;
    while(1)
    {
        pt_msg = (int *)ThreadQueueAPI_GetMsg(pt_testQueueMsg, 100);  /* 100ms超时 */
        if(pt_msg == NULL)
        {
            if(ThreadQueueAPI_IsClosed(pt_testQueueMsg) > 0)
            {
                Debug_printx("SlowConsumer exit, queue is closed");
                break;
            }
            continue;
        }
        Debug_printx("SlowConsumer GetMsg = [%d]", *pt_msg);
        free(pt_msg);
        usleep(200000);  /* 模拟慢速消费（200ms），使队列容易满 */
    }
    return NULL;
}


/* ================================================================== */
/*                                                                    */
/*     main - 主函数                                                   */
/*                                                                    */
/* ================================================================== */

/**
 * @func         main
 * @brief        测试程序主函数
 * @details      分两部分执行:
 *               Part 1: ThreadQueue 测试（生产者-消费者模式）
 *               Part 2: LatestQueue 测试（最新数据模式）
 *
 * @param[in]    argc  参数个数
 * @param[in]    argv  参数字符串
 * @return       0正常退出, -1初始化失败
 */
int main(int argc, char **argv)
{
    int ret = -1;

    /* ============================================================== */
    /* Part 1: ThreadQueue 测试                                        */
    /* ============================================================== */

    T_ThreadQueueMsg *pt_testQueueMsg1 = NULL;
    T_ThreadQueueMsg *pt_testQueueMsg2 = NULL;
    pthread_t pid1, pid2;

    /* 步骤1: 初始化两个队列（容量100），注册数据释放回调 */
    ret = ThreadQueueAPI_InitMsg(&pt_testQueueMsg1, 100, "testQueue1", my_data_callback);
    if(ret < 0)
    {
        Debug_printx("InitThreadQueueMsg1 fail ret = [%d]", ret);
        return -1;
    }
    ret = ThreadQueueAPI_InitMsg(&pt_testQueueMsg2, 100, "testQueue2", my_data_callback);
    if(ret < 0)
    {
        Debug_printx("InitThreadQueueMsg2 fail ret = [%d]", ret);
        ThreadQueueAPI_DestroyMsg(&pt_testQueueMsg1);  /* 清理已创建的队列1 */
        return -1;
    }

    /* 步骤2: 创建消费者线程 */
    pthread_create(&pid1, 0, SetThread_GetQueueMsg1, pt_testQueueMsg1);
    pthread_create(&pid2, 0, SetThread_GetQueueMsg2, pt_testQueueMsg2);

    /* 步骤3: 主线程作为生产者循环发送数据 */
    int icount = 0;
    int i = 0;
    while(1)
    {
        if(NULL != pt_testQueueMsg1)
        {
            /* 向队列1发送数据 */
            int *pt_msg1 = (int *)malloc(sizeof(int));
            if(NULL == pt_msg1)
            {
                Debug_printx("fail");
            }
            else
            {
                *pt_msg1 = icount++;
                ret = ThreadQueueAPI_PutMsg(pt_testQueueMsg1, pt_msg1);
                if(ret == -2)  /* 队列已关闭，数据未被存入，需自行释放 */
                {
                    Debug_printx("Queue1 is closed, free msg [%d]", *pt_msg1);
                    free(pt_msg1);
                }
                else
                {
                    Debug_printx("PutMsg1 = [%d]", *pt_msg1);
                }
            }

            /* 向队列2发送数据 */
            int *pt_msg2 = (int *)malloc(sizeof(int));
            if(NULL == pt_msg2)
            {
                Debug_printx("fail");
            }
            else
            {
                *pt_msg2 = icount++;
                ret = ThreadQueueAPI_PutMsg(pt_testQueueMsg2, pt_msg2);
                if(ret == -2)
                {
                    Debug_printx("Queue2 is closed, free msg [%d]", *pt_msg2);
                    free(pt_msg2);
                }
                else
                {
                    Debug_printx("PutMsg2 = [%d]", *pt_msg2);
                }
            }

            usleep(10000);  /* 10ms发送一次 */

            /* 达到2000条后开始销毁流程 */
            if(icount >= 2 * 1000)
            {
                /* 1) 关闭队列: 阻止新消息写入，唤醒所有等待线程 */
                ThreadQueueAPI_CloseMsg(pt_testQueueMsg1);
                ThreadQueueAPI_CloseMsg(pt_testQueueMsg2);
                Debug_printx("ThreadQueueAPI_CloseMsg done");

                /* 2) 等待消费者线程退出: 消费者检测到关闭后会break退出循环 */
                pthread_join(pid1, NULL);
                pthread_join(pid2, NULL);
                Debug_printx("Consumer threads joined");

                /* 3) 刷新队列: 处理消费者未来得及读取的剩余数据 */
                int flush_count1 = ThreadQueueAPI_FlushMsg(pt_testQueueMsg1, my_data_callback);
                int flush_count2 = ThreadQueueAPI_FlushMsg(pt_testQueueMsg2, my_data_callback);
                Debug_printx("Flushed queue1: [%d] items, queue2: [%d] items", flush_count1, flush_count2);

                /* 4) 销毁队列: 释放所有资源 */
                ThreadQueueAPI_DestroyMsg(&pt_testQueueMsg1);
                ThreadQueueAPI_DestroyMsg(&pt_testQueueMsg2);
                Debug_printx("ThreadQueueAPI_DestroyMsg icount = [%d]", icount);
            }
        }
        else
        {
            /* 队列已销毁，退出主循环 */
            sleep(1);
            Debug_printx("Sleep1");
            break;
        }
    }

    /* ============================================================== */
    /* Part 2: LatestQueue 测试                                        */
    /* ============================================================== */

    Debug_printx("========== LatestQueue Test Start ==========");

    T_LatestQueueMsg *pt_latestQueue = NULL;
    pthread_t pid_latest;

    /* 步骤1: 初始化最新数据队列，注册数据释放回调 */
    ret = ThreadQueueAPI_Latest_InitMsg(&pt_latestQueue, "latestTest", latest_release_callback);
    if(ret < 0)
    {
        Debug_printx("ThreadQueueAPI_Latest_InitMsg fail ret = [%d]", ret);
        return -1;
    }

    /* 步骤2: 创建消费者线程 */
    pthread_create(&pid_latest, 0, SetThread_GetLatestQueueMsg, pt_latestQueue);

    /* 步骤3: 快速Put 20条数据
       生产者每10ms发一条，消费者每50ms才处理一条
       因此大部分数据会在Put时被自动丢弃（通过release_callback释放）
       消费者只能读到部分最新数据 */
    for(i = 0; i < 20; i++)
    {
        int *pt_msg = (int *)malloc(sizeof(int));
        if(pt_msg != NULL)
        {
            *pt_msg = i;
            ret = ThreadQueueAPI_Latest_PutMsg(pt_latestQueue, pt_msg);
            if(ret == -2)
            {
                Debug_printx("LatestQueue is closed, free msg [%d]", *pt_msg);
                free(pt_msg);
            }
            else
            {
                Debug_printx("LatestQueue PutMsg = [%d]", *pt_msg);
            }
        }
        usleep(10000);  /* 10ms发一条 */
    }

    /* 步骤4: 等待消费者处理完剩余数据 */
    usleep(2000000);  /* 等待2秒 */

    /* 步骤5: 安全销毁流程（与ThreadQueue相同） */
    ThreadQueueAPI_Latest_CloseMsg(pt_latestQueue);
    Debug_printx("ThreadQueueAPI_Latest_CloseMsg done");

    pthread_join(pid_latest, NULL);
    Debug_printx("LatestQueue consumer thread joined");

    int flush_count = ThreadQueueAPI_Latest_FlushMsg(pt_latestQueue, latest_flush_callback);
    Debug_printx("LatestQueue flushed: [%d] items", flush_count);

    ThreadQueueAPI_Latest_DestroyMsg(&pt_latestQueue);
    Debug_printx("LatestQueue destroyed");

    Debug_printx("========== LatestQueue Test End ==========");

    /* ============================================================== */
    /* Part 3: ThreadQueueAPI_PutMsgTimeout 测试                           */
    /* ============================================================== */

    Debug_printx("========== ThreadQueueAPI_PutMsgTimeout Test Start ==========");

    T_ThreadQueueMsg *pt_timeoutQueue = NULL;
    pthread_t pid_slow;

    /* 步骤1: 初始化一个小容量队列（容量=1），容易满以触发超时 */
    ret = ThreadQueueAPI_InitMsg(&pt_timeoutQueue, 1, "timeoutQueue", my_data_callback);
    if(ret < 0)
    {
        Debug_printx("ThreadQueueAPI_InitMsg timeoutQueue fail ret = [%d]", ret);
        return -1;
    }

    /* 步骤2: 创建慢速消费者线程（每200ms消费一条） */
    pthread_create(&pid_slow, 0, SetThread_SlowConsumer, pt_timeoutQueue);

    /* 步骤3: 生产者快速发送数据，使用 ThreadQueueAPI_PutMsgTimeout
       队列容量仅3，消费者慢速200ms/条，生产者每20ms发一条
       预期: 队列很快满，部分Put会超时返回-3 */
    int timeout_count = 0;
    int success_count = 0;
    for(i = 0; i < 30; i++)
    {
        int *pt_msg = (int *)malloc(sizeof(int));
        if(pt_msg != NULL)
        {
            *pt_msg = i;
            /* 使用50ms超时等待队列有空间 */
            ret = ThreadQueueAPI_PutMsgTimeout(pt_timeoutQueue, pt_msg, 50);
            if(ret == -2)
            {
                Debug_printx("TimeoutQueue is closed, free msg [%d]", *pt_msg);
                free(pt_msg);
            }
            else if(ret == -3)
            {
                /* 超时，队列仍然满，数据未被存入 */
                Debug_printx("PutTimeout! queue full, discard msg [%d]", *pt_msg);
                free(pt_msg);
                timeout_count++;
            }
            else
            {
                Debug_printx("PutTimeoutQueue msg = [%d]", *pt_msg);
                success_count++;
            }
        }
        usleep(20000);  /* 20ms发一条 */
    }

    Debug_printx("ThreadQueueAPI_PutMsgTimeout test done: success=[%d], timeout=[%d]",
                 success_count, timeout_count);

    /* 步骤4: 安全销毁流程 */
    ThreadQueueAPI_CloseMsg(pt_timeoutQueue);
    Debug_printx("ThreadQueueAPI_CloseMsg timeoutQueue done");

    pthread_join(pid_slow, NULL);
    Debug_printx("SlowConsumer thread joined");

    int flush_count3 = ThreadQueueAPI_FlushMsg(pt_timeoutQueue, my_data_callback);
    Debug_printx("TimeoutQueue flushed: [%d] items", flush_count3);

    ThreadQueueAPI_DestroyMsg(&pt_timeoutQueue);
    Debug_printx("TimeoutQueue destroyed");

    Debug_printx("========== ThreadQueueAPI_PutMsgTimeout Test End ==========");

    Debug_printx("Program exit");
    return 0;
}
