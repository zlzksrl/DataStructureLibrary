/**
 * @file        ThreadManage_Main.c
 * @brief       LinuxARM-PublicLib-线程管理-主程序文件
 * @details     IMX6ULL平台
 *              本文件实现线程管理的核心功能:
 *              1. ThreadAPI_ThreadCreate        - 线程创建（支持完整属性配置）
 *              2. ThreadAPI_SetThreadPolicyPriority - 设置当前线程调度策略与优先级
 *              3. ThreadAPI_SetThreadPriority    - 设置当前线程优先级（保持现有策略）
 *              4. ThreadAPI_PrintThreadAttr      - 打印线程属性信息（运行时获取）
 *              5. ThreadAPI_CreatePrintThreadAttr - 打印线程属性信息（创建时获取）
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

/* _GNU_SOURCE: 启用GNU扩展，提供pthread_getattr_np等Linux特有函数的声明 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include "../include/ThreadManage.h"
#include "ThreadManage_Main.h"
#include "ThreadManage_Maketime.h"



/* ======================== 内部函数前向声明 ======================== */
static int ThreadAPI_CreatePrintThreadAttr(pthread_attr_t *pt_ThreadAttr);


/**
 * @func         ThreadAPI_ThreadCreate
 * @brief        线程管理API-线程创建
 * @details      根据T_ThreadCreateConfig配置创建线程，支持三种属性配置模式:
 *               - eSetAttr == 0: 使用默认属性创建线程（不配置任何属性）
 *               - eSetAttr == 1: 仅配置线程栈大小
 *               - eSetAttr == 2: 配置线程全部属性（栈大小、分离状态、调度策略、优先级等）
 *
 *               线程创建成功后，pt_Config->tThreadPid 会被填充为新线程的ID，
 *               可用于后续的 pthread_join / pthread_detach 等操作。
 *
 *               线程属性（pthread_attr_t）在创建完成后会被立即销毁和释放，
 *               因为 pthread_create 会复制属性内容，不需要保留原始属性对象。
 *
 * @param[in]    pt_Config: 创建线程需要的配置结构体指针
 *                   必填字段: pThreadFunc, sThreadName
 *                   可选字段: eSetAttr, istacksize_MB, eDetachState,
 *                            einheritsched, eSchedPolicy, iSchedPriority
 * @param[out]   无
 * @return       int ret
 * @retval       0: 执行成功
 * @retval       -1: 执行失败（参数无效、属性设置失败、线程创建失败）
 * @warning      sThreadName 如果是动态分配的（如strdup），调用者需在函数返回后自行释放
 * @warning      使用 SCHED_FIFO/SCHED_RR 调度策略通常需要 root 权限
 * @note         创建成功后会自动调用 ThreadAPI_CreatePrintThreadAttr 打印线程属性信息
 * @note         以下参数在超出合法范围时会被自动修正（不会返回错误）:
 *               - istacksize_MB: 超出 [1,32] 范围时自动修正为 8 (MB)
 *               - eDetachState:  非 JOINABLE/DETACHED 时自动修正为 PTHREAD_CREATE_JOINABLE
 *               - eSchedPolicy:  非 SCHED_FIFO/SCHED_RR 时强制回退为 SCHED_OTHER
 *               - iSchedPriority: 非 SCHED_FIFO/SCHED_RR 模式下强制设为 0
 * @author      zlzksrl
 * @date        2025-10-01
 * @Version     V1.0.1
 */

