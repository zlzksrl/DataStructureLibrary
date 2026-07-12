# FileWriter 异步文件写入模块（日志 / CSV / 二进制记录）

> **项目版本**: V1.1.0 | **作者**: zlzksrl | **许可证**: AGPL-3.0
> **目标平台**: IMX6ULL (ARM Linux) | **语言**: C11（依赖 `<stdatomic.h>`）
> **命名规范**: `FileWriterAPI_*`
> **创建日期**: 2026-07-11 | **本次更新**: 2026-07-12（V1.1 抗并发销毁）

---

## 目录

- [一、项目概述](#一项目概述)
- [二、项目结构](#二项目结构)
- [三、架构设计](#三架构设计)
- [四、API 参考](#四api-参考)
- [五、配置详解](#五配置详解)
- [六、文件命名与轮转](#六文件命名与轮转)
- [七、快速上手](#七快速上手)
- [八、构建系统](#八构建系统)
- [九、设计决策](#九设计决策)
- [十、并发与生命周期](#十并发与生命周期)
- [十一、常见问题](#十一常见问题)
- [十二、变更记录](#十二变更记录)
- [许可证](#许可证)

---

## 一、项目概述

FileWriter 是一个**异步文件写入库**，为嵌入式设备（IMX6ULL 等 ARM Linux 平台）提供 `printf` 式格式化接口，攒批写盘（减磁盘磨损），支持日志 / CSV / 二进制三类文件，具备自动文件轮转（按数量+大小+日期）和优雅关闭能力。

业务线程调 `Write(fmt, ...)` **不阻塞、不等磁盘**，内置消费线程按阈值/超时批量 `fwrite` 落盘。

### 核心特性

- **异步写入**：业务线程 → StreamBuffer 入队 → 消费线程批量 `fwrite`，业务线程不被磁盘 I/O 拖慢
- **printf 格式化**：`FileWriterAPI_Write(fw, "ret=%d\n", val)`，vsnprintf 内部处理
- **攒批写盘**：`used >= flush_bytes` 或 `flush_ms` 超时时批量落盘（减少 fwrite 调用次数）
- **时间戳前缀**：可选 `[HH:MM:SS.mmmmmm]` 前缀
- **文件轮转**：按大小 (`max_file_size`) / 跨日 (`auto_rotate_daily`) / 手动 (`Rotate()`)
- **日期子目录**：可配 `date_subdir_prefix`，如 `X` → `/log/X2026_07_11/`
- **命名规则**：`{prefix}_{序号3位}_{YYYY-MM-DD-HH-MM-SS}.{ext}`
- **多实例可重入**：一个进程可 Init 多个 FileWriter，各自独立线程 + 文件
- **SCHED_RR 线程**：消费线程用 ThreadManage 创建，SCHED_RR 策略，可配优先级
- **优雅关闭**：`Destroy` 一步到位——阻止写入 → 排空缓冲 → fflush+fclose → 线程退出 → 释放资源
- **抗并发销毁 (V1.1)**：业务线程持有句柄期间可安全并发调 `Destroy`（如信号处理中直接调），最坏 `Write` 返回 -2，不 UAF / 不 double-free。基于原子引用计数 + CAS 独占释放权 + 两阶段销毁（Phase A 同步 / Phase B 延迟）。
- **运行统计**：`StatsGet` 查累计写盘/丢失字节数、轮转成功/失败次数、缓冲积压

### 典型应用

- **异步日志**：`FileWriterAPI_Write(fw, "[moduleA] ret=%d\n", ret)` → 自动加时间戳 → 攒批写盘
- **CSV 记录**：外部组织行内容 → `FileWriterAPI_Write(fw, "%d,%d,%.3f\n", ts, ch, val)`
- **二进制 bin**：`FileWriterAPI_WriteBin(fw, &frame, sizeof(frame))` → 攒批写盘
- **多实例并发**：一个进程 Init 多个 FileWriter（不同目录/前缀/优先级），各自独立写盘

---

## 二、项目结构

```
FileWriter/
├── include/FileWriter.h              # 公共 API（opaque pointer + extern "C" + Doxygen）
├── src/
│   ├── FileWriter_Main.h             # 内部结构体 T_FILEWRITER 定义 + 宏
│   ├── FileWriter.c                  # 核心实现（含抗并发销毁）
│   └── FileWriter_Maketime.h         # Makefile 生成的版本时间戳（不手写）
├── debug/
│   ├── main.c                        # 7 段功能演示：LOG/CSV/BIN/多实例/Rotate/查询/工具函数
│   ├── stress_test.c                 # 抗并发销毁高压测试（V1.1 引入）
│   ├── Makefile                      # 交叉编译（arm-linux-gnueabihf-gcc）
│   └── ProjectInfo.txt               # 编译产物 MD5 记录（Makefile 生成）
├── 需求文档.md                        # 原始需求与设计决策
├── AI审查结果.md                      # 五轮代码审查记录
└── readme.md                         # 本文件
```

### 依赖库

FileWriter 依赖两个同项目的通用库（`DataStructureLibrary/Library/` 下）：

| 库 | 角色 | 说明 |
|---|---|---|
| **StreamBuffer** | 攒批缓冲 | `PutData` 写入 → `Wait/GetData` 批量取出 `fwrite` |
| **ThreadManage** | 消费线程创建 | 用 `ThreadAPI_ThreadCreate` 创建，SCHED_RR 策略，可配优先级 |

早期设计曾计划用 MemoryPool 做格式化 buffer 池，评估后发现 Alloc/Free 全程同步且 buffer 无跨线程共享，直接用**栈 buffer**（`char buf[1024]`）更简洁。详见需求文档第三节注释。

---

## 三、架构设计

### 数据流

```
业务线程(多个)                                     FileWriter 消费线程(每实例1个)
   │ FileWriterAPI_Write(fmt, ...)                  │
   │  → vsnprintf 到栈 buffer (1KB)                 │
   │  → [可选] 加时间戳前缀                          │
   │  → StreamBuffer.PutData() 入队(memcpy)          │
   │  ← 立即返回，业务线程不等磁盘                    │
   ▼                                                ▼
                  [StreamBuffer 环形攒批缓冲]
                          │  used≥flush_bytes / flush_ms 超时 / Close
                          ▼
                  消费线程主循环:
                    Wait(flush_ms) → 加 file_lock
                    → 跨日检查 → 循环 GetData → fwrite → fflush
                    → 大小轮转检查 → 释放锁
                          │
                          ▼
                  磁盘文件（自动轮转）
```

### 线程模型

- **生产者（业务线程 N 个）**：调 `Write/WriteBin`，无锁走 StreamBuffer 的 CAS/mutex（SB 自持锁）；不阻塞、不等磁盘。
- **消费者（内置线程 1 个）**：由 ThreadManage `ThreadAPI_ThreadCreate` 拉起，`SCHED_RR` 实时策略，优先级可配（1-99）。非 root 权限不足时自动降级到默认策略。
- **解耦点**：StreamBuffer 环形缓冲既是"生产者不阻塞的缓存池"也是"消费者批量取的攒批容器"。

### 并发保护

| 字段 | 保护方式 | 说明 |
|---|---|---|
| `fp / current_* / file_seq / file_written / current_date / stat_*` | `file_lock` (mutex) | 消费线程写盘、Rotate、查询接口共享 |
| `thread_running / shutting_down` | `volatile int` | 跨线程标志位，靠 mutex 隐式内存屏障 |
| `ref_count / destroying / destroy_pending / finalize_taken` | `atomic_int`（V1.1） | 抗并发销毁，无锁访问，`memory_order` 显式指定 |
| StreamBuffer 内部 | SB 自持锁 | 可在任意时刻并发 Put/Get |

**权衡点**：消费线程 `fwrite/fflush` 在 `file_lock` 内做，磁盘 I/O 抖动会阻塞查询接口和 `Rotate`。这是明确的设计选择（简单 > 无锁读），一般 I/O <10ms，eMMC 擦除等极端场景可能几十毫秒。

---

## 四、API 参考

### 生命周期

```c
int  FileWriterAPI_Init   (T_FileWriter **pp, const T_FileWriterConfig *cfg);
int  FileWriterAPI_Destroy(T_FileWriter **pp);
```

- **Init**：目录创建 + 首文件创建 + StreamBuffer 初始化 + 消费线程启动。可重入（多个实例互不干扰）。
- **Destroy**：一步到位——阻止写入 → 排空缓冲 → fclose → 线程 join → 释放资源 → `*pp = NULL`。**保证数据完整落盘**后才返回。

### 写入

```c
int FileWriterAPI_Write   (T_FileWriter *fw, const char *fmt, ...);
int FileWriterAPI_WriteBin(T_FileWriter *fw, const void *data, int len);
```

| API | 用途 | 时间戳前缀 | 超长处理 |
|---|---|---|---|
| `Write` | printf 式格式化文本 | 可选（config.timestamp） | 截断到 1KB |
| `WriteBin` | 二进制原始字节 | 无 | 交由 SB 决定（缓冲满则丢弃） |

**返回值**：`>=0` 入队字节数 / `-1` 参数无效 / `-2` 已关闭 / `-3` 缓冲满被丢弃。

### 轮转与刷新

```c
int FileWriterAPI_Rotate(T_FileWriter *fw);   // 手动切文件
int FileWriterAPI_Flush (T_FileWriter *fw);   // 唤醒消费线程（异步，不等落盘）
```

- **Rotate**：原子完成"drain SB → 序号+1 → 建新文件 → 关旧文件 → 删超额"。事务性：新文件 fopen 失败保持原文件可写，序号回滚。**可能阻塞几十~几百毫秒**（drain 期间）。
- **Flush**：**异步操作**，只唤醒消费线程；返回瞬间数据大概率仍在缓冲。若需同步落盘请 `Flush → sleep`，或直接 `Destroy`。

### 查询

```c
int FileWriterAPI_GetCurrentFileName (T_FileWriter *fw, char *out, int out_len);
int FileWriterAPI_GetCurrentFilePath (T_FileWriter *fw, char *out, int out_len);
int FileWriterAPI_GetCurrentDirPath  (T_FileWriter *fw, char *out, int out_len);
int FileWriterAPI_GetFileCount       (T_FileWriter *fw);   // 同前缀
int FileWriterAPI_GetTotalFileCount  (T_FileWriter *fw);   // 目录下所有
int FileWriterAPI_StatsGet           (T_FileWriter *fw, T_FileWriterStats *out);
```

`T_FileWriterStats` 字段：`bytes_written / bytes_lost / rotate_count / rotate_fail / sb_used / file_count`。

**注意**：查询接口内部拿 `file_lock`，若消费线程正在写盘（fwrite/fflush）会短暂阻塞。

### 工具函数（可脱离实例调用）

```c
int FileWriterAPI_GetTimeString(char *out, int out_len, const char *fmt);
int FileWriterAPI_MakeDirs     (const char *path);
```

`GetTimeString` 支持 `"datetime"` / `"date"` / `"log"` / `"datetime_ms"` 四种格式。

---

## 五、配置详解

```c
typedef struct T_FILEWRITERCONFIG
{
    /* 路径与命名 */
    char     dir_path[256];          // 写入根目录，自动创建多级
    char     date_subdir_prefix[16]; // 日期子目录前缀，如 "X" → /log/X2026_07_11/；空串=不分目录
    char     file_prefix[64];        // 前缀，可含子路径 "sensor/sensor1"
    FileWriterType file_type;        // TXT/LOG/CSV/BIN，决定默认扩展名
    char     file_ext[16];           // 非空覆盖默认扩展名

    /* 文件轮转 */
    int      max_files;              // 0=无限制；>0 超过删最老（按文件名字典序=时间序）
    int      max_file_size;          // 0=不限制；>0 达此大小自动轮转（字节）
    int      auto_rotate_daily;      // 1=跨日自动轮转

    /* 线程 */
    int      thread_priority;        // SCHED_RR 优先级 1~99；0=用默认 20

    /* 写入行为 */
    int      timestamp;              // 1=每行前加时间戳
    int      flush_bytes;            // 攒批阈值，如 4096
    int      flush_ms;               // 定时写盘周期，如 100ms
    int      buffer_capacity;        // StreamBuffer 容量（须 2 的幂，如 65536）

    /* 生命周期（V1.1） */
    int      destroy_wait_ms;        // Destroy 等 in-flight Writer 出保护区超时，<=0=默认 500ms
} T_FileWriterConfig;
```

### 默认值（配为 0 时生效）

| 字段 | 默认 | 说明 |
|---|---|---|
| `buffer_capacity` | 65536 (64KB) | SB 环形缓冲总容量 |
| `flush_bytes` | 4096 (4KB) | 攒批阈值 |
| `flush_ms` | 100 | 定时写盘周期 |
| `thread_priority` | 20 | SCHED_RR 优先级 |
| `destroy_wait_ms` | 500 | 抗并发销毁 spin-wait 超时 |

### 参数调优建议

- **高吞吐日志**：`flush_bytes` 调大到 16KB，`buffer_capacity` 到 256KB，减少 fwrite 调用
- **低延迟落盘**：`flush_ms` 调小到 20-50ms，业务方也可 `Flush + sleep(50)`
- **多实例竞争**：不同实例的 `thread_priority` 分级设置（如日志 10、告警 30、指标 20）

---

## 六、文件命名与轮转

### 命名规则

```
{file_prefix 的文件名部分}_{seq3位}_{YYYY-MM-DD-HH-MM-SS}.{ext}
```

**示例**：

| 配置 | 生成的文件名 | 完整路径 |
|---|---|---|
| `file_prefix="sensor1"`, `.log` | `sensor1_000_2026-07-11-12-41-30.log` | `/log/X2026_07_11/sensor1_000_...log` |
| `file_prefix="sensor/sensor1"`, `.csv` | `sensor1_000_2026-07-11-12-41-30.csv` | `/log/X2026_07_11/sensor/sensor1_000_...csv` |

### 路径组装顺序

```
{dir_path}
  / {date_subdir_prefix}{YYYY_MM_DD}   ← 仅 date_subdir_prefix 非空时
  / {file_prefix 的路径部分}            ← 仅 file_prefix 含 "/" 时
  / {file_prefix 的文件名部分}_{seq}_{datetime}.{ext}
```

### 轮转触发场景

| 场景 | 配置 | 说明 |
|---|---|---|
| 首次 Init | — | 创建 `_000_` 首个文件 |
| 达大小上限 | `max_file_size > 0` | 消费线程 fwrite 后检查累计字节数 |
| 跨日 | `auto_rotate_daily = 1` | 消费线程每批开始时检查日期变化 |
| 手动 | `FileWriterAPI_Rotate(fw)` | 业务方主动调 |

### 删旧策略

- `max_files > 0`：每次 Rotate 后遍历当前目录，匹配 `{prefix_name}_` 开头的文件，按文件名字典序（= 时间序）删最老，跳过当前正在写的文件。
- `max_files == 0`：无限制。

---

## 七、快速上手

### 最小示例

```c
#include "FileWriter.h"

int main(void)
{
    T_FileWriter *fw = NULL;
    T_FileWriterConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.dir_path,           "/log",   sizeof(cfg.dir_path) - 1);
    strncpy(cfg.date_subdir_prefix, "X",      sizeof(cfg.date_subdir_prefix) - 1);
    strncpy(cfg.file_prefix,        "sensor", sizeof(cfg.file_prefix) - 1);
    cfg.file_type       = FILEWRITER_TYPE_LOG;
    cfg.max_files       = 10;
    cfg.max_file_size   = 1024 * 1024;   // 1MB 自动轮转
    cfg.auto_rotate_daily = 1;
    cfg.thread_priority = 20;
    cfg.timestamp       = 1;             // 加时间戳前缀
    cfg.flush_bytes     = 4096;
    cfg.flush_ms        = 100;
    cfg.buffer_capacity = 65536;

    if(FileWriterAPI_Init(&fw, &cfg) != 0)
    {
        return -1;
    }

    for(int i = 0; i < 100; i++)
    {
        FileWriterAPI_Write(fw, "[moduleA] iter=%d ret=%d\n", i, i * 2);
    }

    /* 一步到位：优雅关闭 + 释放 */
    FileWriterAPI_Destroy(&fw);
    return 0;
}
```

### CSV 场景

```c
strncpy(cfg.file_prefix, "sensor/data", sizeof(cfg.file_prefix) - 1);  // 子路径
cfg.file_type = FILEWRITER_TYPE_CSV;
cfg.timestamp = 0;   // CSV 不加时间戳

FileWriterAPI_Init(&fw, &cfg);
FileWriterAPI_Write(fw, "timestamp,channel,voltage\n");   // 表头
FileWriterAPI_Write(fw, "%d,%d,%.3f\n", ts, ch, val);
FileWriterAPI_Destroy(&fw);
```

### 二进制场景

```c
cfg.file_type     = FILEWRITER_TYPE_BIN;
cfg.max_file_size = 10 * 1024 * 1024;   // 10MB 分片

typedef struct { uint32_t magic; uint32_t seq; uint8_t data[16]; } Frame;
Frame frame = { 0xDEADBEEF, seq, {...} };
FileWriterAPI_WriteBin(fw, &frame, sizeof(frame));
```

### 多实例

```c
T_FileWriter *fw_log  = NULL;
T_FileWriter *fw_data = NULL;
FileWriterAPI_Init(&fw_log,  &cfg_log);   // 独立线程 A
FileWriterAPI_Init(&fw_data, &cfg_data);  // 独立线程 B

/* 业务线程可任意选择目标 */
FileWriterAPI_Write   (fw_log,  "event=%d\n", ev);
FileWriterAPI_WriteBin(fw_data, &sample, sizeof(sample));

FileWriterAPI_Destroy(&fw_log);
FileWriterAPI_Destroy(&fw_data);
```

完整演示见 `debug/main.c`（7 段用例）。

---

## 八、构建系统

### 依赖库路径

Makefile 从 `DataStructureLibrary/Library/` 加载依赖：

```
Library/
├── StreamBuffer/include/StreamBuffer.h    + StaticLib/libStreamBuffer.a
└── ThreadManage/include/ThreadManage.h    + StaticLib/libThreadManage.a
```

路径通过 `LIBFILE_PATH` 变量控制（默认 `$(HOME)/zlzksrl/LinuxARM/Program/DataStructureLibrary/Library`），CI/环境可 override：

```bash
make LIBFILE_PATH=/opt/lib
```

### 构建

```bash
cd debug/
make clean && make            # 构建 slib + dlib + app + stress
make app                      # 仅构建功能演示程序
make stress                   # 仅构建高压测试程序
```

产物：

| 文件 | 说明 |
|---|---|
| `libFileWriter.a` | 静态库 |
| `libFileWriter.so` | 动态库（-fPIC） |
| `FileWriter_DebugPro.bin` | 功能演示（`main.c`，链接静态库） |
| `FileWriter_Stress.bin` | 抗并发销毁高压测试（`stress_test.c`，V1.1） |
| `ProjectInfo.txt` | MD5 记录 |
| `../src/FileWriter_Maketime.h` | 版本时间戳（Makefile 生成，勿手改） |

### 编译器要求

V1.1 引入 `<stdatomic.h>`（C11 标准头），Makefile 显式指定 `-std=gnu11`。GCC 4.9+ 支持；若用老工具链（如 GCC 4.7）需调整。

### 安装到公共库目录

```bash
make install_lib
```

会把 `libFileWriter.a/.so` + 头文件复制到 `$(LIBFILE_PATH)/FileWriter/`。

### 部署到开发板

```bash
make install   # scp 到 root@192.168.1.6:/usr/zlzksrl/app/FileWriter/
```

修改 Makefile 里的 `INSTALLBOARD` 变量以匹配你的开发板 IP。

---

## 九、设计决策

### D1. 攒批缓冲：为什么用 StreamBuffer 而不是 ThreadQueue

StreamBuffer 是**字节流**攒批（合并任意长度的多次写入），一次 `GetData` 可拿到跨消息的连续字节流，一次 `fwrite` 落盘。ThreadQueue 是**消息队列**，每条消息独立入队/出队，攒批粒度粗。日志/CSV/字节流场景 StreamBuffer 更贴合。

### D2. 消费线程：SCHED_RR + 可配优先级

不同进程的日志重要性不同，需要精确控制调度。用 ThreadManage 的 SCHED_RR 策略配 1-99 优先级。非 root 权限不足时自动降级到默认策略（仍能工作，只是抢占性弱）。

### D3. 事务性 Rotate

Rotate 早期实现是"先 fclose 旧 → 再 fopen 新"，fopen 失败时会丢失旧 fp。V1.0 改为"先 fopen 新 → 成功后 fclose 旧"，失败保持原状态。同时轮转前先 drain SB 到旧文件，避免跨文件数据错位。

### D4. 格式化 buffer：栈 vs 池

用栈变量 `char buf[1024]`。Alloc/Free 全程同步（Write 入口→vsnprintf→PutData memcpy→返回），buffer 无跨线程共享，池化无意义。1KB 超长自动截断。

### D5. 时间戳格式

`[HH:MM:SS.mmmmmm]`（含微秒，不含年月日——文件名已有日期）。

### D6. 文件命名事务性

文件名含**秒级时间戳 + 3 位序号**，同实例内序号递增确保唯一。跨实例同秒撞车属用户配置错误（相同 dir_path + file_prefix）。

### D7. 关闭策略

`Destroy` 一步到位：`SB.Close` 阻止写入 → `Flush` 唤醒消费线程 → 消费线程感知 `CLOSE_EMPTY` → 兜底排空 → `fclose` → 线程 `pthread_join`。返回后保证所有已入队数据落盘，所有资源释放，`*pp = NULL`。

---

## 十、并发与生命周期

### 生命周期约定

```
FileWriterAPI_Init()                         ← 主线程调
   ↓
业务线程 Write/WriteBin/Flush/Rotate 并发    ← 任意业务线程调
查询接口 GetXxx / StatsGet 并发              ← 任意业务线程调
   ↓
FileWriterAPI_Destroy()                      ← 任意线程调（含信号处理场景）
                                             ← 与业务线程并发安全（V1.1 抗并发销毁）
```

### 并发矩阵

| 调用 A | 调用 B | 并发是否安全 |
|---|---|---|
| `Write/WriteBin` | `Write/WriteBin` | ✅ 安全（SB 内部持锁） |
| `Write` | `Rotate` | ✅ 安全（分别落 SB / drain SB） |
| `Write` | 查询接口 | ✅ 安全（file_lock 保护快照） |
| `Rotate` | 查询接口 | ✅ 安全（file_lock 串行） |
| `Rotate` | `Rotate` | ✅ 安全（file_lock 串行） |
| 任意接口 | `Destroy` | ✅ **安全（V1.1）**——业务线程最坏拿到 -2，不 UAF |

### 抗并发销毁机制（V1.1）

**能力**：Init 后本实例的任何 API 支持与 `Destroy` **真并发**——业务线程持有 `T_FileWriter *` 期间，另一线程随时可以调 `Destroy`。业务线程的 API 调用最坏返回 -2（已关闭），不会 UAF、不会 double-free。

**实现要点**：

- 4 个原子字段（`ref_count / destroying / destroy_pending / finalize_taken`）+ 保护区宏（`FW_ENTER_GUARD / FW_LEAVE_GUARD`）。
- Destroy 分两阶段：
  - **Phase A**（同步阶段）：置 destroying → SB.Close → join 消费线程 → **数据完整落盘** → spin-wait `destroy_wait_ms`（默认 500ms）等 in-flight Writer 出保护区 → CAS 抢释放权。
  - **Phase B**（延迟释放）：Phase A 超时未归零时进入。置 destroy_pending → `*pp=NULL` 归还给用户 → 最后一个 LEAVE 的 Writer 通过 CAS 抢到释放权，独占执行 `fw_final_free`。
- 任何情况下 Destroy **有界返回**（最长 `destroy_wait_ms` + 消费线程 drain 时长），文件数据一定完整落盘。
- 极端情况（业务线程永挂）实例内存延迟释放（约几 KB 泄漏），仍保证不 UAF。

**典型场景**：信号处理器直接调 `Destroy`；插件/GC 系统在不可预测的时刻回收实例。

```c
/* V1.1 允许的用法：信号处理只置标志，主线程收到后 Destroy——
 * 即使业务线程还没停 Write 也不会 UAF */
static volatile sig_atomic_t g_stop = 0;
void sig_handler(int) { g_stop = 1; }

/* 或者更激进：直接在其他线程调 Destroy，业务线程会拿到 -2，安全退出。 */
```

**压力验证**：`debug/stress_test.c` 110 轮多线程高压销毁测试（900 万+ Write / 30 轮专项 Phase B）在 IMX6ULL 上零崩溃通过。

---

## 十一、常见问题

**Q1: `Flush` 返回后数据落盘了吗？**
A: **没有**。Flush 只是唤醒消费线程，返回时数据大概率还在 StreamBuffer 内存。若需强同步落盘：`Flush → sleep(50ms)`，或直接 `Destroy`（保证落盘）。

**Q2: `Write` 返回 -3 是什么意思？数据丢了吗？**
A: 是。StreamBuffer 缓冲满时"丢新"（本次入队被丢弃）。检查 `flush_ms` 是否太长、`buffer_capacity` 是否太小、消费线程优先级是否太低（`StatsGet` 看 `sb_used` 高不高）。

**Q3: 磁盘满会怎样？**
A: 消费线程 `fwrite` 短写/失败时，剩余字节计入 `stats.bytes_lost`，主线程不感知（Write 仍成功入队）。生产环境应周期性 `StatsGet` 监控 `bytes_lost`。

**Q4: SCHED_RR 提示 "fallback to default" 是什么？**
A: 非 root 用户或缺 `CAP_SYS_NICE` 权限时无法使用实时调度，自动降级到 SCHED_OTHER。功能正常，只是抢占性弱。要用 SCHED_RR：`sudo ./FileWriter_DebugPro.bin`，或给可执行文件加 capability：
```bash
sudo setcap 'cap_sys_nice=eip' FileWriter_DebugPro.bin
```

**Q5: 多个 FileWriter 实例的 buffer 独立吗？**
A: 完全独立。每个实例有自己的 StreamBuffer、消费线程、file_lock、fp。互不影响。

**Q6: 单条 log > 1KB 怎么办？**
A: 超长部分被截断（`vsnprintf` 返回值检查），不越界。若确实需要长日志，改 `FW_FORMAT_BUF_SIZE` 宏（`FileWriter_Main.h`）并重新编译库。

**Q7: 支持 fsync 吗（真落盘到 flash）？**
A: 当前只做 `fflush`（stdio buffer → kernel）。若需真正 flush 到 flash：V1.2 计划加 `fsync_ms` 配置字段。断电风险高的场景可暂时用 `Flush → sleep → Destroy` 组合。

**Q8: 文件名冲突会覆盖吗？**
A: 极端场景下会（同实例同秒 seq 相同、跨实例同一路径同前缀）。同实例内 seq 递增天然避免；跨实例请配不同 `file_prefix` 或 `dir_path`。

**Q9: 业务线程还在 Write 时能调 Destroy 吗？(V1.1)**
A: **可以**。Destroy 与任何业务 API 并发安全。业务线程正在写的调用最坏返回 -2（destroying），之后的调用同样返回 -2。库内部通过 atomic 引用计数保护，无 UAF、无 double-free。见"十、并发与生命周期"。

**Q10: Destroy 后的 fw 指针能再用吗？(V1.1)**
A: **不能**。Destroy 返回后 `*pp = NULL`，用户应以此为准。若业务线程持有了旧的局部拷贝 `local_fw = fw`，那份局部指针在 Destroy 返回后即变为**悬空指针**，禁止再次使用（会读到已释放内存）。V1.1 的抗并发销毁承诺的是"**Destroy 未返回期间**的并发安全"，不是"Destroy 之后指针仍有效"。业务侧仍应遵循"Destroy 后立即弃用指针"的通用契约。

**Q11: Destroy 阻塞多久？(V1.1)**
A: 有界。默认最长 `destroy_wait_ms`（500ms） + 消费线程 drain SB + fclose 时间（通常几毫秒）。压测实测平均 4.3ms、最大 9.8ms。若业务线程持保护区超过 `destroy_wait_ms` 未出，走 Phase B——Destroy 立刻返回，实例内存延迟释放，仍不 UAF。

---

## 十二、变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| V1.0.0 | 2026-07-11 | 首版发布：核心 API + 事务性 Rotate + drain-before-rotate + Stats API |
| V1.1.0 | 2026-07-12 | **抗并发销毁**：atomic 引用计数 + CAS 独占释放权 + 两阶段销毁（Phase A 同步 / Phase B 延迟）；跨日 rotate 失败可自愈；配置字段 `destroy_wait_ms`；`debug/stress_test.c` 高压测试；`-std=gnu11`（依赖 `<stdatomic.h>`） |

### V1.2 计划（后续迭代）

- `FileWriterAPI_StatsReset` — 清零累计计数器
- `FileWriterAPI_FlushSync(fw, timeout_ms)` — 同步等待落盘
- `T_FileWriterConfig.fsync_ms` — 周期性 fsync 到 flash
- `T_FileWriterConfig.format_buf_size` — 单条日志上限可配
- `FileWriterAPI_SetLogger(log_fn)` — 库内日志重定向到用户回调
- `T_FileWriterStats.destroy_deferred` — Phase B 触发计数暴露给用户

---

## 许可证

AGPL-3.0

Copyright (C) 2026 zlzksrl.
