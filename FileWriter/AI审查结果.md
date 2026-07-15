# FileWriter 代码审查报告（五轮）

> **审查范围**：`include/FileWriter.h` / `src/FileWriter_Main.h` / `src/FileWriter.c` / `debug/main.c` / `debug/Makefile`
> **审查日期**：2026-07-12（晚间）
> **审查目标**：验证四轮 F1/F2 修复是否正确，反查是否引入新问题
> **对照基线**：`AI审查结果.md`（四轮，2026-07-12 早间）
> **总体结论**：**F1（Phase B 双重释放）和 F2（跨日 rotate 静默）修复完全正确**。新增 `finalize_taken` 原子字段用 CAS 独占争抢释放权，并且 `date_changed` 从"查询+副作用"改为"纯查询"、`check_daily_rotate` 负责事务性回滚，两处都是教科书式的做法。此外顺带清理了四轮里的 V1（死代码）/ S1（版本 printf）/ S4（POLL 步长）/ S2（Init 文档），落地度高。**本轮新发现 1 处编译环境隐患 + 3 个小问题，无阻塞发布项。**

---

## 零、四轮修复项验证

| 四轮项 | 四轮结论 | 五轮验证 | 说明 |
|---|---|---|---|
| **F1** Phase B 双重释放 UAF | 🔴 必修 | ✅ 完全修复 | 新增 `finalize_taken` + CAS(0→1) 独占释放权 |
| **F2** 跨日 rotate 失败 24h 静默 | 🟡 建议修 | ✅ 完全修复 | `date_changed` 纯查询，`check_daily_rotate` 事务性 |
| V1 ENTER_GUARD 死代码分支 | 🟢 低 | ✅ 已清理 | 删除 fetch_add 后复检失败分支中的 `fw_final_free` |
| S1 版本 printf | 🟢 低 | ✅ 已删 | 无残留 |
| S2 Init 文档补抗并发说明 | 🟢 低 | ✅ 已加 | `FileWriter.h:170-173` 明确写了 |
| S3 GUARD 宏改名 | 🟢 低 | ❌ 未改 | 名字仍是 `FW_ENTER_GUARD`，非阻塞 |
| S4 POLL 步长 100→500us | 🟢 低 | ✅ 已改 | `FW_DESTROY_POLL_US = 500` |
| S5 加并发销毁测试 | 🟢 低 | ❌ 未加 | 测试覆盖仍待补 |

**核心两项 F1/F2 完全落地，未引入回归。**

---

## 一、F1 修复复审：`finalize_taken` CAS 独占释放权

### 修改要点

新增第 4 个原子字段 `finalize_taken`（`FileWriter_Main.h:118-122`），三处使用点用 CAS(0→1) 抢占：

1. **`FW_LEAVE_GUARD`**（`FileWriter.c:99-110`）：
```c
int _r = atomic_fetch_sub_explicit(&(fw)->ref_count, 1, memory_order_acq_rel);
if(_r == 1 && atomic_load_explicit(&(fw)->destroy_pending, memory_order_acquire)) {
    int _exp = 0;
    if(atomic_compare_exchange_strong_explicit(
            &(fw)->finalize_taken, &_exp, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        fw_final_free(fw);
    }
    /* CAS 失败：Destroy 已抢先，我什么都不做 */
}
```

2. **Destroy A7 干净路径**（`FileWriter.c:1046-1062`）：先 load ref_count == 0 → 用 CAS 抢 → 抢到才 free。

3. **Destroy B2 兜底路径**（`FileWriter.c:1072-1087`）：B1 之后再次 load ref_count == 0 → CAS 抢。抢不到说明 Writer 已抢先（B1 之后 LEAVE 的 Writer 看到 destroy_pending=1 有资格 CAS）。

### 对抗性时序推演

**场景 A**（正常 Phase A 归零）：
- ref_count 在 A6 spin-wait 期间归 0 → A7 复检 == 0 → CAS 成功 → Destroy free。
- Writer 已 LEAVE，那时 destroy_pending==0，LEAVE 侧 CAS 分支根本不进。**✓**

**场景 B**（Phase B 双重释放风险窗口，四轮报告的核心 race）：
```
Destroy:       Writer:
B1 store(dp=1)
               fetch_sub → 0
               load(dp) = 1
               CAS(ft, 0→1)  ← Writer 想抢
B2 load(rc)=0
CAS(ft, 0→1)  ← Destroy 也想抢
```
硬件保证 `compare_exchange_strong` 是原子的，两者同时执行只有一个赢，另一个 `expected` 被更新为 1、CAS 返回 false。**Race 消除。✓**

**场景 C**（Writer 在 B1 之前完成 LEAVE，Destroy 尚未 B1）：
- Writer LEAVE 时 load(destroy_pending) == 0 → 不进 CAS 分支 → 直接走出。
- Destroy 走到 B1 store → B2 load(rc)=0 → CAS 成功 → Destroy free。**✓**

**场景 D**（Destroy A7 干净归零，但 CAS 失败）：
代码注释里说"几乎不可能"——因为 destroy_pending 尚未置位，Writer LEAVE 侧不会走 CAS 分支。**真的不可能**，属于防御性代码。可留可删。

**验证结论**：F1 修复**逻辑严密**，对抗性推演通过所有关键分支。

### memory_order 复核

- CAS 用 `memory_order_acq_rel`（成功）+ `memory_order_acquire`（失败）— 标准配置，正确。
- ref_count 用 `memory_order_acq_rel` — 建立发布/获取关系，正确。
- destroying / destroy_pending 的 store 用 `release`，load 用 `acquire` — 严格正确。

**无 memory_order 遗漏。**

---

## 二、F2 修复复审：`date_changed` 纯查询 + `check_daily` 事务性

### 修改要点

