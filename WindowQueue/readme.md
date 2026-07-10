# WindowQueue 滑动窗口队列（值拷贝环形缓冲区）

> **项目版本**: V1.0.0 | **作者**: zlzksrl | **许可证**: AGPL-3.0  
> **目标平台**: IMX6ULL (ARM Linux) | **语言**: C (C99)  
> **命名规范**: `WindowQueueAPI_*`  
> **创建日期**: 2026-07-09

---

## 目录

- [一、项目概述](#一项目概述)
- [二、项目结构](#二项目结构)
- [三、架构设计](#三架构设计)
- [四、API 参考](#四api-参考)
- [五、三种窗口访问方式](#五三种窗口访问方式)
- [六、快速上手](#六快速上手)
- [七、构建系统](#七构建系统)
- [八、变更记录](#八变更记录)

---

## 一、项目概述

WindowQueue 是一个**值拷贝环形缓冲区**滑动窗口队列：Init 时预分配 `size × element_size` 连续内存，始终保留**最近 N 条**数据，队列满时自动丢弃最老（零拷贝 O(1)），适用于传感器数据采集、信号处理（中值滤波、移动平均）等只关心最近一段数据的场景。

### 核心特性

- **值拷贝预分配**：Init 一次性 malloc，Put 时 memcpy 拷贝进环形缓冲，零运行时 malloc
- **满则丢老**：队列满时移动读指针丢弃最老数据（O(1)，零拷贝），不阻塞采集
- **三种窗口访问**：`Snapshot`(快照拷贝) / `ForEach`(锁内零拷贝回调) / `SetPutCallback`(入队回调)
- **动态 Resize**：运行中调整容量（变大全保留、变小从最老端丢弃）
- **运行统计**：累计 Put / 丢弃 / 峰值长度
- **多线程安全**：pthread 互斥锁保护

### 与 ThreadQueue / StreamBuffer 的区别

| 库 | 满时行为 | 数据读取 | 内存模型 |
|----|---------|---------|---------|
| **WindowQueue** | 丢弃最老（保留最近 N 条） | 只读遍历整个窗口 | 值拷贝预分配 |
| ThreadQueue | 阻塞生产者 | 消费式 Get（取出即消失） | void* 指针 |
| StreamBuffer | 丢弃本次写入 | 按阈值/超时批量出队 | 字节流预分配 |

---

## 二、项目结构

```
WindowQueue/
├── include/WindowQueue.h         # 公共 API（opaque + extern "C" + Doxygen）
├── src/
│   ├── WindowQueue_Main.h        # 内部结构体 T_WINDOWQUEUEMSG 定义
│   ├── WindowQueue.c             # 核心实现（值拷贝环形缓冲 + 回调 + Resize）
│   └── WindowQueue_Maketime.h    # Makefile 自动生成（不纳入版本控制）
├── debug/
│   ├── main.c                     # 演示（ForEach 中值滤波 + Snapshot 均值 + 入队回调）
│   └── Makefile                   # arm 交叉编译
└── 需求文档.md
```

---

## 三、架构设计

### 3.1 数据流

```
采集线程(多个)                        处理线程(用户自建)
   │ PutData(结构体值拷贝)               │
   ▼                                     │
 [环形缓冲区 buffer]                     │  Snapshot(buf, max)  → 拷贝最新N条
 (预分配, 满丢最老,                      │  ForEach(callback)   → 锁内零拷贝遍历
  零运行时malloc)                        │  SetPutCallback      → 入队回调(view指针数组)
                                         ▼
                                   用户做滤波/统计/记录
```

### 3.2 值拷贝环形缓冲区

```
环形缓冲区示意 (size=8, lget=2, lput=5, nData=3):
  [0] [1] [2] [3] [4] [5] [6] [7]
            ^get         ^put
            |---数据(3)--| 空闲(5)

满: nData == size → Put 时 lget 前进一格(丢弃最老), 零拷贝 O(1)
空: nData == 0
索引回绕: 用 % size (容量任意正整数, 不要求 2 的幂)
```

- **Put**：memcpy 拷贝到 `lput` 位置；满则 `lget` 前进丢弃最老；`nData++`（满时不增）
- **Snapshot**：从最新 N 条拷贝到用户 buf（锁内 memcpy，锁外处理）
- **ForEach**：锁内遍历整个窗口，data 直接指向内部条目（零拷贝）
- **Resize**：重新分配 + 搬移 + 原子替换（O(n) 阻塞操作）

### 3.3 统计语义

| 字段 | 含义 |
|------|------|
| `ulTotalPut` | 累计成功写入次数 |
| `ulTotalDiscarded` | 累计因满丢弃条数（**不含 Resize 缩容丢弃**） |
| `iPeakLength` | 窗口历史峰值条数 |

> 丢包率 = `ulTotalDiscarded / ulTotalPut`。Resize 缩容丢弃不计入（那是用户主动行为，非采集丢包）。

---

## 四、API 参考

### 类型定义

```c
typedef struct T_WINDOWQUEUEMSG T_WindowQueueMsg;           /* opaque 句柄 */

/* ForEach / Flush 回调：data 指向一条 element_size 字节数据 */
typedef void (*WindowQueueDataCb)(const void *data, int index, void *user_ctx);

/* 入队回调：Put 后锁内构建 view 指针数组，零拷贝传给回调 */
typedef void (*WindowQueuePutCb)(const void * const *entries, int count, void *user_ctx);

/* 统计 */
typedef struct {
    unsigned long ulTotalPut;
    unsigned long ulTotalDiscarded;
    int           iPeakLength;
} T_WindowQueueStats;
```

### API 一览（14 个）

| 类别 | 函数 | 说明 |
|------|------|------|
| 生命周期 | `Init` | 初始化（预分配 size×element_size） |
| | `Destroy` | 销毁释放 |
| | `Close` / `Reopen` / `IsClosed` | 关闭/重开/查询 |
| 写入 | `Put` | 写入一条（满则丢最老，返回丢弃 0/1） |
| 窗口访问 | `Snapshot` | 快照拷贝最新 N 条（锁内拷贝，锁外处理） |
| | `ForEach` | 锁内零拷贝遍历整个窗口 |
| | `SetPutCallback` | 注册入队回调（每次 Put 后锁内触发） |
| 容量 | `Resize` | 动态变长/变短（O(n) 阻塞） |
| | `GetLength` / `GetCapacity` | 查询当前条数 / 容量 |
| 刷新 | `Flush` | 回调处理窗口数据后清空 |
| 统计 | `StatsGet` | 获取运行统计 |

### 返回码约定

| 码 | 含义 |
|----|------|
| `0` | 成功（Put 未丢弃） |
| `-1` | 参数无效或未初始化 |
| `>0` | 仅 Put：本次丢弃的最老数据条数（1） |

---

## 五、三种窗口访问方式

| 方式 | 函数 | 拷贝 | 回绕 | 调用方 |
|------|------|------|------|--------|
| **快照拷贝** | `Snapshot(buf,max)` | memcpy 到用户 buf | 合并两段 | 用户主动 |
| **锁内零拷贝** | `ForEach(cb,ctx)` | 零拷贝（data 指向内部） | 顺序遍历 | 用户主动 |
| **入队回调** | `SetPutCallback` | 零拷贝（view 指针数组） | 分段回调 | Put 后自动 |

- Snapshot 适合**中值滤波**（需排序/随机访问，拷贝到连续数组后锁外处理）
- ForEach 适合**移动平均**（顺序聚合，锁内零拷贝）
- SetPutCallback 适合**入队即处理**（每条 Put 后自动触发，view 指针数组零拷贝）
- 三种方式**可配合**（如回调消费大部分 + Snapshot 取零头）

---

## 六、快速上手

### 中值滤波（ForEach 累积法）

```c
#include "WindowQueue.h"

typedef struct { int ts; float value; } Sensor;

/* ForEach 回调：收集 value 到数组 */
void collect(const void *data, int index, void *ctx) {
    ((double*)ctx)[index] = ((const Sensor*)data)->value;
}

/* 处理线程：周期性滤波 */
void process(T_WindowQueueMsg *q) {
    double vals[8];
    while (running) {
        int n = WindowQueueAPI_ForEach(q, collect, vals);  /* 锁内零拷贝收集 */
        if (n > 0) {
            /* 对 vals[0..n-1] 排序取中值（中值滤波） */
            qsort(vals, n, sizeof(double), cmp);
            double median = vals[n / 2];
        }
        usleep(10000);  /* 10ms 处理周期 */
    }
}
```

### 采集 + 入队回调

```c
void on_put(const void * const *entries, int count, void *ctx) {
    /* entries[i] 指向窗口第 i 条（老→新），零拷贝 */
    double sum = 0;
    for (int i = 0; i < count; i++)
        sum += ((const Sensor*)entries[i])->value;
    /* 可做均值/阈值判断等轻量处理 */
}

T_WindowQueueMsg *q;
WindowQueueAPI_Init(&q, 8, sizeof(Sensor), "sensor");
WindowQueueAPI_SetPutCallback(q, on_put, NULL);

Sensor s = { .ts = 0, .value = 3.14f };
WindowQueueAPI_Put(q, &s);   /* 满则丢最老；入队后自动触发 on_put */
```

---

## 七、构建系统

```bash
cd debug
make all         # 静态库 + 动态库 + 测试程序
make clean
./WindowQueue_DebugPro.bin
```

---

## 八、变更记录

### 2026-07-09：创建（V1.0.0）

值拷贝环形缓冲区 + 三种窗口访问 + 入队回调 + 动态 Resize + 运行统计。

### 2026-07-10：初审修复（AI审查.md）

| # | 问题 | 修复 |
|---|------|------|
| P1 | Resize 缩容丢弃计入 `ulTotalDiscarded`（丢包率失真） | 删除该行，加注释"缩容非丢包" |
| P2-3 | Resize 持锁长操作无提示 | 头文件加"O(n) 阻塞操作，勿在高频采集期频繁调用" |
| P3-2 | Snapshot 缓冲说明"返回值×element_size"（调用前未知） | 改为"max_count×element_size" |

### 2026-07-10：二审修复（AI审查_V2.md）

| # | 问题 | 修复 |
|---|------|------|
| 新-P1 | 32 位平台 `queueLen×elementSize` 乘法溢出（堆越界风险） | Init + Resize 加 `SIZE_MAX` 溢出守卫（`#include <stdint.h>`） |
| 新-P2 | 构建产物（.a/.so/.bin/ProjectInfo）入库 | 建议 `git rm --cached` + `.gitignore`（git 管理范畴） |

**未改（审查确认保持现状）**：
- wq_set_name 风格 / mutex 返回值 / init_done 锁外读 / printf 日志 — 与现有库一致
- Destroy UAF 警告 / 半初始化未置 NULL — @warning 文档兜底
- main.c volatile double 跨线程 — 测试代码，演示场景可接受

**审查结论**：*"逻辑层面经两轮推演均无致命缺陷，可发布。"*
