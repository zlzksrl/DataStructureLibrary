# FileWriter 代码审查报告（四轮）

> **审查范围**：`include/FileWriter.h` / `src/FileWriter_Main.h` / `src/FileWriter.c` / `debug/main.c` / `debug/Makefile`
> **审查日期**：2026-07-12
> **审查目标**：**新增"抗并发销毁"机制**的正确性 + 常规问题回归
> **对照基线**：`AI审查结果.md`（三轮，2026-07-12 上午）
> **总体结论**：抗并发销毁的整体思路是对的（两阶段释放 + 原子引用计数），**但 Phase B 的 store/load 分两步操作存在竞态**，可能引发**双重 `fw_final_free` → UAF/mutex 双销毁 / SB 双销毁 / 双 free**。必修 1 项、建议修 1 项、其他风格建议若干。

---

## 零、三轮修复项回归

| 三轮项 | 状态 | 说明 |
|---|---|---|
| R1 死代码参数 | ✅ | `fw_check_file_size_rotate_locked` 已删 `bytes_written` |
| R6 Stats API | ✅ | `FileWriterAPI_StatsGet` + `T_FileWriterStats` 落地 |
| R7 Flush 异步语义 | ✅ | 头文件 `@warning` 明确 |
| R5 Rotate 阻塞 | ✅ | 头文件 `@warning` 明确 |
| R4 查询接口锁内阻塞 | ✅ | 头文件查询区顶部 `@note` |
| R11 降级路径栈大小 | ✅ | `eSetAttr=1 + istacksize_MB=2` |
| R12 SB 实例名 | ✅ | `fw_<name>_sb` |
| — 新增：**抗并发销毁** | 🔴 | 见本轮 F1 |

三轮修复回归全部通过。本轮的问题主要出在**新增的抗并发销毁机制**里。

---

## 一、必修问题

### F1. 🔴🔴🔴 Phase B 的 `atomic_store(destroy_pending)` + `atomic_load(ref_count)` 分两步操作产生 UAF

**位置**：`FileWriter.c:1024-1044`

```c
/* B1. 置 destroy_pending=1（release） */
atomic_store_explicit(&pt->destroy_pending, 1, memory_order_release);

/* B2. 再检查一次 ref_count */
ref_left = atomic_load_explicit(&pt->ref_count, memory_order_acquire);
if(0 == ref_left)
{
    *pp = NULL;
    fw_final_free(pt);
    return 0;
}

/* B3. 交给最后一个出保护区的 Writer 兜底 */
```

**竞态时序**（业务线程 A 在保护区内做慢操作 + Destroy 超时后走 Phase B）：

```
T=0   Writer A：入保护区，ref_count=1，正在做慢操作（fwrite 大块 / vsnprintf 长日志）
T=1   Destroy：destroying=1 → Close SB → join 消费线程 → spin-wait 500ms 超时
T=2   Destroy：ref_left = load(ref_count) = 1 → 走 Phase B
T=3   Destroy：atomic_store(destroy_pending, 1, release)   [B1 完成]
T=4   ★ 竞态窗口 ★
        Writer A 恰好在此瞬间完成慢操作，进入 FW_LEAVE_GUARD：
          fetch_sub(ref_count, 1) → 从 1 变 0（_r 返回旧值 1）
          load(destroy_pending) → 见到 1（B1 已 release）
          → 调 fw_final_free(fw)   ★ 第一次释放 ★
T=5   Destroy：走 B2：load(ref_count) = 0
          → 判断"归 0 了，我兜底"
          → 调 fw_final_free(fw)   ★ 第二次释放 ★
        → StreamBufferAPI_Destroy(NULL) / pthread_mutex_destroy(已销毁) / free(已释放)
        → UAF / double-free / coredump
```

**触发条件**：
- Destroy 与 Write 真正并发（本轮新增机制的目标场景）
- 至少一个 Writer 卡在保护区超过 `destroy_wait_ms`（500ms 默认）
- Writer 完成的瞬间正好落在 B1 与 B2 之间

**触发概率**：低但确实存在。生产环境慢操作（长日志/大 fwrite）多 + 磁盘抖动时，B1/B2 之间时钟差是纳秒~微秒级，Writer 在此窗口完成完全可能。