int ThreadAPI_ThreadCreate(T_ThreadCreateConfig *pt_Config)
{   
    int ret = 0;

    /* ======================== 参数校验 ======================== */
    /* 检查配置结构体指针是否有效 */
    if(NULL == pt_Config)
    {
        ThreadManage_printx("fail");
        return -1;
    }
    /* 检查必填字段: 线程函数指针和线程名称不能为空 */
    if(NULL == pt_Config->pThreadFunc
        || NULL == pt_Config->sThreadName)
    {
        ThreadManage_printx("fail");
        return -1;
    }

    /* ======================== 属性配置 ======================== */
    /*
     * 根据 eSetAttr 的值决定属性配置级别:
     *   0 = 不配置属性，使用系统默认值（pt_ThreadAttr 设为 NULL）
     *   1 = 仅配置栈大小
     *   2 = 配置全部属性（栈大小 + 分离状态 + 调度策略 + 优先级）
     */
    if(1 == pt_Config->eSetAttr
        || 2 == pt_Config->eSetAttr)
    {
        /* 分配 pthread_attr_t 内存（线程属性对象） */
        pt_Config->pt_ThreadAttr = (pthread_attr_t *)malloc(sizeof(pthread_attr_t));
        if(NULL == pt_Config->pt_ThreadAttr)
        {
            ThreadManage_printx("fail");
            return -1;
        }
        memset(pt_Config->pt_ThreadAttr, 0, sizeof(pthread_attr_t));

        /* 步骤1: 初始化线程属性对象（必须在使用属性前调用） */
        ret = pthread_attr_init(pt_Config->pt_ThreadAttr);
        if(0 != ret)
        {
            ThreadManage_printx("pthread_attr_init fail ret = [%d]", ret);
            free(pt_Config->pt_ThreadAttr);
            pt_Config->pt_ThreadAttr = NULL;
            return -1;
        }
            
        /* 步骤2: 设置线程栈大小 */
        /*
         * 栈大小有效范围: 1~32 MB
         * 超出范围时自动修正为默认值 8 MB
         * 注意: 实际设置为字节单位，需乘以 1024*1024 转换
         */
        if(pt_Config->istacksize_MB <= 0 || 32 < pt_Config->istacksize_MB)
        {
            pt_Config->istacksize_MB = 8;
        }
        ret = pthread_attr_setstacksize(pt_Config->pt_ThreadAttr,
                                         pt_Config->istacksize_MB * 1024 * 1024);
        if(0 != ret)
        {
            ThreadManage_printx("pthread_attr_setstacksize fail ret = [%d]", ret);
            pthread_attr_destroy(pt_Config->pt_ThreadAttr);
            free(pt_Config->pt_ThreadAttr);
            pt_Config->pt_ThreadAttr = NULL;
            return -1;
        }

        /* 步骤3~5: 仅在 eSetAttr == 2 时配置完整属性 */
        if(2 == pt_Config->eSetAttr)
        {
            /* 步骤3: 设置线程分离状态 */
            /*
             * PTHREAD_CREATE_JOINABLE (默认): 线程结束后不自动释放资源，
             *                                   需要其他线程调用 pthread_join 回收
             * PTHREAD_CREATE_DETACHED:        线程结束后自动释放资源，
             *                                   不能被 pthread_join
             * 无效值自动修正为 PTHREAD_CREATE_JOINABLE
             */
            if(PTHREAD_CREATE_JOINABLE != pt_Config->eDetachState
                && PTHREAD_CREATE_DETACHED != pt_Config->eDetachState)
            {
                pt_Config->eDetachState = PTHREAD_CREATE_JOINABLE;
            }
            ret = pthread_attr_setdetachstate(pt_Config->pt_ThreadAttr,
                                               pt_Config->eDetachState);
            if(0 != ret)
            {
                ThreadManage_printx("pthread_attr_setdetachstate fail ret = [%d]", ret);
                pthread_attr_destroy(pt_Config->pt_ThreadAttr);
                free(pt_Config->pt_ThreadAttr);
                pt_Config->pt_ThreadAttr = NULL;
                return -1;
            }

            /* 步骤4: 设置调度继承策略与调度参数 */
            /*
             * PTHREAD_INHERIT_SCHED:  新线程继承创建者线程的调度属性（忽略attr中的设置）
             * PTHREAD_EXPLICIT_SCHED: 新线程使用attr中显式设置的调度策略和参数
             */
            if(PTHREAD_EXPLICIT_SCHED == pt_Config->einheritsched)
            {
                /* 设置为显式调度模式 */
                ret = pthread_attr_setinheritsched(pt_Config->pt_ThreadAttr,
                                                    PTHREAD_EXPLICIT_SCHED);
                if(0 != ret)
                {
                    ThreadManage_printx("pthread_attr_setinheritsched fail ret = [%d]", ret);
                    pthread_attr_destroy(pt_Config->pt_ThreadAttr);
                    free(pt_Config->pt_ThreadAttr);
                    pt_Config->pt_ThreadAttr = NULL;
                    return -1;
                }

                /* 步骤4a: 验证并设置调度策略与优先级 */
                /*
                 * 仅 SCHED_FIFO 和 SCHED_RR 支持自定义优先级(1~99)
                 * SCHED_OTHER 不支持设置优先级（始终为0）
                 */
                if(SCHED_RR == pt_Config->eSchedPolicy
                || SCHED_FIFO  == pt_Config->eSchedPolicy)
                {
                    /* 获取系统支持的最大/最小优先级，验证用户设置的优先级是否在合法范围 */
                    int iMaxPriority = sched_get_priority_max(pt_Config->eSchedPolicy);
                    int iMinPriority = sched_get_priority_min(pt_Config->eSchedPolicy);
                    if(iMaxPriority < 0 || iMinPriority < 0)
                    {
                        ThreadManage_printx("sched_get_priority fail");
                        pthread_attr_destroy(pt_Config->pt_ThreadAttr);
                        free(pt_Config->pt_ThreadAttr);
                        pt_Config->pt_ThreadAttr = NULL;
                        return -1;
                    }
                    /* 优先级范围检查: [iMinPriority, iMaxPriority] */
                    if(iMinPriority <= pt_Config->iSchedPriority
                        && pt_Config->iSchedPriority <= iMaxPriority)
                    {
                        /* 优先级合法，继续设置 */
                    }
                    else
                    {
                        ThreadManage_printx("priority[%d] out of range[%d,%d]",
                                            pt_Config->iSchedPriority,
                                            iMinPriority, iMaxPriority);
                        pthread_attr_destroy(pt_Config->pt_ThreadAttr);
                        free(pt_Config->pt_ThreadAttr);
                        pt_Config->pt_ThreadAttr = NULL;
                        return -1;
                    }
                }
                else
                {
                    /* 非实时调度策略，回退到 SCHED_OTHER，优先级强制为0 */
                    pt_Config->eSchedPolicy = SCHED_OTHER;
                    pt_Config->iSchedPriority = 0;
                }

                /* 设置调度策略 */
                ret = pthread_attr_setschedpolicy(pt_Config->pt_ThreadAttr,
                                                   pt_Config->eSchedPolicy);
                if(0 != ret)
                {
                    ThreadManage_printx("pthread_attr_setschedpolicy fail ret = [%d]", ret);
                    pthread_attr_destroy(pt_Config->pt_ThreadAttr);
                    free(pt_Config->pt_ThreadAttr);
                    pt_Config->pt_ThreadAttr = NULL;
                    return -1;
                }

                /* 设置调度优先级 */
                struct sched_param param;
                param.sched_priority = pt_Config->iSchedPriority;
                ret = pthread_attr_setschedparam(pt_Config->pt_ThreadAttr, &param);
                if(0 != ret)
                {
                    ThreadManage_printx("pthread_attr_setschedparam fail ret = [%d]", ret);
                    pthread_attr_destroy(pt_Config->pt_ThreadAttr);
                    free(pt_Config->pt_ThreadAttr);
                    pt_Config->pt_ThreadAttr = NULL;
                    return -1;
                }
            }
            else
            {
                /* 继承创建线程的调度属性（PTHREAD_INHERIT_SCHED） */
                ret = pthread_attr_setinheritsched(pt_Config->pt_ThreadAttr,
                                                    PTHREAD_INHERIT_SCHED);
                if(0 != ret)
                {
                    ThreadManage_printx("pthread_attr_setinheritsched fail ret = [%d]", ret);
                    pthread_attr_destroy(pt_Config->pt_ThreadAttr);
                    free(pt_Config->pt_ThreadAttr);
                    pt_Config->pt_ThreadAttr = NULL;
                    return -1;
                }
            }
        }
    }
    else
    {
        /* eSetAttr == 0: 不配置属性，使用系统默认值 */
        pt_Config->pt_ThreadAttr = NULL;
    }

    /* ======================== 创建线程 ======================== */
    /*
     * pthread_create 参数说明:
     *   参数1: 线程ID输出（存入 pt_Config->tThreadPid）
     *   参数2: 线程属性（NULL则使用默认属性）
     *   参数3: 线程入口函数
     *   参数4: 传递给入口函数的用户参数
     */
    ret = pthread_create(
            &(pt_Config->tThreadPid)
            ,pt_Config->pt_ThreadAttr
            ,pt_Config->pThreadFunc
            ,pt_Config->pThreadFuncUserArg);
    if(0 != ret)
    {
        /* 创建失败: 清理已分配的属性对象 */
        if(NULL != pt_Config->pt_ThreadAttr)
        {
            pthread_attr_destroy(pt_Config->pt_ThreadAttr);
            free(pt_Config->pt_ThreadAttr);
            pt_Config->pt_ThreadAttr = NULL;
        }
        ThreadManage_printx("pthread_create fail ret = [%d]", ret);
        return -1;
    }
    else
    {
        /* 打印线程创建成功信息 */
        ThreadManage_printx("pthread_create [%s] Successful"
                        ,pt_Config->sThreadName);
        /*
         * 打印线程详细属性信息
         * 使用 ThreadAPI_CreatePrintThreadAttr 从 pthread_attr_t 读取属性，
         * 而非 ThreadAPI_PrintThreadAttr（从已运行的线程读取），
         * 避免新线程在打印前就已退出的竞态条件
         */
        if(NULL != pt_Config->pt_ThreadAttr)
        {
            ThreadAPI_CreatePrintThreadAttr(pt_Config->pt_ThreadAttr);
            /* 线程属性已由 pthread_create 复制，可以安全销毁原始属性对象 */
            pthread_attr_destroy(pt_Config->pt_ThreadAttr);
            free(pt_Config->pt_ThreadAttr);
            pt_Config->pt_ThreadAttr = NULL;
        }
    }
    return 0;
}


