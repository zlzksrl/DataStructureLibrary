# ThreadQueue - LinuxARM 线程通讯队列库（ThreadQueueAPI_ 命名版）

> **项目版本**: V1.1.0（`ThreadQueueAPI_PutMsgTimeout` 为 V1.2.0）| **作者**: zlzksrl | **许可证**: AGPL-3.0  
> **目标平台**: IMX6ULL (ARM Linux) | **语言**: C (C99)  
> **命名规范**: `ThreadQueueAPI_*` / `ThreadQueueAPI_Latest_*`  
> **代码审查日期**: 2026-07-09

---

## 目录

- [项目概述](#项目概述)
- [项目结构](#项目结构)
- [架构设计](#架构设计)
- [API 参考](#api-参考)
- [构建系统](#构建系统)
- [代码审查报告](#代码审查报告)
  - [总体评价](#总体评价)
  - [优点](#优点)
  - [改名一致性审查（本次重点）](#改名一致性审查本次重点)
  - [严重问题 (Critical)](#严重问题-critical)
  - [中等问题 (Medium)](#中等问题-medium)
  - [轻微问题 (Minor)](#轻微问题-minor)
  - [建议改进 (Suggestions)](#建议改进-suggestions)
- [快速上手](#快速上手)
- [使用流程](#使用流程)
- [许可证](#许可证)

---

## 项目概述

ThreadQueue 是面向 IMX6ULL (ARM Linux) 平台的 C 语言线程安全队列库，提供两种队列类型：

| 队列类型 | 说明 | 适用场景 |
|---------|------|---------|
| **ThreadQueue** | 环形缓冲区线程安全队列 (FIFO) | 生产者-消费者模式，需要缓存多条消息 |
| **LatestQueue** | 最新数据队列 (只保留最新一条) | 传感器数据、状态信息等只关心最新值的场景 |

### 核心特性

- 多生产者多消费者 (MPMC) 支持
- 队列满/空时自动阻塞等待（带超时变体）
- 队列关闭/重新打开/刷新/销毁完整生命周期管理
- 自动数据释放回调机制，防止内存泄漏
- Opaque pointer 设计，隐藏内部实现细节
- C/C++ 兼容 (`extern "C"`)
- 同时生成静态库 (`.a`) 和动态库 (`.so`)

> **本版变更**：相对原始 ThreadQueue，公共 API 函数命名统一为 `ThreadQueueAPI_` 前缀（`LatestQueue` 系列加 `Latest_`），与同仓库 `ThreadManage`（`ThreadAPI_`）、`WindowQueue`（`WindowQueueAPI_`）风格对齐。

---

## 项目结构

```
ThreadQueue/
├── include/
│   └── ThreadQueue.h          # 公共 API 头文件（用户可见）
├── src/
│   ├── ThreadQueue_Main.h     # 内部数据结构定义头文件
│   ├── ThreadQueue_Main.c     # 核心实现文件
│   └── ThreadQueue_Maketime.h # 自动生成的版本/时间戳头文件（Makefile 生成）
├── debug/
│   ├── main.c                 # 测试程序
│   ├── Makefile               # 构建脚本
│   └── ProjectInfo.txt        # 构建信息（自动生成）
├── LICENSE                    # AGPL-3.0 许可证
└── README.md                  # 本文件
```

---

## 架构设计

### 设计模式

| 模式 | 应用 |
|------|------|
| **Opaque Pointer** | 公共头仅暴露 `typedef struct T_THREADQUEUEMSG T_ThreadQueueMsg;`，内部结构体定义在 `src/ThreadQueue_Main.h` |
| **生产者-消费者** | `pthread_mutex_t` + `pthread_cond_t` 实现阻塞式生产者-消费者 |
| **环形缓冲区** | 固定大小缓冲区，`lget`/`lput`/`nData` 三字段管理读写位置 |
| **回调机制** | `release_callback` 自动释放残留数据；`FlushMsg` 的 callback 自定义处理 |
| **状态机** | 未初始化 → 运行中 → 已关闭 → 已销毁 |

### 数据流

```
ThreadQueue:   生产者 → [buffer 环形缓冲区] → 消费者     (lput↑ nData lget↑)
LatestQueue:   生产者 → [data 单条存储]      → 消费者     (has_data 标志；旧数据经 release_callback 释放)
```

---

## API 参考

### ThreadQueue API（环形缓冲区队列）

| 函数 | 说明 | 返回值 |
|------|------|--------|
| `ThreadQueueAPI_InitMsg()` | 初始化队列 | 0成功, -1失败 |
| `ThreadQueueAPI_PutMsg()` | 发送消息(无限等待) | 0成功, -1参数无效, -2已关闭 |
| `ThreadQueueAPI_PutMsgTimeout()` | 发送消息(带超时) | 0成功, -1无效, -2已关闭, -3超时 |
| `ThreadQueueAPI_GetMsg()` | 获取消息(带超时) | 数据指针或 NULL |
| `ThreadQueueAPI_CloseMsg()` | 关闭队列 | 0成功, -1失败 |
| `ThreadQueueAPI_ReopenMsg()` | 重新打开队列 | 0成功, -1失败 |
| `ThreadQueueAPI_IsClosed()` | 查询关闭状态 | 1已关闭, 0未关闭, -1参数无效 |
| `ThreadQueueAPI_GetLength()` | 查询队列长度 | >=0条数, -1参数无效 |
| `ThreadQueueAPI_FlushMsg()` | 刷新队列(回调处理) | >=0处理条数, -1参数无效 |
| `ThreadQueueAPI_DestroyMsg()` | 销毁队列 | 0成功, -1失败 |

### LatestQueue API（最新数据队列）

| 函数 | 说明 | 返回值 |
|------|------|--------|
| `ThreadQueueAPI_Latest_InitMsg()` | 初始化队列 | 0成功, -1失败 |
| `ThreadQueueAPI_Latest_PutMsg()` | 写入最新数据 | 0成功, -1无效, -2已关闭 |
| `ThreadQueueAPI_Latest_GetMsg()` | 获取最新数据(带超时) | 数据指针或 NULL |
| `ThreadQueueAPI_Latest_CloseMsg()` | 关闭队列 | 0成功, -1失败 |
| `ThreadQueueAPI_Latest_ReopenMsg()` | 重新打开队列 | 0成功, -1失败 |
| `ThreadQueueAPI_Latest_IsClosed()` | 查询关闭状态 | 1/0/-1 |
| `ThreadQueueAPI_Latest_FlushMsg()` | 刷新队列 | 0或1, -1参数无效 |
| `ThreadQueueAPI_Latest_DestroyMsg()` | 销毁队列 | 0成功, -1失败 |

### 错误码约定

| 错误码 | 含义 |
|--------|------|
| `0` | 成功 |
| `-1` | 参数无效或未初始化 |
| `-2` | 队列已关闭（仅 Put 返回） |
| `-3` | 队列满且等待超时（仅 `ThreadQueueAPI_PutMsgTimeout` 返回） |

---

## 构建系统

### 工具链

- **编译器**: `arm-linux-gnueabihf-gcc`（ARM 交叉编译器）
- **归档工具**: `arm-linux-gnueabihf-ar`
- **线程库**: `-pthread`

### 构建目标

```bash
make all         # 构建静态库 + 动态库 + 测试程序
make slib        # 仅构建静态库 libThreadQueue.a
make dlib        # 仅构建动态库 libThreadQueue.so
make app         # 仅构建测试程序
make clean       # 清理构建产物
make install     # 通过 scp 部署到目标板
make install_lib # 安装库文件到本地路径
```

---

## 代码审查报告

### 总体评价

**代码质量评分: 8.0/10** — 整体良好

本版相对原 ThreadQueue **仅做了 API 命名重构**（统一为 `ThreadQueueAPI_` 前缀），未修复任何既有缺陷。命名重构本身**基本完整一致**（声明/定义/注释/调用均已更新），但发现 **2 处 Debug 打印字符串遗漏**（详见改名一致性审查）。原审查报告中的全部 Critical / Medium / Minor 问题**依然存在**，建议在下一版本优先修复 Critical 级别。

---

### 优点

1. **文档质量极高** — 每个 API 都有完整 Doxygen 注释（`@brief`/`@details`/`@param`/`@return`/`@retval`/`@warning`/`@author`/`@date`/`@Version`），内部结构体字段也详尽注释。
2. **Opaque Pointer 设计** — 公共头仅前向声明，内部结构体藏在 `src/ThreadQueue_Main.h`，接口与实现分离。
3. **完整生命周期管理** — `Init → Put/Get → Close → Flush → Destroy`，支持 `Close`/`Reopen`。
4. **自动内存管理** — `release_callback` 在 Destroy/Put 丢弃时自动释放，防内存泄漏。
5. **参数校验全面** — 多数 API 入口有 NULL 检查与 `init_done` 检查。
6. **C++ 兼容** — `extern "C"` 包裹。
7. **测试程序完善** — `debug/main.c` 覆盖 ThreadQueue、LatestQueue、PutMsgTimeout 三场景。

---

### 改名一致性审查（本次重点）

对 `InitThreadQueueMsg` → `ThreadQueueAPI_InitMsg` 等全局改名的一致性核查：

| 位置 | 改名状态 |
|------|---------|
| `include/ThreadQueue.h` 声明 | ✓ 全部 `ThreadQueueAPI_` |
| `include/ThreadQueue.h` 注释/使用流程 | ✓ 已更新 |
| `src/ThreadQueue_Main.c` 函数定义 | ✓ 全部一致 |
| `src/ThreadQueue_Main.c` 注释引用 | ✓ 已更新（如 `ThreadQueueAPI_FlushMsg`、`ThreadQueueAPI_Latest_DestroyMsg`） |
| `src/ThreadQueue_Main.h` 结构体注释 | ✓ 已更新 |
| `debug/main.c` 函数调用 | ✓ 全部 `ThreadQueueAPI_` |
| `debug/Makefile` | ✓ `TARGET_Name = ThreadQueue`（不含函数名，无需改） |

#### ⚠️ R1. `debug/main.c` Debug 打印字符串改名遗漏

**文件**: `debug/main.c` 第 293、299 行

函数**调用**已正确改为 `ThreadQueueAPI_InitMsg`，但两处**打印字符串**里仍是旧名：

```c
ret = ThreadQueueAPI_InitMsg(&pt_testQueueMsg1, 100, "testQueue1", my_data_callback);  // ✓ 调用正确
if(ret < 0)
{
    Debug_printx("InitThreadQueueMsg1 fail ret = [%d]", ret);  // ✗ 字符串漏改
    ...
}
Debug_printx("InitThreadQueueMsg2 fail ret = [%d]", ret);      // ✗ 字符串漏改
```

**影响**: 不影响编译与运行逻辑（仅日志文本），但改名不彻底，日志与实际函数名不符，排查时易误导。

**建议**:
```c
Debug_printx("ThreadQueueAPI_InitMsg1 fail ret = [%d]", ret);
Debug_printx("ThreadQueueAPI_InitMsg2 fail ret = [%d]", ret);
```

---

### 严重问题 (Critical)

#### C1. `ThreadQueueAPI_IsClosed` / `ThreadQueueAPI_Latest_IsClosed` 无锁读取存在可见性风险

**文件**: `src/ThreadQueue_Main.c:402-410`, `855-862`

直接读取 `is_closed` 而无互斥锁保护。注释声称"int 原子读取在大多数平台安全"，但 **ARM 为弱内存序**，另一线程写入 `is_closed` 后，本线程可能因缓存一致性读到旧值。同文件 `ThreadQueueAPI_GetLength()` 正确地加了锁，此处却未加，不一致。

**建议**:
```c
int ThreadQueueAPI_IsClosed(T_ThreadQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg || !pt_QueueMsg->init_done) return -1;
    pthread_mutex_lock(&pt_QueueMsg->mux);
    int closed = pt_QueueMsg->is_closed;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return closed ? 1 : 0;
}
```

#### C2. `init_done` 在 Put/Get 中无锁读取（Use-After-Free 风险）

**文件**: `src/ThreadQueue_Main.c:148, 228, 332, 712, 780`

`PutMsg`/`PutMsgTimeout`/`GetMsg`/`Latest_PutMsg`/`Latest_GetMsg` 在加锁前读 `init_done`。若读取后、加锁前另一线程调用了 `DestroyMsg` 释放队列，则后续 `pthread_mutex_lock` 访问已释放内存（UB）。

**建议**: 将 `init_done` 检查移入锁内；或用引用计数/读写锁管理对象生命周期。

#### C3. `DestroyMsg` 忙等待循环无超时上限

**文件**: `src/ThreadQueue_Main.c:583-594`, `968-979`

```c
while(waiting > 0)
{
    usleep(1000);   // ⚠️ 无最大等待限制
    ...
}
```

若有线程死锁，销毁函数将**永不返回**。

**建议**: 加最大等待次数/超时，超时后打印警告并强制继续：
```c
int max_wait_ms = 5000;
while(waiting > 0 && max_wait_ms > 0) { usleep(1000); max_wait_ms--; ... }
if(waiting > 0) printf("WARNING: Destroy timeout, %d threads still waiting\n", waiting);
```

---

### 中等问题 (Medium)

#### M1. 版本号不一致

| 位置 | 版本号 |
|------|--------|
| `include/ThreadQueue.h:16` 头文件注释 | V1.1.0 |
| `include/ThreadQueue.h:121` `PutMsgTimeout` 注释 | V1.2.0 |
| `src/ThreadQueue_Main.c:10` 实现文件注释 | V1.1.0 |
| `src/ThreadQueue_Maketime.h` / `Makefile:8` | 1.0.01N |

**建议**: 统一版本号管理，在 Makefile 定义后由构建脚本同步到各头文件。

#### M2. 使用 `printf` 作为库的日志输出

**文件**: `src/ThreadQueue_Main.c` 全文

库内所有错误/警告用 `printf` 到 stdout，无法被上层重定向/过滤，无 stdout 的嵌入式环境会丢失。

**建议**: 提供日志回调注册机制（`SetLogger`），或至少用 `fprintf(stderr, ...)`。

#### M3. 部分函数缺少 `init_done` 检查

| 函数 | 是否检查 `init_done` |
|------|---------------------|
| `ThreadQueueAPI_IsClosed` | ❌ |
| `ThreadQueueAPI_GetLength` | ❌ |
| `ThreadQueueAPI_FlushMsg` | ❌ |
| `ThreadQueueAPI_Latest_IsClosed` | ❌ |
| `ThreadQueueAPI_Latest_FlushMsg` | ❌ |

**建议**: 所有访问结构体字段的公共 API 统一检查 `init_done`（注意配合 C1/C2 的加锁改造）。

#### M4. `ThreadQueueAPI_FlushMsg` 释放锁执行回调

**文件**: `src/ThreadQueue_Main.c:536-539`

循环中取出一条数据后 `unlock → callback → lock`。锁释放期间其他线程可 Put，导致 Flush 条数与预期不符；若回调内调用了队列操作可能死锁。

**建议**: 文档明确该行为（已部分说明），或提供不释放锁的 Flush 变体。

#### M5. 队列名称截断 off-by-one

**文件**: `src/ThreadQueue_Main.c:116-117`, `683-684`

缓冲区 `name[MAX+1]` 可存 32 字符 + `'\0'`，但 else 分支只复制 `MAX-1 = 31` 字符，浪费 1 字符：

```c
strncpy(pt->name, sQueueName, MAX_THREADQUEUENAME_LEN - 1);  // ⚠️ 应为 MAX
pt->name[MAX_THREADQUEUENAME_LEN - 1] = '\0';
```

**建议**:
```c
strncpy(pt->name, sQueueName, MAX_THREADQUEUENAME_LEN);
pt->name[MAX_THREADQUEUENAME_LEN] = '\0';
```

#### M6. Makefile `rm` 命令语法不规范

**文件**: `debug/Makefile:72-74`, `113-116`

```makefile
rm -f *.o  -rf           # ⚠️ -rf 应在文件模式之前
rm -f obj_shared/*.o obj_shared/ -rf
```

**建议**:
```makefile
rm -f *.o
rm -rf obj_shared/
```

---

### 轻微问题 (Minor)

#### m1. 错误信息拼写不一致

**文件**: `src/ThreadQueue_Main.c:150, 230, 334`

部分用 `WARNNING!!!!!`（拼写错误多一个 N + 多感叹号），其余用 `WARNING:`。

**建议**: 统一为 `WARNING:`。

#### m2. 使用 `\r\n` 换行符

**文件**: `src/ThreadQueue_Main.c` 全文 `printf`

目标平台为 Linux，应用 `\n`。

#### m3. 版权年份未更新

**文件**: `include/ThreadQueue.h:18`, `src/ThreadQueue_Main.h:11`, `src/ThreadQueue_Main.c:12`

`copyright (C) 2024`，但文件 2026 年仍在修改。

**建议**: 更新为 `copyright (C) 2024-2026`。

#### m4. 测试程序使用已弃用的 `usleep`

**文件**: `debug/main.c:201, 254, ...`

POSIX.1-2008 标记 `usleep` 过时，建议 `nanosleep()`。

#### m5. 环形缓冲区未零初始化

**文件**: `src/ThreadQueue_Main.c:90`

```c
pt->buffer = malloc(iQueueLen * sizeof(void*));  // ⚠️ 未清零
```

虽由 `lget`/`lput`/`nData` 严格管理，但零初始化便于 GDB 调试区分已用/未用槽位。建议 `calloc` 或 `memset`。

#### m6. `ThreadQueue_Maketime.h` 纳入版本控制

**文件**: `src/ThreadQueue_Maketime.h`

由 Makefile 每次构建自动生成，纳入版本控制会导致每次构建后仓库"脏"。建议加入 `.gitignore`。

#### m7. `InitMsg` 每次调用都打印版本信息

**文件**: `src/ThreadQueue_Main.c:78`

```c
printf("ThreadQueueLibVision  = [%s]\r\n", ThreadQueue_PROJECT_MAKETIME);
```

创建多个队列会重复输出。建议用 `static int printed = 0;` 只打印一次。

---

### 建议改进 (Suggestions)

#### S1. 添加 `ThreadQueueAPI_Latest_GetLength`

LatestQueue 缺少查询长度的 API（ThreadQueue 有 `GetLength`）。返回 0 或 1。

#### S2. 添加 `ThreadQueueAPI_TryPutMsg` 非阻塞版本

队列满时立即返回 `-3`，不阻塞。

#### S3. Makefile 支持本地编译

当前硬编码 `arm-linux-gnueabihf-gcc`，建议 `CC ?= arm-linux-gnueabihf-gcc` 支持环境变量切换，便于 x86 调试。

#### S4. 添加 CMake 构建支持

便于集成到其他项目。

#### S5. 添加单元测试框架

当前 `main.c` 为手写集成测试，建议引入 Unity/CMocka 做自动化测试与覆盖率统计。

#### S6. 使用 `CLOCK_MONOTONIC` 统一时钟源

当前超时用 `gettimeofday()`（墙钟），系统校时（NTP）会导致超时异常。建议 `pthread_condattr_setclock(CLOCK_MONOTONIC)` + `clock_gettime(CLOCK_MONOTONIC)`。

---

## 快速上手

### 编译

```bash
cd debug
make clean && make all
```

### 运行测试

```bash
scp ThreadQueue_DebugPro.bin root@192.168.1.6:/tmp/
ssh root@192.168.1.6 "/tmp/ThreadQueue_DebugPro.bin"
```

### 在自己的项目中使用

```c
#include "ThreadQueue.h"

/* 1. 数据释放回调 */
void my_release(void* data) { free(data); }

/* 2. 初始化队列 */
T_ThreadQueueMsg *queue = NULL;
ThreadQueueAPI_InitMsg(&queue, 100, "myQueue", my_release);

/* 3. 生产者：发送数据 */
int *data = malloc(sizeof(int));
*data = 42;
ThreadQueueAPI_PutMsg(queue, data);

/* 4. 消费者：接收数据 */
int *received = (int*)ThreadQueueAPI_GetMsg(queue, 1000);  /* 1秒超时 */
if(received) {
    printf("Got: %d\n", *received);
    free(received);
}

/* 5. 安全销毁流程 */
ThreadQueueAPI_CloseMsg(queue);                  /* 关闭 */
/* ... pthread_join 消费者/生产者线程 ... */
ThreadQueueAPI_FlushMsg(queue, my_release);      /* 刷新残留 */
ThreadQueueAPI_DestroyMsg(&queue);               /* 销毁 */
```

---

## 使用流程

### ThreadQueue 标准流程

```
ThreadQueueAPI_InitMsg()        ← 初始化
        │
┌─→ ThreadQueueAPI_PutMsg()     ← 生产者发送
│       │
│   ThreadQueueAPI_GetMsg()     ← 消费者接收
│       │
└── (循环)
        │
ThreadQueueAPI_CloseMsg()       ← 关闭(阻止新消息)
        │
pthread_join()                  ← 等待线程退出
        │
ThreadQueueAPI_FlushMsg()       ← 刷新残留数据
        │
ThreadQueueAPI_DestroyMsg()     ← 销毁释放资源
```

### LatestQueue 标准流程

```
ThreadQueueAPI_Latest_InitMsg()         ← 初始化
        │
┌─→ ThreadQueueAPI_Latest_PutMsg()      ← 写入最新(自动丢弃旧)
│       │
│   ThreadQueueAPI_Latest_GetMsg()      ← 获取最新
│       │
└── (循环)
        │
ThreadQueueAPI_Latest_CloseMsg()        ← 关闭
        │
pthread_join()                          ← 等待线程退出
        │
ThreadQueueAPI_Latest_FlushMsg()        ← 刷新残留
        │
ThreadQueueAPI_Latest_DestroyMsg()      ← 销毁释放资源
```

---

## 许可证

本项目采用 [GNU Affero General Public License v3.0](LICENSE)。

---

> **审查总结**：本版完成 API 命名统一（`ThreadQueueAPI_`），重构基本彻底，仅 `main.c` 2 处日志字符串漏改（R1）。但既有缺陷（C1 无锁可见性、C2 init_done UAF、C3 销毁无超时等）均未修复，建议下一版本优先处理 Critical 项。