**根因**：`destroy_pending` 的置位和 `ref_count` 的复检不是原子操作，形成**"两个观察者都认为自己是最后一个"**的经典 race。

**修法**：用 `atomic_compare_exchange_strong` 把 B1 变成"独占的置位"，配合复检次序保证释放责任互斥。核心逻辑：

```c
/* Phase B：CAS 独占置 destroy_pending 0→1，然后复检 */
int expected = 0;
atomic_compare_exchange_strong_explicit(
    &pt->destroy_pending, &expected, 1,
    memory_order_acq_rel, memory_order_acquire);
/* CAS 一定成功（Destroy 只调一次，无并发写 destroy_pending）。
 * 关键是 CAS 建立了一个明确的时间点："此后 LEAVE 的 Writer 看到 =1"。 */

ref_left = atomic_load_explicit(&pt->ref_count, memory_order_acquire);
if(0 == ref_left)
{
    /* 所有 Writer 已 LEAVE，且它们 LEAVE 时看到的 destroy_pending 一定是 0
     * （CAS 之前的所有 load 都读到旧值 0），所以它们不会调 fw_final_free。
     * 由本函数兜底释放。 */
    *pp = NULL;
    fw_final_free(pt);
    return 0;
}
/* 仍有 Writer 在保护区。它们后续 LEAVE 时会看到 destroy_pending=1
 * （acq_rel 保证），由最后一个（fetch_sub 返回 1）调 fw_final_free。 */
```

**这样为什么对**：CAS 是**一个原子指令**——它把"置 destroy_pending=1"这个动作压缩到一个不可分割的时间点。CAS 前的所有 LEAVE 者看到的 destroy_pending 是 0（不释放）；CAS 后的所有 LEAVE 者看到的是 1（可能释放）。Destroy 侧的复检 `ref_count == 0` 意味着"CAS 之前 Writer 已经全部 LEAVE"——他们不会调 final_free，所以 Destroy 兜底。反之若 ref_count > 0，Destroy 交给后续 LEAVE 者，最后一个必然独占地完成释放。

**紧急度**：🔴 必修。发布前必须修掉。

---

## 二、建议修问题

### F2. 🟡 跨日 rotate 失败，`current_date` 已被更新，24 小时内不再触发跨日轮转

**位置**：`FileWriter.c:258-269`（`fw_date_changed_locked`） + `FileWriter.c:615-622`（`fw_check_daily_rotate_locked`）

```c
/* fw_date_changed_locked */
if(strcmp(today, fw->current_date) != 0)
{
    strncpy(fw->current_date, today, ...);   // ← 已更新到今天
    return 1;
}

/* fw_check_daily_rotate_locked */
if(fw->config.auto_rotate_daily && fw_date_changed_locked(fw))
{
    return fw_rotate_locked(fw);   // ← 若失败？
}
```

**场景**：跨 0 点瞬间 `fw_rotate_locked` 因磁盘满/权限/目录创建失败返回 -1。此时：
- `current_date` 已经 = 今天
- `fp` 仍指向昨天目录的旧文件
- 数据继续写入昨天的目录
- 下次 date_changed 检查：`current_date` 就是今天，返回 0，不再触发

**结果**：整整 24 小时数据都在昨天目录，直到明天再次跨日才可能自愈。用户完全无感（除非查 stat_rotate_fail）。

**修法**：`current_date` 的更新推迟到 `fw_rotate_locked` 成功之后：

```c
static int fw_check_daily_rotate_locked(T_FileWriter *fw)
{
    char today[FW_DATE_STR_LEN];
    fw_get_date_str(today, sizeof(today));
    if(!fw->config.auto_rotate_daily) return 0;
    if(strcmp(today, fw->current_date) == 0) return 0;

    /* 先尝试 rotate，成功后再更新 current_date；失败下次还会重试 */
    int rc = fw_rotate_locked(fw);   /* rotate 内部 build_paths 会用 current_date */
    /* 但 rotate 内部 build_paths 用的 current_date 还是旧的！需要临时切换 */
    ...
}
```

其实更简单的方法：**把 `current_date` 的更新拆到 rotate 内部**——rotate 里先探测目录能否创建、成功后才更新 current_date。或者：

