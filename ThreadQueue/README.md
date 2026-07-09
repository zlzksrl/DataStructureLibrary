# ThreadQueue - LinuxARM 线程通讯队列库

> **项目版本**: V1.1.0 | **作者**: zlzksrl | **许可证**: AGPL-3.0  
> **目标平台**: IMX6ULL (ARM Linux) | **语言**: C (C99)  
> **代码审查日期**: 2026-05-09

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
  - [严重问题 (Critical)](#严重问题-critical)
  - [中等问题 (Medium)](#中等问题-medium)
  - [轻微问题 (Minor)](#轻微问题-minor)
  - [建议改进 (Suggestions)](#建议改进-suggestions)
- [快速上手](#快速上手)
- [使用流程](#使用流程)
- [许可证](#许可证)

---

## 项目概述

ThreadQueue 是一个面向 IMX6ULL (ARM Linux) 平台的 C 语言线程安全队列库，提供两种队列类型：

| 队列类型 | 说明 | 适用场景 |
|---------|------|---------|
| **ThreadQueue** | 环形缓冲区线程安全队列 (FIFO) | 生产者-消费者模式，需要缓存多条消息 |
| **LatestQueue** | 最新数据队列 (只保留最新一条) | 传感器数据、状态信息等只关心最新值的场景 |

### 核心特性

- 多生产者多消费者 (MPMC) 支持
- 队列满/空时自动阻塞等待
- 带超时的 Put/Get 操作
- 队列关闭/重新打开/刷新/销毁完整生命周期管理
- 自动数据释放回调机制，防止内存泄漏
- Opaque pointer 设计，隐藏内部实现细节
- C/C++ 兼容 (`extern "C"`)
- 同时生成静态库 (`.a`) 和动态库 (`.so`)

---

## 项目结构

```
ThreadQueue/
├── include/
│   └── ThreadQueue.h          # 公共 API 头文件（用户可见）
├── src/
│   ├── ThreadQueue_Main.h     # 内部数据结构定义头文件
│   ├── ThreadQueue_Main.c     # 核心实现文件
│   └── ThreadQueue_Maketime.h # 自动生成的版本/时间戳头文件
├── debug/
│   ├── main.c                 # 测试程序
│   ├── Makefile               # 构建脚本
│   ├── ProjectInfo.txt        # 构建信息（自动生成）
│   ├── libThreadQueue.a       # 静态库
│   ├── libThreadQueue.so      # 动态库
│   ├── obj_static/            # 静态库目标文件
│   └── obj_shared/            # 动态库目标文件
├── LICENSE                    # AGPL-3.0 许可证
└── README.md                  # 本文件
```

---

## 架构设计

### 设计模式

| 模式 | 应用 |
|------|------|
| **Opaque Pointer (不透明指针)** | 公共头文件仅暴露 `typedef struct T_THREADQUEUEMSG T_ThreadQueueMsg;`，内部结构体定义在 `src/ThreadQueue_Main.h` 中，实现了接口与实现的分离 |
| **生产者-消费者** | 通过 `pthread_mutex_t` + `pthread_cond_t` 实现经典的阻塞式生产者-消费者模式 |
| **环形缓冲区 (Ring Buffer)** | ThreadQueue 使用固定大小的环形缓冲区，通过 `lget`/`lput`/`nData` 三个字段管理读写位置 |
| **回调机制** | 通过 `release_callback` 实现自定义数据释放，通过 `FlushThreadQueueMsg` 的 callback 实现自定义数据处理 |
| **状态机** | 队列具有明确的生命周期状态：未初始化 → 已初始化(运行中) → 已关闭 → 已销毁 |

### 数据流

```
ThreadQueue (环形缓冲区):
  生产者 → [buffer环形缓冲区] → 消费者
            lput↑         lget↑
            nData 记录当前数据条数

LatestQueue (单条存储):
  生产者 → [data单条存储] → 消费者
            has_data 标志是否有数据
            旧数据通过 release_callback 自动释放
```

---

## API 参考

### ThreadQueue API (环形缓冲区队列)

| 函数 | 说明 | 返回值 |
|------|------|--------|
| `ThreadQueueAPI_InitMsg()` | 初始化队列 | 0成功, -1失败 |
| `ThreadQueueAPI_PutMsg()` | 发送消息(无限等待) | 0成功, -1参数无效, -2队列已关闭 |
| `ThreadQueueAPI_PutMsgTimeout()` | 发送消息(带超时) | 0成功, -1参数无效, -2队列已关闭, -3超时 |
| `ThreadQueueAPI_GetMsg()` | 获取消息(带超时) | 数据指针或NULL |
| `ThreadQueueAPI_CloseMsg()` | 关闭队列 | 0成功, -1失败 |
| `ThreadQueueAPI_ReopenMsg()` | 重新打开队列 | 0成功, -1失败 |
| `ThreadQueueAPI_IsClosed()` | 查询关闭状态 | 1已关闭, 0未关闭, -1参数无效 |
| `ThreadQueueAPI_GetLength()` | 查询队列长度 | >=0数据条数, -1参数无效 |
| `FlushThreadQueueMsg()` | 刷新队列(回调处理) | >=0处理条数, -1参数无效 |
| `DestroyThreadQueueMsg()` | 销毁队列 | 0成功, -1失败 |

### LatestQueue API (最新数据队列)

| 函数 | 说明 | 返回值 |
|------|------|--------|
| `InitLatestQueueMsg()` | 初始化队列 | 0成功, -1失败 |
| `PutLatestQueueMsg()` | 写入最新数据 | 0成功, -1参数无效, -2队列已关闭 |
| `GetLatestQueueMsg()` | 获取最新数据(带超时) | 数据指针或NULL |
| `CloseLatestQueueMsg()` | 关闭队列 | 0成功, -1失败 |
| `ThreadQueueAPI_Latest_ReopenMsg()` | 重新打开队列 | 0成功, -1失败 |
| `ThreadQueueAPI_Latest_IsClosed()` | 查询关闭状态 | 1已关闭, 0未关闭, -1参数无效 |
| `FlushLatestQueueMsg()` | 刷新队列 | 0或1, -1参数无效 |
| `DestroyLatestQueueMsg()` | 销毁队列 | 0成功, -1失败 |

### 错误码约定

| 错误码 | 含义 |
|--------|------|
| 0 | 成功 |
| -1 | 参数无效或未初始化 |
| -2 | 队列已关闭（仅 Put 返回） |
| -3 | 队列满且等待超时（仅 `ThreadQueueAPI_PutMsgTimeout` 返回） |

---

## 构建系统

### 工具链

- **编译器**: `arm-linux-gnueabihf-gcc` (ARM 交叉编译器)
- **归档工具**: `arm-linux-gnueabihf-ar`
- **线程库**: `-pthread`

### 构建目标

```bash
make all        # 构建静态库 + 动态库 + 测试程序
make slib       # 仅构建静态库 libThreadQueue.a
make dlib       # 仅构建动态库 libThreadQueue.so
make app        # 仅构建测试程序
make clean      # 清理构建产物
make install    # 通过 scp 部署到目标板
make install_lib # 安装库文件到本地路径
```

---

## 代码审查报告

### 总体评价

**代码质量评分: 8.0/10** — 整体质量良好

本项目是一个设计规范、文档完善的嵌入式 C 语言库。代码结构清晰，注释详尽，API 设计合理，线程安全机制基本正确。以下按严重程度列出发现的问题和改进建议。

---

### 优点

1. **文档质量极高** — 每个 API 函数都有完整的 Doxygen 风格注释，包含 `@brief`、`@details`、`@param`、`@return`、`@retval`、`@warning`、`@author`、`@date`、`@Version` 等标签。内部结构体字段也有详细注释。这是很多开源项目都做不到的。

2. **Opaque Pointer 设计** — 公共头文件 [`include/ThreadQueue.h`](include/ThreadQueue.h:59) 仅暴露 `typedef struct T_THREADQUEUEMSG T_ThreadQueueMsg;` 前向声明，内部结构体定义在 [`src/ThreadQueue_Main.h`](src/ThreadQueue_Main.h:75) 中，实现了良好的封装性。

3. **完整的生命周期管理** — 提供了 `Init → Put/Get → Close → Flush → Destroy` 完整的队列生命周期管理流程，支持 `Close`（优雅关闭）和 `Reopen`（重新打开）操作。

4. **自动内存管理** — 通过 `release_callback` 机制，在队列销毁时自动释放残留数据，在 LatestQueue 的 Put 操作中自动丢弃旧数据，有效防止内存泄漏。

5. **参数校验全面** — 每个 API 入口都有 NULL 指针检查、`init_done` 状态检查（大部分函数），防止无效参数导致崩溃。

6. **C++ 兼容** — 头文件使用 `extern "C"` 包裹，支持 C/C++ 混合编程。

7. **测试程序完善** — [`debug/main.c`](debug/main.c:1) 覆盖了 ThreadQueue、LatestQueue、ThreadQueueAPI_PutMsgTimeout 三种场景，演示了正确的 Close → Join → Flush → Destroy 安全销毁流程。

---

### 严重问题 (Critical)

#### C1. `ThreadQueueAPI_IsClosed` / `ThreadQueueAPI_Latest_IsClosed` 无锁读取存在可见性风险

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:402) 第 402-410 行, 第 855-862 行

**描述**: `ThreadQueueAPI_IsClosed()` 直接读取 `is_closed` 字段而没有使用互斥锁保护。虽然注释声称"对于 int 类型的原子读取在大多数平台上是安全的"，但这在 ARM 架构上存在内存可见性问题。ARM 处理器具有弱内存序 (weak memory ordering)，一个线程写入 `is_closed` 后，另一个线程可能因为缓存一致性问题而读取到旧值。

```c
// 当前代码 (无锁读取)
int ThreadQueueAPI_IsClosed(T_ThreadQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg) return -1;
    return pt_QueueMsg->is_closed ? 1 : 0;  // ⚠️ 无锁读取
}
```

**对比**: 同文件的 `ThreadQueueAPI_GetLength()` 正确地使用了互斥锁保护。

**建议修复**:
```c
int ThreadQueueAPI_IsClosed(T_ThreadQueueMsg *pt_QueueMsg)
{
    if(NULL == pt_QueueMsg) return -1;
    int closed;
    pthread_mutex_lock(&pt_QueueMsg->mux);
    closed = pt_QueueMsg->is_closed;
    pthread_mutex_unlock(&pt_QueueMsg->mux);
    return closed ? 1 : 0;
}
```

或者使用 C11 原子操作 `_Atomic int` / `atomic_load()`。

---

#### C2. `init_done` 字段在 Put/Get 函数中无锁读取

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:148) 第 148, 228, 332, 712, 780 行

**描述**: `ThreadQueueAPI_PutMsg`、`ThreadQueueAPI_PutMsgTimeout`、`ThreadQueueAPI_GetMsg`、`PutLatestQueueMsg`、`GetLatestQueueMsg` 等函数在加锁前先读取 `init_done` 字段。在多线程环境下，如果在读取 `init_done` 和加锁之间另一个线程调用了 `Destroy` 将队列销毁，则会导致对已释放内存的访问（Use-After-Free）。

```c
int ThreadQueueAPI_PutMsg(T_ThreadQueueMsg *pt_QueueMsg, void *data)
{
    // ... NULL 检查 ...
    if(!pt_QueueMsg->init_done)  // ⚠️ 无锁读取，此时队列可能正被另一线程销毁
    {
        return -1;
    }
    pthread_mutex_lock(&pt_QueueMsg->mux);  // 如果队列已被销毁，此处为未定义行为
    // ...
}
```

**建议**: 将 `init_done` 检查移到锁内，或使用引用计数/读写锁保护整个对象的生命周期。

---

#### C3. `DestroyThreadQueueMsg` / `DestroyLatestQueueMsg` 中的忙等待循环

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:583) 第 583-594 行, 第 969-980 行

**描述**: 销毁函数使用 `usleep(1000)` 忙等待所有阻塞线程退出。这种方式的缺陷：
- 浪费 CPU 周期（虽然 1ms 间隔影响不大）
- 没有最大等待时间限制，理论上可能无限等待
- 如果有线程死锁，销毁函数将永远无法返回

```c
while(waiting > 0)
{
    usleep(1000);  // ⚠️ 无最大等待限制
    pthread_mutex_lock(&(*ppt_QueueMsg)->mux);
    waiting = (*ppt_QueueMsg)->nFullThread + (*ppt_QueueMsg)->nEmptyThread;
    pthread_mutex_unlock(&(*ppt_QueueMsg)->mux);
}
```

**建议**: 添加最大等待次数或超时机制，超出后打印警告并强制退出：
```c
int max_wait_ms = 5000; // 最多等5秒
while(waiting > 0 && max_wait_ms > 0)
{
    usleep(1000);
    max_wait_ms -= 1;
    // ... 重新读取 waiting ...
}
if(waiting > 0) {
    printf("WARNING: Destroy timeout, %d threads still waiting\n", waiting);
}
```

---

### 中等问题 (Medium)

#### M1. 版本号不一致

**涉及文件**: 多处

| 位置 | 版本号 |
|------|--------|
| [`include/ThreadQueue.h`](include/ThreadQueue.h:16) 头文件注释 | V1.1.0 |
| [`include/ThreadQueue.h`](include/ThreadQueue.h:121) `ThreadQueueAPI_PutMsgTimeout` 注释 | V1.2.0 |
| [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:10) 实现文件注释 | V1.1.0 |
| [`src/ThreadQueue_Maketime.h`](src/ThreadQueue_Maketime.h:4) 构建时间戳 | 1.0.01N |
| [`debug/Makefile`](debug/Makefile:8) | 1.0.01N |

**建议**: 统一版本号管理，建议在 Makefile 中定义版本号，通过构建脚本自动同步到头文件。

---

#### M2. 使用 `printf` 作为库的日志输出

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:58) 全文

**描述**: 库中所有错误和警告信息都使用 `printf` 输出到 stdout。作为底层库，这存在以下问题：
- 无法被上层应用控制或重定向
- `printf` 输出可能与其他日志交错
- 在无 stdout 的嵌入式环境中输出丢失
- 无法按级别过滤日志

**建议**: 提供日志回调注册机制，或至少使用 `fprintf(stderr, ...)` 输出错误信息：
```c
// 方案1: 回调机制
typedef void (*LogCallback)(int level, const char* msg);
void SetThreadQueueLogger(LogCallback cb);

// 方案2: 至少使用 stderr
fprintf(stderr, "ERROR: NULL == ppt_QueueMsg ##%s->%d\n", __FUNCTION__, __LINE__);
```

---

#### M3. 部分函数缺少 `init_done` 检查

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:402)

| 函数 | 是否检查 `init_done` | 
|------|---------------------|
| `ThreadQueueAPI_PutMsg` | ✅ 检查 |
| `ThreadQueueAPI_PutMsgTimeout` | ✅ 检查 |
| `ThreadQueueAPI_GetMsg` | ✅ 检查 |
| `ThreadQueueAPI_CloseMsg` | ✅ 检查 |
| `ThreadQueueAPI_ReopenMsg` | ✅ 检查 |
| `ThreadQueueAPI_IsClosed` | ❌ **未检查** |
| `ThreadQueueAPI_GetLength` | ❌ **未检查** |
| `FlushThreadQueueMsg` | ❌ **未检查** |
| `ThreadQueueAPI_Latest_IsClosed` | ❌ **未检查** |
| `FlushLatestQueueMsg` | ❌ **未检查** |

**建议**: 所有访问结构体字段的公共 API 都应检查 `init_done`，保持一致性。

---

#### M4. `FlushThreadQueueMsg` 释放锁执行回调可能导致状态不一致

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:536) 第 536-539 行

**描述**: `FlushThreadQueueMsg` 在循环中先取出一条数据，然后释放锁执行回调，再重新获取锁。在锁释放期间，其他线程可以 Put 新数据到队列中，这可能导致：
- Flush 的数据条数与预期不符
- 如果回调中调用了队列的其他操作，可能产生死锁

```c
while(pt_QueueMsg->nData > 0)
{
    data = (pt_QueueMsg->buffer)[pt_QueueMsg->lget++];
    // ...
    pthread_mutex_unlock(&pt_QueueMsg->mux);  // ⚠️ 释放锁
    callback(data);                            // ⚠️ 回调期间其他线程可操作队列
    pthread_mutex_lock(&pt_QueueMsg->mux);
}
```

**建议**: 在文档中明确说明此行为（已部分说明），或提供一个不释放锁的 Flush 变体。

---

#### M5. 队列名称截断逻辑存在 off-by-one

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:108) 第 108-118 行

