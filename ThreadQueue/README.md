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
| `ThreadQueueAPI_FlushMsg()` | 刷新队列(回调处理) | >=0处理条数, -1参数无效 |
| `ThreadQueueAPI_DestroyMsg()` | 销毁队列 | 0成功, -1失败 |

### LatestQueue API (最新数据队列)

| 函数 | 说明 | 返回值 |
|------|------|--------|
| `ThreadQueueAPI_Latest_InitMsg()` | 初始化队列 | 0成功, -1失败 |
| `ThreadQueueAPI_Latest_PutMsg()` | 写入最新数据 | 0成功, -1参数无效, -2队列已关闭 |
| `ThreadQueueAPI_Latest_GetMsg()` | 获取最新数据(带超时) | 数据指针或NULL |
| `ThreadQueueAPI_Latest_CloseMsg()` | 关闭队列 | 0成功, -1失败 |
| `ThreadQueueAPI_Latest_ReopenMsg()` | 重新打开队列 | 0成功, -1失败 |
| `ThreadQueueAPI_Latest_IsClosed()` | 查询关闭状态 | 1已关闭, 0未关闭, -1参数无效 |
| `ThreadQueueAPI_Latest_FlushMsg()` | 刷新队列 | 0或1, -1参数无效 |
| `ThreadQueueAPI_Latest_DestroyMsg()` | 销毁队列 | 0成功, -1失败 |

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