```c
static int fw_check_daily_rotate_locked(T_FileWriter *fw)
{
    char today[FW_DATE_STR_LEN];
    char saved_date[FW_DATE_STR_LEN];
    int rc;

    if(!fw->config.auto_rotate_daily) return 0;

    fw_get_date_str(today, sizeof(today));
    if(strcmp(today, fw->current_date) == 0) return 0;

    /* 保存旧日期，切换到新日期做 rotate；失败则回滚 */
    memcpy(saved_date, fw->current_date, sizeof(saved_date));
    strncpy(fw->current_date, today, sizeof(fw->current_date) - 1);
    fw->current_date[sizeof(fw->current_date) - 1] = '\0';

    rc = fw_rotate_locked(fw);
    if(rc != 0)
    {
        memcpy(fw->current_date, saved_date, sizeof(fw->current_date));
    }
    return rc;
}
```

同时 `fw_date_changed_locked` 改为**只查询不写**：

```c
static int fw_date_changed_locked(T_FileWriter *fw)
{
    char today[FW_DATE_STR_LEN];
    fw_get_date_str(today, sizeof(today));
    return (strcmp(today, fw->current_date) != 0) ? 1 : 0;
}
```

**紧急度**：🟡 中。触发要求"跨日 + Rotate 失败"双条件，但触发后完全静默 24h，问题严重。

---

## 三、验证通过但值得留意（不修）

### V1. `FW_ENTER_GUARD` 内 fetch_add 后复检 destroying=1 的 final_free 分支是死代码

**位置**：`FileWriter.c:82-88`

```c
if(atomic_load(&fw->destroying)) {
    _r = atomic_fetch_sub(&fw->ref_count, 1);
    if(_r == 1 && atomic_load(&fw->destroy_pending)) {
        fw_final_free(fw);        // ← 死代码分支
    }
    return err;
}
```

要让这个 `fw_final_free` 被调用需要：
1. 第一次 load destroying = 0（否则第一行就 return）
2. fetch_add 之后 destroying = 1
3. 此时 destroy_pending 也已经 = 1
4. 且此 Writer 是最后一个引用（_r == 1）

Phase B 的 destroy_pending=1 在时序上一定在 destroying=1 之后（Destroy Phase A 完成才进 Phase B），所以第一次 load destroying = 0 的时刻，destroy_pending 也必然 = 0。**这个分支的 destroy_pending == 1 条件在语义上不可达**。

**建议**：删除或改成 `assert(destroy_pending == 0)`。不修也没问题，只是死代码降低可读性。

**紧急度**：🟢 低。

---

### V2. Init 里 `atomic_init` 与 `memset` 的关系

**位置**：`FileWriter.c:786`（memset）+ `FileWriter.c:819-821`（atomic_init）

C11 严格说：未 `ATOMIC_VAR_INIT` 或 `atomic_init` 的原子对象访问是 UB。这里先 `memset` 归零再 `atomic_init`，形式上正确。glibc/GCC 实现下 `atomic_int` 本质是普通 int，memset 归零后立即可用 —— 但依赖实现。

`atomic_init` 本身不是原子操作（C11 §7.17.2.2/2），要求"在其他线程访问前"调用。此处消费线程尚未创建、外部无并发，**安全**。

**紧急度**：🟢 无。

---

### V3. 消费线程读 `fw->thread_running`（volatile int）无显式内存屏障

**位置**：`FileWriter.c:653`

`while(fw->thread_running)` 中 volatile 只防编译器优化，不保证跨 CPU 可见。**但每轮循环调 `StreamBufferAPI_Wait` 内部有 mutex，隐式建立 acquire/release**，能感知主线程写。Destroy 里 `pt->thread_running = 0` 之后紧跟 `StreamBufferAPI_Flush`（内部有 mutex 唤醒），同样建立屏障。

**验证通过**：当前实现正确，但脆弱——未来若改成"完全无锁的 SB API"或 flush_ms=0 使 Wait 立返，就可能出问题。

**紧急度**：🟢 无（当前正确）。

---

### V4. `FileWriterAPI_StatsGet` 内部调用 `FileWriterAPI_GetFileCount`，导致 ref_count 瞬时 = 2

