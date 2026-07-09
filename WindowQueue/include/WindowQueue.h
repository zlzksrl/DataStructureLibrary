/**
 * @file        WindowQueue.h
 * @brief       LinuxARM-PublicLib-滑动窗口队列-公共API头文件
 * @details     IMX6ULL平台
 *              本文件提供 WindowQueue 滑动窗口队列的公共API。
 *              WindowQueue 是一个有界环形缓冲区，始终保留最近 N 条数据，
 *              队列满时自动丢弃最老数据（零拷贝，O(1)），适用于传感器数据采集、
 *              信号处理（移动平均、中值滤波、Savitzky-Golay 等）等只关心
 *              最近一段数据的场景。
 *
 *              核心特性:
 *              - 有界环形缓冲区: 满则丢弃最老数据（读指针前进，零拷贝 O(1)）
 *              - 值拷贝预分配:   Init 时预分配 size×element_size 连续内存，
 *                                队列持有数据副本，无需 release_callback
 *              - 只读窗口访问:   Snapshot（快照拷贝）/ ForEach（锁内零拷贝回调）
 *              - 动态调整容量:   运行中 Resize 变长/变短
 *              - 运行统计:       累计 Put 次数 / 累计丢弃次数 / 峰值窗口长度
 *              - 多线程安全:     pthread_mutex_t 保护，支持多生产者多消费者
 *
 *              使用示例:
 *              @code
 *              // 1. 定义传感器数据结构
 *              typedef struct { int ts; float value; } Sensor;
 *
 *              // 2. 初始化队列：容量8，元素大小=sizeof(Sensor)
 *              T_WindowQueueMsg *q = NULL;
 *              WindowQueueAPI_Init(&q, 8, sizeof(Sensor), "sensor");
 *
 *              // 3. 采集线程不断 Put（满则自动丢最老）
 *              Sensor s = { .ts = 1, .value = 3.14f };
 *              WindowQueueAPI_Put(q, &s);
 *
 *              // 4. 处理线程做中值滤波：快照最近5条
 *              Sensor buf[5];
 *              int n = WindowQueueAPI_Snapshot(q, buf, 5);  // n<=5, 老->新
 *              median_filter(buf, n);
 *
 *              // 5. 销毁
 *              WindowQueueAPI_Destroy(&q);
 *              @endcode
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-09
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-09
 * @Version     V1.0.0
 * @brief       创建文件，提供滑动窗口队列全套API
 * @author      zlzksrl
 */
#ifndef __WindowQueue_H__
#define __WindowQueue_H__