**描述**: 队列名称缓冲区大小为 `MAX_THREADQUEUENAME_LEN+1 = 33` 字节，可存储 32 个字符 + '\0'。当名称长度 >= 32 时，else 分支只复制 `MAX_THREADQUEUENAME_LEN - 1 = 31` 个字符，浪费了 1 个字符的空间。

```c
// 缓冲区: char name[33]  → 最多存 32 字符 + '\0'
if(iNameLen < MAX_THREADQUEUENAME_LEN)  // < 32
{
    strncpy(pt_QueueMsg->name, sQueueName, iNameLen);     // OK
    pt_QueueMsg->name[iNameLen] = '\0';
}
else
{
    strncpy(pt_QueueMsg->name, sQueueName, MAX_THREADQUEUENAME_LEN - 1); // ⚠️ 只复制31字符，应为32
    pt_QueueMsg->name[MAX_THREADQUEUENAME_LEN - 1] = '\0';
}
```

**建议**: else 分支改为：
```c
strncpy(pt_QueueMsg->name, sQueueName, MAX_THREADQUEUENAME_LEN);
pt_QueueMsg->name[MAX_THREADQUEUENAME_LEN] = '\0';
```

---

#### M6. Makefile `rm` 命令语法不规范

**文件**: [`debug/Makefile`](debug/Makefile:72) 第 72-74, 113-116 行

