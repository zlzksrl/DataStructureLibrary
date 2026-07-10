# MemoryPool 内存池（固定大小对象池）

> **项目版本**: V1.0.0 | **作者**: zlzksrl | **许可证**: AGPL-3.0  
> **目标平台**: IMX6ULL (ARM Linux) | **语言**: C (C99)  
> **命名规范**: `MemPoolAPI_*`  
> **创建日期**: 2026-07-10

---

## 一、项目概述

MemoryPool 是一个**固定大小对象的内存池/对象池**：Init 时预分配 N 个 `element_size` 槽位，提供 Alloc/Free（类似 malloc/free），槽位**循环复用**，运行时零 malloc、零碎片。主要配合 ThreadQueue 使用，消除"每条消息 malloc/free"的开销。

### 核心特性
- **三种池满策略**：DROP(返回NULL) / GROW(动态扩容) / BLOCK(阻塞等待) —— Init 配默认 + 三个 Alloc 接口显式选择
- **零 malloc 循环复用**：空闲链表 LIFO（内嵌 next），O(1) Alloc/Free
- **通用**：element_size 任意，槽位不解释内容（含 0x00 安全，适合任意 struct/union/二进制）
- **线程安全**：mutex 保护；BLOCK 模式用 cond（CLOCK_MONOTONIC）

### 与 malloc 的区别
每次 Alloc 返回**固定 element_size** 大小空间（无需传 size），Free 也无需 size。换来：无运行时 malloc/碎片 + 免 size 参数 + O(1)。

---

## 二、API 一览（10 个）

| 类别 | 函数 |
|------|------|
| 生命周期 | `Init` / `Destroy` |
| 分配 | `Alloc`(默认) / `AllocDrop` / `AllocGrow` / `AllocBlock(timeo)` |
| 释放 | `Free` |
| 查询 | `GetFreeCount` / `GetUsedCount` / `StatsGet` |

```c
typedef enum { MEMPOOL_MODE_DROP=0, MEMPOOL_MODE_GROW=1, MEMPOOL_MODE_BLOCK=2 } MemPoolMode;
typedef struct { int element_size, init_count; MemPoolMode mode; int grow_count, block_timeo; } T_MemPoolConfig;
```

---

## 三、三种池满策略

| 策略 | 池满时 | 适用 |
|------|--------|------|
| **DROP** | 返回 NULL | 可丢、生产者不能卡（日志/采集） |
| **GROW** | malloc 新 chunk 扩容 | 不能丢、内存可增长 |
| **BLOCK** | cond 阻塞等 Free 归还 | 不能丢、生产者可阻塞（指令/控制） |

---

## 四、测试结果

> 测试程序 `debug/main.c`（8 个 Part），x86 gcc + arm 板（IMX6ULL）均验证通过。

### 4.1 单线程基础（Part 1-3）

| Part | 模式 | 结果 | 验证 |
|------|------|------|------|
| 1 | DROP | alloc=8 drop=4 cap=8 | 满返回 NULL + 丢弃统计 ✓ |
| 2 | GROW | alloc=12 cap=12 grow=8 | 满动态扩容 ✓ |
| 3 | BLOCK | blocker got slot after woken | 满阻塞 + Free signal 唤醒 ✓ |

### 4.2 多线程 + ThreadQueue 三模式压测 10×1000（Part 4-6）

**arm 板（单核 Cortex-A7）结果：**

| Part | 模式 | alloc | free | drop | grow | cap | peak | 解读 |
|------|------|-------|------|------|------|-----|------|------|
| 4 | BLOCK | 10000 | 10000 | 0 | 0 | 8 | 8 | **不丢不扩**（满则阻塞），8 槽循环 10000 次 |
| 5 | GROW | 10000 | 10000 | 0 | 96 | 104 | 102 | 0 丢，扩容 96（init 8→104），peak=102 |
| 6 | DROP | 80 | 80 | 9920 | 0 | 8 | 8 | 0 扩，**丢 99%**（单核 producer 先跑满一轮） |