/**
 * @func         ThreadAPI_SetThreadPolicyPriority
 * @brief        线程管理API-设置当前线程的策略与优先级
 * @details      修改调用线程自身的调度策略和优先级。
 *               内部通过 pthread_setschedparam 实现。
 *
 *               调度策略说明:
 *               - SCHED_OTHER: 默认分时调度策略，优先级必须为0（不支持自定义优先级）
 *               - SCHED_FIFO:  先进先出实时策略，优先级范围1~99，需要root权限
 *               - SCHED_RR:    时间片轮转实时策略，优先级范围1~99，需要root权限
 *
 *               对于 SCHED_FIFO/SCHED_RR:
 *               - 优先级数值越大，优先级越高
 *               - 会自动验证优先级是否在系统支持的范围内
 *               - 超出范围时返回错误（-1），不会自动回退
 *
 * @param[in]    eSchedPolicy: 线程调度策略
 *               可选值: SCHED_OTHER, SCHED_FIFO, SCHED_RR
 * @param[in]    iSchedPriority: 线程优先级
 *               SCHED_OTHER: 必须为0
 *               SCHED_FIFO/SCHED_RR: 1~99（数值越大优先级越高）
 * @param[out]   无
 * @return       int ret
 * @retval       0: 执行成功
 * @retval       -1: 执行失败（参数无效、权限不足等）
 * @warning      使用实时调度策略（SCHED_FIFO/SCHED_RR）需要 root 权限或 CAP_SYS_NICE 能力
 * @note         此函数设置的是调用线程自身的属性，不是其他线程的
 * @author      zlzksrl
 * @date        2025-10-01
 * @Version     V1.0.1
 */

