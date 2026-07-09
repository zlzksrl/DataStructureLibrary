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
 *              - 多种消费方式:    GetData(拷贝) / GetDataAddress(零拷贝逐段) / 回调(零拷贝自动)
 *              - 优雅关闭:        Close() 阻止写入并唤醒 Wait；可 Reopen() 恢复
 *              - 多线程安全:      pthread 互斥锁 + 条件变量
 *
 *              典型应用: 异步日志写文件、串口/网络数据攒批发送、
 *                       任何"攒字节流 + 定量/定时批量处理"场景。
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
/*     类型定义                                                       */
/*                                                                    */
/* ================================================================== */

/** @brief 流缓冲句柄前向声明（隐藏内部实现，opaque pointer） */
typedef struct T_STREAMBUFFER T_StreamBuffer;

/**
 * @enum         StreamBufferStatus
 * @brief        Wait 返回状态码 / 零拷贝回调触发状态（统一枚举）
 * @details      同时用于 StreamBufferAPI_Wait 的返回值与 StreamBufferConsumeCb 的 status。
 *               **值 >0 一律表示"有数据"**（这些情况会触发零拷贝回调）；
 *               值 <=0 表示无数据或错误（不触发回调）。
 *
 *               消费判据：r>0 → 有数据(取); r==0 → 超时空(继续);
 *                        r==-1 → Flush空(继续); r<=-2 → 关闭空/错误(退出)。
 */
typedef enum
{
    /* ===== 有数据 (>0)：Wait 会触发零拷贝回调 ===== */
    STREAMBUFFER_STATUS_CLOSE_DATA    = 3,   /**< 队列已关闭，但有数据（取完后再 Wait 返回 CLOSE_EMPTY） */
    STREAMBUFFER_STATUS_TRIGGER       = 2,   /**< 达阈值(used≥iFlushBytes)或被唤醒(Flush/PutData)，有数据 */
    STREAMBUFFER_STATUS_TIMEOUT_DATA  = 1,   /**< 超时，但有数据（未达阈值） */
    /* ===== 无数据 (<=0)：不触发回调 ===== */
    STREAMBUFFER_STATUS_TIMEOUT_EMPTY = 0,   /**< 超时，且无数据 */
    STREAMBUFFER_STATUS_FLUSH_EMPTY   = -1,  /**< 被 Flush 唤醒，且无数据 */
    STREAMBUFFER_STATUS_CLOSE_EMPTY   = -2,  /**< 队列已关闭，且无数据（消费循环可退出） */
    STREAMBUFFER_STATUS_INVALID       = -3,  /**< 参数无效 */
    STREAMBUFFER_STATUS_NOINIT        = -4   /**< 未初始化/已销毁 */
} StreamBufferStatus;


/* ================================================================== */
/*                                                                    */
/*     零拷贝消费回调（可选，特殊场景；建议单消费者）                  */
/*                                                                    */
/*  注册后，Wait 在返回前会阻塞调用回调消费（替代/配合 GetData）：     */
/*  - 仅当 Wait 返回 >0（有数据）才调回调；                           */
/*  - 库直接把内部缓冲的连续数据段指针传给回调，不拷贝；               */
/*  - 环形回绕时分两次回调（两段连续内存），各自独立返回消费量；        */
/*  - 回调返回本次消费量，库据此偏移推进 read；未消费的剩余保留在缓冲， */
/*    经 Wait 的 used 形参返回，下次触发继续回调。                     */
/*  回调在 Wait 内、持锁执行（须快速返回，适合阻塞但不耗时的操作）；   */
/*  回调与 GetData 可配合。                                           */
/*                                                                    */
/*  ⚠️ 并发约束: 回调与 GetDataAddress 均为"零拷贝返回内部地址"，建议 */
/*     仅在**单消费者**场景使用；多消费者场景请用 GetData（拷贝式），   */
/*     避免多个线程持有内部地址导致的覆盖竞争。                        */
/*                                                                    */
/* ================================================================== */