> **关键发现**：arm 单核上 DROP 丢 99%，因为 producer 先跑完一轮 1000 次（占满 8 槽后 992 丢弃），consumer 才有机会跑。BLOCK/GROW 不受影响（BLOCK 阻塞等、GROW 扩容让 consumer 有机会）。

### 4.3 init_count 扫描（Part 7-8，arm 单核）

找"不丢(DROP)/不扩(GROW)"的最小 init_count：

**DROP 扫描（找 drop=0）：**

| init | alloc | drop | peak | 判定 |
|------|-------|------|------|------|
| 8 | 8 | 992 | 8 | 丢 99% |
| 64 | 64 | 936 | 64 | 丢 |
| 256 | 256 | 744 | 256 | 丢 |
| 512 | 512 | 488 | 512 | 丢 |
| **1024** | 1000 | **0** | 1000 | **[OK no-drop]** |

**GROW 扫描（找 grow=0）：**

| init | grow | cap | peak | 判定 |
|------|------|-----|------|------|
| 8 | 792 | 800 | 798 | 扩 |
| 64 | 672 | 736 | 735 | 扩 |
| 512 | 488 | 1000 | 1000 | 扩 |
| **1024** | **0** | 1024 | 889 | **[OK no-grow]** |

### 4.4 arm vs x86 阈值对比

| 模式 | x86 多核不丢/不扩最小 init | **arm 单核** | BLOCK |
|------|--------------------------|------------|-------|
| DROP 不丢 | 16~64 | **1024** | **8** |
| GROW 不扩 | 64 | **1024** | **8** |

> **差异原因**：x86 多核 producer/consumer 并行，consumer 及时 Free，小 init 即够；arm 单核 producer 先跑满一轮（1000 条），需 init ≥ 一轮条数才不丢不扩。**BLOCK 模式任意 init 都不丢不扩**（满则阻塞，与 init 无关）。

### 4.5 性能调优建议

| 场景 | 推荐模式 + init | 理由 |
|------|----------------|------|
| **数据不能丢不能扩** | **BLOCK + init 8** | 满则阻塞等归还，不丢不扩，最省内存 |
| 可丢（如日志） | DROP + init 按可接受丢弃率 | init 越大丢越少；arm 单核需 init≥一轮条数才 0 丢 |
| 可扩（峰值不固定） | GROW + 小 init | 扩到峰值后稳定循环复用 |
| 高频配合队列 | BLOCK + init 8 | 8 槽位承载 10000 次流转，零每条 malloc（实测 alloc==free==10000） |

---

## 五、配合 ThreadQueue 的典型用法

```c
union Msg { TypeA a; TypeB b; };
T_MemPoolConfig cfg = { sizeof(union Msg), 8, MEMPOOL_MODE_BLOCK, 0, 0 };
T_MemPool *pool;  MemPoolAPI_Init(&pool, &cfg, "msgpool");

/* 生产者：从池取(无malloc) → 填数据 → 入队 */
union Msg *m = (union Msg *)MemPoolAPI_Alloc(pool);
m->a = ...;  ThreadQueueAPI_PutMsg(q, m);

/* 消费者：取队 → 用数据 → 归还池(无free) */
union Msg *m = (union Msg *)ThreadQueueAPI_GetMsg(q, 1000);
... 用 m ...;  MemPoolAPI_Free(pool, m);
```

> 实测：8 个槽位循环复用承载 10000 次消息流转，**零每条 malloc/free**，capacity 恒定 8，alloc==free==10000（无泄漏）。

---

## 六、构建

```bash
cd debug
make all         # 静态库 + 动态库 + 测试程序
./MemoryPool_DebugPro.bin
```

> 测试程序链接 ThreadQueue 库（Makefile 已配 `-I` / 静态链接 `libThreadQueue.a`）。

---

## 七、项目结构

