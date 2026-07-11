/**
 * @file        ThreadQueue.h
 * @brief       LinuxARM-PublicLib-线程通讯队列-公共API头文件
 * @details     IMX6ULL平台
 *              本文件提供两种队列类型的公共API:
 *
 *              1. ThreadQueue - 环形缓冲区线程安全队列
 *                 适用于需要缓存多条消息的生产者-消费者场景
 *                 支持多生产者多消费者，队列满/空时自动阻塞
 *
 *              2. LatestQueue - 最新数据队列
 *                 只保留最新一条数据，自动丢弃旧数据
 *                 适用于传感器数据、状态信息等只关心最新值的场景
 *
 * @author      zlzksrl
 * @Version     V1.1.0
 * @date        2026-05-07
 * @copyright   copyright (C) 2024
 */

/**
 * @date        2024-08-15
 * @Version     V1.0.0
 * @brief       创建文件，提供基础队列API
 * @author      zlzksrl
 *
 * @date        2026-05-07
 * @Version     V1.1.0
 * @brief       新增 Close/Reopen/IsClosed/GetLength/Flush 等队列管理API;
 *              新增 LatestQueue 最新数据队列全套API;
 *              修复 PutThreadQueueMsg 返回值文档错误
 * @author      zlzksrl
 */
#ifndef __ThreadQueue_H__
#define __ThreadQueue_H__

