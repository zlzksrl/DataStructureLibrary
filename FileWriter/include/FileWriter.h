/**
 * @file        FileWriter.h
 * @brief       LinuxARM-PublicLib-异步文件写入模块-公共API头文件
 * @details     IMX6ULL平台
 *              本文件提供 FileWriter 异步文件写入模块的公共API。
 *              FileWriter 基于 StreamBuffer（攒批缓冲）+ MemoryPool（格式化buffer池）
 *              + ThreadManage（消费线程管理），提供 printf 式格式化写入接口，
 *              攒批写盘（减磁盘磨损），支持日志/CSV/二进制，自动文件轮转
 *              （按数量+大小+日期），优雅关闭。
 *
 *              核心特性:
 *              - 异步写入:          业务线程 Write→StreamBuffer 入队，内置消费线程批量 fwrite
 *              - printf 格式化:     FileWriterAPI_Write(fw, "ret=%d\n", val)
 *              - 攒批写盘:          used≥阈值 或 超时 或 关闭时批量 fwrite（减磁盘 I/O）
 *              - 时间戳前缀:        可选 [HH:MM:SS.mmmmmm] 前缀
 *              - 文件轮转:          按大小(max_file_size) / 跨日(auto_rotate_daily) / 手动(Rotate)
 *              - 日期子目录:        可配 date_subdir_prefix，如 X → /log/X2026_07_11/
 *              - 文件命名:          {prefix}{序号3位}_{YYYY-MM-DD-HH-MM-SS}.{ext}
 *              - 多实例可重入:      一个进程 Init 多个 FileWriter（各自独立线程+文件）
 *              - SCHED_RR 线程:     消费线程用 ThreadManage 创建，SCHED_RR 策略，可配优先级
 *              - 优雅关闭:          Close→排空缓冲→fflush+fclose→线程退出
 *
 *              典型应用: 异步日志、CSV 记录、二进制 bin 文件
 *              @code
 *              T_FileWriterConfig cfg = {
 *                  .dir_path = "/log",
 *                  .date_subdir_prefix = "X",
 *                  .file_prefix = "sensor1",
 *                  .file_ext = ".log",
 *                  .max_files = 10,
 *                  .max_file_size = 1024*1024,      // 1MB 自动轮转
 *                  .auto_rotate_daily = 1,
 *                  .thread_priority = 20,
 *                  .timestamp = 1,
 *                  .flush_bytes = 4096,
 *                  .flush_ms = 100,
 *                  .buffer_capacity = 65536
 *              };
 *              T_FileWriter *fw;
 *              FileWriterAPI_Init(&fw, &cfg);
 *
 *              // 业务线程：printf 式写入
 *              FileWriterAPI_Write(fw, "[%s] ret=%d\n", "moduleA", ret);
 *
 *              // 优雅关闭
 *              // 优雅关闭（一步到位：排空+落盘+释放）
 *              FileWriterAPI_Destroy(&fw);
 *              @endcode
 *
 * @author      zlzksrl
 * @Version     V1.0.0
 * @date        2026-07-11
 * @copyright   copyright (C) 2026
 */

/**
 * @date        2026-07-11
 * @Version     V1.0.0
 * @brief       创建文件，提供异步文件写入全套API
 * @author      zlzksrl
 */
#ifndef __FileWriter_H__
#define __FileWriter_H__