```
MemoryPool/
├── include/MemoryPool.h         # 公共 API（10 API + MemPoolMode 枚举）
├── src/
│   ├── MemoryPool_Main.h        # 内部结构体（chunk 链表 + free_list 内嵌 next + mutex/cond）
│   ├── MemoryPool.c             # 实现（三模式 Alloc + LIFO Free + 扩容）
│   └── MemoryPool_Maketime.h    # Makefile 自动生成
├── debug/
│   ├── main.c                   # 8 Part 测试（单线程基础 + 多线程三模式 + init 扫描）
│   └── Makefile                 # arm 交叉编译
└── 需求文档.md                  # 完整需求（含 LIFO+mutex 讨论）
```

---

## 八、实现要点

- **空闲链表 LIFO**（头取头插）：空闲槽开头内嵌 next 指针，零额外内存，O(1) Alloc/Free，缓存热（重用刚释放槽）。
- **对齐**：槽位向上补齐到 `max_align_t`（union 实现，跨平台），保证可存任意类型。
- **GROW 多 chunk**：扩容的 chunk 串入同一 free_list，Destroy 时遍历释放。
- **BLOCK 同步**：mutex + cond（CLOCK_MONOTONIC），Free 时 signal 唤醒等待者。
- **Free 不校验指针**（像 malloc，错指针 UB，文档/函数头写清）；Alloc 返回未初始化（像 malloc）。

---

> MemoryPool 经需求→实现→x86/arm 验证→多线程压测→init 扫描调优完整闭环，三模式 + 配合队列循环复用 + 峰值监控全部验证可靠。

---

## 九、变更记录

### 2026-07-10：第一轮 AI 审查修复（见 `AI审查结果.md`）

| # | 问题 | 修复 |
|---|------|------|
| 1 | BLOCK 超时 stolen wakeup（ETIMEDOUT 后直接返回 NULL，未重判 free_list） | ETIMEDOUT 后持锁重判 `free_list`，仍空才返回 NULL，否则 break 走正常分配 |
| 2 | 乘法溢出（32 位 `count×align_size` 溢出→堆溢出） | 新增 `mp_size_overflow()`，Init/GROW 在 `malloc` 前校验 |
| 3 | GROW malloc 失败未计入统计 | 两处失败分支补 `total_drop++` |
| 4 | `ulTotalDrop` 语义模糊（DROP 丢弃 + BLOCK 超时混计） | 头文件注释说明复合语义 |
| 6 | Init 未校验 mode 范围 / 负 block_timeo | 加 mode 范围(0~2)校验 + block_timeo 负值处理 |
| 8 | Makefile CFLAGS 缺 `-pthread` | CFLAGS 补 `-pthread`（编译+链接都带） |
| 11 | 头文件对齐注释写 "max_align_t"（实现用 union 近似） | 改为"按平台最大常用类型" |

### 2026-07-10：第二轮审查修复（修复后复核）

| # | 问题 | 修复 |
|---|------|------|
| A | `block_timeo` 负值夹紧到 0 = 无限阻塞（静默挂起，且与 `AllocBlock` 显式接口不一致） | 改为 Init **拒绝**（负值返回 -1），统一"负值=失败" |
| 1 | `timedwait` 返回非 ETIMEDOUT 错误（EINVAL 等）时忙等空转 | 加 `else if(r != 0 && r != EINTR)` → `total_drop++` + 返回 NULL（EINTR 继续等，EINVAL 不忙等） |

**未改（审查确认保持现状）**：
- 问题 5（Destroy 未 broadcast）—— `@warning` 文档兜底，与 ThreadQueue/StreamBuffer 一致
- 问题 7（printf 日志）—— 与现有库一致
- 问题 9（clock_gettime 返回值）—— 修复 2 已断联动（EINVAL→返回 NULL 不忙等），Linux 保证不失败
- 问题 10/12-14 —— 防御性/风格，低优先

**审查结论**：*"可投入生产。核心并发与生命周期逻辑无致命缺陷。"*