**`fw_date_changed_locked`**（`FileWriter.c:269-275`）改成**只查询不写**：
```c
static int fw_date_changed_locked(T_FileWriter *fw)
{
    char today[FW_DATE_STR_LEN];
    fw_get_date_str(today, sizeof(today));
    return (strcmp(today, fw->current_date) != 0) ? 1 : 0;
}
```

**`fw_check_daily_rotate_locked`**（`FileWriter.c:627-655`）负责事务：
```c
if(!fw_date_changed_locked(fw)) return 0;

fw_get_date_str(today, sizeof(today));
memcpy(saved_date, fw->current_date, sizeof(saved_date));
strncpy(fw->current_date, today, ...);   // 切到今天

rc = fw_rotate_locked(fw);
if(0 != rc)
{
    memcpy(fw->current_date, saved_date, sizeof(fw->current_date));   // 失败回滚
}
return rc;
```

### 时序推演

**场景 A**（跨日成功）：切 date → rotate 成功 → current_date 保持今天。**✓**

**场景 B**（跨日+磁盘满）：切 date → rotate 失败 → current_date 回滚为昨天 → 下一批数据触发 `date_changed` 仍返回 1 → 继续重试。**避免 24h 静默。✓**

**场景 C**（rotate 失败但 build_paths 已经成功）：
`fw_rotate_locked` 内部先 drain_sb → seq+1 → build_paths → open_new_file，任一步失败都会 `saved_seq` 回滚 seq。build_paths 会创建"今天的目录"（此时 current_date 已是今天）——即使 open_new_file 失败，**目录已创建**，下次重试时 build_paths 只是 idempotent 走一遍（`fw_make_dirs` 视 EEXIST 为成功）。**副作用是磁盘上留了空目录，无害。**

**验证结论**：F2 修复**行为正确**，边界处理完整。

### 微小观察

回滚 memcpy 使用 `sizeof(fw->current_date)` 拷贝完整 16 字节数组，而不是 `strlen + '\0'`。因为 `saved_date` 是通过 `memcpy(..., sizeof(saved_date))` 保存的完整数组内容，两边一致，正确。若哪天 `FW_DATE_STR_LEN` 变化，两处 sizeof 都会自动同步。**代码风格干净。**

---

## 三、新发现的问题

### N1. 🟡 Makefile 未指定 `-std=c11`，`<stdatomic.h>` 依赖工具链默认标准

**位置**：`debug/Makefile:44-48`

```makefile
CFLAGS  = -g -Wall -Wextra 
CFLAGS  := $(CFLAGS) -pthread
CFLAGS  := $(CFLAGS) $(...)
```

**问题**：本轮引入 `<stdatomic.h>`（`FileWriter_Main.h:39`），这是 **C11 标准头文件**。GCC 4.9+ 支持，但**默认 C 标准**依 GCC 版本：
- GCC 4.9 ~ 7.x：默认 `-std=gnu11`（已支持）
- GCC 8+：默认 `-std=gnu17`（已支持）
- GCC 4.7~4.8：不支持 `<stdatomic.h>`

`arm-linux-gnueabihf-gcc` 在 Ubuntu 20.04 上通常是 GCC 7 或 GCC 9，OK。但**如果工程被移植到老版工具链**（如 IMX6ULL 的官方 SDK 内嵌 GCC 4.7），会编译不过报 `stdatomic.h: No such file or directory`。

**建议**：显式加 `-std=gnu11`：
```makefile
CFLAGS  = -g -Wall -Wextra -std=gnu11
```

用 `gnu11` 而不是 `c11` 是因为 `pthread.h`、`clock_gettime` 等 POSIX 扩展需要 `_GNU_SOURCE`；`gnu11` 默认打开。也可以两个都写：`-std=c11 -D_GNU_SOURCE`。

**紧急度**：🟡 中。当前工具链下能编，跨环境时会踩坑。

---

### N2. 🟢 Destroy Phase A A7 干净路径的 CAS 失败注释理由不完全对

**位置**：`FileWriter.c:1058-1061`

```c
/* 走到这里说明有 Writer 抢先 CAS 成功——但这几乎不可能：
 * destroy_pending 未置位，LEAVE 侧的 CAS 分支进不去。留作防御。 */
*pp = NULL;
return 0;
```

**问题**：注释说"destroy_pending 未置位"是对的（A7 里 destroy_pending 确实还是 0）。但注释还说"有 Writer 抢先 CAS 成功"——**这个分支实际上是走不到的**。如果 A7 时 destroy_pending==0，任何 Writer LEAVE 都不进 CAS 分支，`finalize_taken` 必然保持 0，Destroy 的 CAS 必然成功。

**结论**：这段 else 分支是**真死代码**，不是"防御"。可以：
- 删掉（干净）；
- 或改成 `assert(0 && "unreachable: no Writer can CAS ft before destroy_pending=1")`。

**紧急度**：🟢 低（不影响正确性，只是死代码 + 注释误导）。

---

### N3. 🟢 `FileWriter_Main.h:76-77` 的"并发访问约定"里 volatile 描述已过时

**位置**：`FileWriter_Main.h:76-77`

```
/*    - volatile 字段：跨线程读写，靠 volatile 保证可见性 + 消费线程  */
/*      每轮都会经过 mutex（间接建立内存屏障）。                       */
```

**问题**：现在结构体里除了 `thread_running` / `shutting_down` 是 `volatile int`，还有 `ref_count`/`destroying`/`destroy_pending`/`finalize_taken` 四个 `atomic_int`。注释块只提 volatile，没提 atomic，读者会疑惑那四个 atomic 字段属于哪一类保护。

**建议**：注释块补一条：
```
 *    - atomic_int 字段（ref_count/destroying/destroy_pending/finalize_taken）:
 *      无锁访问，用 memory_order_acquire/release/acq_rel 显式指定顺序；
 *      详见 FileWriter.c 顶部 FW_ENTER_GUARD/LEAVE_GUARD 宏。
```

