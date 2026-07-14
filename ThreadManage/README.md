# ThreadManage — 线程 + 线程池管理库（V1.1.0）

> 目标平台：IMX6ULL (ARM Linux) · 语言：C99 · 依赖：`pthread` + `ThreadQueue` + `RedBlackTree` + `KernelLinkedList`
> 状态：**V1.1.0 已完成 2 轮独立 AI 审查**（详见 `AI审查结果.md`），生产可用

## 这是什么

一套建立在 pthread 之上的用户态线程管理库，两大能力：

1. **单线程创建**：三级渐进式属性配置（`eSetAttr` 0/1/2），覆盖默认、栈大小、完整（分离状态 / 继承策略 / 调度策略 / 优先级）三档。参数越界自动修正为安全值。
2. **线程池**：最小/最大范围内动态扩缩容 + 空闲超时自缩 + 3 种任务提交（阻塞 / 带超时 / 非阻塞）+ 运行时 Resize + 统计快照。魔术字防重复销毁，`iActiveCallers` + `iLiveThreadNum` 双计数防 UAF。

**它不是**：
- 内核实时线程调度器（SCHED_FIFO/RR 可用，但优先级反转、CPU 亲和绑核等未内建）
- 分布式或跨进程任务队列（进程内线程池，`ThreadQueue` 为进程内环形缓冲）

## 目录结构

```
ThreadManage/
├── include/ThreadManage.h        # 公共 API（唯一对外头文件）
├── src/
│   ├── ThreadManage_Main.h       # 内部：T_ThreadPoolHandle 完整定义
│   ├── ThreadManage_Main.c       # 单线程创建 + 属性/优先级/打印 API
│   ├── ThreadManage_Pool.h       # 内部：T_PoolTask / T_WorkerNode
│   ├── ThreadManage_Pool.c       # 线程池核心 ~1500 行
│   └── ThreadManage_Maketime.h   # 版本时间戳（构建时生成）
├── debug/
│   ├── main.c                    # 三大测试段：ThreadCreate / ThreadPool / NewFeatures
│   └── Makefile                  # arm-linux-gnueabihf 交叉编译，产出静态/动态库 + 测试 bin
├── LICENSE
├── readme.md                     # 本文件
└── AI审查结果.md                 # 一~二轮审查记录（含修复落地表）
```

## 快速开始

### 场景 A：创建一个单线程

```c
#include "ThreadManage.h"

static void *worker(void *arg)
{
    (void)arg;
    /* 用户逻辑 */
    return NULL;
}

int main(void)
{
    T_ThreadCreateConfig cfg = {0};
    cfg.pThreadFunc        = worker;
    cfg.pThreadFuncUserArg = NULL;
    cfg.sThreadName        = "my_worker";
    cfg.eSetAttr           = 2;                     /* 完整属性 */
    cfg.istacksize_MB      = 4;
    cfg.eDetachState       = PTHREAD_CREATE_DETACHED;
    cfg.einheritsched      = PTHREAD_EXPLICIT_SCHED;
    cfg.eSchedPolicy       = SCHED_RR;
    cfg.iSchedPriority     = 50;

    ThreadAPI_ThreadCreate(&cfg);                   /* cfg.tThreadPid 被回填 */
    return 0;
}
```

### 场景 B：创建一个线程池并提交任务

```c
#include "ThreadManage.h"

static void *task(void *arg)
{
    int id = *(int *)arg;
    /* 用户业务 */
    (void)id;
    return NULL;
}

int main(void)
{
    T_ThreadPoolConfig cfg = {
        .iMinNum        = 2,        /* 最小 2 线程 */
        .iMaxNum        = 8,        /* 峰值可扩到 8 线程 */
        .iQueueMaxSize  = 64,       /* 队列容量 */
        .iIdleTimeoutMs = 5000,     /* 5s 空闲则缩回 iMinNum；0=禁用缩容 */
    };
    T_ThreadPoolHandle *pool = ThreadAPI_ThreadPoolCreate(cfg);

    int ids[100];
    for (int i = 0; i < 100; i++)
    {
        ids[i] = i;
        ThreadAPI_ThreadPoolAddTask(pool, task, &ids[i]);   /* 阻塞版本 */
        /* 或： ThreadAPI_ThreadPoolAddTaskTry(pool, task, &ids[i]);          非阻塞 */
        /* 或： ThreadAPI_ThreadPoolAddTaskTimeout(pool, task, &ids[i], 100); 100ms 超时 */
    }

    ThreadAPI_ThreadPoolWaitAllDone(pool, 30000);   /* 30s 内等待所有任务完成 */
    ThreadAPI_ThreadPoolDestroy(pool);              /* 会等待所有 in-flight 任务收尾 */
    return 0;
}
```

## 阅读顺序（新读者）

1. 本 `readme.md` —— 一分钟建立总体印象
2. `include/ThreadManage.h` —— 看 API 一览 + `T_ThreadCreateConfig` 三档属性开关 + `T_ThreadPoolConfig`/`T_ThreadPoolStats`
3. `src/ThreadManage_Main.h` —— 看 `T_ThreadPoolHandle` 完整字段（并发保护约定）
4. `src/ThreadManage_Pool.h` —— 看 `T_WorkerNode`（含 `tRbNode`/`tBusyEntry` 侵入式节点）
5. `src/ThreadManage_Pool.c` —— 顶部读 `Validate` / `Destroy` / `CreateWorker` 三段
6. `debug/main.c` —— 三大测试段展示典型用法与并发验证方式
7. `AI审查结果.md` —— 关心已知问题与迭代历史，尤其"修复落地记录"

## API 一览

### 单线程