```makefile
rm -f *.o  -rf           # ⚠️ -rf 应在文件模式之前
rm -f obj_shared/*.o obj_shared/ -rf
```

**建议**: 改为标准语法：
```makefile
rm -f *.o
rm -rf obj_shared/
```

---

### 轻微问题 (Minor)

#### m1. 错误信息格式不一致

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:150) 第 150, 228, 332 行

部分错误信息使用 `WARNNING!!!!!`（拼写错误，多了一个 N，且多个感叹号），而其他地方使用 `WARNING:`。

```
WARNNING!!!!!0x... Not Init     // ⚠️ 拼写错误 + 多个感叹号
WARNING: Queue ... is not closed // ✅ 正确格式
```

**建议**: 统一为 `WARNING:` 格式。

---

#### m2. 使用 `\r\n` 换行符

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:58) 全文

所有 `printf` 语句使用 `\r\n` (Windows 风格换行)，但本项目目标平台为 Linux，应使用 `\n`。

**建议**: 将所有 `\r\n` 替换为 `\n`。

---

#### m3. 版权年份未更新

**文件**: [`include/ThreadQueue.h`](include/ThreadQueue.h:18), [`src/ThreadQueue_Main.h`](src/ThreadQueue_Main.h:11), [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:12)

版权声明为 `copyright (C) 2024`，但文件修改日期为 2026 年。