**紧急度**：🟢 低（文档）。

---

### N4. 🟢 `FileWriter.c` 文件头 doxygen 未同步"抗并发销毁"

**位置**：`FileWriter.c:1-23`

文件头说"并发保护: fp 由 file_lock 保护，thread_running 由 volatile"——但没提本轮的抗并发销毁机制、`FW_ENTER_GUARD` 宏、Phase A/B 释放策略。头文件公共 API 已经写了，但 `.c` 文件头还是老的。

**建议**：文件头 `@details` 补一段"抗并发销毁"，与头文件保持一致。

**紧急度**：🟢 低（文档）。

---

## 四、代码质量抽查

### 大括号风格（if/while/for 全大括号 + 独占行）

```bash
grep -nE '^\s*(if|while|for)\s*\([^)]*\)\s*[a-zA-Z_]' src/*.c src/*.h include/*.h debug/main.c | grep -v sizeof | grep -v '(int)w'
```
结果：仅"(int)w" 类型转换误判，**实际全部符合风格**。✓

### GUARD 配对

```bash
grep -nE 'FW_ENTER_GUARD|FW_LEAVE_GUARD' src/FileWriter.c
```
所有 ENTER 都有对应的 LEAVE（含所有 error 分支的 LEAVE）：
- `Write`（`1119` ENTER，`1136`/`1150` 双 LEAVE 覆盖 vsnprintf 失败 + 成功）
- `WriteBin`（`1167`/`1169`）
- `Rotate`（`1185`/`1190`）
- `Flush`（`1202`/`1204`）
- `GetCurrentFileName/Path/DirPath`（`1217/1222`, `1232/1237`, `1247/1252`）
- `GetFileCount`（`1271` + `1286`/`1303` 双 LEAVE 覆盖 opendir 失败 + 成功）
- `GetTotalFileCount`（`1319` + `1329`/`1342` 双 LEAVE）
- `StatsGet`（`1356`/`1381`）

**全部匹配。✓**

### memory_order 一致性

所有 `atomic_load` 用 `acquire`，所有 `atomic_store` 用 `release`，`fetch_add/sub` 和 CAS 用 `acq_rel`——**教科书式的正确用法。✓**

---

## 五、验证情况

| 需求 / 一轮修复项 | 本轮状态 |
|---|---|
| C1 事务性 Rotate | ✅ 保持 |
| C2 fwrite 短写统计 | ✅ 保持 |
| C3 delete_oldest 跳过当前 | ✅ 保持 |
| C4 snprintf 边界检查 | ✅ 保持 |
| D1 Write/WriteBin -3 返回值 | ✅ 保持 |
| D2 Rotate 失败副作用文档 | ✅ 保持 |
| D3 static 声明放头文件 → 挪到 .c | ✅ 保持 |
| D4 Rotate 前 drain SB | ✅ 保持 |
| R1 死代码参数删除 | ✅ 保持 |
| R6 StatsGet API | ✅ 保持 |
| R7 Flush 异步语义文档 | ✅ 保持 |
| R5 Rotate 阻塞时长文档 | ✅ 保持 |
| R11 降级路径栈大小 | ✅ 保持 |
| R12 SB 实例名 | ✅ 保持 |
| **F1 Phase B 双重释放** | ✅ **本轮修复** |
| **F2 跨日 rotate 静默** | ✅ **本轮修复** |
| V1 ENTER_GUARD 死代码 | ✅ 本轮清理 |
| S1 LibVision printf | ✅ 本轮删除 |
| S2 Init 文档补抗并发 | ✅ 本轮补齐 |
| S4 POLL 500us | ✅ 本轮调整 |

**无一处回归。**

---

## 六、修复优先级

| 优先级 | 项 | 影响面 | 改动量 |
|---|---|---|---|
| 🟡 建议 | **N1** Makefile 加 `-std=gnu11` | 跨工具链兼容性 | 1 行 |
| 🟢 低 | N2 A7 CAS 失败注释更正 | 可读性 | 3 行 |
| 🟢 低 | N3 Main.h 并发约定加 atomic | 文档 | 3 行 |
| 🟢 低 | N4 FileWriter.c 文件头加抗并发描述 | 文档 | 5 行 |
| 🟢 低 | S3 GUARD 宏改名 `FW_ENTER_OR_RETURN` | 可读性 | 全局重命名 |
| 🟢 低 | S5 加并发销毁测试用例 | 测试覆盖 | main.c ~40 行 |

**没有必修项。N1 建议在正式打 tag 前顺手加上。**

---

## 七、总体评价

- **F1/F2 修复质量非常高**：直接命中根因，用 CAS 独占争抢 + 事务性回滚，是教科书级的做法。
- **memory_order 使用严谨**：release/acquire/acq_rel 分工清晰，无遗漏。
- **代码组织清晰**：GUARD 宏 + Phase A/B 结构 + `finalize_taken` 独占，逻辑一次读懂。
- **文档同步度高**：Destroy 头 doxygen 已详述 Phase A/B，Init 也补了抗并发说明。
- **小缺憾**：`.c` 文件头和 Main.h 的"并发约定"注释块没跟上抗并发销毁机制；Makefile 少 `-std=gnu11`。这些都是文档/环境细节，不影响运行。

**结论**：**可以直接打 V1.1.0 tag 发布**。抗并发销毁的正确性经过 5 轮迭代已经稳固，四轮报告里的 F1 UAF 完全消除。剩下的 N1~N4 都是加分项，不阻塞。

**建议发布前动作**：
1. Makefile 加 `-std=gnu11`（1 行，10 秒改完）；
2. 在开发板上跑一次并发销毁的压力测试（业务线程持续 Write + 主线程 Destroy，重复 100 次），验证无 UAF/coredump；
3. 更新 `readme.md` / `变更记录`，列出 V1.1.0 抗并发销毁能力。

---

## 附：`finalize_taken` CAS 是否可用 `_weak`