| API | 用途 |
|-----|------|
| `ThreadAPI_ThreadCreate`             | 按 `T_ThreadCreateConfig` 创建线程（三档属性） |
| `ThreadAPI_SetThreadPolicyPriority`  | 修改当前线程的调度策略 + 优先级 |
| `ThreadAPI_SetThreadPriority`        | 保留当前策略，仅改优先级 |
| `ThreadAPI_PrintThreadAttr`          | 打印指定线程的属性（调试用） |

### 线程池

| API | 用途 |
|-----|------|
| `ThreadAPI_ThreadPoolCreate`         | 创建线程池（含互斥锁 / cond / 队列 / 初始工作线程） |
| `ThreadAPI_ThreadPoolDestroy`        | 销毁线程池；等待 in-flight 任务与工作线程收尾 |
| `ThreadAPI_ThreadPoolAddTask`        | 阻塞版：队列满时阻塞等待 |
| `ThreadAPI_ThreadPoolAddTaskTimeout` | 带超时：队列满时最多等 N ms |
| `ThreadAPI_ThreadPoolAddTaskTry`     | 非阻塞：队列满立即返回 -2（内部 1ms 超时缓解 TOCTOU） |
| `ThreadAPI_ThreadPoolResize`         | 运行时调整 min/max；扩容立即，缩容标记空闲线程退出 |
| `ThreadAPI_ThreadPoolWaitAllDone`    | 等所有已入队任务完成（超时可选） |
| `ThreadAPI_ThreadPoolBusyThreadNumGet` | 查当前忙碌线程数 |
| `ThreadAPI_ThreadPoolLiveThreadNumGet` | 查当前存活线程数 |
| `ThreadAPI_ThreadPoolTaskQueueLenGet`  | 查当前队列待处理任务数 |
| `ThreadAPI_ThreadPoolStatsGet`       | 快照：累计提交/完成、峰值忙碌/队列长度 |

## 关键特性

- **渐进式属性配置**：`eSetAttr=0` 默认属性 / `=1` 仅栈大小 / `=2` 完整属性；越界参数自动修正为安全值（栈大小→8MB、非法分离态→JOINABLE、非实时策略→SCHED_OTHER+0）
- **多种任务提交语义**：阻塞 / 带超时 / 非阻塞，覆盖同步、限时同步、快速失败三种模式
- **动态扩缩容**：AddTask 触发扩容（`iBusyThreadNum + iTaskQueueLen >= iLiveThreadNum` 且未达 `iMaxNum`），`iIdleTimeoutMs` 触发空闲缩容至 `iMinNum`
- **运行时 Resize**：可在线调整 min/max，扩容立即创建，缩容标记空闲 worker 退出（忙碌 worker 完成当前任务后自行退出）
- **侵入式数据结构**：worker 节点内嵌红黑树节点（按 `pthread_t` 索引 O(log n)）+ 内核链表节点（忙碌链表 O(1)），零额外分配
- **CLOCK_MONOTONIC 超时**：`WaitAllDone` / `AddTaskTimeout` 的超时不受系统时间跳变（NTP、手改）影响
- **魔术字防护**：`THREADPOOL_MAGIC=0x5448504C` 防重复销毁 + 野指针访问；`Validate` 在每个 API 入口检查
- **UAF 防护**：`iActiveCallers` 计数在 AddTask 系列中递增，Destroy 等待归零后再释放同步原语与内存（**注**：查询类 API 不参与该计数，与 Destroy 并发时依赖 Validate→Lock 窗口极短的隐式保证，详见 `AI审查结果.md` R2-M2）

## 构建

```bash
cd debug/
make          # 交叉编译产出：libThreadManage.a、libThreadManage.so、ThreadManage_DebugPro.bin
./ThreadManage_DebugPro.bin
```

构建依赖：Makefile 通过 `LIBFILE_PATH` 变量指向公共库根目录（默认 `~/zlzksrl/LinuxARM/Program/DataStructureLibrary/Library/`），需要该路径下已安装 `KernelLinkedList`、`RedBlackTree`、`ThreadQueue` 的头文件与静态库。

其它目标：

| 目标 | 说明 |
|------|------|
| `make slib`         | 只出静态库 `libThreadManage.a` |
| `make dlib`         | 只出动态库 `libThreadManage.so`（`-fPIC`） |
| `make app`          | 只出测试可执行 `ThreadManage_DebugPro.bin` |
| `make clean`        | 清 `.o` 和产物 |
| `make install`      | `scp` 测试 bin 到 `INSTALLBOARD` 目标板 |
| `make install_lib`  | 安装库到 `INSTALL_PATH`（`StaticLib` + `SharedLib` + `include`） |

## 错误码

各 API 通用约定：

| 码 | 含义 |
|----|------|
|  0 或 >=0 | 成功 |
| -1 | 参数无效 / 未初始化 / 线程池已关闭 / 队列已满（部分 API） |
| -2 | 超时（`AddTaskTimeout`）或队列满（`AddTaskTry`） |

线程池创建失败返回 `NULL`（非负数码），查询类 API 失败返回 `-1`。

## 版本历史

- **V1.1.0** (2026-05-08 → 2026-07-14) — 累积 2 轮独立 AI 审查
  - 一轮 (2026-05-09)：全面检查功能/稳定性/安全性，共 3 中 4 低 3 信息（FUN-01/02/03/04/05, STA-01/02, SEC-01/02/03）
  - 二轮 (2026-07-14)：独立复审，补 1 项 UAF 竞态（R2-M1 Resize 扩容）+ 1 项文档表述修正（R2-M2 查询 API）+ 4 项风格/可维护性
- **V1.0.1** (2025-10-01) — 首版发布

已知问题与修复落地跟踪见 `AI审查结果.md`。