#ifdef __cplusplus
 extern "C" {
#endif


/* ================================================================== */
/*                                                                    */
/*     类型定义                                                        */
/*                                                                    */
/* ================================================================== */

/** @brief 滑动窗口队列句柄前向声明（隐藏内部实现，opaque pointer） */
typedef struct T_WINDOWQUEUEMSG T_WindowQueueMsg;

/**
 * @func         WindowQueueDataCb
 * @brief        窗口数据回调函数原型
 * @details      用于 ForEach / Flush，按 老→新 顺序逐条调用。
 *               - ForEach: 回调在互斥锁内执行，data 指向队列内部环形缓冲条目，
 *                 零拷贝。回调必须快速返回，否则阻塞采集线程的 Put；
 *                 且禁止在回调内调用本队列任何 API（互斥锁非递归，会死锁）。
 *               - Flush:   回调在锁内执行，data 指向待处理的窗口条目。
 * @param[in]    data:      指向一条 element_size 字节的数据（只读）
 * @param[in]    index:     窗口内序号，0=最老，n-1=最新
 * @param[in]    user_ctx:  调用者透传的上下文指针
 */
typedef void (*WindowQueueDataCb)(const void *data, int index, void *user_ctx);


/* ================================================================== */
/*                                                                    */
/*     生命周期管理                                                    */
/*                                                                    */
/*  使用流程:                                                         */
/*    Init() -> Put()/Snapshot()/ForEach()                            */
/*    -> Close() -> Reopen()(可选) -> Flush() -> Destroy()            */
/*                                                                    */
/*  错误码约定:                                                       */
/*      0  : 成功                                                     */
/*     -1  : 参数无效或未初始化                                        */
/*     -2  : 队列已关闭（仅 Put 返回）                                  */
/*     >0  : 仅 Put，本次丢弃的最老数据条数（1）                        */
/*                                                                    */
/* ================================================================== */

/**
 * @func         WindowQueueAPI_Init
 * @brief        滑动窗口队列API-初始化队列
 * @details      分配 T_WindowQueueMsg 结构体，并预分配 iQueueLen × iElementSize
 *               字节的连续环形缓冲区内存，初始化互斥锁、队列名称与统计字段。
 *               调用成功后队列处于"已初始化、未关闭"状态。
 * @param[in]    ppt_QueueMsg:  队列句柄的二级指针，调用前 *ppt_QueueMsg 必须为 NULL
 * @param[in]    iQueueLen:     环形缓冲区容量（元素个数），必须 > 0
 * @param[in]    iElementSize:  单个元素字节数（sizeof(用户结构体)），必须 > 0
 * @param[in]    sQueueName:    队列名称，用于调试日志标识
 * @return       int ret
 * @retval       0:   初始化成功
 * @retval       -1:  参数无效、内存分配失败或 *ppt_QueueMsg 非空（重复初始化）
 * @warning      ppt_QueueMsg 不能为 NULL，*ppt_QueueMsg 必须为 NULL
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_Init(T_WindowQueueMsg **ppt_QueueMsg,
                        int iQueueLen, int iElementSize, const char *sQueueName);

/**
 * @func         WindowQueueAPI_Destroy
 * @brief        滑动窗口队列API-销毁队列，释放所有资源
 * @details      释放预分配的环形缓冲区内存、销毁互斥锁、释放结构体内存，
 *               并将 *ppt_QueueMsg 置为 NULL。值拷贝模式下无需释放单条数据。
 * @param[in]    ppt_QueueMsg: 队列句柄的二级指针
 * @return       int ret
 * @retval       0:   销毁成功
 * @retval       -1:  参数无效或队列未初始化
 * @warning      销毁后 *ppt_QueueMsg 被置为 NULL，不能再使用该指针；
 *               多线程环境下应确保无其他线程正在访问队列（建议先 Close 并 join 相关线程）
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_Destroy(T_WindowQueueMsg **ppt_QueueMsg);

/**
 * @func         WindowQueueAPI_Close
 * @brief        滑动窗口队列API-关闭队列，阻止新数据写入
 * @details      在锁内设置 is_closed 标志。关闭后：
 *               - Put 返回 -2，数据不被写入；
 *               - Snapshot / ForEach / GetLength 等只读操作仍可正常使用
 *                 （可读取关闭前残留的窗口数据，用于最后处理）。
 *               适用于采集停止后冻结窗口的场景。
 * @param[in]    pt_QueueMsg: 队列句柄
 * @return       int ret
 * @retval       0:   关闭成功
 * @retval       -1:  参数无效或队列未初始化
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_Close(T_WindowQueueMsg *pt_QueueMsg);

/**
 * @func         WindowQueueAPI_Reopen
 * @brief        滑动窗口队列API-重新打开已关闭的队列
 * @details      重置 is_closed 标志，使 Put 恢复可用。
 *               本结构无阻塞等待线程，故仅需队列处于"已关闭"状态即可重新打开
 *               （窗口中的残留数据保持不变）。
 * @param[in]    pt_QueueMsg: 队列句柄
 * @return       int ret
 * @retval       0:   重新打开成功
 * @retval       -1:  参数无效、未初始化或队列未处于关闭状态
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_Reopen(T_WindowQueueMsg *pt_QueueMsg);

/**
 * @func         WindowQueueAPI_IsClosed
 * @brief        滑动窗口队列API-查询队列是否已关闭
 * @param[in]    pt_QueueMsg: 队列句柄
 * @return       int ret
 * @retval       1:   队列已关闭
 * @retval       0:   队列未关闭
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_IsClosed(T_WindowQueueMsg *pt_QueueMsg);


/* ================================================================== */
/*                                                                    */
/*     写入与窗口访问                                                  */
/*                                                                    */
/*  说明:                                                             */
/*    Put      - 写入最新数据，满则丢弃最老（永不阻塞）                  */
/*    Snapshot - 快照拷贝最新N条到用户缓冲（锁内拷贝，锁外处理）          */
/*    ForEach  - 锁内零拷贝回调遍历整个窗口                            */
/*                                                                    */
/* ================================================================== */

/**
 * @func         WindowQueueAPI_Put
 * @brief        滑动窗口队列API-写入一条数据（永不阻塞）
 * @details      将 data 指向的 element_size 字节按值拷贝到环形缓冲尾部（成为最新）。
 *               - 未满：直接写入，nData++；
 *               - 已满：读指针 lget 前进一格丢弃最老（零拷贝），再写入新的，nData 不变。
 *               返回本次因满而丢弃的条数，便于上层统计丢包率。
 *               统计字段同步更新：ulTotalPut++，丢弃时 ulTotalDiscarded++，
 *               并按需更新 iPeakLength。
 * @param[in]    pt_QueueMsg: 队列句柄
 * @param[in]    data:        待写入的数据指针（仅作拷贝源，不能为 NULL）
 * @return       int ret
 * @retval       0:   写入成功，未丢弃数据
 * @retval       1:   写入成功，且丢弃了 1 条最老数据
 * @retval       -1:  参数无效或队列未初始化
 * @retval       -2:  队列已关闭，数据未被写入
 * @warning      data 不能为 NULL；Put 返回后用户可立即复用/释放 data 缓冲
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_Put(T_WindowQueueMsg *pt_QueueMsg, const void *data);

/**
 * @func         WindowQueueAPI_Snapshot
 * @brief        滑动窗口队列API-快照拷贝最新N条数据（只读，不消费）
 * @details      将窗口中**最新的 min(max_count, nData) 条**数据按 老→新 顺序
 *               连续拷贝到 out_buf。当窗口数据多于 max_count 时，只取最近的 max_count 条。
 *               拷贝在互斥锁内完成后立即释放锁，用户可在锁外对 out_buf 排序/求和，
 *               不阻塞采集线程。零内部 malloc。
 * @param[in]    pt_QueueMsg: 队列句柄
 * @param[out]   out_buf:     用户提供的输出缓冲，至少需 返回值×element_size 字节，不能为 NULL
 * @param[in]    max_count:   期望拷贝的最大条数，必须 > 0
 * @return       int ret
 * @retval       >=0: 实际拷贝的条数 n（0 <= n <= min(max_count, nData)）
 * @retval       -1:  参数无效或队列未初始化
 * @warning      out_buf 不能为 NULL；拷贝的是结构体值，用户按自身类型访问
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_Snapshot(T_WindowQueueMsg *pt_QueueMsg,
                            void *out_buf, int max_count);

/**
 * @func         WindowQueueAPI_ForEach
 * @brief        滑动窗口队列API-回调遍历整个窗口（只读，零拷贝）
 * @details      按 老→新 顺序，对窗口中每条数据调用一次 callback。
 *               回调在互斥锁内执行，data 直接指向队列内部环形缓冲条目（零拷贝），
 *               index 为窗口内序号（0=最老）。
 * @param[in]    pt_QueueMsg: 队列句柄
 * @param[in]    callback:    回调函数，不能为 NULL
 * @param[in]    user_ctx:    透传给回调的上下文指针，可为 NULL
 * @return       int ret
 * @retval       >=0: 遍历处理的数据条数 n
 * @retval       -1:  参数无效或队列未初始化
 * @warning      回调必须快速返回（在锁内执行，会阻塞采集线程的 Put）；
 *               禁止在回调内调用本队列任何 API（互斥锁非递归，会死锁）
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_ForEach(T_WindowQueueMsg *pt_QueueMsg,
                           WindowQueueDataCb callback, void *user_ctx);


/* ================================================================== */
/*                                                                    */
/*     容量与查询                                                      */
/*                                                                    */
/* ================================================================== */

/**
 * @func         WindowQueueAPI_Resize
 * @brief        滑动窗口队列API-动态调整队列容量
 * @details      重新分配 new_size×element_size 内存，按 FIFO 顺序搬移现有数据：
 *               - 变大：保留全部现有数据，容量扩展；
 *               - 变小：从最老端丢弃 (nData-new_size) 条多余数据，其余保留。
 *               全程在互斥锁内完成，原子替换内部 buffer 指针并重置读写偏移。
 * @param[in]    pt_QueueMsg: 队列句柄
 * @param[in]    new_size:    新的容量（元素个数），必须 > 0
 * @return       int ret
 * @retval       0:   调整成功
 * @retval       -1:  参数无效、new_size<=0 或内存分配失败
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_Resize(T_WindowQueueMsg *pt_QueueMsg, int new_size);

/**
 * @func         WindowQueueAPI_GetLength
 * @brief        滑动窗口队列API-获取当前窗口数据条数
 * @param[in]    pt_QueueMsg: 队列句柄
 * @return       int ret
 * @retval       >=0: 当前窗口数据条数 n（0 <= n <= capacity）
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_GetLength(T_WindowQueueMsg *pt_QueueMsg);

/**
 * @func         WindowQueueAPI_GetCapacity
 * @brief        滑动窗口队列API-获取队列容量
 * @param[in]    pt_QueueMsg: 队列句柄
 * @return       int ret
 * @retval       >0:  队列容量 size
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_GetCapacity(T_WindowQueueMsg *pt_QueueMsg);


/* ================================================================== */
/*                                                                    */
/*     刷新                                                            */
/*                                                                    */
/* ================================================================== */

/**
 * @func         WindowQueueAPI_Flush
 * @brief        滑动窗口队列API-刷新队列，回调处理窗口数据并清空
 * @details      按 老→新 顺序，对窗口中每条数据调用一次 callback，然后清空队列（nData=0）。
 *               典型用途是销毁前对窗口数据做最后一次处理（如落盘、统计输出）。
 *               回调在互斥锁内执行。callback 为 NULL 时仅清空队列，不对数据回调
 *               （值拷贝模式下销毁前通常无需处理残留）。
 * @param[in]    pt_QueueMsg: 队列句柄
 * @param[in]    callback:    数据处理回调，可为 NULL（NULL 时仅清空队列）
 * @param[in]    user_ctx:    透传给回调的上下文指针，可为 NULL
 * @return       int ret
 * @retval       >=0: 刷新处理的数据条数
 * @retval       -1:  参数无效或队列未初始化
 * @warning      回调内禁止调用本队列任何 API（互斥锁非递归，会死锁）
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_Flush(T_WindowQueueMsg *pt_QueueMsg,
                         WindowQueueDataCb callback, void *user_ctx);


/* ================================================================== */
/*                                                                    */
/*     运行统计                                                        */
/*                                                                    */
/* ================================================================== */

/**
 * @struct       T_WindowQueueStats
 * @brief        滑动窗口队列运行统计信息
 * @details      用于监控采集负载与丢包率，字段由库内部维护，通过 StatsGet 读出。
 */
typedef struct T_WINDOWQUEUESTATS
{
    unsigned long ulTotalPut;        /**< 累计成功 Put 次数 */
    unsigned long ulTotalDiscarded;  /**< 累计因满丢弃的最老数据条数（丢包率 = ulTotalDiscarded / ulTotalPut） */
    int           iPeakLength;       /**< 窗口历史峰值数据条数 */
} T_WindowQueueStats;

/**
 * @func         WindowQueueAPI_StatsGet
 * @brief        滑动窗口队列API-获取运行统计信息
 * @param[in]    pt_QueueMsg: 队列句柄
 * @param[out]   pt_Stats:    统计信息输出结构体指针，不能为 NULL
 * @return       int ret
 * @retval       0:   获取成功
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_StatsGet(T_WindowQueueMsg *pt_QueueMsg, T_WindowQueueStats *pt_Stats);


/* ================================================================== */
/*                                                                    */
/*     入队回调（Push 轻量处理，可选）                                 */
/*                                                                    */
/*  说明:                                                             */
/*    注册后，每次成功 Put 在互斥锁内构建当前窗口的只读指针视图，       */
/*    并调用回调。零拷贝（直接指向队列内部条目）。                      */
/*    适合“每条数据入队即做轻量处理”（累加、均值、阈值判断等）。        */
/*    未注册回调时，Put 无任何额外开销。                               */
/*                                                                    */
/*  ⚠️ 约束（与 ForEach 相同，均在锁内）:                              */
/*    - 回调必须快速返回（适合 O(n) 简单运算，勿做排序等重运算）        */
/*    - 禁止在回调内调用本队列任何 API（mutex 非递归，会死锁）          */
/*    - entries 仅在回调期间有效（指向队列内部，返回后可能被覆盖）      */
/*                                                                    */
/* ================================================================== */

/**
 * @func         WindowQueuePutCb
 * @brief        入队回调函数原型
 * @details      每次成功 Put 后在互斥锁内调用，传入当前完整窗口的只读指针视图。
 *               entries[i] 指向第 i 条数据（老->新），零拷贝。
 * @param[in]    entries:   指针数组，count 个，每个指向一条 element_size 字节数据（老->新）
 * @param[in]    count:     当前窗口数据条数（0 <= count <= capacity）
 * @param[in]    user_ctx:  调用者透传的上下文指针
 * @warning      entries 仅在回调执行期间有效；回调必须快速返回；禁调本队列 API
 */
typedef void (*WindowQueuePutCb)(const void * const *entries, int count, void *user_ctx);

/**
 * @func         WindowQueueAPI_SetPutCallback
 * @brief        滑动窗口队列API-注册/取消入队回调
 * @details      注册后，每次成功 Put 会在互斥锁内构建窗口指针视图并调用 cb。
 *               传入 cb=NULL 可取消回调。未注册时 Put 无额外开销。
 *               可在任意时刻调用（线程安全，内部加锁）。
 * @param[in]    pt_QueueMsg: 队列句柄
 * @param[in]    cb:           回调函数，NULL 表示取消
 * @param[in]    user_ctx:     透传给回调的上下文指针，可为 NULL
 * @return       int ret
 * @retval       0:   设置成功
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int WindowQueueAPI_SetPutCallback(T_WindowQueueMsg *pt_QueueMsg,
                                  WindowQueuePutCb cb, void *user_ctx);


#ifdef __cplusplus
 }
#endif

#endif