#ifdef __cplusplus
 extern "C" {
#endif


/* ================================================================== */
/*                                                                    */
/*     类型定义                                                       */
/*                                                                    */
/* ================================================================== */

/** @brief FileWriter 句柄前向声明（隐藏内部实现，opaque pointer） */
typedef struct T_FILEWRITER T_FileWriter;

/**
 * @enum         FileWriterType
 * @brief        文件类型枚举
 * @details      决定文件扩展名（若 cfg.file_ext 为空则用此枚举的默认扩展名）。
 */
typedef enum
{
    FILEWRITER_TYPE_TXT = 0,   /**< 文本文件，扩展名 .txt（默认） */
    FILEWRITER_TYPE_LOG,       /**< 日志文件，扩展名 .log */
    FILEWRITER_TYPE_CSV,       /**< CSV 表格，扩展名 .csv */
    FILEWRITER_TYPE_BIN        /**< 二进制文件，扩展名 .bin */
} FileWriterType;


/* ================================================================== */
/*                                                                    */
/*     配置                                                           */
/*                                                                    */
/* ================================================================== */

/**
 * @struct       T_FileWriterConfig
 * @brief        FileWriter 配置参数（Init 时传入）
 * @details      配置路径命名、轮转策略、线程优先级、写入行为。
 */
typedef struct T_FILEWRITERCONFIG
{
    /* ---- 路径与命名 ---- */
    char     dir_path[256];          /**< 写入根目录（绝对或相对），自动创建多级 */
    char     date_subdir_prefix[16]; /**< 日期子目录前缀，如 "X" → /log/X2026_07_11/；空串=不分日期目录 */
    char     file_prefix[64];        /**< 文件名前缀，可含子路径如 "sensor/sensor1"；不含"/"则文件直接在日期目录下 */
    FileWriterType file_type;        /**< 文件类型枚举(TXT/LOG/CSV/BIN)；file_ext 非空时优先用 file_ext */
    char     file_ext[16];           /**< 扩展名，非空时覆盖 file_type 的默认扩展名；空串则按 file_type 自动选 */

    /* ---- 文件轮转 ---- */
    int      max_files;              /**< 当前写目录下【同文件名前缀】的最大文件数量，0=无限制；>0 超过删最老。
                                          匹配规则：当前目录下以 file_prefix 的文件名部分开头 + "_" 的文件。
                                          例：file_prefix="sensor/sensor1" → 统计 .../sensor/sensor1_*.csv 数量 */
    int      max_file_size;          /**< 单文件最大字节，0=不限制；>0 达此大小自动轮转。1G=1024*1024*1024 */
    int      auto_rotate_daily;      /**< 1=跨日自动轮转（建新日期目录+新文件）；0=不自动 */

    /* ---- 线程 ---- */
    int      thread_priority;        /**< 消费线程 SCHED_RR 优先级(1~99，如 20)；0=用默认 */

    /* ---- 写入行为 ---- */
    int      timestamp;              /**< 1=每行前加时间戳 [HH:MM:SS.mmmmmm] ；0=不加 */
    int      flush_bytes;            /**< 攒批字节阈值(如 4096)，达此值触发写盘 */
    int      flush_ms;               /**< 定时写盘周期(ms，如 100) */
    int      buffer_capacity;        /**< StreamBuffer 容量(字节，须2的幂，如65536) */
} T_FileWriterConfig;


/* ================================================================== */
/*                                                                    */
/*     生命周期                                                       */
/*                                                                    */
/*  使用流程:                                                         */
/*    Init() -> Write()/WriteBin() ...                               */
/*    -> Destroy() (一步到位：排空+fclose+线程退出+释放资源)         */
/*                                                                    */
/* ================================================================== */

/**
 * @func         FileWriterAPI_Init
 * @brief        FileWriterAPI-初始化异步文件写入实例
 * @details      创建目录/文件，初始化 MemoryPool + StreamBuffer，
 *               用 ThreadManage 创建消费线程（SCHED_RR + 配置优先级）。
 *               可重入：多次 Init 创建独立实例。
 * @param[in]    pp:  句柄二级指针，调用前 *pp 必须为 NULL
 * @param[in]    cfg: 配置参数
 * @return       int ret
 * @retval       0:   初始化成功
 * @retval       -1:  参数无效、目录/文件创建失败、内存分配失败或线程创建失败
 * @warning      pp 不能为 NULL，*pp 必须为 NULL
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_Init(T_FileWriter **pp, const T_FileWriterConfig *cfg);

/**
 * @func         FileWriterAPI_Destroy
 * @brief        FileWriterAPI-销毁实例（一步到位：优雅关闭 + 释放资源）
 * @details      依次执行：
 *               1. StreamBuffer.Close（阻止新写入）
 *               2. 消费线程排空缓冲（全部 fwrite 落盘）
 *               3. fflush + fclose 文件
 *               4. 线程退出（ThreadManage）
 *               5. Destroy StreamBuffer + MemoryPool
 *               6. free 结构体，*pp = NULL
 *               返回后保证文件数据完整，所有资源释放。
 * @param[in]    pp: 句柄二级指针
 * @return       int ret
 * @retval       0:   销毁成功（数据完整落盘）
 * @retval       -1:  参数无效或 *pp 已为 NULL
 * @warning      调用前必须确保所有业务线程已停止调用 Write/WriteBin/Flush/Rotate/查询接口，
 *               本函数与写入/查询并发不安全（由调用者负责生命周期同步）。
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_Destroy(T_FileWriter **pp);


/* ================================================================== */
/*                                                                    */
/*     写入                                                           */
/*                                                                    */
/* ================================================================== */

/**
 * @func         FileWriterAPI_Write
 * @brief        FileWriterAPI-格式化写入（printf 式）
 * @details      内部 vsnprintf 格式化到 MemoryPool 分配的 buffer，
 *               [可选] 前置时间戳 [HH:MM:SS.mmmmmm]，
 *               然后 StreamBuffer.PutData 入队（满则丢新，不阻塞业务）。
 *               Write 返回前归还 buffer 到 MemoryPool。
 * @param[in]    fw:  句柄
 * @param[in]    fmt: 格式串（同 printf）
 * @param[in]    ...: 可变参数
 * @return       int ret
 * @retval       >=0: 入队字节数
 * @retval       -1:  参数无效或未初始化
 * @retval       -2:  已关闭，不再接收写入
 * @retval       -3:  StreamBuffer 缓冲空间不足，本段被丢弃（满则丢新）
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_Write(T_FileWriter *fw, const char *fmt, ...);

/**
 * @func         FileWriterAPI_WriteBin
 * @brief        FileWriterAPI-二进制写入（原样，不格式化、不加时间戳）
 * @details      将 data 的 len 字节直接 StreamBuffer.PutData 入队。
 * @param[in]    fw:   句柄
 * @param[in]    data: 二进制数据指针，不能为 NULL
 * @param[in]    len:  字节数，必须 > 0
 * @return       int ret
 * @retval       >=0: 入队字节数
 * @retval       -1:  参数无效或未初始化
 * @retval       -2:  已关闭
 * @retval       -3:  StreamBuffer 缓冲空间不足，本段被丢弃（满则丢新）
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_WriteBin(T_FileWriter *fw, const void *data, int len);


/* ================================================================== */
/*                                                                    */
/*     轮转与刷新                                                     */
/*                                                                    */
/* ================================================================== */

/**
 * @func         FileWriterAPI_Rotate
 * @brief        FileWriterAPI-手动轮转（创建新文件，关闭当前文件）
 * @details      内部动作（原子完成，与消费线程互斥）：
 *               1. 排空 StreamBuffer 剩余数据到当前文件（避免跨文件错位）；
 *               2. fopen 新文件（成功后再 fclose 旧文件，失败保持旧文件可写）；
 *               3. 序号 +1；
 *               4. 若 max_files > 0 则删除超额的旧文件。
 *               失败时保持原状态：仍写入原文件，file_seq 不变。
 * @param[in]    fw: 句柄
 * @return       int ret
 * @retval       0:   轮转成功
 * @retval       -1:  参数无效；或 fopen 新文件失败（原文件仍可写）
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_Rotate(T_FileWriter *fw);

/**
 * @func         FileWriterAPI_Flush
 * @brief        FileWriterAPI-立即触发一次写盘（异步唤醒）
 * @details      唤醒消费线程不等阈值/超时，尽快 fwrite 当前缓冲。
 * @param[in]    fw: 句柄
 * @return       int ret
 * @retval       0:   已触发
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_Flush(T_FileWriter *fw);


/* ================================================================== */
/*                                                                    */
/*     查询                                                           */
/*                                                                    */
/* ================================================================== */

/**
 * @func         FileWriterAPI_GetCurrentFileName
 * @brief        获取当前写文件名（不含路径）
 * @details      如 "sensor1_000_2026-07-11-12-41-30.csv"
 * @param[in]    fw:      句柄
 * @param[out]   out:     输出缓冲
 * @param[in]    out_len: 输出缓冲容量
 * @return       int ret
 * @retval       0:   成功
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_GetCurrentFileName(T_FileWriter *fw, char *out, int out_len);

/**
 * @func         FileWriterAPI_GetCurrentFilePath
 * @brief        获取当前写文件绝对路径（含文件名）
 * @details      如 "/log/X2026_07_11/sensor/sensor1_000_2026-07-11-12-41-30.csv"
 * @param[in]    fw:      句柄
 * @param[out]   out:     输出缓冲
 * @param[in]    out_len: 输出缓冲容量
 * @return       int ret
 * @retval       0:   成功
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_GetCurrentFilePath(T_FileWriter *fw, char *out, int out_len);

/**
 * @func         FileWriterAPI_GetCurrentDirPath
 * @brief        获取当前写文件所在目录绝对路径（不含文件名）
 * @details      如 "/log/X2026_07_11/sensor"
 * @param[in]    fw:      句柄
 * @param[out]   out:     输出缓冲
 * @param[in]    out_len: 输出缓冲容量
 * @return       int ret
 * @retval       0:   成功
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_GetCurrentDirPath(T_FileWriter *fw, char *out, int out_len);

/**
 * @func         FileWriterAPI_GetFileCount
 * @brief        获取当前目录下同前缀的文件数量
 * @param[in]    fw: 句柄
 * @return       int ret
 * @retval       >=0: 文件数量
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_GetFileCount(T_FileWriter *fw);

/**
 * @func         FileWriterAPI_GetTotalFileCount
 * @brief        获取当前目录下所有文件的数量（不限前缀）
 * @details      统计当前写文件所在目录下的全部文件（含其他前缀/类型的文件）。
 *               与 GetFileCount 的区别：GetFileCount 只统计同前缀的，
 *               GetTotalFileCount 统计目录下所有文件。
 * @param[in]    fw: 句柄
 * @return       int ret
 * @retval       >=0: 文件数量
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_GetTotalFileCount(T_FileWriter *fw);


/* ================================================================== */
/*                                                                    */
/*     工具函数（独立于 FileWriter 实例，可直接调用）                  */
/*                                                                    */
/* ================================================================== */

/**
 * @func         FileWriterAPI_GetTimeString
 * @brief        获取当前时间的格式化字符串
 * @details      根据 fmt 格式输出时间字符串。
 *               支持的格式符：
 *               - "datetime"  → "YYYY-MM-DD-HH-MM-SS"（用于文件名，如 2026-07-11-12-41-30）
 *               - "date"      → "YYYY_MM_DD"（用于日期目录，如 2026_07_11）
 *               - "log"       → "[HH:MM:SS.mmmmmm]"（用于日志时间戳前缀）
 *               - "datetime_ms" → "YYYY-MM-DD-HH-MM-SS.mmmmmm"
 * @param[out]   out:     输出缓冲
 * @param[in]    out_len: 输出缓冲容量
 * @param[in]    fmt:     格式串（"datetime"/"date"/"log"/"datetime_ms"）
 * @return       int ret
 * @retval       0:   成功
 * @retval       -1:  参数无效
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_GetTimeString(char *out, int out_len, const char *fmt);

/**
 * @func         FileWriterAPI_MakeDirs
 * @brief        递归创建多级目录（mkdir -p 语义）
 * @details      逐级检查路径是否存在，不存在则创建。
 *               如传入 "/log/X2026_07_11/sensor"，依次创建 /log、/log/X2026_07_11、/log/X2026_07_11/sensor。
 * @param[in]    path: 目录路径（绝对或相对），不能为 NULL
 * @return       int ret
 * @retval       0:   成功（目录已存在或创建成功）
 * @retval       -1:  参数无效或创建失败
 * @author       zlzksrl
 * @date         2026-07-11
 * @Version      V1.0.0
 */
int FileWriterAPI_MakeDirs(const char *path);


#ifdef __cplusplus
 }
#endif

#endif