**建议**: 更新为 `copyright (C) 2024-2026`。

---

#### m4. 测试程序使用已弃用的 `usleep`

**文件**: [`debug/main.c`](debug/main.c:201) 第 201, 254, 357, 433, 505 行

`usleep()` 在 POSIX.1-2008 中已被标记为过时，建议使用 `nanosleep()` 替代。

---

#### m5. 环形缓冲区未零初始化

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:90) 第 90 行

```c
pt_QueueMsg->buffer = malloc(iQueueLen * sizeof(void*));  // ⚠️ 未清零
```

虽然缓冲区由 `lget`/`lput`/`nData` 严格管理，但零初始化有助于调试（例如通过 GDB 查看缓冲区内容时区分已用和未用槽位）。

**建议**: 使用 `calloc` 或在 `malloc` 后 `memset` 清零。

---

#### m6. `ThreadQueue_Maketime.h` 不应纳入版本控制

**文件**: [`src/ThreadQueue_Maketime.h`](src/ThreadQueue_Maketime.h:1)

此文件由 Makefile 每次构建时自动生成，包含构建时间戳。纳入版本控制会导致每次构建后仓库显示为"脏"状态。

**建议**: 将 `src/ThreadQueue_Maketime.h` 添加到 `.gitignore`。

---

#### m7. `ThreadQueueAPI_InitMsg` 每次调用都打印版本信息