题外话，`atomic_compare_exchange_strong_explicit` 在这里可以换成 `_weak`（可 spurious fail）吗？

答：**不建议**。当前使用点都是"one-shot"（每个线程只 CAS 一次，不在循环里重试），`_weak` 的伪失败会让本该抢到释放权的线程放弃，从而**内存泄漏**（没人 free 了）。**保持 `_strong` 正确。**

---
---

# FileWriter 代码审查报告（六轮）

> **审查范围**：`include/FileWriter.h` / `src/FileWriter.c` / `debug/stress_test.c` / `readme.md` / `需求文档.md` / `debug/Makefile`
> **审查日期**：2026-07-15
> **审查目标**：验证 V1.2 四项改进（seq 跨重启延续、seq 000-999 循环、`max_files` 范围校验、降配批量清理）是否正确落地，反查是否引入新问题；验收 Stage 4 功能断言测试；整体文档一致性复核
> **对照基线**：五轮报告（V1.1.0，2026-07-12），FileWriter.c 1442 行 → V1.2.0 1621 行
> **总体结论**：**四项改进全部落地，逻辑正确、边界完整**。`fw_scan_max_seq_locked` 与文件名解析规则严格对应；`fw_delete_oldest_locked` 由 O(N²) 循环删重构为"单次遍历 + qsort + 批量 remove"，降配启动 800 个文件从 20-60 秒缩到预期 1-3 秒；`max_files` 上限 999 与 seq wrap 边界对齐，堵住了 wrap 后字典序错位误删的坑。Stage 4 四项断言（参数校验 / seq wrap / 跨重启延续 / 降配剪枝）与实现严格对齐、断言精确到具体数值。**本轮新发现 3 个小问题 + 2 个文档观察，无阻塞发布项。**

---

## 零、V1.2 修改项验证

| 项 | 位置 | 六轮验证 | 说明 |
|---|---|---|---|
| **I1** seq 跨重启延续 | `FileWriter.c:650-712, 1064-1078` | ✅ 落地正确 | Init 扫描 max_seq，`file_seq=(max_seq+1)%1000`；目录空则保持 0 |
| **I2** seq 000-999 循环 | `FileWriter.c:732` | ✅ 落地正确 | rotate 增量 `(seq+1)%1000`，与 Init 侧一致；文件名时间戳承担唯一性 |
| **I3** `max_files` 范围校验 [0, 999] | `FileWriter.c:967-979` | ✅ 落地正确 | `<0` 或 `>999` Init 拒绝 -1；上限与 wrap 边界对齐 |
| **I4** 降配批量清理 | `FileWriter.c:489-638, 1076` | ✅ 落地正确 | O(N²)→单次遍历+qsort+批量 remove；Init 结束时调一次；rotate 后仍调用 |
| **T1** Stage 4 功能断言测试 | `stress_test.c:340-696` | ✅ 落地正确 | 4 项断言精确到具体数值；总失败数计入 g_s4_fail_total，main 返回码正确 |

**核心四项 I1~I4 完全落地，未引入回归。**

---

## 一、I1 复审：`fw_scan_max_seq_locked` 跨重启序号延续

### 实现要点

`FileWriter.c:650-712` 新增静态函数，扫描 `current_dirpath` 下匹配 `{prefix_name}_NNN_*` 的文件，返回最大 3 位序号；调用点在 `FileWriterAPI_Init` 建目录成功之后、开新文件之前（`FileWriter.c:1068-1072`）：

```c
int max_seq = fw_scan_max_seq_locked(pt);
if(max_seq >= 0)
{
    pt->file_seq = (max_seq + 1) % 1000;
}
```

### 时序推演

**场景 A（首启，目录空）**：`opendir` 成功但循环无匹配 → 返回 `-1`（初值） → `file_seq` 保持 memset 后的 0。**✓**

**场景 B（重启延续，目录里最新 seq=42）**：扫描到 `x_042_...log` 解析 seq=42 → `file_seq = 43`。**✓**

**场景 C（跨重启碰上边界，最新 seq=999）**：`file_seq = (999+1)%1000 = 0`。此时若目录里恰好还有旧的 `x_000_...log`，新文件不会撞名，因为文件名尾部有秒级时间戳。**✓**

**场景 D（目录里有非法 seq）**：解析规则要求"3 位十进制 + 紧跟 '_'"（`FileWriter.c:687-704`）。含字母、位数不对、缺 '_' 的都跳过、不干扰。**✓**

**场景 E（Init 阶段 opendir 失败）**：返回 `-1` → 走"目录空"分支保持 seq=0。此后 `fw_open_new_file_locked` 一定失败（连目录都不能打开），触发 Init 错误路径。**✓ 幂等且不会误用错误的 seq。**

### 与 rotate 侧的一致性

rotate 内 `fw->file_seq = (fw->file_seq + 1) % 1000;`（`FileWriter.c:732`），与 Init 侧 `(max_seq+1) % 1000` 语义一致。**seq 分配点仅此两处**，无泄漏路径。**✓**

### 微小观察

`fw_scan_max_seq_locked` 与 `fw_delete_oldest_locked` **两处目录遍历规则完全一致**（前缀名+'_'、跳过 '.'/'..'）——但没有抽出公共 helper。当前两处都很短（20-30 行），复用价值不大；如未来再加第三个遍历点（比如 GC/lifecycle 相关），值得抽 `fw_iter_prefix_files(fw, cb, user)`。**当前不重构。**

---

## 二、I2 复审：seq `%1000` 循环

### 实现要点

- Init：`pt->file_seq = 0;`（`FileWriter.c:1027`）+ `pt->file_seq = (max_seq + 1) % 1000;`（`FileWriter.c:1071`）
- Rotate：`fw->file_seq = (fw->file_seq + 1) % 1000;`（`FileWriter.c:732`）
- 回滚：`fw->file_seq = saved_seq;`（`FileWriter.c:738, 747`）