/**
 * @func         StreamBufferConsumeCb
 * @brief        零拷贝消费回调原型
 * @details      Wait 返回 >0（有数据）后、返回前在锁内阻塞调用。
 *               data 直接指向内部缓冲的一段连续数据；环形回绕时分两次调用
 *               （第一段到缓冲末尾、第二段从头），各自独立返回消费量。
 *               库按返回值偏移推进 read；未消费的剩余保留在缓冲，下次触发继续回调。
 *
 *               返回值约束：必须 ∈ [0, len]。
 *               - 返回 <0 或 ==0：按 0 处理（不推进 read），并打印错误警告；
 *               - 返回 > len：按 len 处理（钳位），并打印错误警告。
 *               （这是错误用法，库做钳位保护以避免越界/状态错乱。）
 * @param[in]    status:    触发状态（STREAMBUFFER_STATUS_TIMEOUT_DATA / _TRIGGER / _CLOSE_DATA）
 * @param[in]    data:      本段数据首地址（内部缓冲，只读，回调返回后可能被覆盖）
 * @param[in]    len:       本段数据字节数
 * @param[in]    user_ctx:  SetConsumeCallback 时透传的上下文
 * @return       本次实际消费的字节数（库会钳位到 [0, len]）
 * @warning      在锁内执行，须快速返回（阻塞但不耗时的操作）；禁调本队列 API；
 *               data 仅在回调期间有效，返回后不得持有；建议单消费者场景使用
 */
typedef int (*StreamBufferConsumeCb)(StreamBufferStatus status, const char *data, int len, void *user_ctx);


/* ================================================================== */
/*                                                                    */
/*     配置与统计                                                     */
/*                                                                    */
/* ================================================================== */

/**
 * @struct       T_StreamBufferConfig
 * @brief        流缓冲配置参数（Init 时传入）
 * @details      容量必须为 2 的幂（内部用 mask 高效回绕，非 2 的幂 Init 报错）。
 *               Init 时 cfg 为 NULL 则使用默认值。
 */
typedef struct T_STREAMBUFFERCONFIG
{
    int iCapacity;    /**< 缓冲总容量(字节)，**必须为 2 的幂**，如 65536。默认 65536。0=用默认。 */
    int iFlushBytes;  /**< 触发出队的字节阈值，必须 <= iCapacity，如 4096。默认 4096。0=用默认。 */
} T_StreamBufferConfig;

/**
 * @struct       T_StreamBufferStats
 * @brief        流缓冲运行统计信息
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
/*    -> Close() (阻止写入+broadcast唤醒) -> [Reopen() 可选]         */
/*    -> Destroy()                                                   */
/*    （Wait/GetData/回调 由用户消费线程调用）                         */
/*                                                                    */
/* ================================================================== */

/**
 * @func         StreamBufferAPI_Init
 * @brief        流缓冲API-初始化
 * @details      分配 T_StreamBuffer 结构体，按 cfg 预分配环形缓冲连续内存，
 *               初始化互斥锁、条件变量、阈值与统计字段。库不创建任何线程。
 *
 *               校验规则（不满足返回 -1）：
 *               - *pp 必须为 NULL（防重复初始化）；
 *               - iCapacity > 0 且**必须为 2 的幂**（非 2 的幂报错）；
 *               - iFlushBytes > 0 且 **<= iCapacity**（阈值大于容量不允许）。
 * @param[in]    pp:   句柄二级指针，调用前 *pp 必须为 NULL
 * @param[in]    cfg:  配置参数，可为 NULL（用默认：容量65536/阈值4096）
 * @param[in]    name: 名称，用于调试日志标识
 * @return       int ret
 * @retval       0:   初始化成功
 * @retval       -1:  参数无效、内存分配失败、*pp 非空、容量非 2 的幂、或阈值>容量
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
 *               幂等：*pp 为 NULL 时返回 -1（不重复销毁）。
 * @param[in]    pp: 句柄二级指针
 * @return       int ret
 * @retval       0:   销毁成功
 * @retval       -1:  参数无效、未初始化或 *pp 已为 NULL（重复销毁）
 * @warning      销毁后 *pp 被置为 NULL；调用前需确保无其它线程正在访问
 *               （建议 Close → pthread_join → Destroy）
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_Destroy(T_StreamBuffer **pp);

/**
 * @func         StreamBufferAPI_Close
 * @brief        流缓冲API-关闭（阻止写入并唤醒所有等待者）
 * @details      设置关闭标志，此后 PutData 返回 -2；并 **broadcast 唤醒所有**阻塞在
 *               Wait 的线程。不清空缓冲：已写入的数据仍可由 GetData/GetDataAddress 取出。
 *               Wait 在关闭后：有数据→CLOSE_DATA(3)；无数据→CLOSE_EMPTY(-2)。
 *               幂等：对已关闭的队列再次 Close 返回 0。
 *               可由 Reopen() 恢复（Close 可逆）。
 * @param[in]    p: 句柄
 * @return       int ret
 * @retval       0:   关闭成功（含已关闭再调）
 * @retval       -1:  参数无效或未初始化
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_Close(T_StreamBuffer *p);

/**
 * @func         StreamBufferAPI_Reopen
 * @brief        流缓冲API-重新打开已关闭的队列（Close 可逆）
 * @details      重置关闭标志，恢复 PutData 写入。不清空缓冲（保留剩余数据）。
 *               前置条件：队列必须处于已关闭状态（is_closed==1）。
 * @param[in]    p: 句柄
 * @return       int ret
 * @retval       0:   重新打开成功
 * @retval       -1:  参数无效、未初始化或队列未处于关闭状态
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_Reopen(T_StreamBuffer *p);

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
 * @details      将 buf 的 len 字节紧凑追加到环形缓冲 write 指针处（无 malloc）。
 *               - 写入时若跨越回绕边界（write+len > capacity），分两段 memcpy
 *                 （write 到尾 + 头到剩余），保证变长紧凑、零浪费；
 *               - 写入成功后若 used ≥ 阈值，**signal 唤醒一个**阻塞在 Wait 的消费者；
 *               - 空间不足（剩余 = capacity-used < len）：**丢弃本段**（不入队），
 *                 统计 ulDropped+=len，返回 -3，不阻塞；
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
/*  唤醒策略:                                                         */
/*    PutData/Flush 用 signal（唤醒一个），Close 用 broadcast（全部） */
/*  多消费者:                                                          */
/*    建议单消费者。多消费者场景请用 GetData(拷贝式)，                 */
/*    避免回调/GetDataAddress 的内部地址被多线程并发持有。             */
/*  三种消费方式:                                                      */
/*    设计上支持配合使用，但实际应用通常只用其中一种。                 */
/*  轮询消费者:                                                        */
/*    不使用 Wait、仅用 GetData/GetDataAddress 轮询的消费者，需自行   */
/*    定期检查 IsClosed() 以响应关闭。                                 */
/*                                                                    */
/* ================================================================== */

