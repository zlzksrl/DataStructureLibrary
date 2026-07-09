/**
 * @file        StreamBuffer.h
 * @brief       LinuxARM-PublicLib-流缓冲区-公共API头文件
 * @details     IMX6ULL平台
 *              本文件提供 StreamBuffer 流缓冲区的公共API。
 *              StreamBuffer 是一个通用的字节流环形缓冲库，核心模式为
 *              "生产者攒字节流 + 消费者按阈值/超时批量出队"。
 *
 *              库**不创建消费线程**：线程由用户自行创建与调度（线程数、优先级、
 *              消费方式完全由用户决定）。库只提供缓冲 + 触发等待 + 出队。
 *
 *              核心特性:
 *              - 字节流环形缓冲: 预分配连续内存，变长字节紧凑存储，环形复用，零 malloc
 *              - 写入不阻塞:      缓冲满则丢弃本次写入（满则丢新），立即返回
 *              - 两步式消费:      Wait() 阻塞等待触发（used≥阈值/超时/关闭），
 *                                 GetData() 非阻塞出队（取出当前可用字节）
 *              - 优雅关闭:        Close() 阻止写入并唤醒 Wait；GetData 仍可取空剩余，
 *                                 保证下游数据完整
 *              - 多线程安全:      pthread 互斥锁 + 条件变量
 *
 *              典型应用: 异步日志写文件、串口/网络数据攒批发送、
 *                       任何"攒字节流 + 定量/定时批量处理"场景。
 *
 *              使用示例（异步日志写文件，用户自建消费线程）:
 *              @code
 *              // 用户消费线程：Wait 等触发 -> GetData 出队 -> 自行消费
 *              // Wait 返回值 >0 一律表示"有数据可出队"
 *              void *consumer(void *arg) {
 *                  T_StreamBuffer *sb = (T_StreamBuffer *)arg;
 *                  char buf[4096];
 *                  int used;
 *                  while (1) {
 *                      int r = StreamBufferAPI_Wait(sb, 1000, &used); // 等阈值/超时(1s)/关闭
 *                      if (r > 0) {                                  // 有数据(1/2/3)
 *                          int n;
 *                          while ((n = StreamBufferAPI_GetData(sb, buf, sizeof(buf))) > 0)
 *                              fwrite(buf, 1, n, fp);               // 用户自行消费
 *                      }
 *                      if (r <= -2) break;                          // 关闭空(-2)/错误(-3/-4)，退出
 *                      // r==0 : 超时无数据；r==-1 : 被 Flush 唤醒无数据 —— 均继续等
 *                  }
 *                  return NULL;
 *              }
 *
 *              // 主线程
 *              T_StreamBufferConfig cfg = { 65536, 4096 };   // 容量64K, 阈值4K
 *              T_StreamBuffer *sb = NULL;
 *              StreamBufferAPI_Init(&sb, &cfg, "logbuf");
 *              fp = fopen("log.csv", "w");
 *              pthread_create(&tid, NULL, consumer, sb);
 *
 *              // 业务线程写入（格式化好的字节，含 \r\n）
 *              StreamBufferAPI_PutData(sb, line, len);
 *
 *              // 优雅关闭：阻止写入 + 唤醒 Wait（已broadcast，无需额外Flush），
 *              // 消费线程会取空剩余后退出
 *              StreamBufferAPI_Close(sb);
 *              pthread_join(tid, NULL);
 *              fclose(fp);
 *              StreamBufferAPI_Destroy(&sb);
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
 * @brief       创建文件，提供流缓冲区全套API
 * @author      zlzksrl
 */
#ifndef __StreamBuffer_H__
#define __StreamBuffer_H__

