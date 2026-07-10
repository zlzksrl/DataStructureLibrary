/**
 * @file        MemoryPool.h
 * @brief       LinuxARM-PublicLib-内存池（固定大小对象池）-公共API头文件
 * @details     IMX6ULL平台
 *              本文件提供 MemoryPool 内存池的公共API。
 *              MemoryPool 是一个固定大小对象的内存池/对象池：Init 时预分配 N 个
 *              `element_size` 字节槽位，提供 Alloc/Free（类似 malloc/free），槽位
 *              循环复用，运行时零 malloc、零碎片。
 *
 *              与 malloc 的唯一区别：每次 Alloc 返回**固定 element_size** 大小的空间
 *              （无需传 size），Free 也无需 size。槽位是纯字节块，库不解释内容，
 *              适合任意 struct/union、二进制流（含 0x00 安全）。
 *
 *              核心特性:
 *              - 三种池满策略: DROP(返回NULL) / GROW(动态扩容) / BLOCK(阻塞等待)
 *                             —— Init 配默认模式 + 三个 Alloc 接口显式选择
 *              - 零 malloc 循环复用: 空闲链表(内嵌 next)，O(1) Alloc/Free
 *              - 通用: element_size 任意，槽位不解释内容（类似 StreamBuffer 字节流）
 *              - 线程安全: mutex 保护空闲链表；BLOCK 模式用 cond
 *
 *              空间流转:
 *              ```
 *              Init: malloc N 槽位 → 串成 free_list(空闲槽开头存 next)
 *              Alloc: 取 free_list 链头给用户(用户覆盖 next, 整块用)
 *              Free:  槽位写 next 挂回链头
 *                     ┌──────────────────────────────────────┐
 *                     ▼                                      │
 *                [free_list] ──Alloc──→ [用户] ──Free────────┘
 *              池满: DROP→NULL / GROW→新chunk串入 / BLOCK→cond等
 *              ```
 *
 *              典型应用: 配合 ThreadQueue，消除"每条消息 malloc/free"开销
 *              @code
 *              union Msg { TypeA a; TypeB b; };
 *              T_MemPoolConfig cfg = { sizeof(union Msg), 64,
 *                                     MEMPOOL_MODE_BLOCK, 0, 0 };
 *              T_MemPool *pool;
 *              MemPoolAPI_Init(&pool, &cfg, "msgpool");
 *
 *              // 生产者
 *              union Msg *m = (union Msg *)MemPoolAPI_Alloc(pool);  // 从池取(无malloc)
 *              m->a = ...;
 *              ThreadQueueAPI_PutMsg(q, m);
 *              // 消费者
 *              union Msg *m = (union Msg *)ThreadQueueAPI_GetMsg(q, 1000);
 *              ... 用 m ...
 *              MemPoolAPI_Free(pool, m);                             // 归还池(无free)
 *              @endcode
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-10
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-10
 * @Version     V1.0.0
 * @brief       创建文件，提供内存池全套API
 * @author      zlzksrl
 */
#ifndef __MemoryPool_H__
#define __MemoryPool_H__