/**
 * @func         StreamBufferAPI_Wait
 * @brief        流缓冲API-阻塞等待出队触发条件
 * @details      阻塞直到以下情况返回（返回值见 StreamBufferStatus 枚举；**>0 一律有数据**）：
 *               - 达阈值(used≥iFlushBytes) 或被 PutData/Flush signal 唤醒且有数据：TRIGGER(2)；
 *               - 被 Flush signal 唤醒且无数据：FLUSH_EMPTY(-1)；
 *               - 超时 timeo 到达：有数据→TIMEOUT_DATA(1)；无数据→TIMEOUT_EMPTY(0)；
 *               - 队列被 Close(broadcast)：有数据→CLOSE_DATA(3)；无数据→CLOSE_EMPTY(-2)。
 *
 *               timeo=0：不等待，立即按当前状态计算并返回（used>0→TRIGGER；used==0且未关闭→
 *               TIMEOUT_EMPTY；已关闭按数据情况→CLOSE_DATA/CLOSE_EMPTY）。
 *
 *               若注册了零拷贝回调（SetConsumeCallback）且本次返回 >0，则 Wait 在返回前
 *               先阻塞调用回调消费（回绕分两次，每段 len 为该连续段长度，data+len 不越界；
 *               回调返回值钳位到 [0,len]），按返回值偏移推进 read；随后按"剩余 used + 关闭状态"
 *               **重新计算返回码与 used**：剩余>0+已关闭→CLOSE_DATA(3)；剩余>0+未关闭→TRIGGER(2)；
 *               剩余==0+已关闭→CLOSE_EMPTY(-2)；剩余==0+未关闭→TIMEOUT_EMPTY(0)。
 *               剩余可继续用 GetData/GetDataAddress 取出（回调与二者可配合）。
 * @param[in]    p:     句柄
 * @param[in]    timeo: 最大等待时间(ms)，0=不阻塞立即返回
 * @param[out]   used:  输出返回时的未消费字节数（注册回调时为回调消费后的剩余量），可为 NULL
 * @return       int ret（值为 StreamBufferStatus 枚举常量）
 * @retval       STREAMBUFFER_STATUS_CLOSE_DATA(3):    关闭，有数据
 * @retval       STREAMBUFFER_STATUS_TRIGGER(2):       阈值/被唤醒，有数据
 * @retval       STREAMBUFFER_STATUS_TIMEOUT_DATA(1):  超时，有数据
 * @retval       STREAMBUFFER_STATUS_TIMEOUT_EMPTY(0): 超时，无数据
 * @retval       STREAMBUFFER_STATUS_FLUSH_EMPTY(-1):  被 Flush 唤醒，无数据
 * @retval       STREAMBUFFER_STATUS_CLOSE_EMPTY(-2):  关闭，无数据
 * @retval       STREAMBUFFER_STATUS_INVALID(-3):      参数无效
 * @retval       STREAMBUFFER_STATUS_NOINIT(-4):       未初始化/已销毁
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_Wait(T_StreamBuffer *p, int timeo, int *used);

/**
 * @func         StreamBufferAPI_GetData
 * @brief        流缓冲API-非阻塞出队（拷贝式，合并回绕两段）
 * @details      从 read 指针起取出 min(max, used) 字节拷贝到 buf（环形回绕时按两段拷贝
 *               并合并为连续输出），推进 read、减少 used，统计 ulConsumed。不阻塞：无数据返回 0。
 *               Close 后仍可调用取剩余。**多消费者场景请用本函数**（拷贝式，安全）。
 *               若已注册零拷贝回调，Wait 会先回调消费；本函数可用于取回调未消费完的剩余。
 * @param[in]    p:   句柄
 * @param[out]   buf: 输出缓冲，不能为 NULL，容量至少 max 字节
 * @param[in]    max: 最多取出的字节数，必须 > 0
 * @return       int ret
 * @retval       >0:  实际取出的字节数
 * @retval       0:   当前无数据可取（缓冲空）
 * @retval       -1:  参数无效或未初始化
 * @warning      buf 不能为 NULL
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_GetData(T_StreamBuffer *p, char *buf, int max);

/**
 * @func         StreamBufferAPI_GetDataAddress
 * @brief        流缓冲API-零拷贝出队（输出本段数据地址，不拷贝）
 * @details      从 read 指针起，输出当前本段连续数据的地址（不拷贝），按规则消费：
 *               - 本段连续长度 seg = min(used, capacity-read)（到回绕点或 write）；
 *               - 实际消费 consume = min(seg, max)；
 *               - *out_buf = buffer+read；推进 read、减少 used、累计 consumed。
 *               遇回绕只输出本段（不合并两段），第二段下次调用获取。
 *               本段 > max → 消费 max；本段 <= max → 消费整段。返回实际消费长度。
 *               非阻塞；Close 后仍可调用取剩余。与回调/GetData 可配合。
 * @param[in]    p:       句柄
 * @param[out]   out_buf: 输出本段数据首地址（内部指针，零拷贝），不能为 NULL；无数据时置 NULL
 * @param[in]    max:     期望最大消费字节数，必须 > 0
 * @return       int ret
 * @retval       >0:  实际消费字节数（*out_buf 指向该长度数据）
 * @retval       0:   无数据可取（*out_buf 置 NULL）
 * @retval       -1:  参数无效或未初始化
 * @warning      ⚠️ 零拷贝并发风险：*out_buf 指向内部缓冲，须立即使用、不得跨操作持有。
 *               **本接口可能与 PutData 在不同线程同时调用**：推进 read 后该段内存即可被新的
 *               PutData 写入覆盖——用户须保证使用 *out_buf 期间本队列无 PutData 写入该段
 *               （单消费者+用完即弃可满足）。多消费者请用 GetData（拷贝式）。
 * @author       zlzksrl
 * @date         2026-07-09
 * @Version      V1.0.0
 */