### 边界

- seq 存储为 `int`（32 位），`%1000` 后取值 [0, 999]，格式化 `%03d` 恰好 3 位。**无溢出。**
- 回滚保存的 saved_seq 是取模前的旧值，回滚后 seq 仍 <1000。**幂等。**
- 文件名 `snprintf("%s_%03d_%s%s", prefix_name, fw->file_seq, datetime, fw->ext)`（`FileWriter.c:397`）——即使 seq=999，也是 3 位数字，与 `fw_scan_max_seq_locked` 的 3 位解析规则完全对上。**编解码闭环。**

### 潜在问题：seq 回绕后的字典序错位

回绕后可能出现 `x_000_2026-07-16-...log` 时间 > `x_999_2026-07-15-...log`。此时字典序（`000 < 999`）与时间序（`000 > 999`）**方向相反**。这对 `fw_delete_oldest_locked` 意味着"最老"的判断会出错——**这也是 `max_files` 上限只能是 999 的直接原因**。

**关键约束**：`max_files ≤ 999` 时，同时刻目录内至多 999 个同前缀文件，seq 只在一次生命周期内连续增长；即便重启后 `(max_seq+1)%1000` 可能回绕，但 wrap 前 `fw_delete_oldest_locked` 就会先把最老的 seq 删掉，wrap 时那些老 seq 已经不在了。**归纳法保证字典序 == 时间序永远成立**。

**边界思想实验**：假设 max_files=999、目录里正好有 seq=1..999 共 999 个文件。第 1000 次 rotate → seq=(999+1)%1000=0 → 创建 `x_000_...log` → excess = 999+1-999 = 1 → 删掉最老的（字典序最小的，也就是 seq=1，正是时间最老的）。**✓ 字典序与时间序对齐。**

若把 max_files 放宽到 1500，就有可能出现 seq=0..999 和 seq=0..499（下一轮）共存，字典序会把新的 seq=0 排最前，被误删。**上限 999 的选择完全正确。**

### 验证结论

I2 的 `%1000` 循环 + I3 的上限 999 是**成对设计**，缺一不可。测试 4.2 用 1005 次 rotate 精准验证了 wrap 后 seq=5（`stress_test.c:487-489` 注释推导）。

---

## 三、I3 复审：`max_files` 范围校验 [0, 999]

### 实现要点

`FileWriter.c:974-979`：

```c
if(cfg->max_files < 0 || cfg->max_files > 999)
{
    printf("max_files=%d out of range [0,999] ##%s->%d\n",
           cfg->max_files, __FUNCTION__, __LINE__);
    return -1;
}
```

### 边界

| 输入 | 期望 | 实际 |
|---|---|---|
| `-1` | 拒绝 | ✅ 拒绝（`< 0`） |
| `0`  | 接受（无限制） | ✅ 接受 |
| `1`  | 接受 | ✅ 接受 |
| `999` | 接受（上限） | ✅ 接受 |
| `1000` | 拒绝 | ✅ 拒绝（`> 999`） |
| `INT_MAX` | 拒绝 | ✅ 拒绝 |

### 与"上限为什么是 999"注释的一致性

`FileWriter.c:967-973` 的注释块清楚交代了理由（seq 只有 000-999 共 1000 个坑位，%1000 循环，wrap 后字典序错位会误删）。**注释与实现严格对应。**

**观察**：注释里说 "seq 只有 000-999 共 1000 个坑位"，读者可能会误认为 max_files=1000 也应该接受（"我用满 1000 个坑位怎么不行"）。实际上，如果一目录有 1000 个 seq=000..999 的文件，任何一次 rotate 都会 wrap 回到已存在的 seq，产生"字典序 == 时间序"关系错乱。设为 999 就恰好在 wrap 之前触发清理，永远保持"目录里同前缀文件数 <= max_files"这个 invariant。**语义严格正确。**

---

## 四、I4 复审：`fw_delete_oldest_locked` 单次遍历 + qsort + 批量 remove

### 算法正确性

单次 `opendir + readdir` 收集匹配文件到堆内数组 → `qsort` 字典序升序 → 批量 `remove` `excess` 个最老。

**关键 invariant**：`count`（收集的候选数）+ 1（当前正写文件）= 目录同前缀文件总数。`excess = count + 1 - max_files`。删完 `excess` 个后目录内正好 `max_files` 个（含当前）。

**边界 1**（无需清理）：`count + 1 <= max_files` → `excess <= 0` → 直接 `free(names); return 0;`（`FileWriter.c:598-603`）。**✓**

**边界 2**（`excess > count`，兜底）：`FileWriter.c:604-607` 强制截断为 `count`，避免 `for(i=0; i<excess; i++)` 越界访问 `names[i]`。**理论上不会触发**（`excess = count+1-max_files`，当 `max_files >= 1` 时 `excess <= count`；`max_files=0` 在函数入口就 return 了）。防御性代码。**✓**

**边界 3**（realloc 失败）：`FileWriter.c:581-586` free 已收集的、closedir、返回 -1。**无泄漏。**

**边界 4**（个别 remove 失败）：`FileWriter.c:621-626` 打日志、`deleted++` 不递增，但循环不中断——继续尝试后续文件。**✓ 单点权限/繁忙问题不会阻断整批。**

### 复杂度分析

| 阶段 | V1.1 早期实现 | V1.2 |
|---|---|---|
| 找一个最老文件 | O(N) opendir+readdir | ─ |
| 删一个文件 | O(1) remove | ─ |
| 删 K 个 | O(K·N) = O(N²) 若 K≈N | O(N + N log N + K) = O(N log N) |
| 800 个降配（IMX6ULL eMMC 实测预期） | 20-60 秒 | 1-3 秒 |