#ifdef __cplusplus
 extern "C" {
#endif


/* ================================================================== */
/*                                                                    */
/*     类型定义                                                       */
/*                                                                    */
/* ================================================================== */

/** @brief 内存池句柄前向声明（隐藏内部实现，opaque pointer） */
typedef struct T_MEMPOOL T_MemPool;

/**
 * @enum         MemPoolMode
 * @brief        池满策略（Alloc 时无空闲槽位的处理方式）
 */
typedef enum
{
    MEMPOOL_MODE_DROP  = 0,   /**< 用完丢：池满返回 NULL（用户自行处理） */
    MEMPOOL_MODE_GROW  = 1,   /**< 动态扩容：池满 malloc 新 chunk 加入池 */
    MEMPOOL_MODE_BLOCK = 2    /**< 阻塞等待：池满 cond 等待 Free 归还 */
} MemPoolMode;


/* ================================================================== */
/*                                                                    */
/*     配置与统计                                                     */
/*                                                                    */
/* ================================================================== */

/**
 * @struct       T_MemPoolConfig
 * @brief        内存池配置参数（Init 时传入）
 * @details      element_size 经内部对齐补齐（max_align_t）后作为实际槽位大小，
 *               保证返回地址可存任意类型。Init 时 cfg 为 NULL 用默认值。
 */
typedef struct T_MEMPOOLCONFIG
{
    int         element_size;  /**< 单个槽位字节数(如 sizeof(union Msg))。默认 64。0=用默认。 */
    int         init_count;    /**< 初始槽位数。默认 64。0=用默认。 */
    MemPoolMode mode;          /**< Alloc() 默认模式(DROP/GROW/BLOCK) */
    int         grow_count;    /**< GROW 每次扩容新增槽位数；mode=MEMPOOL_MODE_GROW 时必须 >0，否则 Init 失败 */
    int         block_timeo;   /**< BLOCK 模式默认阻塞超时(ms)，Alloc() 使用；0=无限等待，>0 超时返 NULL */
} T_MemPoolConfig;

/**
 * @struct       T_MemPoolStats
 * @brief        内存池运行统计信息
 */
typedef struct T_MEMPOOLSTATS
{
    unsigned long ulTotalAlloc;   /**< 累计 Alloc 成功次数 */
    unsigned long ulTotalFree;    /**< 累计 Free 归还次数 */
    unsigned long ulTotalDrop;    /**< 累计 DROP 丢弃次数(池满返回 NULL) */
    unsigned long ulTotalGrow;    /**< 累计 GROW 扩容槽位数 */
    int           iPeakUsed;      /**< 峰值已用槽位数 */
    int           iCapacity;      /**< 总容量(含 GROW 扩容，槽位数) */
} T_MemPoolStats;


/* ================================================================== */
/*                                                                    */
/*     生命周期                                                       */
/*                                                                    */
/*  使用流程:                                                         */
/*    Init() -> Alloc()/Free() ... -> Destroy()                      */
/*                                                                    */
/* ================================================================== */

/**
 * @func         MemPoolAPI_Init
 * @brief        内存池API-初始化
 * @details      按 cfg 预分配 init_count 个 element_size 槽位（内部对齐补齐），
 *               建立空闲链表，初始化 mutex/cond(BLOCK 用 CLOCK_MONOTONIC) 与统计。
 *               库不创建任何线程。
 *
 *               校验规则（不满足返回 -1）：
 *               - *pp 必须为 NULL；
 *               - element_size > 0、init_count > 0；
 *               - mode = MEMPOOL_MODE_GROW 时 grow_count 必须 >0。
 * @param[in]    pp:   句柄二级指针，*pp 必须为 NULL
 * @param[in]    cfg:  配置参数，可为 NULL(用默认)
 * @param[in]    name: 名称，调试日志用
 * @return       int ret
 * @retval       0:   初始化成功
 * @retval       -1:  参数无效、内存分配失败、*pp 非空、或 GROW 模式 grow_count<=0
 * @warning      pp 不能为 NULL，*pp 必须为 NULL
 * @author       zlzksrl
 * @date         2026-07-10
 * @Version      V1.0.0
 */
int MemPoolAPI_Init(T_MemPool **pp, const T_MemPoolConfig *cfg, const char *name);

/**
 * @func         MemPoolAPI_Destroy
 * @brief        内存池API-销毁，释放所有资源
 * @details      释放所有 chunk 内存（含 GROW 扩容的）、销毁 mutex/cond、释放结构体，
 *               将 *pp 置 NULL。幂等：*pp==NULL 返回 -1。
 * @warning      调用前需确保所有 Alloc 出的槽位都已 Free 归还（否则用户持有已释放内存，
 *               属调用者责任）；并确保无其它线程正在访问。
 * @param[in]    pp: 句柄二级指针
 * @return       int ret
 * @retval       0:   销毁成功
 * @retval       -1:  参数无效、未初始化或 *pp 已为 NULL(重复销毁)
 * @author       zlzksrl
 * @date         2026-07-10
 * @Version      V1.0.0
 */
int MemPoolAPI_Destroy(T_MemPool **pp);


/* ================================================================== */
/*                                                                    */
/*     分配 / 释放                                                    */
/*                                                                    */
/*  说明:                                                             */
/*    - Alloc 返回固定 element_size 字节(对齐后)，内容【未初始化】      */
/*      (与 malloc 一致，用户需自行填数据)；                            */
/*    - Free 的 elem 必须是本池 Alloc 出来的地址(不校验，错指针 UB)；    */
/*    - 三种池满策略：DROP/GROW/BLOCK，可按场景选接口。                  */
/*                                                                    */
/* ================================================================== */

/**
 * @func         MemPoolAPI_Alloc
 * @brief        内存池API-分配一个槽位(默认模式)
 * @details      按 Init 配置的 cfg.mode 行为：
 *               - DROP  → 池满返回 NULL
 *               - GROW  → 池满动态扩容(grow_count 个)
 *               - BLOCK → 池满用 cfg.block_timeo 阻塞(0=无限，>0 超时返 NULL)
 *               返回的内存**未初始化**(像 malloc)，用户自行填数据。
 * @param[in]    p: 句柄
 * @return       void* 槽位首地址(element_size 字节)
 * @retval       非NULL: 分配成功
 * @retval       NULL:   池满(DROP) / 扩容失败(GROW) / 超时(BLOCK) / 参数无效
 * @author       zlzksrl
 * @date         2026-07-10
 * @Version      V1.0.0
 */
void *MemPoolAPI_Alloc(T_MemPool *p);

/**
 * @func         MemPoolAPI_AllocDrop
 * @brief        内存池API-分配(满则返回NULL，显式 DROP)
 * @details      池满立即返回 NULL，不阻塞、不扩容。适合"可丢、生产者不能卡"场景。
 *               返回内存未初始化。
 * @param[in]    p: 句柄
 * @return       void* 槽位首地址；NULL=池满或参数无效
 * @author       zlzksrl
 * @date         2026-07-10
 * @Version      V1.0.0
 */
void *MemPoolAPI_AllocDrop(T_MemPool *p);

/**
 * @func         MemPoolAPI_AllocGrow
 * @brief        内存池API-分配(满则动态扩容，显式 GROW)
 * @details      池满时 malloc 新 chunk(grow_count 个槽位)串入空闲链表，再分配。
 *               适合"不能丢、内存可增长"场景。返回内存未初始化。
 * @param[in]    p: 句柄
 * @return       void* 槽位首地址；NULL=扩容失败(grow_count<=0 或 malloc 失败)或参数无效
 * @author       zlzksrl
 * @date         2026-07-10
 * @Version      V1.0.0
 */
void *MemPoolAPI_AllocGrow(T_MemPool *p);

/**
 * @func         MemPoolAPI_AllocBlock
 * @brief        内存池API-分配(满则阻塞等待，显式 BLOCK)
 * @details      池满时 cond 阻塞等待其它线程 Free 归还槽位，被唤醒后分配。
 *               适合"不能丢、生产者可阻塞"场景。返回内存未初始化。
 * @param[in]    p:     句柄
 * @param[in]    timeo: 最大等待时间(ms)，0=无限等待，>0=超时返回 NULL
 * @return       void* 槽位首地址；NULL=超时或参数无效
 * @author       zlzksrl
 * @date         2026-07-10
 * @Version      V1.0.0
 */
void *MemPoolAPI_AllocBlock(T_MemPool *p, int timeo);

/**
 * @func         MemPoolAPI_Free
 * @brief        内存池API-归还一个槽位
 * @details      将 elem 挂回空闲链表头(LIFO)，并 signal 唤醒一个 BLOCK 等待者。
 *               ⚠️ **elem 必须是本池 Alloc 出来的地址**——库**不校验**，传错指针
 *               (别的池/野指针/已 Free 过)为未定义行为(同 free)。GROW 扩容的槽位
 *               也归还到同一空闲链表。
 * @param[in]    p:    句柄
 * @param[in]    elem: 待归还的槽位地址(本池 Alloc 出的)
 * @return       int ret
 * @retval       0:   归还成功
 * @retval       -1:  参数无效(p/elem 为 NULL 或未初始化)
 * @warning      elem 必须是本池 Alloc 出来的；不得对同一地址重复 Free
 * @author       zlzksrl
 * @date         2026-07-10
 * @Version      V1.0.0
 */
int MemPoolAPI_Free(T_MemPool *p, void *elem);


/* ================================================================== */
/*                                                                    */
/*     查询与统计                                                     */
/*                                                                    */
/* ================================================================== */

/**
 * @func         MemPoolAPI_GetFreeCount
 * @brief        内存池API-获取当前空闲槽位数
 * @param[in]    p: 句柄
 * @return       int ret
 * @retval       >=0: 当前空闲槽位数
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-10
 * @Version      V1.0.0
 */
int MemPoolAPI_GetFreeCount(T_MemPool *p);

/**
 * @func         MemPoolAPI_GetUsedCount
 * @brief        内存池API-获取当前已用槽位数(= 容量 - 空闲)
 * @param[in]    p: 句柄
 * @return       int ret
 * @retval       >=0: 当前已用槽位数
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-10
 * @Version      V1.0.0
 */
int MemPoolAPI_GetUsedCount(T_MemPool *p);

/**
 * @func         MemPoolAPI_StatsGet
 * @brief        内存池API-获取运行统计信息
 * @param[in]    p:   句柄
 * @param[out]   st:  统计信息输出，不能为 NULL
 * @return       int ret
 * @retval       0:   获取成功
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-10
 * @Version      V1.0.0
 */
int MemPoolAPI_StatsGet(T_MemPool *p, T_MemPoolStats *st);


#ifdef __cplusplus
 }
#endif

#endif