int ThreadAPI_SetThreadPolicyPriority(int eSchedPolicy, int iSchedPriority)
{ 
    int ret = 0;
    struct sched_param param;
    pthread_t tThreadPid = pthread_self();

    /*
     * 根据调度策略类型处理优先级:
     * - SCHED_FIFO/SCHED_RR: 验证优先级范围，合法则使用用户设置的值
     * - 其他策略: 回退到 SCHED_OTHER，优先级强制为0
     */
    if(SCHED_RR == eSchedPolicy
    || SCHED_FIFO  == eSchedPolicy)
    {    
        /* 获取系统支持的优先级范围 */
        int iMaxPriority = sched_get_priority_max(eSchedPolicy);
        int iMinPriority = sched_get_priority_min(eSchedPolicy);
        if(iMaxPriority < 0 || iMinPriority < 0)
        {
            ThreadManage_printx("sched_get_priority fail");
            return -1;
        }
        /* 验证用户设置的优先级是否在合法范围 [iMinPriority, iMaxPriority] */
        if(iMinPriority <= iSchedPriority && iSchedPriority <= iMaxPriority)
        {
            /* 优先级合法，使用用户设置的值 */
            param.sched_priority = iSchedPriority;
        }
        else
        {
            /* 优先级超出范围，返回错误（与 ThreadAPI_ThreadCreate 行为一致） */
            ThreadManage_printx("priority[%d] out of range[%d,%d]",
                                iSchedPriority, iMinPriority, iMaxPriority);
            return -1;
        }
    }
    else
    {
        /* 非实时调度策略，使用 SCHED_OTHER，优先级为0 */
        eSchedPolicy   = SCHED_OTHER;
        param.sched_priority = 0;
    }

    /* 打印设置信息（用于调试） */
    ThreadManage_printx("[%lu]->[%d],[%d]"
            ,(unsigned long)tThreadPid
            ,eSchedPolicy
            ,param.sched_priority
    );

    /* 调用 pthread_setschedparam 设置调度参数 */
    ret = pthread_setschedparam(tThreadPid, eSchedPolicy, &param);
    if(ret != 0)
    {
        ThreadManage_printx("pthread_setschedparam fail ret = [%d]", ret);
        return -1;
    }
    return 0;
}