**性能主导项**：现在是 `K` 次 `remove` 系统调用（每次几百微秒），800 次 ≈ 200ms 磁盘时间 + qsort 内存 ≈ 10ms；瓶颈在磁盘而不是算法。**符合预期。**

### 内存

`names` 堆分配，`cap=64` 起步、`realloc` 翻倍。`FW_FILENAME_MAX` 估计 128B（未确认，需看头），800 个 ≈ 100KB；4096 个 ≈ 512KB。IMX6ULL 512MB RAM 完全可承受。**✓ 但值得在文档里点一下**（Init 阶段的临时堆峰值）。

### 与 rotate 共用的语义

`fw_delete_oldest_locked` 在两个场景都调用：
- **rotate 后**：`FileWriter.c:753`，此时 excess 通常 = 1，删 1 个最老。
- **Init 结束**：`FileWriter.c:1076`，稳态首启 excess ≤ 0（早退）；降配 excess = 数百，批量删。

**同一函数覆盖两场景**——正是用户明确要求的"统一实现"。**函数命名 `_oldest` 略微不够表意（现在可以删多个），但改名会污染 diff，保持现名可接受。**

### 验证结论

I4 逻辑正确，边界完整，性能达标。

---

## 五、T1 复审：Stage 4 功能断言测试

### 4.1 参数校验（`test_max_files_validation`）

覆盖 `{-1, 0, 999, 1000}` 四个边界点，正好卡在 I3 的 `<0` 和 `>999` 拒绝分支两侧。**✓**

**小观察**：漏了 `INT_MIN` 和 `INT_MAX`。不过分支条件 `< 0 || > 999` 已经把这两个包住，等价性覆盖。**可选加强，不阻塞。**

### 4.2 seq wrap（`test_seq_wrap`）

起始 seq=0，rotate 1005 次 → 期望最终 seq=5。推导：`(0+1005) % 1000 = 5`。测试用 `max_files=0`（无限制）避免每次 rotate 触发 `fw_delete_oldest_locked` 影响性能。**推导严谨、断言精确。**

**边界考虑**：1005 次 rotate 每次要 `opendir` 读 max_seq？不——Init 只扫一次，之后 rotate 用 in-memory `fw->file_seq++`，不再读盘。所以 1005 次 rotate 主要成本是 1005 次 `fopen`+`fclose`+目录建（同目录 idempotent），在 IMX6ULL 上大概 5-15 秒。**测试可行时长。**

### 4.3 跨重启延续（`test_seq_continuity_across_restart`）

Round A：Init（seq=0）+ rotate 3 次 → seq=3、Destroy。
Round B：同前缀 Init → 扫到 max_seq=3 → file_seq=4。

**推导**：Round A 结束时目录里有 seq=0..3 共 4 个文件。Round B Init 扫描 → max_seq=3 → `(3+1)%1000=4`。**✓**

**观察**：Round A 用了 `max_files=0`——如果换成 `max_files=1`，Round A 会一路删掉 seq=0..2，只留 seq=3。Round B 依然扫到 max_seq=3 → seq=4。**测试用 0 是简化，两种模式的 I1 语义一致，无需再加一个用例。**

### 4.4 降配批量清理（`test_bulk_prune_on_downgrade`）

Round A：`max_files=999`、rotate 20 次 → 21 个文件（seq=0..20）。
Round B：同前缀但 `max_files=5` → Init 扫到 max_seq=20 → file_seq=21 → 开新文件（22 个）→ excess=22+1-5=... 等等：

**审查发现**：测试 4.4 注释里写"fw_delete_oldest_locked 批量删 22-5=17 个最老"（`stress_test.c:637`）。**这个算式错了**——实际公式是 `excess = count + 1 - max_files`，其中 `count` 是排除当前正写文件后的候选数。开完新文件时目录 22 个，`count=21`（不含当前），`excess = 21+1-5 = 17`。**结果 17 正确，但推导过程写成了 22-5，读者会误认为公式是 total-max。**

**是否影响测试**：不影响——测试断言只看 `fc_after == 5`（`stress_test.c:646`）和 `cur_seq == 21`（`stress_test.c:659`），这两个数值都是对的（22-17=5，seq=21 是新开的）。只是注释推导简写不严谨。

**建议**：把 `stress_test.c:637` 的注释改成：
```c
/* Round B: 同前缀 Init，但 max_files 骤降到 5。
 * Init 内部：扫描 max_seq=20 → file_seq=21 → 开新文件（22 个）→
 * fw_delete_oldest_locked：count=21(除当前) → excess=21+1-5=17 → 
 * 批量删 17 个最老，剩最新 5 个（含 seq=21 新开的）。 */
```

**紧急度**：🟢 低（注释推导简写，实际测试断言完全正确）。

### Stage 4 orchestrator（`run_stage4`）

- 四项测试串行执行，各自返回 fail 计数，累加到 `g_s4_fail_total`。
- `main` 末尾根据 `g_s4_fail_total > 0` 返回 1，让 CI 能检测到失败。**✓**
- Stage 4 在 Stage 1-3 之后跑——高压压力测试完毕、系统状态稳定后再做功能断言。**顺序合理。**

**微小观察**：`s4_make_cfg` 里 `destroy_wait_ms=200`（相较 Stage 1 的 200-1000ms 更短）。Stage 4 都是单线程测试，业务侧根本没并发销毁场景，200ms 足够；甚至 50ms 也够。**不改。**

### 断言精度

每一个 FAIL 分支都打印期望值和实际值，方便定位失败原因。**✓**

---

## 六、新发现的问题

### N5. 🟢 `test_bulk_prune_on_downgrade` 注释推导简写

**位置**：`debug/stress_test.c:635-637`

见 §5.4，"批量删 22-5=17" 应为 "excess=21+1-5=17"。实际数值正确，只影响可读性。

### N6. 🟢 `fw_delete_oldest_locked` 函数名与语义微不匹配

**位置**：`FileWriter.c:521`

