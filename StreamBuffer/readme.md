# StreamBuffer 流缓冲区（通用字节流环形缓冲库）

> **项目版本**: V1.0.0 | **作者**: zlzksrl | **许可证**: AGPL-3.0  
> **目标平台**: IMX6ULL (ARM Linux) | **语言**: C (C99)  
> **命名规范**: `StreamBufferAPI_*`  
> **创建日期**: 2026-07-09

---

## 目录

- [一、项目概述](#一项目概述)
- [二、项目结构](#二项目结构)
- [三、架构设计](#三架构设计)
- [四、API 参考](#四api-参考)
- [五、三种消费方式](#五三种消费方式)
- [六、快速上手](#六快速上手)
- [七、优雅关闭与 Reopen](#七优雅关闭与-reopen)
- [八、构建系统](#八构建系统)
- [九、设计决策](#九设计决策)
- [十、验证](#十验证)
- [十一、变更记录](#十一变更记录)
- [许可证](#许可证)

---

## 一、项目概述

StreamBuffer 是一个**通用的字节流环形缓冲库**，核心模式为 **"生产者攒字节流 + 消费者按阈值/超时批量出队"**。

库**不创建消费线程**：线程由用户自行创建与调度（线程数、优先级、消费方式完全由用户决定）。库只提供 **缓冲 + 触发等待 + 出队** 三类原语。

### 核心特性

- **字节流环形缓冲**：预分配连续内存，变长字节紧凑存储，环形复用，**零 malloc**
- **写入不阻塞**：缓冲满则丢弃本次写入（满则丢新），立即返回
- **三种消费方式**：`GetData`(拷贝) / `GetDataAddress`(零拷贝逐段) / 回调(零拷贝自动)
- **优雅关闭**：`Close()` 阻止写入并 broadcast 唤醒；可 `Reopen()` 恢复（Close 可逆）
- **触发灵活**：`used≥阈值` / 超时 / Flush / Close，唤醒策略 signal/broadcast 分明
- **多线程安全**：`pthread_mutex_t` + `pthread_cond_t`
- **运行统计**：写入/丢弃/消费字节数 + 峰值

### 典型应用

- **异步日志写文件**：业务线程格式化日志入队，消费线程批量 `fwrite`（减小磁盘磨损）
- **二进制文件/数据流写入**：传感器原始帧、结构体流、图像分片等入队，批量写 `.bin`（缓冲区是 `unsigned char` 字节流，不区分文本/二进制）
- **串口/网络攒发**：攒一批字节一次 `write`/`send`，减少系统调用
- **任何"攒字节流 + 定量/定时批量处理"场景**

---

## 二、项目结构

```
StreamBuffer/
├── include/StreamBuffer.h         # 公共 API（opaque pointer + extern "C" + Doxygen）
├── src/
│   ├── StreamBuffer_Main.h        # 内部结构体 T_STREAMBUFFER 定义
│   ├── StreamBuffer.c             # 核心实现（环形缓冲 + Wait/消费/Close/Reopen）
│   └── StreamBuffer_Maketime.h    # Makefile 自动生成的版本时间戳（不手写）
├── debug/
│   ├── main.c                     # 演示（4 Part：GetData/回调/GetDataAddress/Reopen）
│   └── Makefile                   # arm 交叉编译（.a/.so + 测试程序）
├── 需求文档.md                    # 需求规格 + 设计决策（D1~D10）
└── readme.md                      # 本文件
```

---

## 三、架构设计

### 3.1 数据流

```
业务线程(多个)                        用户消费线程(自建, 优先级自定)
   │ PutData(变长字节)                    │
   ▼                                     │
 ──→ [字节流环形缓冲]                     │
     (预分配, 变长紧凑,                    │  Wait(等 used≥阈值/超时/关闭)
      环形复用, 零malloc)                 │  GetData / GetDataAddress / 回调 出队
                                         ▼
                                  用户自行消费(fwrite / send / write 串口…)
```

### 3.2 字节流环形缓冲区

容量强制为 **2 的幂**，用 `mask = capacity-1` 做 `&` 高效回绕。

```
缓冲区示意 (capacity=8, read=2, write=5, used=3):
  [0][1][2][3][4][5][6][7]
        ^read        ^write
        |--数据(3)--| 空闲(5)

满: used == capacity；空: used == 0
read/write 推进用 & mask 回绕；跨回绕读/写分两段处理
```

- **写入**：紧凑追加到 `write`；跨回绕边界时分两段 `memcpy`（write 到尾 + 头到剩余），保证变长紧凑、零浪费。
- **读取**：`GetData` 合并回绕两段为连续输出；`GetDataAddress` 只给本段（不合并，零拷贝）。

### 3.3 为什么容量必须是 2 的幂

环形缓冲的 read/write 到达末尾要回到开头（"回绕"）。**2 的幂让回绕能用位运算 `&` 替代除法 `%`，快很多**：

```c
pos = (pos + n) & mask;   /* mask = capacity - 1，要求 capacity 为 2 的幂 */
```

**原理**：2 的幂二进制是"单个 1 + 一串 0"，减 1 变成"全 1 掩码"，此时 `x & mask`（保留低位）== `x % capacity`：

| capacity | mask = capacity-1 |
|----------|-------------------|
| 8 (`1000`) | 7 (`0111`) |
| 16 (`10000`) | 15 (`01111`) |
| 65536 | 65535 (`0111111111111111`) |

例 capacity=8（mask=7）：
```
9  & 7 = 1001 & 0111 = 0001 = 1   （9  % 8 = 1，相同）
10 & 7 = 1010 & 0111 = 0010 = 2   （10 % 8 = 2，相同）
```

- **为什么快**：`&` 是 1 条指令 / 1 周期；`%` 是除法，CPU 几十周期（ARM Cortex-A7 尤其慢）。环形缓冲每次读写都回绕，高频场景累积省下大量 CPU。
- **为什么必须**："`& mask` 等价 `% capacity`"只在容量为 2 的幂时成立。非 2 的幂（如 5000）mask 非全 1，`&` 会算错回绕位置 → 越界/数据错乱，故 Init 强制校验、非 2 的幂直接报错。
- **代价 vs 收益**：容量只能选 8/16/.../65536 等（实际缓冲大小本就取整，够用），换来高频回绕性能。这是环形缓冲的经典优化（Linux kfifo、DPDK ring、freeRTOS StreamBuffer 都如此）。

> 若未来需要任意容量：把实现里的 `& mask` 改回 `% capacity` 并去掉 2 的幂校验即可，代价是回绕变慢。

### 3.4 状态机

```
未初始化 ──Init()──→ 运行中 ──Close()──→ 已关闭 ──Reopen()──→ 运行中
                                  │
                                  └──Destroy()──→ 已销毁
```

---

## 四、API 参考

### 4.1 类型定义

```c
/* 句柄（opaque） */
typedef struct T_STREAMBUFFER T_StreamBuffer;

/* 状态码：统一用于 Wait 返回值与回调 status（>0 一律有数据） */
typedef enum {
    STREAMBUFFER_STATUS_CLOSE_DATA    = 3,   /* 关闭，但有数据 */
    STREAMBUFFER_STATUS_TRIGGER       = 2,   /* 达阈值/被唤醒，有数据 */
    STREAMBUFFER_STATUS_TIMEOUT_DATA  = 1,   /* 超时，但有数据 */
    STREAMBUFFER_STATUS_TIMEOUT_EMPTY = 0,   /* 超时，无数据 */
    STREAMBUFFER_STATUS_FLUSH_EMPTY   = -1,  /* 被 Flush 唤醒，无数据 */
    STREAMBUFFER_STATUS_CLOSE_EMPTY   = -2,  /* 关闭，无数据 */
    STREAMBUFFER_STATUS_INVALID       = -3,  /* 参数无效 */
    STREAMBUFFER_STATUS_NOINIT        = -4   /* 未初始化/已销毁 */
} StreamBufferStatus;

/* 零拷贝回调（仅 Wait 返回 >0 时触发） */
typedef int (*StreamBufferConsumeCb)(StreamBufferStatus status,
                                     const char *data, int len, void *user_ctx);

/* 配置 */
typedef struct { int iCapacity; int iFlushBytes; } T_StreamBufferConfig;

/* 统计 */
typedef struct {
    unsigned long ulTotalPut, ulDropped, ulConsumed;
    int iPeakUsed;
} T_StreamBufferStats;
```

### 4.2 API 一览（13 个）

| 类别 | 函数 | 说明 |
|------|------|------|
| 生命周期 | `Init` | 初始化（校验 capacity 为 2 的幂、阈值≤容量） |
| | `Destroy` | 销毁释放（幂等：*pp==NULL 返回 -1） |
| | `Close` | 关闭阻止写入 + broadcast 唤醒（幂等） |
| | `Reopen` | 重开（Close 可逆，不清空） |
| | `IsClosed` | 查询关闭状态 |
| 写入 | `PutData` | 写字节流（不阻塞，满则丢新，跨回绕两段 memcpy） |
| 消费 | `Wait` | 阻塞等触发（7 种状态码；注册回调则自动零拷贝消费+状态重算） |
| | `GetData` | 拷贝式出队（合并回绕两段） |
| | `GetDataAddress` | 零拷贝出队（本段地址，不合并回绕） |
| | `Flush` | signal 唤醒一个等待者 |
| | `SetConsumeCallback` | 注册/取消零拷贝回调 |
| 查询 | `GetLength` | 当前未消费字节数 used |
| | `StatsGet` | 运行统计 |

### 4.3 返回码约定

| 函数 | 返回值 |
|------|--------|
| `Init`/`Destroy`/`Close`/`Reopen`/`Flush`/`SetConsumeCallback`/`StatsGet` | `0` 成功 / `-1` 失败 |
| `PutData` | `>=0` 写入字节 / `-1` 无效 / `-2` 已关闭 / `-3` 满则丢新 |
| `Wait` | 见 `StreamBufferStatus` 枚举（`>0` 有数据 / `<=0` 无数据或错误） |
| `GetData`/`GetDataAddress` | `>0` 取出/消费字节 / `0` 无数据 / `-1` 无效 |
| `IsClosed` | `1` 已关闭 / `0` 未关闭 / `-1` 无效 |
| `GetLength` | `>=0` 字节数 / `-1` 无效 |

---

## 五、三种消费方式

| 方式 | 函数 | 拷贝 | 回绕 | 调用方 | 适用 |
|------|------|------|------|--------|------|
| **A 拷贝式** | `GetData(buf,max)` | memcpy 到用户 buf | 合并两段 | 用户主动 | **多消费者安全**、通用 |
| **A' 零拷贝逐段** | `GetDataAddress(&ptr,max)` | 零拷贝（输出地址） | 不合并，只本段 | 用户主动 | 单消费者、零拷贝 |
| **B 回调式** | `SetConsumeCallback`+`Wait` | 零拷贝 | 分两次回调 | Wait 内触发 | 单消费者、自动消费 |

- 三种方式**设计上支持配合**（如回调消费大部分 + GetData 取零头），**实际应用通常只用一种**。
- **回调用法**：`Wait` 返回 `>0` 时，先回调消费（回绕分两次、返回值钳位 [0,len]），按消费量偏移推进 read，再按"剩余 used + 关闭状态"重算返回码。
- **多消费者请用 `GetData`**（拷贝式）；回调/GetDataAddress 返回内部地址，不建议多线程并发持有。

### 消费循环（GetData 式）

```c
while (1) {
    int used, r = StreamBufferAPI_Wait(sb, 1000, &used);  /* 等/超时/关闭 */
    if (r > 0)                                            /* 有数据 */
        while (StreamBufferAPI_GetData(sb, buf, sizeof(buf)) > 0)
            fwrite(buf, 1, n, fp);
    if (r <= -2) break;                                   /* 关闭空/错误，退出 */
}
```

---

## 六、快速上手

### 异步日志写文件（GetData 拷贝式）

```c
#include "StreamBuffer.h"

/* 生产者线程：格式化日志入队 */
void *producer(void *arg) {
    T_StreamBuffer *sb = arg;
    char line[128];
    for (int i = 0; i < N; i++) {
        int n = snprintf(line, sizeof(line), "ts=%d,val=%d\r\n", i, i*2);
        StreamBufferAPI_PutData(sb, line, n);   /* 不阻塞，满则丢新 */
    }
    return NULL;
}

/* 消费者线程：批量写文件 */
void *consumer(void *arg) {
    T_StreamBuffer *sb = arg;
    char buf[8192];
    int used;
    while (1) {
        int r = StreamBufferAPI_Wait(sb, 1000, &used);   /* 等1s/阈值/关闭 */
        if (r > 0)
            while (StreamBufferAPI_GetData(sb, buf, sizeof(buf)) > 0)
                fwrite(buf, 1, n, fp);
        if (r <= -2) break;                              /* 关闭排空，退出 */
    }
    return NULL;
}

int main(void) {
    T_StreamBufferConfig cfg = { 65536, 4096 };   /* 容量64K(2的幂), 阈值4K */
    T_StreamBuffer *sb = NULL;
    StreamBufferAPI_Init(&sb, &cfg, "logbuf");

    FILE *fp = fopen("log.csv", "w");
    pthread_create(&tid_p, NULL, producer, sb);
    pthread_create(&tid_c, NULL, consumer, sb);

    /* ... 业务运行 ... */

    pthread_join(tid_p, NULL);
    StreamBufferAPI_Close(sb);        /* 阻止写入 + broadcast 唤醒 */
    pthread_join(tid_c, NULL);        /* 消费者取空剩余后退出 */
    fclose(fp);
    StreamBufferAPI_Destroy(&sb);
    return 0;
}
```

### 零拷贝回调式（替代 GetData）

```c
int my_cb(StreamBufferStatus st, const char *data, int len, void *ctx) {
    fwrite(data, 1, len, (FILE *)ctx);   /* 零拷贝：直接 fwrite 内部地址 */
    return len;                          /* 全消费 */
}
StreamBufferAPI_SetConsumeCallback(sb, my_cb, fp);
/* 之后 Wait 返回 >0 时自动调 my_cb 消费 */
```

### 零拷贝逐段式（GetDataAddress）

```c
char *ptr;
int n = StreamBufferAPI_GetDataAddress(sb, &ptr, 4096);  /* 本段地址，回绕不合并 */
if (n > 0) fwrite(ptr, 1, n, fp);   /* 须立即使用，不得持有 */
```

### 二进制写 bin 文件（定长结构体流）

库缓冲区是 `unsigned char` 字节流，**不解释内容**，写二进制与写文本完全一样——只是 `fopen` 用 `"wb"`、`fwrite` 原样落盘。注意字节流不维护记录边界，定长数据按 `sizeof` 对齐即可：

```c
typedef struct { int ts; short val[4]; } Frame;   /* 定长二进制帧 */

/* 生产者：写入一帧（二进制，原样入队） */
Frame f = { .ts = t };
StreamBufferAPI_PutData(sb, (const char *)&f, sizeof(f));

/* 消费者：取出字节，fwrite 到 bin 文件 */
FILE *fp = fopen("data.bin", "wb");
char buf[8192];
int used;
while (1) {
    int r = StreamBufferAPI_Wait(sb, 1000, &used);
    if (r > 0) {
        int n;
        while ((n = StreamBufferAPI_GetData(sb, buf, sizeof(buf))) > 0)
            fwrite(buf, 1, n, fp);          /* 二进制写，原样落盘 */
    }
    if (r <= -2) break;
}
fclose(fp);
```

> **变长二进制**需应用层自己界定边界（如长度前缀：先 Put 4 字节长度、再 Put 数据，消费端先读长度再读数据）。库只管搬运字节，不拆包。

---

## 七、优雅关闭与 Reopen

### 优雅关闭（保证数据完整落盘）

```
Close()              ← 阻止新写入(PutData 返回 -2) + broadcast 唤醒所有 Wait
    │                        (不清空缓冲，剩余仍可取出)
消费者线程：
   Wait 返回 CLOSE_DATA(3) → GetData/GetDataAddress 取数据
   → 取空后 Wait 返回 CLOSE_EMPTY(-2) → 退出
主线程：
   pthread_join(消费者) → Destroy()
```

- `Close` 内部已 `broadcast`，**关闭排空无需额外 Flush**。
- `Close`/`Destroy` 幂等。

### Reopen（Close 可逆）

```c
StreamBufferAPI_Close(sb);
StreamBufferAPI_PutData(sb, ...);   /* 返回 -2（已关闭） */
StreamBufferAPI_Reopen(sb);         /* 重置关闭标志，保留缓冲数据 */
StreamBufferAPI_PutData(sb, ...);   /* 返回 >=0（恢复写入） */
```

---

## 八、构建系统

### 工具链

- 编译器：`arm-linux-gnueabihf-gcc`，归档：`arm-linux-gnueabihf-ar`
- 线程库：`-pthread`

### 构建目标

```bash
cd debug
make all         # 静态库 + 动态库 + 测试程序
make slib        # 仅 libStreamBuffer.a
make dlib        # 仅 libStreamBuffer.so
make app         # 仅测试程序 StreamBuffer_DebugPro.bin
make clean
make install     # scp 部署到目标板
make install_lib # 安装库到本地公共路径
```

首次 `make` 会自动生成 `src/StreamBuffer_Maketime.h`（版本时间戳）。

---

## 九、设计决策

| 编号 | 决策 | 选定 |
|------|------|------|
| D1 | 缓冲满策略 | 有界 + 满则丢新（不阻塞，统计丢弃数） |
| D2 | 内存模型 | 字节流环形缓冲（预分配、变长紧凑、环形复用、零 malloc） |
| D3 | 触发与唤醒 | used≥阈值/超时/Close；PutData/Flush signal，Close broadcast；timeo=0 立即返回 |
| D4 | 消费方式 | GetData(拷贝) / GetDataAddress(零拷贝逐段) / 回调(零拷贝自动)，可配合 |
| D5 | 消费线程 | 用户创建（库不创建线程） |
| D6 | 库名/前缀 | StreamBuffer / `StreamBufferAPI_*` |
| D7 | 关闭可逆 | Close 可逆，提供 Reopen |
| D8 | 容量约束 | capacity 强制 2 的幂（mask 回绕）；flush_bytes ≤ capacity，Init 校验 |
| D9 | 多消费者 | 建议单消费者；多消费者用 GetData 拷贝式 |
| D10 | 回调返回值 | 钳位到 [0,len]（越界警告）；Close/Destroy 幂等 |

详见 `需求文档.md`。

---

## 十、验证

### x86 + arm 板均通过（0 错误 0 警告）

| Part | 验证项 | 结果 |
|------|--------|------|
| 1 | GetData 拷贝式（生产 300 条 + 写文件 + 优雅关闭） | put=5235 = consumed（无丢失），peak=1040，消费者 r=-2 排空退出 ✓ |
| 2 | 回调零拷贝 | consumed=690，final r=-2 used=0 ✓ |
| 3 | GetDataAddress 逐段 | 回绕分两段 4096+4096=8192 ✓ |
| 4 | Reopen | close→-2，reopen→PutData 恢复 ✓ |
| 校验 | 非 2 的幂 Init | ret=-1（拒绝）✓ |

### 落盘数据完整性

- `demo1_output.csv` 末行 `ts=299,value=598` → 300 条全部落盘
- `demo2_output.csv` 末行 `cb=99` → 100 条全部落盘

端到端（PutData → 缓冲 → 出队 → fwrite）数据完整，无丢条无截断。

---

## 十一、变更记录

### 2026-07-09：按 AI 代码审查修复（见 `AI审查.md`）

| 级别 | 修改 | 说明 |
|------|------|------|
| 🔴 P0 必修 | `Wait` 超时 deadline 移到 while 循环外（循环内复用） | 修复"deadline 反复刷新→周期 Flush + 零散数据下消费者永不超时"的条件变量经典反模式 |
| 🟠 P1 | `PutData` 的 `dropped` 统计改记**原始 len**（非截断后） | 避免 Put 大段（len>capacity）时统计失真 |
| 🟠 P1 | `Wait` 入口校验 `timeo<0` → 返回 `INVALID` | 负超时不再被静默当作立即返回 |
| 🟡 P2 | cond 改用 `CLOCK_MONOTONIC`（`pthread_condattr_setclock` + `clock_gettime`） | 超时不受 NTP/手动改时跳变影响 |
| ⚪ P3 | 删除 `Destroy` 中冗余的 `init_done=0` | free 前置零无意义 |

**未采纳（保持现状）**：回调消费空后 status 降级为 `TIMEOUT_EMPTY`（已确认的重算规则，文档已说明）、`printf` 日志与 `init_done` 锁外读（与 ThreadQueue/WindowQueue 库保持一致）。

**验证**：x86 gcc 编译 0 错误 0 警告，运行 `put==consumed`（无丢失）、三种消费/Reopen/Init 校验全部正常，无回归；`CLOCK_MONOTONIC` 在 x86 也通过。建议 arm 板重新 `make` 验证。

---

## 许可证

本项目采用 [GNU Affero General Public License v3.0](LICENSE)。

---

> StreamBuffer 是一个面向嵌入式 ARM Linux 的通用字节流环形缓冲库，适用于异步日志、串口/网络攒发等"攒数据 + 批量处理"场景。零 malloc、写入不阻塞、三种消费方式、优雅关闭，已在 IMX6ULL 平台验证。