/**
 * @func         ThreadAPI_SetThreadPriority
 * @brief        线程管理API-设置线程的优先级
 * @details      在保持当前调度策略不变的情况下，修改调用线程的优先级。
 *               内部实现:
 *               1. 通过 pthread_getschedparam 获取当前线程的调度策略
 *               2. 调用 ThreadAPI_SetThreadPolicyPriority 设置新的优先级
 *
 *               注意: 如果当前策略是 SCHED_OTHER，则优先级只能为0，
 *                     此函数调用不会有效果。
 *
 * @param[in]    iSchedPriority: 要设置的线程优先级
 *               SCHED_FIFO/SCHED_RR: 1~99
 *               SCHED_OTHER: 只能为0
 * @param[out]   无
 * @return       int ret
 * @retval       0: 执行成功
 * @retval       -1: 执行失败
 * @warning      需要root权限才能修改实时线程的优先级
 * @author      zlzksrl
 * @date        2025-10-01
 * @Version     V1.0.1
 */

int ThreadAPI_SetThreadPriority(int iSchedPriority)
{ 
    int ret = 0;
    struct sched_param param;
    pthread_t tThreadPid = pthread_self();
    int eSchedPolicy;

    /* 获取当前线程的调度策略与优先级 */
    if(0 != pthread_getschedparam(tThreadPid, &eSchedPolicy, &param))
    {
        ThreadManage_printx("pthread_getschedparam fail");
        return -1;
    }

    /* 保持当前策略不变，仅更新优先级 */
    ret = ThreadAPI_SetThreadPolicyPriority(eSchedPolicy, iSchedPriority);
    return ret;
}