#ifdef __cplusplus
 extern "C" {
#endif


/* ================================================================== */
/*                                                                    */
/*     ThreadQueue - 环形缓冲区线程安全队列 API                         */
/*                                                                    */
/*  使用流程:                                                         */
/*    InitThreadQueueMsg() → PutThreadQueueMsg() / GetThreadQueueMsg() */
/*    → CloseThreadQueueMsg() → FlushThreadQueueMsg()                 */
/*    → DestroyThreadQueueMsg()                                       */
/*                                                                    */
/*  错误码约定:                                                       */
/*     0  : 成功                                                      */
/*    -1  : 参数无效或未初始化                                         */
/*    -2  : 队列已关闭（仅 Put 返回）                                   */
/*                                                                    */
/* ================================================================== */

/** @brief 线程队列结构体前向声明（隐藏内部实现） */
typedef struct T_THREADQUEUEMSG T_ThreadQueueMsg;

/**
 * @func         InitThreadQueueMsg
 * @brief        初始化线程队列
 * @details      分配队列结构体和环形缓冲区内存，初始化互斥锁和条件变量。
 *               调用成功后，*ppt_QueueMsg 指向有效的队列结构体。
 *               release_callback 用于在 Destroy 时自动释放残留数据:
 *               - 非 NULL: 残留数据被逐条调用 callback(data) 释放
 *               - NULL: 残留数据被直接丢弃（可能内存泄漏，会打印警告）
 * @param[in]    ppt_QueueMsg:      队列结构体指针的指针，调用前 *ppt_QueueMsg 必须为 NULL
 * @param[in]    iQueueLen:         环形缓冲区容量（元素个数），建议设置稍大的值以减少阻塞
 * @param[in]    sQueueName:        队列名称，用于调试日志标识，最长31个字符
 * @param[in]    release_callback:  数据释放回调函数，可为 NULL
 * @return       int ret
 * @retval       0:    初始化成功
 * @retval       -1:   初始化失败（参数无效或内存分配失败）
 * @warning      ppt_QueueMsg 不能为 NULL，*ppt_QueueMsg 必须为 NULL
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int InitThreadQueueMsg(T_ThreadQueueMsg **ppt_QueueMsg, int iQueueLen, const char *sQueueName, void (*release_callback)(void* data));

/**
 * @func         PutThreadQueueMsg
 * @brief        向线程队列发送消息（生产者调用）
 * @details      将 data 指针存入环形缓冲区。如果队列已满，生产者线程将阻塞等待
 *               直到有消费者取出数据腾出空间。如果队列已关闭，立即返回 -2。
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @param[in]    data:        待发送的数据指针（不能为 NULL）
 * @return       int ret
 * @retval       0:    发送成功
 * @retval       -1:   参数无效或队列未初始化
 * @retval       -2:   队列已关闭，数据未被存入
 * @warning      data 不能为 NULL；调用者负责 data 的内存管理
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int PutThreadQueueMsg(T_ThreadQueueMsg *pt_QueueMsg, void *data);

/**
 * @func         PutThreadQueueMsgTimeout
 * @brief        向线程队列发送消息（带超时，生产者调用）
 * @details      将 data 指针存入环形缓冲区。如果队列已满，生产者线程将阻塞等待
 *               直到有消费者取出数据腾出空间，或等待时间超过 timeo 毫秒后超时返回。
 *               如果队列已关闭，立即返回 -2。
 *               与 PutThreadQueueMsg 的区别:
 *               - PutThreadQueueMsg: 队列满时无限阻塞等待
 *               - PutThreadQueueMsgTimeout: 队列满时带超时等待，超时后返回 -3
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @param[in]    data:        待发送的数据指针（不能为 NULL）
 * @param[in]    timeo:       超时等待时间，单位: 毫秒(ms)，队列满时最长等待时间
 * @return       int ret
 * @retval       0:    发送成功
 * @retval       -1:   参数无效或队列未初始化
 * @retval       -2:   队列已关闭，数据未被存入
 * @retval       -3:   队列满且等待超时，数据未被存入
 * @warning      data 不能为 NULL；超时返回时调用者需自行处理 data 的内存释放
 * @author       zlzksrl
 * @date         2026-05-08
 * @Version      V1.2.0
 */
int PutThreadQueueMsgTimeout(T_ThreadQueueMsg *pt_QueueMsg, void *data, int timeo);

/**
 * @func         GetThreadQueueMsg
 * @brief        从线程队列获取消息（消费者调用）
 * @details      从环形缓冲区取出最早存入的数据指针。如果队列为空，消费者线程
 *               将阻塞等待直到有生产者写入数据或超时。
 *               队列关闭后仍可读取剩余数据，关闭且为空时返回 NULL。
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @param[in]    timeo:       超时等待时间，单位: 毫秒(ms)
 * @return       void* 返回取出的数据指针
 * @retval       非NULL: 成功获取的数据指针
 * @retval       NULL:   队列为空且超时，或队列已关闭且为空，或参数无效
 * @warning      返回的数据指针由调用者负责释放（如果是从堆分配的）
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
void* GetThreadQueueMsg(T_ThreadQueueMsg *pt_QueueMsg, int timeo);

/**
 * @func         CloseThreadQueueMsg
 * @brief        关闭线程队列，阻止新消息写入
 * @details      设置 is_closed 标志，并广播唤醒所有阻塞的生产者和消费者线程。
 *               关闭后:
 *               - PutThreadQueueMsg() 将返回 -2
 *               - GetThreadQueueMsg() 仍可读取队列中的剩余数据
 *               - 队列为空且关闭时，GetThreadQueueMsg() 返回 NULL
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @return       int ret
 * @retval       0:    关闭成功
 * @retval       -1:   参数无效或队列未初始化
 * @warning      关闭操作不可逆（除非调用 ReopenThreadQueueMsg）
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int CloseThreadQueueMsg(T_ThreadQueueMsg *pt_QueueMsg);

/**
 * @func         ReopenThreadQueueMsg
 * @brief        重新打开已关闭的线程队列
 * @details      重置 is_closed 标志和读写偏移量，使队列恢复可用状态。
 *               前置条件:
 *               - 队列必须处于已关闭状态
 *               - 队列中不能有剩余数据（需先 Flush）
 *               - 不能有等待中的线程（需先 join 消费者/生产者线程）
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @return       int ret
 * @retval       0:    重新打开成功
 * @retval       -1:   前置条件不满足或参数无效
 * @warning      调用前需确保所有等待线程已退出，且队列数据已刷新完毕
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int ReopenThreadQueueMsg(T_ThreadQueueMsg *pt_QueueMsg);

/**
 * @func         IsThreadQueueClosed
 * @brief        查询线程队列是否已关闭
 * @details      供外部代码（如消费者线程）判断队列关闭状态，以决定是否退出循环。
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @return       int ret
 * @retval       1:    队列已关闭
 * @retval       0:    队列未关闭（正常运行中）
 * @retval       -1:   参数无效
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int IsThreadQueueClosed(T_ThreadQueueMsg *pt_QueueMsg);

/**
 * @func         GetThreadQueueLength
 * @brief        查询线程队列当前数据长度
 * @details      在互斥锁保护下读取 nData 字段，返回当前队列中的数据条数。
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @return       int ret
 * @retval       >=0:  队列中当前数据条数
 * @retval       -1:   参数无效
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int GetThreadQueueLength(T_ThreadQueueMsg *pt_QueueMsg);

/**
 * @func         FlushThreadQueueMsg
 * @brief        刷新线程队列，通过回调处理所有剩余数据
 * @details      逐条取出队列中的剩余数据，对每条数据调用 callback 进行处理。
 *               典型用法是在销毁队列前释放所有剩余数据的内存。
 *               调用期间会短暂释放互斥锁以执行回调，允许其他线程操作队列。
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @param[in]    callback:    数据处理回调函数，每条剩余数据都会被传入
 * @return       int ret
 * @retval       >=0:  刷新处理的数据条数
 * @retval       -1:   参数无效
 * @warning      callback 中不应长时间阻塞，否则会影响队列性能
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int FlushThreadQueueMsg(T_ThreadQueueMsg *pt_QueueMsg, void (*callback)(void* data));

/**
 * @func         DestroyThreadQueueMsg
 * @brief        销毁线程队列，释放所有资源
 * @details      如果队列未关闭，会自动先关闭。等待所有阻塞线程退出后，
 *               释放残留数据（通过 release_callback 逐条释放）、释放环形缓冲区、
 *               销毁互斥锁和条件变量、释放结构体内存，并将 *ppt_QueueMsg 置为 NULL。
 *               如果未注册 release_callback，残留数据仅打印警告（可能内存泄漏）。
 * @param[in]    ppt_QueueMsg: 队列结构体指针的指针
 * @return       int ret
 * @retval       0:    销毁成功
 * @retval       -1:   参数无效或队列未初始化
 * @warning      销毁后 *ppt_QueueMsg 被置为 NULL，不能再使用该指针
 * @note         建议先调用 FlushThreadQueueMsg 以便在销毁前自定义处理残留数据
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int DestroyThreadQueueMsg(T_ThreadQueueMsg **ppt_QueueMsg);


/* ================================================================== */
/*                                                                    */
/*     LatestQueue - 最新数据队列 API                                  */
/*                                                                    */
/*  特点: 只保留最新一条数据，自动丢弃旧数据                              */
/*  适用场景: 传感器数据、状态信息等只关心最新值的场景                     */
/*                                                                    */
/*  使用流程:                                                         */
/*    InitLatestQueueMsg() → PutLatestQueueMsg()                       */
/*    / GetLatestQueueMsg()                                            */
/*    → CloseLatestQueueMsg() → FlushLatestQueueMsg()                  */
/*    → DestroyLatestQueueMsg()                                        */
/*                                                                    */
/*  错误码约定:                                                       */
/*     0  : 成功                                                      */
/*    -1  : 参数无效或未初始化                                         */
/*    -2  : 队列已关闭（仅 Put 返回）                                   */
/*                                                                    */
/* ================================================================== */

/** @brief 最新数据队列结构体前向声明（隐藏内部实现） */
typedef struct T_LATESTQUEUEMSG T_LatestQueueMsg;

/**
 * @func         InitLatestQueueMsg
 * @brief        初始化最新数据队列
 * @details      分配结构体内存，初始化互斥锁和条件变量，注册数据释放回调。
 *               release_callback 用于在 Put 时自动释放被丢弃的旧数据:
 *               - 非 NULL: 旧数据被丢弃时自动调用 callback(旧数据)
 *               - NULL: 旧数据被直接丢弃（可能内存泄漏，会打印警告）
 * @param[in]    ppt_QueueMsg:      队列结构体指针的指针，*ppt_QueueMsg 必须为 NULL
 * @param[in]    sQueueName:        队列名称，用于调试日志，最长31个字符
 * @param[in]    release_callback:  数据释放回调函数，可为 NULL
 * @return       int ret
 * @retval       0:    初始化成功
 * @retval       -1:   初始化失败（参数无效或内存分配失败）
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int InitLatestQueueMsg(T_LatestQueueMsg **ppt_QueueMsg, const char *sQueueName, void (*release_callback)(void* data));

/**
 * @func         PutLatestQueueMsg
 * @brief        向最新数据队列写入数据（生产者调用）
 * @details      如果队列中存在未读旧数据:
 *               - 已注册 release_callback: 调用 callback(旧数据) 释放
 *               - 未注册 release_callback: 直接丢弃旧数据（打印警告）
 *               然后存入新数据并唤醒等待的消费者。
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @param[in]    data:        待发送的数据指针（不能为 NULL）
 * @return       int ret
 * @retval       0:    发送成功
 * @retval       -1:   参数无效或队列未初始化
 * @retval       -2:   队列已关闭，数据未被存入
 * @warning      data 不能为 NULL
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int PutLatestQueueMsg(T_LatestQueueMsg *pt_QueueMsg, void *data);

/**
 * @func         GetLatestQueueMsg
 * @brief        从最新数据队列获取最新数据（消费者调用）
 * @details      如果队列中没有数据(has_data==0)，消费者线程将阻塞等待
 *               直到生产者写入新数据或超时。
 *               取出数据后清空 has_data 标志。
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @param[in]    timeo:       超时等待时间，单位: 毫秒(ms)
 * @return       void* 返回最新的数据指针
 * @retval       非NULL: 成功获取的最新数据
 * @retval       NULL:   超时无数据，或队列已关闭且无数据，或参数无效
 * @warning      返回的数据指针由调用者负责释放（如果是从堆分配的）
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
void* GetLatestQueueMsg(T_LatestQueueMsg *pt_QueueMsg, int timeo);

/**
 * @func         CloseLatestQueueMsg
 * @brief        关闭最新数据队列，阻止新消息写入
 * @details      设置 is_closed 标志，广播唤醒所有阻塞的消费者线程。
 *               关闭后 PutLatestQueueMsg() 返回 -2。
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @return       int ret
 * @retval       0:    关闭成功
 * @retval       -1:   参数无效或队列未初始化
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int CloseLatestQueueMsg(T_LatestQueueMsg *pt_QueueMsg);

/**
 * @func         IsLatestQueueClosed
 * @brief        查询最新数据队列是否已关闭
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @return       int ret
 * @retval       1:    队列已关闭
 * @retval       0:    队列未关闭
 * @retval       -1:   参数无效
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int IsLatestQueueClosed(T_LatestQueueMsg *pt_QueueMsg);

/**
 * @func         FlushLatestQueueMsg
 * @brief        刷新最新数据队列，通过回调处理剩余数据
 * @details      如果队列中有未读数据，取出并通过 callback 处理。
 *               由于 LatestQueue 最多只有一条数据，返回值为 0 或 1。
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @param[in]    callback:    数据处理回调函数
 * @return       int ret
 * @retval       0:    队列为空，无需刷新
 * @retval       1:    成功刷新1条数据
 * @retval       -1:   参数无效
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int FlushLatestQueueMsg(T_LatestQueueMsg *pt_QueueMsg, void (*callback)(void* data));

/**
 * @func         ReopenLatestQueueMsg
 * @brief        重新打开已关闭的最新数据队列
 * @details      前置条件:
 *               - 队列必须处于已关闭状态
 *               - 队列中不能有剩余数据（需先 Flush）
 *               - 不能有等待中的线程
 * @param[in]    pt_QueueMsg: 队列结构体指针
 * @return       int ret
 * @retval       0:    重新打开成功
 * @retval       -1:   前置条件不满足或参数无效
 * @warning      调用前需确保队列数据已刷新完毕
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int ReopenLatestQueueMsg(T_LatestQueueMsg *pt_QueueMsg);

/**
 * @func         DestroyLatestQueueMsg
 * @brief        销毁最新数据队列，释放所有资源
 * @details      如果队列未关闭，会自动先关闭。等待所有阻塞线程退出后，
 *               释放残留数据（通过 release_callback）、销毁互斥锁和条件变量、
 *               释放结构体内存，并将 *ppt_QueueMsg 置为 NULL。
 * @param[in]    ppt_QueueMsg: 队列结构体指针的指针
 * @return       int ret
 * @retval       0:    销毁成功
 * @retval       -1:   参数无效或队列未初始化
 * @warning      销毁后 *ppt_QueueMsg 被置为 NULL，不能再使用该指针
 * @author       zlzksrl
 * @date         2026-05-07
 * @Version      V1.1.0
 */
int DestroyLatestQueueMsg(T_LatestQueueMsg **ppt_QueueMsg);


#ifdef __cplusplus
 }
#endif

#endif