**文件**: [`src/ThreadQueue_Main.c`](src/ThreadQueue_Main.c:78) 第 78 行

```c
printf("ThreadQueueLibVision  = [%s]\r\n", ThreadQueue_PROJECT_MAKETIME);
```

每次调用 `ThreadQueueAPI_InitMsg` 都会打印库版本信息。如果创建多个队列，会重复输出。

**建议**: 使用 `static int printed = 0;` 标志确保只打印一次，或移除此输出。

---

### 建议改进 (Suggestions)

#### S1. 添加 `GetLatestQueueLength` API

ThreadQueue 有 `ThreadQueueAPI_GetLength()` 查询队列长度，但 LatestQueue 没有对应的 API。建议添加：

```c
int GetLatestQueueLength(T_LatestQueueMsg *pt_QueueMsg);
// 返回 0 或 1
```

---

#### S2. 添加 `TryPutThreadQueueMsg` 非阻塞版本

当前 `ThreadQueueAPI_PutMsg` 在队列满时阻塞，`ThreadQueueAPI_PutMsgTimeout` 需要指定超时。建议添加一个非阻塞版本：

```c
int TryPutThreadQueueMsg(T_ThreadQueueMsg *pt_QueueMsg, void *data);
// 队列满时立即返回 -3，不阻塞
```

---