/**
 * @func         ThreadAPI_PrintThreadAttr
 * @brief        线程管理API-打印线程当前的配置信息
 * @details      通过 pthread_getattr_np 获取指定线程的运行时属性信息，
 *               并格式化打印以下内容:
 *               - 线程名称
 *               - 线程ID
 *               - 栈大小（字节和MB）
 *               - 分离状态（JOINABLE/DETACHED）
 *               - 调度继承策略（INHERIT/EXPLICIT）
 *               - 调度策略（OTHER/FIFO/RR）
 *               - 调度优先级
 *
 *               pthread_getattr_np 是 Linux 特有函数（非POSIX标准），
 *               可以获取已创建线程的属性，包括内核可能调整后的实际值。
 *
 * @param[in]    sThreadName: 线程名称（自定义字符串，用于日志标识）
 * @param[in]    tThreadPid: 线程号（pthread_t类型）
 * @param[out]   无
 * @return       int ret
 * @retval       0: 执行成功
 * @retval       -1: 执行失败（参数无效、获取属性失败）
 * @warning      pthread_getattr_np 是 Linux 特有函数，不可移植到其他 POSIX 系统
 * @warning      目标线程必须仍然存活，否则行为未定义
 * @author      zlzksrl
 * @date        2025-10-01
 * @Version     V1.0.1
 */

int ThreadAPI_PrintThreadAttr(const char *sThreadName, pthread_t tThreadPid)
{ 
    /* 参数校验 */
    if (NULL == sThreadName) 
    {
        ThreadManage_printx("fail");
        return -1;
    }

    int ret = 0;
    pthread_attr_t t_ThreadAttr; /* 线程属性对象（用于存储获取到的属性） */

    /* 初始化属性对象（pthread_getattr_np 要求传入已初始化的attr） */
    ret = pthread_attr_init(&t_ThreadAttr);
    if (ret != 0)
    {
        ThreadManage_printx("pthread_attr_init fail ret = [%d]", ret);
        return -1;
    }

    /* 
     * 通过 pthread_getattr_np 获取线程的运行时属性
     * 注意: 此函数是 GNU 扩展，非 POSIX 标准
     * 它能获取内核实际使用的属性值（可能不同于创建时设置的值）
     */
    ret = pthread_getattr_np(tThreadPid, &t_ThreadAttr);
    if (ret != 0)
    {
        ThreadManage_printx("pthread_getattr_np fail ret = [%d]", ret);
        pthread_attr_destroy(&t_ThreadAttr);
        return -1;
    }

    /* 定义属性变量 */
    int istacksize = 0;        /* 栈大小（字节） */

    int eDetachState;           /* 分离状态 */
    char *sDetachState = NULL;  /* 分离状态字符串描述 */

    int einheritsched;          /* 调度继承策略 */
    char *sInheritsched = NULL; /* 继承策略字符串描述 */

    int eSchedPolicy;           /* 调度策略 */
    char *sSchedPolicy = NULL;  /* 调度策略字符串描述 */

    int iSchedPriority = -1;    /* 调度优先级 */
    struct sched_param t_SchedParam;
    
    /* 获取分离状态 */
    ret = pthread_attr_getdetachstate(&t_ThreadAttr, &eDetachState);
    if (ret != 0)
    {
        ThreadManage_printx("pthread_attr_getdetachstate fail");
        sDetachState = strdup("fail");
    }
    else
    {
        if(PTHREAD_CREATE_JOINABLE == eDetachState)
        {
            sDetachState = strdup("PTHREAD_CREATE_JOINABLE");
        }
        else if(PTHREAD_CREATE_DETACHED == eDetachState)
        {
            sDetachState = strdup("PTHREAD_CREATE_DETACHED");
        }
        else
        {
            sDetachState = strdup("unknown");
        }
    }

    /* 获取调度继承策略 */
    ret = pthread_attr_getinheritsched(&t_ThreadAttr, &einheritsched);
    if (ret != 0)
    {
        ThreadManage_printx("pthread_attr_getinheritsched fail");
        sInheritsched = strdup("fail");
    }
    else
    {
        if(PTHREAD_INHERIT_SCHED == einheritsched)
        {
            sInheritsched = strdup("PTHREAD_INHERIT_SCHED");
        }
        else if(PTHREAD_EXPLICIT_SCHED == einheritsched)
        {
            sInheritsched = strdup("PTHREAD_EXPLICIT_SCHED");
        }
        else
        {
            sInheritsched = strdup("unknown");
        }
    }

    /* 获取调度策略 */
    ret = pthread_attr_getschedpolicy(&t_ThreadAttr, &eSchedPolicy);
    if (ret != 0)
    {
        ThreadManage_printx("pthread_attr_getschedpolicy fail");
        sSchedPolicy = strdup("fail");
    }
    else
    {
        if(SCHED_OTHER == eSchedPolicy)
        {
            sSchedPolicy = strdup("SCHED_OTHER");
        }
        else if(SCHED_FIFO == eSchedPolicy)
        {
            sSchedPolicy = strdup("SCHED_FIFO");
        }
        else if(SCHED_RR == eSchedPolicy)
        {
            sSchedPolicy = strdup("SCHED_RR");
        }
        else
        {
            sSchedPolicy = strdup("unknown");
        }
    }

    /* 获取调度参数（含优先级） */
    ret = pthread_attr_getschedparam(&t_ThreadAttr, &t_SchedParam);
    if (ret != 0) 
    {
        ThreadManage_printx("pthread_attr_getschedparam fail");
    }
    else
    {
        iSchedPriority = t_SchedParam.sched_priority;
    }

    /* 获取栈大小 */
    ret = pthread_attr_getstacksize(&t_ThreadAttr, (size_t *)&istacksize);
    if (ret != 0) 
    {
        ThreadManage_printx("pthread_attr_getstacksize fail");
    }

    /* 格式化打印所有属性信息 */
    /* 安全检查: strdup 可能返回 NULL（内存不足时），使用 "(null)" 占位避免 UB */
    ThreadManage_printx("Name:[%s],ID = [%lu],StackSize_Bytes = [%d] = [%d]MB,"
                        "DetachState = [%s],Inheritsched = [%s],"
                        "SchedPolicy = [%s],SchedPriority = [%d]"
            ,sThreadName
            ,(unsigned long)tThreadPid
            ,istacksize
            ,(istacksize / 1024 / 1024)
            ,sDetachState  ? sDetachState  : "(null)"
            ,sInheritsched ? sInheritsched : "(null)"
            ,sSchedPolicy  ? sSchedPolicy  : "(null)"
            ,iSchedPriority);

    /* 释放 strdup 分配的字符串内存（free(NULL) 安全，无需额外判断） */
    free(sDetachState);
    free(sInheritsched);
    free(sSchedPolicy);

    /* 销毁属性对象 */
    pthread_attr_destroy(&t_ThreadAttr);
    return 0;
}