代码里已有注释确认。**验证通过**：Destroy spin-wait 只关心归 0，重入线程完整 LEAVE 后必然归 0。

---

## 四、代码风格 / 文档

### S1. 版本 printf 未删

**位置**：`FileWriter.c:778`

上一轮讨论中你决定保留（可重入不引入静态变量）+"测试完毕删除"。**当前仍在**，请在正式发布前删。

---

### S2. 头文件 Init 说明未提及抗并发销毁

**位置**：`FileWriter.h:163-179`

Destroy 的 doxygen 已详述 Phase A/B，但 Init 那里只字未提。建议在 Init 说明里补一句：

> **抗并发销毁**：Init 后本实例的 Write/WriteBin/Flush/Rotate/查询接口支持与 Destroy 并发（详见 Destroy 的 `@details` 与 `config.destroy_wait_ms`）。

---

### S3. GUARD 宏内隐含 `return err` 可读性差

**位置**：`FileWriter.c:78-89`

`FW_ENTER_GUARD(fw, -2);` 这一行在源码里看不出"如果销毁中会替我 return"。建议改名为 `FW_ENTER_OR_RETURN(fw, err)`，或改成函数返回 bool 让调用点显式 `if(!fw_enter(fw)) return err;`。

**紧急度**：🟢 低。

---

### S4. `FW_DESTROY_POLL_US = 100` 轮询过密

**位置**：`FileWriter_Main.h:65`

500ms 超时 / 100us 步长 = 每次 Destroy 最多 5000 次 spin。改成 500us 或 1000us，响应仍在毫秒级，CPU 占用下降 5-10 倍。IMX6ULL 上尤其明显。

---

### S5. `debug/main.c` 缺"Destroy 与 Write 并发"用例

如果宣称抗并发销毁，测试用例必须覆盖之。建议加 Part 8：
- 起一个业务线程持续 Write 500ms
- 主线程 300ms 时调 Destroy
- 观察是否有崩溃、日志是否有 "destroy deferred"
- 用 valgrind / ASan / TSan 跑一遍

---

## 五、修复优先级

| 优先级 | 项 | 影响 | 改动量 |
|---|---|---|---|
| 🔴 必修 | **F1** Phase B UAF | 多线程销毁并发场景下必现 | ~10 行（CAS 化） |
| 🟡 中 | **F2** 跨日 rotate 失败静默 | 极端环境 24h 静默 | ~15 行 |
| 🟢 低 | V1 死代码分支 | 可读性 | 删 3 行 |
| 🟢 低 | S1 版本 printf | 日志清洁 | 1 行 |
| 🟢 低 | S2 Init 文档补抗并发说明 | 文档 | 头文件 3 行 |
| 🟢 低 | S3 GUARD 宏改名 | 可读性 | 全局重命名 |
| 🟢 低 | S4 POLL 步长 | CPU 占用 | 1 行 |
| 🟢 低 | S5 加并发销毁测试 | 测试覆盖 | main.c ~40 行 |

---

## 六、总体评价

- **抗并发销毁方向正确**：ref_count + destroying + destroy_pending 三原子字段 + 两阶段释放 + LEAVE 时接管释放责任，是经典的引用计数式安全销毁设计。
- **F1 是新代码的经典 race**：两步原子操作总有窗口，用 CAS 合并即可，改动约 10 行。
- **F2 是原有代码的边界问题**：三轮时没有触发（跨日 + rotate 失败双条件），但存在 24 小时静默的风险。修不修都可，但值得知道。
- **测试覆盖不匹配新功能**：main.c 里没有跨线程 Destroy 用例，抗并发销毁的正确性目前**只靠推理未经实测**。
- **常规质量高**：三轮修复项全部维持，风格 `if/while/for` 大括号一致，doxygen 完整，锁使用约定注释清晰。

**建议**：F1 必修，S5 补测试并跑 TSan/valgrind，之后打 V1.1.0 tag。F2 视时间安排 —— 修比不修好，但生产环境要求苛刻的场合才必须修（跨日轮转失败本身就是罕见事件）。

**串行使用（一线程 Write + 同一线程 Destroy）场景下当前代码完全正确**，即使 F1 不修也不会触发。F1 只在真正的多线程销毁并发下才是必现问题。
