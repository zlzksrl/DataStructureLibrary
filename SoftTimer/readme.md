# SoftTimer — 用户态软件定时器（V1.0.0）

> 目标平台：IMX6ULL (ARM Linux) · 语言：C99 · 依赖：`ThreadManage`
> 状态：**V1.0.0 已完成 4 轮独立 AI 审查**，具备生产部署条件

## 这是什么

一个基于 pthread 的软件定时器库：调度线程 `cond_timedwait(CLOCK_MONOTONIC)` 睡到堆顶 deadline，到点后把回调投递给线程池并发执行。计时与执行分离，支持单次/周期、任意状态安全删除（含 cb 内自删、含 cb 运行中他线程删），毫秒级精度。

**它不是**：
- 硬实时定时器（IMX6ULL 上典型 1–10ms 抖动，微秒级请另寻方案 —— 详见 `设计探讨.md §1`）
- 内核级 timer（保持用户态实现，与 pthread 模型统一 —— 详见 `设计探讨.md §3`）

## 目录结构

```
SoftTimer/
├── include/SoftTimer.h            # 公共 API（唯一对外头文件）
├── src/
│   ├── SoftTimer_Main.h           # 内部结构与状态机（含 ASCII 状态迁移图）
│   ├── SoftTimer.c                # 核心实现 ~1300 行
│   └── SoftTimer_Maketime.h       # 版本时间戳（构建时生成）
├── debug/
│   ├── main.c                     # Part 1-8 共 9 段测试用例
│   └── Makefile                   # arm-linux-gnueabihf 交叉编译
├── readme.md                      # 本文件
├── 需求文档.md                    # V1.0 需求规格 + 设计决策 D1-D13
├── 实现方案.md                    # 数据结构 + 调度伪码 + 锁分析 + 测试矩阵
├── 设计探讨.md                    # 精度 / 优先级 / 内核替代方案 三个工程问题
└── AI审查结果.md                  # 一~四轮审查过程与修复记录
```

## 快速开始

```c
#include "SoftTimer.h"

static void on_tick(T_SoftTimerHandle *h, void *ctx)
{
    (void)h; (void)ctx;
    /* 用户逻辑 */
}

int main(void)
{
    T_SoftTimerMgr    *mgr = NULL;
    T_SoftTimerHandle *h   = NULL;

    T_SoftTimerConfig cfg = {
        .sMgrName       = "demo",
        .iMinNum        = 2,
        .iMaxNum        = 4,
        .iQueueMaxSize  = 32,
        .iIdleTimeoutMs = 5000,
        .iSchedPriority = 0,      /* 0=SCHED_OTHER；1-99=SCHED_FIFO */
    };
    SoftTimerAPI_Init(&mgr, cfg);

    T_SoftTimerAlarm a = {
        .cb             = on_tick,
        .user_ctx       = NULL,
        .iFirstDelayMs  = 100,
        .iPeriodMs      = 1000,   /* 0=单次；>0=周期 */
        .ePeriodMode    = SOFTTIMER_PERIOD_FROM_END,
        .sTimerName     = "tick",
    };
    SoftTimerAPI_SetAlarm(mgr, a, &h);

    /* ... 业务运行 ... */

    SoftTimerAPI_Delete(mgr, &h);      /* h 被置 NULL */
    SoftTimerAPI_Destroy(&mgr);        /* mgr 被置 NULL */
    return 0;
}
```

## 阅读顺序（新读者）

1. 本 `readme.md` —— 一分钟建立总体印象
2. `include/SoftTimer.h` —— 看架构 ASCII 图 + quick-start + 5 个 API 契约
3. `src/SoftTimer_Main.h` —— 看状态迁移图 + 五条 Delete 路径对照
4. `src/SoftTimer.c` —— 顶部内存所有权表 + 调度线程/wrapper/Delete 三段决策表
5. `需求文档.md` —— 若想了解 D1-D13 设计决策的动机
6. `实现方案.md` —— 若要修改核心逻辑或做二次开发
7. `设计探讨.md` —— 精度/优先级/内核替代等工程问题
8. `AI审查结果.md` —— 若关心迭代历史与已知设计权衡（4 轮审查全记录）

## 五个 API

| API | 用途 |
|-----|------|
| `SoftTimerAPI_Init`      | 创建管理器（含线程池 + 调度线程）|
| `SoftTimerAPI_Destroy`   | 销毁管理器；扫尾所有 handle |
| `SoftTimerAPI_SetAlarm`  | 注册定时器；返回句柄 |
| `SoftTimerAPI_Delete`    | 删除定时器；任意状态可调（含 cb 内自删、含 cb 运行中他线程删）|
| `SoftTimerAPI_StatsGet`  | 读取运行统计快照 |

## 关键特性

- **非轮询**：调度线程在 `cond_timedwait` 里由内核挂起，零 CPU 占用
- **两种周期漂移**：`FROM_END`（间隔精确、无风暴）/ `FROM_SCHEDULED`（栅格精确、overrun 时 snap 跳过）
- **任意状态删除**：五条 Delete 路径覆盖墓碑/在堆/已投池/运行中(自删)/运行中(他线程)
- **二级指针**：Delete 成功后 `*ppt_Handle` 置 NULL，重复删自然返回 -1
- **多实例**：支持一个进程内多个独立 `T_SoftTimerMgr` 并存
- **单次墓碑语义**：单次 fire 后 node 自动释放，handle 保留成墓碑等 Delete 回收

## 精度定位

| 层级 | 精度 |
|------|------|
| API 表达 | 1 ms（`int iFirstDelayMs / iPeriodMs`）|
| 内部时间 | 纳秒（`struct timespec` + `CLOCK_MONOTONIC`）|
| IMX6ULL 常态 | 1–10 ms 抖动（受内核 tick 与调度延迟影响）|
| 池满时下限 | 10 ms（`ST_POOL_ADDTASK_RETRY_MS` 退避）|
| 建议最短周期 | FROM_END 10ms / FROM_SCHEDULED 20ms |

**升配三件套**（若要 <5ms 周期）：`iSchedPriority > 0` + worker 优先级透传（V1.1）+ mutex `PTHREAD_PRIO_INHERIT`（V1.1）。详见 `设计探讨.md §2`。

## 构建

```bash
cd debug/
make          # arm-linux-gnueabihf-gcc 交叉编译
./main        # 运行 9 段测试用例
```

编译时可选宏：
- `-DST_LOG_LEVEL=0..3` 日志级别（0 静默 / 1 只 ERROR / 2 ERROR+WARN / **3 全部（默认）**）
- `-DST_DEBUG_DISPATCH_DELAY_MS=200` 拉长 DISPATCHED 窗口，供 Path C 测试用（生产为 0）

## 错误码

| 码 | 含义 |
|----|------|
|  0 | 成功 |
| -1 | 参数无效或未初始化（含重复删除时 `*ppt_Handle==NULL`）|
| -2 | 管理器正在销毁 / 已销毁 |
| -3 | 句柄非本管理器 / 已失效 |
| -4 | 资源不足（内存分配失败等，仅 SetAlarm 返回）|

## 版本历史

- **V1.0.0** (2026-07-14) — 首版发布，经 4 轮独立 AI 审查
  - 一轮：基础正确性与账目一致性（C1-C5, M1-M9, L1-L8）
  - 二轮：边界条件与文档漂移（C6, M10-M14, L9-L15）
  - 三轮：并发 UAF 修复与文档精化（C7, M15, L16-L20）
  - 四轮：全面复核，无新问题，达到生产交付标准

未来演进（V1.1+）方向见 `实现方案.md §十四`。
