/**
 * @file        ThreadManage.h
 * @brief       LinuxARM-PublicLib-线程管理
 * @details     IMX6ULL平台    
 * 
 * @author      zlzksrl
 * @Version     V1.0.1
 * @date        2025-10-01
 * @copyright   copyright (C) 2025
 */
 
/**
 * @date        2025-10-01
 * @Version     V1.0.1
 * @brief       创建文件
 * @author      zlzksrl
 */
#ifndef __ThreadManage_H__
#define __ThreadManage_H__

#ifdef __cplusplus
 extern "C" {
#endif

#include <pthread.h>

/* 线程函数原型 */
typedef void *(*ThreadFunctionType)(void *pThreadFuncUserArg);

/* 线程创建配置 */
typedef struct
{
    /* 1、配置线程函数及参数 */
    ThreadFunctionType pThreadFunc;
    void *pThreadFuncUserArg;

    /* 2、配置线程名称 */
    char *sThreadName;

    /* 3、是否配置线程属性 */
    int eSetAttr;
    /* 0 == eSetAttr:不配置线程属性，使用默认值 */
    /* 1 == eSetAttr:配置线程属性，但只配置栈大小 */
    /* 2 == eSetAttr:配置线程以下所有属性 */

    /* 4、配置栈大小 1-32 */
    int istacksize_MB;

    /* 5、配置分离状态 */
    int eDetachState;
    /* (1)PTHREAD_CREATE_JOINABLE:（默认值）线程执行完函数后不会自行释放资源； */
    /* (2)PTHREAD_CREATE_DETACHED:线程执行完函数后，会自行终止并释放占用的资源。 */

    /* 6、配置继承策略 */
    int einheritsched;
    /* (1)PTHREAD_INHERIT_SCHED :新线程将忽略 attr 中设置的调度策略和参数，继承创建者线程的调度属性 */
    /* (2)PTHREAD_EXPLICIT_SCHED:新线程使用 attr 中显式设置的调度策略和参数 */

    /* 7、配置调度策略与优先级----einheritsched 设置为 PTHREAD_EXPLICIT_SCHED 有用 */
    int eSchedPolicy;              /* 线程调度策略 */
    /* (1)SCHED_OTHER :(默认值)：普通策略(分时调度算法)，按照优先级调度,是动态优先级，不支持设置优先级 */
    /* (2)SCHED_FIFO  :先进先出。一个FIFO会持续执行，直到线程阻塞、结束、有更高优先级的线程就绪 */
    /* (3)SCHED_RR    :轮转策略。给每个线程分配执行时间(时间片)，当一个线程的时间片耗尽时，下一个线程执行 */
    /* 以下是Linux特有 */
    /* (4)SCHED_BATCH :批处理调度策略（Linux 特有） 适合非交互式、CPU 密集型任务 */
    /* (5)SCHED_IDLE  :低优先级调度策略（Linux 特有）  仅在系统空闲时运行，优先级极低 */

    /* 8、配置优先级-----------einheritsched 设置为 PTHREAD_EXPLICIT_SCHED 有用 */
    int iSchedPriority;
    /* 调度优先级1-99，数值越大优先级越高（当 eSchedPolicy 为 SCHED_FIFO/SCHED_RR 模式有用） */

    /* 以下内部使用，用户无需设置 */
    pthread_attr_t *pt_ThreadAttr;   /* 线程属性 */
    pthread_t tThreadPid;            /* 线程PID */
    struct timespec interval;        /* 时间片（仅仅SCHED_RR模式） */
}T_ThreadCreateConfig;

typedef struct  T_THREADPOOLHANDLE T_ThreadPoolHandle; 


/**
* @func         ThreadAPI_ThreadCreate
* @brief        线程管理API-线程创建
* @param[in]    t_Config:创建线程需要的配置
* @param[out]   无
* @return       int ret
* @retval       大于等于0:执行成功
                小于0:执行失败，错误代码
* @warning
* @author      zlzksrl
* @date        2025-10-01
* @Version     V1.0.1
*/

int ThreadAPI_ThreadCreate(T_ThreadCreateConfig *pt_Config);


/**
* @func         ThreadAPI_SetThreadPolicyPriority
* @brief        线程管理API-设置当前线程的策略与优先级
* @param[in]    eSchedPolicy:线程调度策略
* @param[in]    iSchedPriority:线程优先级
* @param[out]   无
* @return       int ret
* @retval       大于等于0:执行成功
                小于0:执行失败，错误代码
* @warning
* @author      zlzksrl
* @date        2025-10-01
* @Version     V1.0.1
*/

int ThreadAPI_SetThreadPolicyPriority(int eSchedPolicy,int iSchedPriority);


/**
* @func         ThreadAPI_SetThreadPriority
* @brief        线程管理API-设置线程的优先级
* @param[in]    iSchedPriority:线程优先级
* @param[out]   无
* @return       int ret
* @retval       大于等于0:执行成功
                小于0:执行失败，错误代码
* @warning
* @author      zlzksrl
* @date        2025-10-01
* @Version     V1.0.1
*/

int ThreadAPI_SetThreadPriority(int iSchedPriority);


/**
* @func         ThreadAPI_PrintThreadAttr
* @brief        线程管理API-打印线程当前的配置信息
* @param[in]    sThreadName:线程名称，自定义的字符串
* @param[in]    tThreadPid:线程号
* @param[out]   无
* @return       int ret
* @retval       大于等于0:执行成功
                小于0:执行失败，错误代码
* @warning
* @author      zlzksrl
* @date        2025-10-01
* @Version     V1.0.1
*/

int ThreadAPI_PrintThreadAttr(const char *sThreadName,pthread_t tThreadPid);




typedef struct  T_THREADPOOLCONFIG
{
    int iMinNum;         /* 线程池最小数量 */
    int iMaxNum;         /* 线程池最大数量 */
    int iQueueMaxSize;   /* 任务队列中最大的任务个数 */
    int iIdleTimeoutMs;  /* 空闲线程超时缩容时间(ms)，0=禁用缩容 */
}T_ThreadPoolConfig;

/**
 * @struct       T_ThreadPoolStats
 * @brief        线程池统计信息结构体
 * @details      用于获取线程池运行期间的统计数据，辅助监控和性能分析
 * @author      zlzksrl
 * @date        2026-05-08
 * @Version     V1.1.0
 */
typedef struct T_THREADPOOLSTATS
{
    unsigned long ulTotalTasksSubmitted;  /**< 总提交任务数 */
    unsigned long ulTotalTasksProcessed;  /**< 总完成任务数 */
    int iPeakBusyThreadNum;               /**< 峰值忙碌线程数 */
    int iPeakTaskQueueLen;                /**< 峰值任务队列长度 */
} T_ThreadPoolStats;

/**
* @func         ThreadAPI_ThreadPoolCreate
* @brief        线程管理API-线程池创建
* @param[in]    t_Config:线程池配置参数
* @param[out]   无
* @return       T_ThreadPoolHandle *pt_ThreadPoolHandle 
* @retval       NULL  = pt_ThreadPoolHandle:线程池创建失败
                NULL  != pt_ThreadPoolHandle:线程池创建成功，返回线程池句柄
* @warning
* @author      zlzksrl
* @date        2025-10-01
* @Version     V1.0.1
*/

T_ThreadPoolHandle* ThreadAPI_ThreadPoolCreate(T_ThreadPoolConfig t_Config);

/**
* @func         ThreadAPI_ThreadPoolDestroy
* @brief        线程管理API-线程池注销
* @param[in]    pt_ThreadPoolHandle:线程池句柄
* @param[out]   无
* @return       int ret
* @retval       大于等于0:执行成功
                小于0:执行失败，错误代码
* @warning
* @author      zlzksrl
* @date        2025-10-01
* @Version     V1.0.1
*/

int ThreadAPI_ThreadPoolDestroy(T_ThreadPoolHandle *pt_ThreadPoolHandle);

/**
* @func         ThreadAPI_ThreadPoolAddTask
* @brief        线程管理API-线程池添加任务
* @param[in]    pt_ThreadPoolHandle:线程池句柄
* @param[in]    TaskFunc:任务函数
* @param[in]    pUserArg:任务函数用户参数
* @param[out]   无
* @return       int ret
* @retval       大于等于0:执行成功
                小于0:执行失败，错误代码
* @warning
* @author      zlzksrl
* @date        2025-10-01
* @Version     V1.0.1
*/

int ThreadAPI_ThreadPoolAddTask(
                        T_ThreadPoolHandle *pt_ThreadPoolHandle
                        ,ThreadFunctionType TaskFunc
                        ,void *pUserArg);



/**
* @func         ThreadAPI_ThreadPoolBusyThreadNumGet
* @brief        线程管理API-获取线程池正在工作的线程个数
* @param[in]    pt_ThreadPoolHandle:线程池句柄
* @param[out]   无
* @return       int iBusyThreadNum
* @retval       大于等于0:正在工作的线程个数
                小于0:执行失败，错误代码
* @warning
* @author      zlzksrl
* @date        2025-10-01
* @Version     V1.0.1
*/

int ThreadAPI_ThreadPoolBusyThreadNumGet(T_ThreadPoolHandle *pt_ThreadPoolHandle);

/**
* @func         ThreadAPI_ThreadPoolLiveThreadNumGet
* @brief        线程管理API-获取线程池存活的线程个数
* @param[in]    pt_ThreadPoolHandle:线程池句柄
* @param[out]   无
* @return       int iLiveThreadNum
* @retval       大于等于0:存活的线程个数
                小于0:执行失败，错误代码
* @warning
* @author      zlzksrl
* @date        2025-10-01
* @Version     V1.0.1
*/

int ThreadAPI_ThreadPoolLiveThreadNumGet(T_ThreadPoolHandle *pt_ThreadPoolHandle);

/**
* @func         ThreadAPI_ThreadPoolTaskQueueLenGet
* @brief        线程管理API-获取线程池任务队列中待处理的任务个数
* @param[in]    pt_ThreadPoolHandle:线程池句柄
* @param[out]   无
* @return       int iTaskQueueLen
* @retval       大于等于0:待处理的任务个数
                小于0:执行失败，错误代码
* @author      zlzksrl
* @date        2026-05-08
* @Version     V1.1.0
*/

int ThreadAPI_ThreadPoolTaskQueueLenGet(T_ThreadPoolHandle *pt_ThreadPoolHandle);

/**
* @func         ThreadAPI_ThreadPoolWaitAllDone
* @brief        线程管理API-等待线程池中所有任务完成
* @details      阻塞等待直到所有已提交的任务执行完毕
*               （忙碌线程数为0且任务队列为空）。
*               当iTimeoutMs为0时，无限等待。
* @param[in]    pt_ThreadPoolHandle:线程池句柄
* @param[in]    iTimeoutMs:超时等待时间，单位:毫秒(ms)，0=无限等待
* @param[out]   无
* @return       int ret
* @retval       0:所有任务已完成
                -1:执行失败，错误代码
                -2:等待超时
* @warning      调用期间不应添加新任务，否则可能永远无法等待完成
* @author      zlzksrl
* @date        2026-05-08
* @Version     V1.1.0
*/

int ThreadAPI_ThreadPoolWaitAllDone(T_ThreadPoolHandle *pt_ThreadPoolHandle, int iTimeoutMs);

/**
* @func         ThreadAPI_ThreadPoolAddTaskTimeout
* @brief        线程管理API-线程池添加任务（带超时）
* @details      与ThreadAPI_ThreadPoolAddTask类似，但当任务队列已满时，
*               不会无限阻塞，而是等待最多iTimeoutMs毫秒。
*               当iTimeoutMs为0时，行为与ThreadAPI_ThreadPoolAddTask相同。
* @param[in]    pt_ThreadPoolHandle:线程池句柄
* @param[in]    TaskFunc:任务函数
* @param[in]    pUserArg:任务函数用户参数
* @param[in]    iTimeoutMs:超时等待时间，单位:毫秒(ms)，0=无限等待
* @param[out]   无
* @return       int ret
* @retval       大于等于0:执行成功
                小于0:执行失败，错误代码
                -2:等待超时，任务未添加
* @author      zlzksrl
* @date        2026-05-08
* @Version     V1.1.0
*/

int ThreadAPI_ThreadPoolAddTaskTimeout(
                        T_ThreadPoolHandle *pt_ThreadPoolHandle
                        ,ThreadFunctionType TaskFunc
                        ,void *pUserArg
                        ,int iTimeoutMs);

/**
* @func         ThreadAPI_ThreadPoolAddTaskTry
* @brief        线程管理API-线程池尝试添加任务（非阻塞）
* @details      尝试将任务添加到线程池。如果任务队列已满，立即返回失败，不会阻塞。
* @param[in]    pt_ThreadPoolHandle:线程池句柄
* @param[in]    TaskFunc:任务函数
* @param[in]    pUserArg:任务函数用户参数
* @param[out]   无
* @return       int ret
* @retval       大于等于0:执行成功
                小于0:执行失败，错误代码
                -2:任务队列已满，任务未添加
* @author      zlzksrl
* @date        2026-05-08
* @Version     V1.1.0
*/

int ThreadAPI_ThreadPoolAddTaskTry(
                        T_ThreadPoolHandle *pt_ThreadPoolHandle
                        ,ThreadFunctionType TaskFunc
                        ,void *pUserArg);

/**
* @func         ThreadAPI_ThreadPoolResize
* @brief        线程管理API-动态调整线程池大小
* @details      调整线程池的最小和最大线程数。
*               - 如果新的最小线程数大于当前存活线程数，会立即创建新线程。
*               - 如果新的最大线程数小于当前存活线程数，空闲线程会在超时后
*                 自动退出（需配置iIdleTimeoutMs启用缩容）。
* @param[in]    pt_ThreadPoolHandle:线程池句柄
* @param[in]    iNewMinNum:新的最小线程数
* @param[in]    iNewMaxNum:新的最大线程数
* @param[out]   无
* @return       int ret
* @retval       大于等于0:执行成功
                小于0:执行失败，错误代码
* @warning      iNewMinNum必须大于0且小于等于iNewMaxNum
* @author      zlzksrl
* @date        2026-05-08
* @Version     V1.1.0
*/

int ThreadAPI_ThreadPoolResize(T_ThreadPoolHandle *pt_ThreadPoolHandle,
                               int iNewMinNum, int iNewMaxNum);

/**
* @func         ThreadAPI_ThreadPoolStatsGet
* @brief        线程管理API-获取线程池统计信息
* @param[in]    pt_ThreadPoolHandle:线程池句柄
* @param[out]   pt_Stats:统计信息输出结构体指针
* @return       int ret
* @retval       大于等于0:执行成功
                小于0:执行失败，错误代码
* @author      zlzksrl
* @date        2026-05-08
* @Version     V1.1.0
*/

int ThreadAPI_ThreadPoolStatsGet(T_ThreadPoolHandle *pt_ThreadPoolHandle,
                                 T_ThreadPoolStats *pt_Stats);


/**
* @func         pthread_create
* @brief        创建一个新的线程并开始执行指定的线程函数
* @param[in]    thread: 指向 pthread_t 类型的指针，用于返回新创建线程的线程ID
* @param[in]    attr: 指向 pthread_attr_t 类型的指针，用于指定线程属性
*               若为 NULL，则使用默认属性
* @param[in]    start_routine: 线程函数（入口函数）的指针，新线程从此函数开始执行
* @param[in]    arg: 传递给线程函数的参数（void* 类型）
* @param[out]   无
* @return       int ret
* @retval       0: 线程创建成功
*               非0: 创建失败，返回错误码（如 EAGAIN: 资源不足, EINVAL: 属性无效）
* @warning      新线程与主线程共享进程地址空间，需注意同步与资源竞争
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/

//int pthread_create(pthread_t *thread, const pthread_attr_t *attr,void *(*start_routine)(void *), void *arg);


/**
* @func         pthread_exit
* @brief        终止调用该函数的线程，并返回一个退出状态值
* @param[in]    value_ptr: 指向 void* 的指针，用于传递线程退出状态
*               该值可被 pthread_join 获取；若不需要返回值可设为 NULL
* @param[out]   无
* @return       void
* @retval       无返回值（函数不返回）
* @warning      主线程中调用此函数仅终止主线程，整个进程是否退出取决于是否有其他非分离线程
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/

//void pthread_exit(void *value_ptr);



/**
* @func         pthread_self
* @brief        获取当前调用线程的线程ID
* @param[in]    无
* @param[out]   无
* @return       pthread_t
* @retval       当前线程的线程ID
* @warning      线程ID仅在当前进程中唯一，不能跨进程使用
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//pthread_t pthread_self(void);


/**
* @func         pthread_equal
* @brief        比较两个线程ID是否代表同一个线程
* @param[in]    t1: 第一个线程ID
* @param[in]    t2: 第二个线程ID
* @param[out]   无
* @return       int ret
* @retval       非0: 两个线程ID相等（代表同一线程）
*               0: 两个线程ID不相等
* @warning      不能用于比较不同进程中的线程ID
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//int pthread_equal(pthread_t t1, pthread_t t2);


/**
* @func         pthread_join
* @brief        等待指定线程结束，并回收其资源
* @param[in]    thread: 要等待的线程ID
* @param[out]   value_ptr: 指向 void* 指针的指针，用于获取线程的退出状态
*               若不需要退出状态，可设为 NULL
* @return       int ret
* @retval       0: 成功等待线程结束
*               非0: 失败（如 ESRCH: 线程不存在, EDEADLK: 死锁）
* @warning      只能对处于 joinable 状态的线程调用；不能对自身或已分离线程调用
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//int pthread_join(pthread_t thread, void **value_ptr);

/**
* @func         pthread_detach
* @brief        将指定线程设置为分离状态（detached），使其终止时自动释放资源
* @param[in]    thread: 要设置为分离状态的线程ID
* @param[out]   无
* @return       int ret
* @retval       0: 设置成功
*               非0: 设置失败（如 ESRCH: 线程不存在, EINVAL: 线程已分离）
* @warning      分离状态的线程不能再被 pthread_join；否则行为未定义
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//int pthread_detach(pthread_t thread);

/**
* @func         pthread_cancel
* @brief        向指定线程发送取消请求（Cancellation Request）
* @param[in]    thread: 目标线程的线程ID
* @param[out]   无
* @return       int ret
* @retval       0: 取消请求成功发送
*               非0: 发送失败（如 ESRCH: 线程不存在）
* @warning      线程是否响应取消请求取决于其取消状态和类型；
*               默认为延迟取消（在取消点响应），需配合 pthread_testcancel 使用
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//int pthread_cancel(pthread_t thread);


/**
* @func         pthread_cleanup_push
* @brief        将一个线程清理函数（cleanup handler）压入当前线程的清理函数栈
* @param[in]    routine: 清理函数指针，函数原型为 void func(void *)
* @param[in]    arg: 传递给清理函数的参数（void* 类型）
* @param[out]   无
* @return       void
* @retval       无返回值
* @warning      该宏必须与 pthread_cleanup_pop 成对使用，且必须位于同一作用域和同一代码块中；
*               通常实现为宏，可能包含不匹配的 '{' 或 '}'，不能跨函数或跨作用域使用。
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//void pthread_cleanup_push(void (*routine)(void *), void *arg);


/**
* @func         pthread_cleanup_pop
* @brief        从当前线程的清理函数栈中弹出一个清理函数
* @param[in]    execute: 执行控制标志
*               - 非0值：弹出后立即执行该清理函数
*               - 0：仅弹出，不执行
* @param[out]   无
* @return       void
* @retval       无返回值
* @warning      必须与 pthread_cleanup_push 成对使用，且在同一作用域内；
*               如果在 pthread_cleanup_push 和 pthread_cleanup_pop 之间发生非正常返回
*               （如 longjmp、goto 跳出），可能导致清理函数未被正确弹出或执行。
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//void pthread_cleanup_pop(int execute);

/**
* @func         pthread_setschedparam
* @brief        设置线程的调度策略和调度参数（如优先级）
* @param[in]    thread: 目标线程的线程ID
* @param[in]    policy: 调度策略，可取值包括 SCHED_OTHER、SCHED_FIFO、SCHED_RR 等
* @param[in]    param: 指向 sched_param 结构的指针，用于指定调度参数（如 sched_priority）
* @param[out]   无
* @return       int ret
* @retval       0: 执行成功
*               非0: 执行失败，返回错误码（如 EINVAL: 参数无效, EPERM: 权限不足）
* @warning      使用实时调度策略（SCHED_FIFO/SCHED_RR）通常需要 root 权限或 CAP_SYS_NICE 能力
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param);

/**
* @func         pthread_setcancelstate
* @brief        设置线程的取消状态（是否允许被取消）
* @param[in]    state: 取消状态，可取值：
*                       PTHREAD_CANCEL_ENABLE  : 允许线程被取消（默认）
*                       PTHREAD_CANCEL_DISABLE : 禁止线程被取消
* @param[out]   oldstate: 指向 int 的指针，用于保存之前的取消状态（可为 NULL）
* @return       int ret
* @retval       0: 执行成功
*               非0: 执行失败，返回错误码
* @warning      禁用取消状态可用于保护临界区，但需确保不会永久禁用导致无法取消
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//int pthread_setcancelstate(int state, int *oldstate);



/**
* @func         pthread_setcanceltype
* @brief        设置线程的取消类型（取消的执行方式）
* @param[in]    type: 取消类型，可取值：
*                     PTHREAD_CANCEL_DEFERRED : 延迟取消（默认），仅在取消点响应取消请求
*                     PTHREAD_CANCEL_ASYNCHRONOUS : 异步取消，可随时被取消（危险，不推荐）
* @param[out]   oldtype: 指向 int 的指针，用于保存之前的取消类型（可为 NULL）
* @return       int ret
* @retval       0: 执行成功
*               非0: 执行失败，返回错误码
* @warning      异步取消可能导致资源泄漏或状态不一致，应尽量避免使用
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//int pthread_setcanceltype(int type, int *oldtype);



/**
* @func         pthread_testcancel
* @brief        创建一个线程取消点（Cancellation Point）
* @param[in]    无
* @param[out]   无
* @return       void
* @retval       无返回值
* @warning      当线程处于 PTHREAD_CANCEL_DEFERRED 模式时，必须调用此函数或进入系统调用等取消点，
*               才能响应之前收到的取消请求。否则取消请求会被挂起。
* @author      
* @date        2025-10-07
* @Version     V1.0.1
*/
//void pthread_testcancel(void);



#ifdef __cplusplus
 }
#endif

#endif
