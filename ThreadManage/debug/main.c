/**
 * @file        main.c
 * @brief       线程管理测试程序
 * @details     测试线程创建和线程池功能（含新增API测试）
 * 
 * @author      zlzksrl
 * @Version     V1.1.0
 * @date        2026-05-08
 * @copyright   copyright (C) 2024
 */
 
/**
 * @date        2025-10-01
 * @Version     V1.0.1
 * @brief       创建文件
 * @author      zlzksrl
 *
 * @date        2026-05-08
 * @Version     V1.1.0
 * @brief       新增: 任务队列长度查询、等待所有任务完成、带超时任务添加、
 *              动态调整线程池大小、线程池统计信息、空闲缩容测试
 * @author      zlzksrl
 */
#include <stdlib.h>
#include <stdio.h>  
#include <unistd.h>
#include <pthread.h>
#include <string.h>

#include "../include/ThreadManage.h"


#if 1
                
#define Debug_printx(format,...)\
                do\
                {\
                    printf("[Debug]-[#####]-["format"@line:[%d]@func:[%s]]\r\n",##__VA_ARGS__,__LINE__,__FUNCTION__);\
                }while(0)
#else
                
#define Debug_printx(format,...)\
                do\
                {\
                }while(0)
#endif

/* ======================== 测试1: 线程创建 ======================== */

void *SetThread_Test1_ThreadCreate(void *pUserArg)
{
    while(1)
    {
        sleep(2);
    }
    return NULL;
}


void *SetThread_Test2_ThreadCreate(void *pUserArg)
{
    while(1)
    {
        sleep(2);
    }
    return NULL;
}

void *SetThread_Test3_ThreadCreate(void *pUserArg)
{
    while(1)
    {
        sleep(2);
    }
    return NULL;
}


/**
* @func         Test_ThreadCreate
* @brief        测试线程创建功能
*/
void Test_ThreadCreate(void)
{
    int ret = -1;
    
    Debug_printx("========== Test_ThreadCreate Start ==========");

    /* 测试1: 默认属性创建线程 */
    T_ThreadCreateConfig t_Config_Test1;
    memset(&t_Config_Test1, 0, sizeof(T_ThreadCreateConfig));
    t_Config_Test1.pThreadFunc        = SetThread_Test1_ThreadCreate;
    t_Config_Test1.pThreadFuncUserArg = NULL;
    t_Config_Test1.sThreadName        = strdup("Test1");
    ret = ThreadAPI_ThreadCreate(&t_Config_Test1);
    if(ret < 0)
    {
        Debug_printx("Test1 fail");
    }
    if (NULL != t_Config_Test1.sThreadName) free(t_Config_Test1.sThreadName);
    sleep(1);

    /* 测试2: 只设置栈大小 */
    T_ThreadCreateConfig t_Config_Test2;
    memset(&t_Config_Test2, 0, sizeof(T_ThreadCreateConfig));
    t_Config_Test2.pThreadFunc        = SetThread_Test2_ThreadCreate;
    t_Config_Test2.pThreadFuncUserArg = NULL;
    t_Config_Test2.sThreadName        = strdup("Test2");
    t_Config_Test2.eSetAttr           = 1;
    t_Config_Test2.istacksize_MB      = 1;
    ret = ThreadAPI_ThreadCreate(&t_Config_Test2);
    if(ret < 0)
    {
        Debug_printx("Test2 fail");
    }
    if (NULL != t_Config_Test2.sThreadName) free(t_Config_Test2.sThreadName);
    sleep(1);

    /* 测试3: 完整属性设置 */
    T_ThreadCreateConfig t_Config_Test3;
    memset(&t_Config_Test3, 0, sizeof(T_ThreadCreateConfig));
    t_Config_Test3.pThreadFunc        = SetThread_Test3_ThreadCreate;
    t_Config_Test3.pThreadFuncUserArg = NULL;
    t_Config_Test3.sThreadName        = strdup("Test3");
    t_Config_Test3.eSetAttr           = 2;
    t_Config_Test3.istacksize_MB      = 4;
    t_Config_Test3.eDetachState       = PTHREAD_CREATE_DETACHED;
    t_Config_Test3.einheritsched      = PTHREAD_EXPLICIT_SCHED;
    t_Config_Test3.eSchedPolicy       = SCHED_RR;
    t_Config_Test3.iSchedPriority     = 50;
    ret = ThreadAPI_ThreadCreate(&t_Config_Test3);
    if(ret < 0)
    {
        Debug_printx("Test3 fail");
    }
    if (NULL != t_Config_Test3.sThreadName) free(t_Config_Test3.sThreadName);
    sleep(1);

    Debug_printx("========== Test_ThreadCreate End ==========");
}


/* ======================== 测试2: 线程池（原有功能） ======================== */

/**
 * @brief  线程池任务函数 - 简单延时模拟工作
 */
void *ThreadPool_TaskFunc(void *pUserArg)
{
    int *piTaskId = (int *)pUserArg;
    if (piTaskId != NULL)
    {
        Debug_printx("任务[%d] 开始执行, 线程[%lu]"
                , *piTaskId, (unsigned long)pthread_self());
        
        int iWorkTime = (*piTaskId % 3) + 1;
        sleep(iWorkTime);
        
        Debug_printx("任务[%d] 执行完成, 耗时[%ds]"
                , *piTaskId, iWorkTime);
    }
    return NULL;
}

/**
 * @func         Test_ThreadPool
 * @brief        测试线程池基础功能
 */
void Test_ThreadPool(void)
{
    int ret = -1;
    int i;

    Debug_printx("========== 线程池测试 开始 ==========");

    /*
     * 配置线程池参数:
     * - min=2, max=5: 线程数在2~5之间自动调节
     * - idle_timeout=3000ms: 空闲3秒后自动缩容回minNum
     * 任务多时自动扩容，任务少时自动缩容回最小数量
     */
    T_ThreadPoolConfig tPoolConfig;
    memset(&tPoolConfig, 0, sizeof(T_ThreadPoolConfig));
    tPoolConfig.iMinNum        = 2;
    tPoolConfig.iMaxNum        = 20;
    tPoolConfig.iQueueMaxSize  = 200;
    tPoolConfig.iIdleTimeoutMs = 3000; /* 3秒空闲后自动缩容 */

    Debug_printx("创建线程池: 最小线程数=[%d], 最大线程数=[%d], 队列容量=[%d], 空闲缩容超时=[%dms]"
            , tPoolConfig.iMinNum, tPoolConfig.iMaxNum, tPoolConfig.iQueueMaxSize
            , tPoolConfig.iIdleTimeoutMs);

    T_ThreadPoolHandle *ptPool = ThreadAPI_ThreadPoolCreate(tPoolConfig);
    if (ptPool == NULL)
    {
        Debug_printx("线程池创建失败!");
        return;
    }
    Debug_printx("线程池创建成功");

    /* 检查初始状态（应等于iMinNum=2） */
    int iLive = ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool);
    int iBusy = ThreadAPI_ThreadPoolBusyThreadNumGet(ptPool);
    Debug_printx("初始状态: 工作线程数量=[%d], 忙碌线程数量=[%d], 使用线程数量=[%d]"
            , iLive, iBusy, iBusy);

    /* ======================== 阶段1: 大量任务触发扩容 ======================== */
    Debug_printx("--- 阶段1: 提交100个任务, 触发自动扩容 ---");
    int aiTaskIds[100];
    for (i = 0; i < 100; i++)
    {
        aiTaskIds[i] = i + 1;
        ret = ThreadAPI_ThreadPoolAddTask(ptPool, ThreadPool_TaskFunc, &aiTaskIds[i]);
        if (ret != 0)
        {
            Debug_printx("添加任务[%d] 失败 ret=[%d]", aiTaskIds[i], ret);
        }
    }

    /* 观察扩容过程：每秒打印线程状态，可以看到线程数从2增长到5 */
    for (i = 0; i < 5; i++)
    {
        iLive = ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool);
        iBusy = ThreadAPI_ThreadPoolBusyThreadNumGet(ptPool);
        int iQueueLen = ThreadAPI_ThreadPoolTaskQueueLenGet(ptPool);
        Debug_printx("[%ds] 工作线程数量=[%d], 忙碌线程数量=[%d], 使用线程数量=[%d], 队列任务数=[%d]"
                , i, iLive, iBusy, iBusy, iQueueLen);
        sleep(1);
    }

    /* 等待所有任务完成 */
    ret = ThreadAPI_ThreadPoolWaitAllDone(ptPool, 30000);
    iLive = ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool);
    iBusy = ThreadAPI_ThreadPoolBusyThreadNumGet(ptPool);
    Debug_printx("所有任务完成: 工作线程数量=[%d], 忙碌线程数量=[%d], 使用线程数量=[%d]"
            , iLive, iBusy, iBusy);

    /* ======================== 阶段2: 空闲等待缩容 ======================== */
    Debug_printx("--- 阶段2: 空闲等待, 观察自动缩容 (超时3秒) ---");
    for (i = 0; i < 5; i++)
    {
        iLive = ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool);
        iBusy = ThreadAPI_ThreadPoolBusyThreadNumGet(ptPool);
        Debug_printx("[%ds] 工作线程数量=[%d], 忙碌线程数量=[%d], 使用线程数量=[%d]"
                , i, iLive, iBusy, iBusy);
        sleep(1);
    }
    iLive = ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool);
    Debug_printx("缩容完成: 工作线程数量=[%d] (应缩回到最小线程数=[%d])"
            , iLive, tPoolConfig.iMinNum);

    /* ======================== 阶段3: 再次提交少量任务 ======================== */
    Debug_printx("--- 阶段3: 提交2个任务, 无需扩容 ---");
    int aiTaskIds2[2];
    for (i = 0; i < 2; i++)
    {
        aiTaskIds2[i] = i + 10;
        ThreadAPI_ThreadPoolAddTask(ptPool, ThreadPool_TaskFunc, &aiTaskIds2[i]);
    }
    ThreadAPI_ThreadPoolWaitAllDone(ptPool, 10000);
    iLive = ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool);
    iBusy = ThreadAPI_ThreadPoolBusyThreadNumGet(ptPool);
    Debug_printx("阶段3完成: 工作线程数量=[%d], 忙碌线程数量=[%d], 使用线程数量=[%d]"
            , iLive, iBusy, iBusy);

    /* ======================== 最终统计信息 ======================== */
    T_ThreadPoolStats tStats;
    ret = ThreadAPI_ThreadPoolStatsGet(ptPool, &tStats);
    if (ret == 0)
    {
        Debug_printx("统计信息: 总提交任务=[%lu], 总完成任务=[%lu], 峰值忙碌线程数=[%d], 峰值队列长度=[%d]"
                , tStats.ulTotalTasksSubmitted, tStats.ulTotalTasksProcessed
                , tStats.iPeakBusyThreadNum, tStats.iPeakTaskQueueLen);
    }

    /* 销毁线程池 */
    Debug_printx("正在销毁线程池...");
    ThreadAPI_ThreadPoolDestroy(ptPool);
    Debug_printx("线程池已销毁");

    Debug_printx("========== 线程池测试 结束 ==========");
}


