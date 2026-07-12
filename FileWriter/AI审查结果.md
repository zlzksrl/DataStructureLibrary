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

若哪天改成循环重试 CAS，才应该用 `_weak`（更好的 ARM 汇编生成）。当前实现选择 `_strong` 是正确的。