#ifdef __cplusplus
 extern "C" {
#endif


/* ================================================================== */
/*                                                                    */
/*     返回值约定（各函数语义不同，详见各函数 Doxygen）                */
/*                                                                    */
/*  通用（Init/Destroy/Close/Flush/GetLength/IsClosed/StatsGet）:     */
/*       0  : 成功                                                    */
/*      -1  : 参数无效或未初始化                                       */
/*                                                                    */
/*  PutData:                                                          */
/*      >=0  : 实际写入字节数                                          */
/*      -1   : 参数无效或未初始化                                      */
/*      -2   : 队列已关闭                                              */
/*      -3   : 缓冲满，本次写入被丢弃（满则丢新）                       */
/*                                                                    */
/*  Wait（返回值=唤醒事件；>0 一律有数据，数据量看 used 形参）:        */
/*       3  : 队列已关闭，但仍有数据（取完后再 Wait 返回 -2）           */
/*       2  : 触发阈值（used≥iFlushBytes）或被唤醒，有数据             */
/*       1  : 超时，但有数据（未达阈值）                                */
/*       0  : 超时，且无数据                                            */
/*      -1  : 被 Flush 唤醒，且无数据                                   */
/*      -2  : 队列已关闭，且无数据（消费循环可退出）                    */
/*      -3  : 参数无效                                                 */
/*      -4  : 未初始化/已销毁                                          */
/*  消费判据：r>0=有数据(取); r==0=超时空(继续);                       */
/*           r==-1=Flush空(继续); r<=-2=关闭空/错误(退出)              */
/*                                                                    */
/*  GetData:                                                          */
/*      >0  : 实际取出字节数                                          */
/*       0  : 无数据可取                                              */
/*      -1  : 参数无效或未初始化                                       */
/*                                                                    */
/* ================================================================== */


/* ================================================================== */
/*                                                                    */
/*     类型定义                                                       */
/*                                                                    */
/* ================================================================== */

/** @brief 流缓冲句柄前向声明（隐藏内部实现，opaque pointer） */
typedef struct T_STREAMBUFFER T_StreamBuffer;


/* ================================================================== */
/*                                                                    */
/*     零拷贝消费回调（可选，特殊场景）                                */
/*                                                                    */
/*  注册后，Wait 在返回前会阻塞调用回调消费（替代 GetData）：          */
/*  - 仅当 used>0（有数据）时才调回调，无数据直接返回对应状态；         */
/*  - 库直接把内部缓冲的连续数据段指针传给回调，不拷贝；               */
/*  - 环形回绕时分两次回调（两段连续内存），各自独立返回消费量；        */
/*  - 回调返回本次消费量，库据此推进 read；未消费的剩余保留在缓冲，     */
/*    下次触发继续回调。                                               */
/*  回调在 Wait 内、持锁执行（须快速返回，适合阻塞但不耗时的操作）；   */
/*  不注册则继续用 GetData 拷贝式消费。两者二选一。                    */
/*                                                                    */
/* ================================================================== */

/**
 * @enum         StreamBufferCbStatus
 * @brief        零拷贝回调触发状态类型（传给 StreamBufferConsumeCb 的 status 参数）
 */
typedef enum
{
    STREAMBUFFER_CB_FLUSH      = 1,   /**< 被 Flush 唤醒触发 */
    STREAMBUFFER_CB_THRESHOLD  = 2,   /**< used 达阈值(iFlushBytes)触发 */
    STREAMBUFFER_CB_CLOSE      = 3    /**< 队列关闭触发（仍有数据时） */
} StreamBufferCbStatus;

/**
 * @func         StreamBufferConsumeCb
 * @brief        零拷贝消费回调原型
 * @details      Wait 触发后、返回前在锁内阻塞调用（**仅当 used>0 有数据时才调**）。
 *               data 直接指向内部缓冲的一段连续数据；环形回绕时分两次调用
 *               （第一段到缓冲末尾、第二段从头），各自独立返回消费量。
 *               库按返回值推进 read；未消费的剩余保留在缓冲，下次触发继续回调。
 * @param[in]    status:    触发状态（STREAMBUFFER_CB_FLUSH / _THRESHOLD / _CLOSE）
 * @param[in]    data:      本段数据首地址（内部缓冲，只读，回调返回后可能被覆盖）
 * @param[in]    len:       本段数据字节数
 * @param[in]    user_ctx:  SetConsumeCallback 时透传的上下文
 * @return       本次实际消费的字节数
 * @retval       >=0: 消费量（0 表示未消费，库不推进 read；必须 <= len）
 * @warning      在锁内执行，须快速返回（阻塞但不耗时的操作）；禁调本队列 API；
 *               data 仅在回调期间有效，返回后不得持有
 */
typedef int (*StreamBufferConsumeCb)(StreamBufferCbStatus status, const char *data, int len, void *user_ctx);


/* ================================================================== */
/*                                                                    */
/*     配置与统计                                                     */
/*                                                                    */
/* ================================================================== */

/**
 * @struct       T_StreamBufferConfig
 * @brief        流缓冲配置参数（Init 时传入）
 * @details      建议容量为 2 的幂（内部用 mask 高效回绕）。
 *               Init 时 cfg 为 NULL 则使用默认值。
 */
typedef struct T_STREAMBUFFERCONFIG
{
    int iCapacity;    /**< 缓冲总容量(字节)，建议 2 的幂，如 65536。默认 65536。0=用默认。 */
    int iFlushBytes;  /**< 触发出队的字节阈值：used 达到此值即唤醒 Wait。如 4096。默认 4096。0=用默认。 */
} T_StreamBufferConfig;

/**
 * @struct       T_StreamBufferStats
 * @brief        流缓冲运行统计信息
 * @details      字段由库内部维护，通过 StatsGet 读出，用于监控写入/丢弃/消费情况。
 */
typedef struct T_STREAMBUFFERSTATS
{
    unsigned long ulTotalPut;    /**< 累计成功写入的字节数 */
    unsigned long ulDropped;     /**< 累计因缓冲满被丢弃的字节数（满则丢新） */
    unsigned long ulConsumed;    /**< 累计已出队的字节数 */
    int           iPeakUsed;     /**< 缓冲历史峰值已用字节数 */
} T_StreamBufferStats;


/* ================================================================== */
/*                                                                    */
/*     生命周期                                                       */
/*                                                                    */
/*  使用流程:                                                         */
/*    Init() -> PutData() ...                                        */
/*    -> Close() (阻止写入+唤醒Wait) -> Destroy()                    */
/*    （Wait/GetData 由用户消费线程调用）                              */
/*                                                                    */
/* ================================================================== */

/**
 * @func         StreamBufferAPI_Init
 * @brief        流缓冲API-初始化
 * @details      分配 T_StreamBuffer 结构体，按 cfg 预分配环形缓冲连续内存，
 *               初始化互斥锁、条件变量、阈值与统计字段。调用成功后处于
 *               "已初始化、未关闭"状态。库不创建任何线程。
 * @param[in]    pp:   句柄二级指针，调用前 *pp 必须为 NULL
 * @param[in]    cfg:  配置参数，可为 NULL（用默认：容量65536/阈值4096）
 * @param[in]    name: 名称，用于调试日志标识
 * @return       int ret
 * @retval       0:   初始化成功
 * @retval       -1:  参数无效、内存分配失败或 *pp 非空（重复初始化）
 * @warning      pp 不能为 NULL，*pp 必须为 NULL
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_Init(T_StreamBuffer **pp, const T_StreamBufferConfig *cfg, const char *name);

/**
 * @func         StreamBufferAPI_Destroy
 * @brief        流缓冲API-销毁，释放所有资源
 * @details      释放缓冲内存、销毁锁与条件变量、释放结构体，并将 *pp 置为 NULL。
 *               不负责停止用户线程——调用前应先 Close 并 join 用户消费线程。
 * @param[in]    pp: 句柄二级指针
 * @return       int ret
 * @retval       0:   销毁成功
 * @retval       -1:  参数无效或未初始化
 * @warning      销毁后 *pp 被置为 NULL；调用前需确保无其它线程正在访问
 *               （建议 Close → pthread_join → Destroy）
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_Destroy(T_StreamBuffer **pp);

/**
 * @func         StreamBufferAPI_Close
 * @brief        流缓冲API-关闭（阻止写入并唤醒等待者）
 * @details      设置关闭标志，此后 PutData 返回 -2；并广播唤醒所有阻塞在 Wait 的线程。
 *               **不清空缓冲**：已写入的数据仍可由 GetData 取出，便于"关闭文件前先
 *               取空剩余落盘"。Wait 在关闭后：若仍有数据返回 3，取空后返回 -2。
 *               （Close 内部已 broadcast，关闭排空无需额外调用 Flush。）
 * @param[in]    p: 句柄
 * @return       int ret
 * @retval       0:   关闭成功
 * @retval       -1:  参数无效或未初始化
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_Close(T_StreamBuffer *p);

/**
 * @func         StreamBufferAPI_IsClosed
 * @brief        流缓冲API-查询是否已关闭
 * @param[in]    p: 句柄
 * @return       int ret
 * @retval       1:   已关闭
 * @retval       0:   未关闭
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_IsClosed(T_StreamBuffer *p);


/* ================================================================== */
/*                                                                    */
/*     写入                                                           */
/*                                                                    */
/* ================================================================== */

/**
 * @func         StreamBufferAPI_PutData
 * @brief        流缓冲API-写入一段字节流（不阻塞）
 * @details      将 buf 的 len 字节紧凑追加到环形缓冲 write 指针处（一次 memcpy，无 malloc）。
 *               写入成功后若 used ≥ 阈值，唤醒一个阻塞在 Wait 的消费者。
 *               - 空间足够：写入，返回写入字节数（=len）；
 *               - 缓冲剩余空间不足：**丢弃本段**（不入队），统计 ulDropped+=len，返回 -3，**不阻塞**；
 *               - len 超过缓冲容量时按容量截断；
 *               - Close 后调用：返回 -2。
 *               日志场景：调用者先用 snprintf/vsnprintf 格式化好（含 \r\n）再传入。
 * @param[in]    p:   句柄
 * @param[in]    buf: 字节串，不能为 NULL
 * @param[in]    len: 字节数，必须 > 0
 * @return       int ret
 * @retval       >=0: 实际写入字节数
 * @retval       -1:  参数无效或未初始化
 * @retval       -2:  已关闭，不再接收写入
 * @retval       -3:  缓冲空间不足，本段被丢弃（满则丢新）
 * @warning      buf 不能为 NULL；写入成功后 buf 可立即复用
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_PutData(T_StreamBuffer *p, const char *buf, int len);


/* ================================================================== */
/*                                                                    */
/*     消费（由用户线程调用）                                          */
/*                                                                    */
/*  典型循环:                                                         */
/*    while (1) {                                                    */
/*        int used, r = Wait(p, timeo, &used);                       */
/*        if (r > 0) while (GetData(p,buf,max) > 0) { 消费 }        */
/*        if (r <= -2) break;  // 关闭空(-2)/错误(-3/-4)              */
/*    }                                                              */
/*                                                                    */
/* ================================================================== */

/**
 * @func         StreamBufferAPI_Wait
 * @brief        流缓冲API-阻塞等待出队触发条件
 * @details      阻塞直到以下情况返回（**返回值 >0 一律表示有数据可出队**，调用者应循环
 *               GetData 取完；数据量见 used 形参）：
 *               - 触发阈值（used≥iFlushBytes）或被 PutData 唤醒且有数据：返回 2；
 *               - 被 Flush 唤醒：
 *                 · used>0（有数据）：返回 2；
 *                 · used==0（无数据）：返回 -1；
 *               - 超时 timeo 到达：
 *                 · used>0（有数据）：返回 1；
 *                 · used==0（无数据）：返回 0；
 *               - 队列被 Close：
 *                 · 还有数据（used>0）：返回 3（取完后再 Wait 返回 -2）；
 *                 · 无数据（used==0）：返回 -2（消费循环可退出）。
 *               实现使用 pthread_cond_timedwait，timeo=0 表示不等待（立即按当前状态返回）。
 *               若注册了零拷贝消费回调（SetConsumeCallback）且本次 used>0，则 Wait 在返回前
 *               先阻塞调用回调消费（回绕分两次），按返回值偏移推进 read，剩余(未消费)量
 *               通过 used 形参返回；此时无需再调 GetData。
 * @param[in]    p:     句柄
 * @param[in]    timeo: 最大等待时间(ms)，0=不阻塞立即返回
 * @param[out]   used:  输出返回时的未消费字节数（注册回调时为回调消费后的剩余量），可为 NULL
 * @return       int ret
 * @retval       3:   队列已关闭，但仍有数据
 * @retval       2:   触发阈值/被唤醒，有数据
 * @retval       1:   超时，但有数据
 * @retval       0:   超时，且无数据
 * @retval       -1:  被 Flush 唤醒，且无数据
 * @retval       -2:  队列已关闭，且无数据
 * @retval       -3:  参数无效
 * @retval       -4:  未初始化/已销毁
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_Wait(T_StreamBuffer *p, int timeo, int *used);

/**
 * @func         StreamBufferAPI_GetData
 * @brief        流缓冲API-非阻塞出队（取出当前可用字节）
 * @details      从 read 指针起取出 min(max, used) 字节拷贝到 buf（环形回绕时按两段拷贝），
 *               推进 read 指针、减少 used，统计 ulConsumed。**不阻塞**：无数据时返回 0。
 *               Close 后仍可调用，用于取空剩余数据；取空后返回 0。
 * @param[in]    p:   句柄
 * @param[out]   buf: 输出缓冲，不能为 NULL，容量至少 max 字节
 * @param[in]    max: 最多取出的字节数，必须 > 0
 * @return       int ret
 * @retval       >0:  实际取出的字节数
 * @retval       0:   当前无数据可取（缓冲空）
 * @retval       -1:  参数无效或未初始化
 * @warning      buf 不能为 NULL；返回的字节为连续拷贝（已合并回绕两段）
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_GetData(T_StreamBuffer *p, char *buf, int max);

/**
 * @func         StreamBufferAPI_Flush
 * @brief        流缓冲API-唤醒等待者（不等阈值/超时）
 * @details      唤醒一个阻塞在 Wait 的消费者，使其立即返回检查。
 *               - 若缓冲有数据：Wait 返回 2；
 *               - 若缓冲无数据：Wait 返回 -1。
 *               用于"想立即消费当前缓冲"的场景。不影响数据，不阻塞。
 * @param[in]    p: 句柄
 * @return       int ret
 * @retval       0:   已唤醒（或无等待者）
 * @retval       -1:  参数无效或未初始化
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_Flush(T_StreamBuffer *p);

/**
 * @func         StreamBufferAPI_SetConsumeCallback
 * @brief        流缓冲API-注册零拷贝消费回调（可选，特殊场景）
 * @details      注册后，Wait 在触发且 used>0 时，会先阻塞调用回调消费（回绕分两次），
 *               按返回值偏移推进 read，剩余量经 Wait 的 used 形参返回，替代 GetData。
 *               传 cb=NULL 取消回调，恢复 GetData 拷贝式消费。可在任意时刻调用（线程安全）。
 * @param[in]    p:         句柄
 * @param[in]    cb:        零拷贝消费回调，NULL=取消
 * @param[in]    user_ctx:  透传给回调的上下文（如 FILE*），可为 NULL
 * @return       int ret
 * @retval       0:   设置成功
 * @retval       -1:  参数无效或未初始化
 * @warning      回调在 Wait 内持锁执行，须快速返回、禁调本队列 API
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_SetConsumeCallback(T_StreamBuffer *p, StreamBufferConsumeCb cb, void *user_ctx);


/* ================================================================== */
/*                                                                    */
/*     查询与统计                                                     */
/*                                                                    */
/* ================================================================== */

/**
 * @func         StreamBufferAPI_GetLength
 * @brief        流缓冲API-获取当前未消费字节数
 * @param[in]    p: 句柄
 * @return       int ret
 * @retval       >=0: 当前缓冲内未消费字节数 used
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_GetLength(T_StreamBuffer *p);

/**
 * @func         StreamBufferAPI_StatsGet
 * @brief        流缓冲API-获取运行统计信息
 * @param[in]    p:   句柄
 * @param[out]   st:  统计信息输出，不能为 NULL
 * @return       int ret
 * @retval       0:   获取成功
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_StatsGet(T_StreamBuffer *p, T_StreamBufferStats *st);


#ifdef __cplusplus
 }
#endif

#endif