/**
 * @func         ThreadAPI_CreatePrintThreadAttr
 * @brief        线程管理API-打印线程属性信息（创建时使用）
 * @details      与 ThreadAPI_PrintThreadAttr 不同，此函数直接从 pthread_attr_t
 *               属性对象读取信息，而不是从已运行的线程获取。
 *               在 pthread_create 成功后调用，打印创建时配置的线程属性信息。
 *
 *               打印内容:
 *               - 栈大小（字节和MB）
 *               - 分离状态（JOINABLE/DETACHED）
 *               - 调度继承策略（INHERIT/EXPLICIT）
 *               - 调度策略（OTHER/FIFO/RR）
 *               - 调度优先级
 *
 * @param[in]    pt_ThreadAttr: 线程属性对象指针（pthread_attr_t*）
 * @param[out]   无
 * @return       int ret
 * @retval       0: 执行成功（包括 pt_ThreadAttr 为 NULL 时直接返回0）
 * @retval       -1: 获取属性过程中出错（不影响主流程）
 * @warning      此函数目前未在公共头文件中声明，仅作为内部调试工具
 * @note         当 pt_ThreadAttr 为 NULL 时，函数直接返回0（视为无属性可打印）
 * @author      zlzksrl
 * @date        2025-10-01
 * @Version     V1.0.1
 */