#### S3. Makefile 支持本地编译

当前 Makefile 硬编码 `arm-linux-gnueabihf-gcc` 交叉编译器，无法在 x86 开发机上编译测试。建议：

```makefile
# 支持通过环境变量切换工具链
CC ?= arm-linux-gnueabihf-gcc
# 或添加 x86 编译目标
make all CROSS=     # 使用默认 gcc
```

---

#### S4. 添加 CMake 构建支持

对于更广泛的使用场景，建议添加 `CMakeLists.txt`，支持 `cmake` 构建，便于集成到其他项目中。

---

#### S5. 添加单元测试框架

当前测试程序 ([`debug/main.c`](debug/main.c:1)) 是手动编写的集成测试。建议引入单元测试框架（如 Unity、CMocka），实现自动化测试和覆盖率统计。

---

#### S6. 考虑使用 `pthread_condattr_setclock` 统一时钟源

当前超时计算使用 `gettimeofday()` 获取墙上时钟时间。但 `pthread_cond_timedwait` 默认使用 `CLOCK_REALTIME`，如果系统时间被修改（如 NTP 校时），可能导致超时行为异常。

**建议**: 使用 `CLOCK_MONOTONIC` 时钟：
```c
pthread_condattr_t attr;
pthread_condattr_init(&attr);
pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
pthread_cond_init(&cond, &attr);
// 然后使用 clock_gettime(CLOCK_MONOTONIC, &ts) 计算超时
```

---

## 快速上手

### 编译

```bash
cd debug
make clean && make all
```

### 运行测试

```bash
# 在目标板上
scp ThreadQueue_DebugPro.bin root@192.168.1.6:/tmp/
ssh root@192.168.1.6 "/tmp/ThreadQueue_DebugPro.bin"
```

### 在自己的项目中使用

```c
#include "ThreadQueue.h"

// 1. 定义数据释放回调
void my_release(void* data) {
    free(data);
}

// 2. 初始化队列
T_ThreadQueueMsg *queue = NULL;
ThreadQueueAPI_InitMsg(&queue, 100, "myQueue", my_release);

// 3. 生产者线程: 发送数据
int *data = malloc(sizeof(int));
*data = 42;
ThreadQueueAPI_PutMsg(queue, data);

// 4. 消费者线程: 接收数据
int *received = (int*)ThreadQueueAPI_GetMsg(queue, 1000);  // 1秒超时
if(received) {
    printf("Got: %d\n", *received);
    free(received);
}

// 5. 安全销毁流程
ThreadQueueAPI_CloseMsg(queue);           // 关闭队列
// ... join 消费者/生产者线程 ...
FlushThreadQueueMsg(queue, my_release); // 刷新残留数据
DestroyThreadQueueMsg(&queue);          // 销毁队列
```

---

## 使用流程

### ThreadQueue 标准使用流程

```
ThreadQueueAPI_InitMsg()          ← 初始化队列
        │
        ▼
┌─→ ThreadQueueAPI_PutMsg()       ← 生产者发送消息
│       │
│   ThreadQueueAPI_GetMsg()       ← 消费者接收消息
│       │
└─── (循环)
        │
        ▼
ThreadQueueAPI_CloseMsg()         ← 关闭队列(阻止新消息)
        │
        ▼
pthread_join()                ← 等待线程退出
        │
        ▼
FlushThreadQueueMsg()         ← 刷新残留数据
        │
        ▼
DestroyThreadQueueMsg()       ← 销毁队列释放资源
```

### LatestQueue 标准使用流程

```
InitLatestQueueMsg()          ← 初始化队列
        │
        ▼
┌─→ PutLatestQueueMsg()       ← 生产者写入最新数据(自动丢弃旧数据)
│       │
│   GetLatestQueueMsg()       ← 消费者获取最新数据
│       │
└─── (循环)
        │
        ▼
CloseLatestQueueMsg()         ← 关闭队列
        │
        ▼
pthread_join()                ← 等待线程退出
        │
        ▼
FlushLatestQueueMsg()         ← 刷新残留数据
        │
        ▼
DestroyLatestQueueMsg()       ← 销毁队列释放资源
```

---

## 许可证

本项目采用 [GNU Affero General Public License v3.0](LICENSE) 许可证。

---

> **审查总结**: 本项目代码质量整体良好，架构设计合理，文档注释非常完善。主要需要关注的是线程安全方面的几个无锁读取问题（C1、C2），以及版本号不一致等中等问题。建议在下一个版本中优先修复 Critical 级别的问题。