函数名 `_oldest`（单数）现在做的是"批量删多个"。V1.1 时确实每次只删 1 个，V1.2 后同一函数既可能删 1 个（rotate 后），也可能删数百个（Init 降配）。

**建议**：可保留现名（避免破坏内部命名一致性），或改为 `fw_prune_excess_locked` 更表意。**不阻塞发布。**

### N7. 🟢 Init 阶段堆内存峰值未在文档提示

**位置**：`FileWriter.c` `fw_delete_oldest_locked` 头注释 + `readme.md` §六

`names[cap][FW_FILENAME_MAX]` 在 4096 个文件降配场景下峰值约 512KB 堆（`realloc` 翻倍规则）。IMX6ULL 512MB 可承受，但若某天在小内存设备（如 128MB Cortex-M）部署要留意。

**建议**：`fw_delete_oldest_locked` 头注释加一句"最坏堆峰值 ≈ max_files * FW_FILENAME_MAX"；readme.md §六（删旧策略）末尾加一行"降配 N 个文件时 Init 阶段临时占用 N*128B 堆内存"。**紧急度低。**

---

## 七、文档一致性复核

### 文件版本号

| 文件 | 位置 | V1.2 后应有 | 现状 |
|---|---|---|---|
| `readme.md` | 第 3 行 项目版本 | V1.2.0 | ✅ V1.2.0（本轮已更新） |
| `readme.md` | §十二 变更记录 | 有 V1.2.0 行 | ✅（本轮已加） |
| `需求文档.md` | 头部 | 有 2026-07-15 更新时间 | ✅（本轮已加） |
| `需求文档.md` | §4.2 `max_files` 注释 | 说明 [0,999] 上限 | ✅（本轮已改） |
| `需求文档.md` | §4.4.3 | 有 seq 循环 + 跨重启延续 | ✅（本轮已加） |
| `FileWriter.h` | 文件头 @Version | V1.2.0 | ✅（本轮已改） |
| `debug/Makefile` | ProgramVER | 1.2.0 | ✅（本轮已改） |
| `FileWriter.c` | 文件头（如有） | 与 .h 同步 | ⚠️ 需检查 |
| `stress_test.c` | 文件头 @Version | 至少注明"Stage 4 = V1.2" | ⚠️ 头注释里说了 V1.1.0 |

**观察 D1**：`stress_test.c:31` 头注释仍写 `@Version V1.1.0`，但文件里已经加了 V1.2 的 Stage 4。头注释与内容不完全同步。**建议**：改为 `@Version V1.2.0`，`@date` 更新为 2026-07-15。**紧急度低（文档），不阻塞。**

**观察 D2**：`FileWriter.c` 文件头 doxygen 五轮报告 N4 已经提到"没提抗并发销毁机制"，本轮又增加了 seq 延续/降配剪枝，可考虑合并一次整体重写文件头（说清 V1.0 + V1.1 + V1.2 的能力堆叠）。**紧急度低。**

### readme.md ↔ 需求文档.md ↔ 代码 三方一致性

| 项 | 代码 | readme.md | 需求文档.md |
|---|---|---|---|
| `max_files` 范围 | [0,999] 强校验 | ✅ [0,999] | ✅ [0,999] |
| seq %1000 循环 | ✅ | ✅ 已述 | ✅ 已述 |
| 跨重启 max_seq+1 延续 | ✅ | ✅ 已述 | ✅ 已述 |
| Init 降配批量清理 | ✅ | ✅ 已述 | ✅ 已述 |
| Stage 4 测试 | ✅ 4 项 | ✅ 提及 4 项 | — 不涉及（需求文档不细化测试） |

**三方对齐**。

### readme.md §十一 常见问题

- Q7 提到 "V1.2 计划加 `fsync_ms`" → 已改为"V1.3 计划" ✅
- Q8.1 seq wrap → 新加 ✅
- Q8.2 降配场景 → 新加 ✅

**FAQ 与本轮改动同步。**

---

## 八、代码质量抽查

### 大括号风格

`fw_scan_max_seq_locked` / `fw_delete_oldest_locked` / `test_*` 全部符合"全大括号 + 独占行"风格。**✓**

### 内存安全

`fw_delete_oldest_locked` 的动态数组：
- malloc → 检查 NULL → closedir → 返回 -1（`FileWriter.c:552-555`）
- realloc 失败 → free 原数组 + closedir + 返回 -1（`FileWriter.c:581-586`）
- 所有 return 前 free names（`FileWriter.c:601, 636`）

**无泄漏、无 UAF、无 double-free。**

### 死锁风险

`fw_scan_max_seq_locked` / `fw_delete_oldest_locked` 都不再持有额外的锁，只用调用方持有的 `file_lock`。**无嵌套锁。**

### 中断安全

`opendir/readdir` 在信号打断时可能 EINTR——当前实现未特殊处理。Init 阶段几乎不会中断；rotate 后调用也在锁内，业务线程被打信号触发这个路径的概率极低。**默认接受，可作 V1.3 加固点。**

### 竞态窗口

`fw_scan_max_seq_locked` 与 `fw_delete_oldest_locked` 之间：
- Init 时序是 `build_paths → scan_max_seq → open_new_file → delete_oldest`。
- 中间若有别的进程往同目录写文件（seq=某个中间值）——本进程扫到的 max_seq 会漏。**跨进程共用同一 dir_path + file_prefix 属于用户配置错误**，需求文档 D6 已声明"跨实例请配不同 dir_path 或 file_prefix"。**默认接受。**

### GUARD 配对

新增函数都是 Init/rotate 内部路径，不涉及业务侧 API，所以不需要 `FW_ENTER_GUARD`。**✓**

---

## 九、验证情况