int StreamBufferAPI_GetDataAddress(T_StreamBuffer *p, char **out_buf, int max);

/**
 * @func         StreamBufferAPI_Flush
 * @brief        流缓冲API-唤醒等待者（不等阈值/超时）
 * @details      signal 唤醒一个阻塞在 Wait 的消费者，使其立即返回检查。
 *               - 若缓冲有数据：Wait 返回 TRIGGER(2)（注册回调则触发回调）；
 *               - 若缓冲无数据：Wait 返回 FLUSH_EMPTY(-1)。
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
 * @details      注册后，Wait 在返回 >0（有数据）时，会先阻塞调用回调消费（回绕分两次，
 *               返回值钳位到 [0,len]），按返回值偏移推进 read，剩余量经 Wait 的 used 形参
 *               返回（剩余可继续用 GetData/GetDataAddress 取出，可配合）。传 cb=NULL 取消回调。
 *               可在任意时刻调用（线程安全）。
 * @param[in]    p:         句柄
 * @param[in]    cb:        零拷贝消费回调，NULL=取消
 * @param[in]    user_ctx:  透传给回调的上下文（如 FILE*），可为 NULL
 * @return       int ret
 * @retval       0:   设置成功
 * @retval       -1:  参数无效或未初始化
 * @warning      回调在 Wait 内持锁执行，须快速返回、禁调本队列 API；建议单消费者场景
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