/* ======================== 测试3: 新增API测试 ======================== */

/**
 * @brief  快速任务函数（短延时）
 */
void *FastTaskFunc(void *pUserArg)
{
    int *piTaskId = (int *)pUserArg;
    if (piTaskId != NULL)
    {
        usleep(100000); /* 100ms */
        Debug_printx("快速任务[%d] 完成", *piTaskId);
    }
    return NULL;
}

/**
 * @func         Test_NewFeatures
 * @brief        测试新增API功能
 * @details      测试内容:
 *               1. 带超时任务添加 (AddTaskTimeout)
 *               2. 非阻塞任务添加 (AddTaskTry)
 *               3. 动态调整线程池大小 (Resize)
 *               4. 任务队列长度查询 (TaskQueueLenGet)
 *               5. 统计信息 (StatsGet)
 */
void Test_NewFeatures(void)
{
    int ret;
    int i;

    Debug_printx("========== 新增API测试 开始 ==========");

    T_ThreadPoolConfig tPoolConfig;
    memset(&tPoolConfig, 0, sizeof(T_ThreadPoolConfig));
    tPoolConfig.iMinNum        = 2;
    tPoolConfig.iMaxNum        = 10;
    tPoolConfig.iQueueMaxSize  = 4;
    tPoolConfig.iIdleTimeoutMs = 5000;

    T_ThreadPoolHandle *ptPool = ThreadAPI_ThreadPoolCreate(tPoolConfig);
    if (ptPool == NULL)
    {
        Debug_printx("线程池创建失败!");
        return;
    }

    /* ---- 测试1: AddTaskTry（非阻塞添加） ---- */
    Debug_printx("--- 测试非阻塞添加任务(AddTaskTry) ---");
    int aiTaskIds[20];
    int iAdded = 0;
    for (i = 0; i < 20; i++)
    {
        aiTaskIds[i] = i + 100;
        ret = ThreadAPI_ThreadPoolAddTaskTry(ptPool, FastTaskFunc, &aiTaskIds[i]);
        if (ret == 0)
        {
            iAdded++;
        }
        else if (ret == -2)
        {
            Debug_printx("队列已满, 任务[%d]被拒绝, 已添加=[%d]", i + 100, iAdded);
            break;
        }
    }
    Debug_printx("非阻塞添加: 成功添加=[%d]个任务", iAdded);

    ret = ThreadAPI_ThreadPoolWaitAllDone(ptPool, 5000);
    Debug_printx("等待完成: 工作线程数量=[%d], 队列任务数=[%d]"
            , ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool)
            , ThreadAPI_ThreadPoolTaskQueueLenGet(ptPool));

    /* ---- 测试2: AddTaskTimeout（带超时添加） ---- */
    Debug_printx("--- 测试带超时添加任务(AddTaskTimeout) ---");
    for (i = 0; i < 4; i++)
    {
        aiTaskIds[i] = i + 200;
        ThreadAPI_ThreadPoolAddTask(ptPool, FastTaskFunc, &aiTaskIds[i]);
    }
    Debug_printx("队列已填满, 队列任务数=[%d]", ThreadAPI_ThreadPoolTaskQueueLenGet(ptPool));

    aiTaskIds[4] = 204;
    Debug_printx("带3秒超时添加任务...");
    ret = ThreadAPI_ThreadPoolAddTaskTimeout(ptPool, FastTaskFunc, &aiTaskIds[4], 3000);
    Debug_printx("带超时添加结果: ret=[%d] (0=成功, -2=超时)", ret);

    ThreadAPI_ThreadPoolWaitAllDone(ptPool, 10000);

    /* ---- 测试3: Resize（动态调整大小） ---- */
    Debug_printx("--- 测试动态调整线程池大小(Resize) ---");
    Debug_printx("调整前: 工作线程数量=[%d]", ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool));
    
    ret = ThreadAPI_ThreadPoolResize(ptPool, 5, 10);
    Debug_printx("Resize(最小5,最大10) ret=[%d]", ret);
    sleep(1);
    Debug_printx("扩容后: 工作线程数量=[%d]", ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool));

    ret = ThreadAPI_ThreadPoolResize(ptPool, 2, 6);
    Debug_printx("Resize(最小2,最大6) ret=[%d]", ret);
    Debug_printx("缩容请求后: 工作线程数量=[%d]", ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool));

    /* ---- 测试4: 空闲缩容观察 ---- */
    Debug_printx("--- 测试空闲自动缩容(等待8秒) ---");
    Debug_printx("缩容前: 工作线程数量=[%d]", ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool));
    sleep(8);
    Debug_printx("缩容后: 工作线程数量=[%d] (应缩回到最小线程数=2)", ThreadAPI_ThreadPoolLiveThreadNumGet(ptPool));

    /* ---- 测试5: 最终统计信息 ---- */
    Debug_printx("--- 统计信息 ---");
    T_ThreadPoolStats tStats;
    ret = ThreadAPI_ThreadPoolStatsGet(ptPool, &tStats);
    if (ret == 0)
    {
        Debug_printx("总提交任务=[%lu], 总完成任务=[%lu], 峰值忙碌线程数=[%d], 峰值队列长度=[%d]"
                , tStats.ulTotalTasksSubmitted, tStats.ulTotalTasksProcessed
                , tStats.iPeakBusyThreadNum, tStats.iPeakTaskQueueLen);
    }

    ThreadAPI_ThreadPoolDestroy(ptPool);
    Debug_printx("========== 新增API测试 结束 ==========");
}


/**
* @func         main
* @brief        main函数
*/
int main(int argc, char **argv)
{
    Debug_printx("线程管理库测试程序启动 (V1.1.0)");

    /* 测试线程创建 */
    Test_ThreadCreate();

    /* 测试线程池（动态扩缩容） */
    Test_ThreadPool();

    /* 测试新增API */
    Test_NewFeatures();

    Debug_printx("线程管理库测试程序结束");

    while(1)
    {
        sleep(1);
    }
    return 0;
}