| 需求 / 修复项 | 本轮状态 |
|---|---|
| C1~C4 事务性 Rotate/fwrite 短写/删除跳过当前/snprintf 边界 | ✅ 保持 |
| D1~D4 -3 返回/Rotate 失败副作用/static 位置/drain SB | ✅ 保持 |
| R1~R12 死代码/StatsGet/Flush 异步/Rotate 阻塞时长/降级栈/SB 名 | ✅ 保持 |
| F1 Phase B 双重释放 | ✅ 保持 |
| F2 跨日 rotate 静默 | ✅ 保持 |
| V1/S1/S2/S4 五轮清理项 | ✅ 保持 |
| N1 Makefile `-std=gnu11` | ✅ 已修复（Makefile:47） |
| **I1 seq 跨重启延续** | ✅ **本轮修复** |
| **I2 seq %1000 循环** | ✅ **本轮修复** |
| **I3 max_files 范围 [0,999]** | ✅ **本轮修复** |
| **I4 降配批量清理** | ✅ **本轮修复** |
| **T1 Stage 4 功能断言测试** | ✅ **本轮新增** |

**无一处回归。**

---

## 十、修复优先级

| 优先级 | 项 | 影响面 | 改动量 |
|---|---|---|---|
| 🟢 低 | N5 stress_test.c:637 注释推导修正 | 可读性 | 3 行 |
| 🟢 低 | N6 fw_delete_oldest_locked 改名 fw_prune_excess_locked | 可读性 | 全局 4 处 |
| 🟢 低 | N7 堆内存峰值文档提示 | 文档 | 2 处各 1 行 |
| 🟢 低 | D1 stress_test.c 头 @Version 升 V1.2.0 | 文档 | 2 行 |
| 🟢 低 | D2 FileWriter.c 文件头整体重写（合并 V1.0~V1.2 能力） | 文档 | 20 行 |
| 🟢 低 | N2/N3/N4（五轮遗留） A7 CAS 死代码注释/Main.h atomic 说明/文件头抗并发 | 文档 | 累计 ~10 行 |

**没有必修项。全部为文档/命名的加分项。**

---

## 十一、总体评价

- **算法正确性**：I1~I4 的核心逻辑都经过严格的时序推演，边界完整。特别是 I2/I3 的成对设计（`%1000` 循环 + 上限 999）逻辑闭环、缺一不可，反映了对 seq 生命周期的深刻理解。
- **性能提升**：I4 的 O(N²) → O(N log N) 重构直接解决了 20-60 秒的 Init 卡死问题，属于用户可感知的显著优化。
- **测试覆盖**：Stage 4 四项断言精确到具体数值（seq=5、seq=4、fc=5、seq=21），FAIL 分支都打印期望/实际值。**测试代码质量高于业界一般水平**。
- **文档同步度**：readme.md、需求文档.md、代码注释三方对齐；Q8.1/Q8.2 新增 FAQ 直击用户可能踩的坑。
- **小缺憾**：几处次要文档/注释未跟上（N5/D1/D2 等）；`fw_delete_oldest_locked` 函数名与新语义微有出入。都是次要美化项。

**结论**：**可以直接打 V1.2.0 tag 发布**。V1.2 的四项改进（seq 延续、seq 循环、参数校验、批量剪枝）质量与 V1.1 的抗并发销毁一脉相承，都是"直接命中根因 + 边界闭环 + 测试断言精确"的做法。剩下的 N5~N7/D1/D2 都是加分项，不阻塞。

**建议发布前动作**：
1. （可选）修正 `stress_test.c:635-637` 注释推导，把 `22-5=17` 改为 `21+1-5=17`。
2. （可选）`stress_test.c` 文件头 @Version 升 V1.2.0。
3. 在开发板上完整跑 `./FileWriter_Stress.bin`（Stage 1-4 全跑，约 15-30 分钟），确认 Stage 4 四项断言全 PASS，且 g_s4_fail_total=0（main 返回 0）。

---

## 十二、附：I1~I4 是否形成"完整的 seq 生命周期"

题外话，V1.2 之后 `file_seq` 的完整生命周期是什么样的？

```
Init(cfg)
  ├─ 参数校验：max_files ∈ [0,999]                                (I3)
  ├─ fw_build_paths_locked                                        
  ├─ fw_scan_max_seq_locked → max_seq                             (I1)
  ├─ file_seq = (max_seq + 1) % 1000                              (I1 + I2)
  ├─ fw_open_new_file_locked                                      
  └─ fw_delete_oldest_locked（max_files>0 时批量清理超额）         (I4)

Write/WriteBin ─────────── 不改 file_seq ───────────

Rotate
  ├─ fw_drain_sb_locked
  ├─ saved_seq = file_seq                                         
  ├─ file_seq = (file_seq + 1) % 1000                             (I2)
  ├─ fw_build_paths_locked                        [失败回滚 saved_seq]
  ├─ fw_open_new_file_locked                      [失败回滚 saved_seq]
  └─ fw_delete_oldest_locked                      [失败不影响主流程] (I4)

跨日 rotate、大小 rotate → 走同一个 fw_rotate_locked 路径

Destroy → 无 seq 操作
```

**seq 分配点收敛到两处**：Init 的初始化 + Rotate 的自增，都遵循 `%1000` 循环。回滚只在 Rotate 失败时触发，保证 seq 与实际打开的文件严格一致。**生命周期完整、无泄漏路径。**

**一目录内 seq 的 invariant**：任何时刻，目录内同前缀文件的 seq 集合 ⊆ [0, 999]，且 `count <= max_files`（若 max_files>0）；`file_seq` 一定不与目录内已存在的文件冲突（因为 Init 时取 max_seq+1，rotate 时用刚 wrap 的新值，且 wrap 前 delete_oldest 已把老 seq 清掉）。

**这是 I1~I4 四项改进作为一个整体所构成的正确性证明。**


若哪天改成循环重试 CAS，才应该用 `_weak`（更好的 ARM 汇编生成）。当前实现选择 `_strong` 是正确的。