static int ThreadAPI_CreatePrintThreadAttr(pthread_attr_t *pt_ThreadAttr)
{ 
    /* 属性指针为空时直接返回（无属性可打印） */
    if(NULL == pt_ThreadAttr)
    {
        return 0;
    }
    int ret = 0;

    /* 定义属性变量 */
    int istacksize = -1;        /* 栈大小（字节） */

    int eDetachState;           /* 分离状态 */
    char *sDetachState = NULL;  /* 分离状态字符串描述 */

    int einheritsched;          /* 调度继承策略 */
    char *sInheritsched = NULL; /* 继承策略字符串描述 */

    int eSchedPolicy;           /* 调度策略 */
    char *sSchedPolicy = NULL;  /* 调度策略字符串描述 */

    int iSchedPriority = -1;    /* 调度优先级 */
    struct sched_param t_SchedParam;
    
    /* 获取分离状态 */
    ret = pthread_attr_getdetachstate(pt_ThreadAttr, &eDetachState);
    if (ret != 0) 
    {
        ThreadManage_printx("pthread_attr_getdetachstate fail");
        sDetachState = strdup("fail");
    }
    else
    {
        if(PTHREAD_CREATE_JOINABLE == eDetachState)
        {
            sDetachState = strdup("PTHREAD_CREATE_JOINABLE");
        }
        else if(PTHREAD_CREATE_DETACHED == eDetachState)
        {
            sDetachState = strdup("PTHREAD_CREATE_DETACHED");
        }
        else
        {
            sDetachState = strdup("unknown");
        }
    }

    /* 获取调度继承策略 */
    ret = pthread_attr_getinheritsched(pt_ThreadAttr, &einheritsched);
    if (ret != 0) 
    {
        ThreadManage_printx("pthread_attr_getinheritsched fail");
        sInheritsched = strdup("fail");
    }
    else
    {
        if(PTHREAD_INHERIT_SCHED == einheritsched)
        {
            sInheritsched = strdup("PTHREAD_INHERIT_SCHED");
        }
        else if(PTHREAD_EXPLICIT_SCHED == einheritsched)
        {
            sInheritsched = strdup("PTHREAD_EXPLICIT_SCHED");
        }
        else
        {
            sInheritsched = strdup("unknown");
        }
    }

    /* 获取调度策略 */
    ret = pthread_attr_getschedpolicy(pt_ThreadAttr, &eSchedPolicy);
    if (ret != 0) 
    {
        ThreadManage_printx("pthread_attr_getschedpolicy fail");
        sSchedPolicy = strdup("fail");
    }
    else
    {
        if(SCHED_OTHER == eSchedPolicy)
        {
            sSchedPolicy = strdup("SCHED_OTHER");
        }
        else if(SCHED_FIFO == eSchedPolicy)
        {
            sSchedPolicy = strdup("SCHED_FIFO");
        }
        else if(SCHED_RR == eSchedPolicy)
        {
            sSchedPolicy = strdup("SCHED_RR");
        }
        else
        {
            sSchedPolicy = strdup("unknown");
        }
    }

    /* 获取调度参数（含优先级） */
    ret = pthread_attr_getschedparam(pt_ThreadAttr, &t_SchedParam);
    if (ret != 0) 
    {
        ThreadManage_printx("pthread_attr_getschedparam fail");
    }
    else
    {
        iSchedPriority = t_SchedParam.sched_priority;
    }

    /* 获取栈大小 */
    ret = pthread_attr_getstacksize(pt_ThreadAttr, (size_t * )&istacksize);
    if (ret != 0) 
    {
        ThreadManage_printx("pthread_attr_getstacksize fail");
    }

    /* 格式化打印属性信息（不含线程名称和ID，因为此时线程尚未创建） */
    /* 安全检查: strdup 可能返回 NULL（内存不足时），使用 "(null)" 占位避免 UB */
    ThreadManage_printx("StackSize_Bytes = [%d] = [%d]MB,"
                        "DetachState = [%s],Inheritsched = [%s],"
                        "SchedPolicy = [%s],SchedPriority = [%d]"
            ,istacksize
            ,(istacksize / 1024 / 1024)
            ,sDetachState  ? sDetachState  : "(null)"
            ,sInheritsched ? sInheritsched : "(null)"
            ,sSchedPolicy  ? sSchedPolicy  : "(null)"
            ,iSchedPriority);

    /* 释放 strdup 分配的字符串内存（free(NULL) 安全，无需额外判断） */
    free(sDetachState);
    free(sInheritsched);
    free(sSchedPolicy);
    return 0;
}

