# FileWriter 代码审查报告（三轮）

> **审查范围**：`include/FileWriter.h` / `src/FileWriter_Main.h` / `src/FileWriter.c` / `debug/main.c` / `debug/Makefile`
> **审查日期**：2026-07-12
> **审查目标**：正确性、并发安全、错误处理、资源生命周期、边界条件、文档一致性、性能、可维护性
> **对照基线**：二轮报告 R1–R13（2026-07-12 上午）
> **总体结论**：二轮 13 项中 **7 项完全修复**（含全部 🟡 中优先级），5 项未处理（均为 🟢 低优先级），1 项部分完成。新写的 `FileWriterAPI_StatsGet` 实现正确、锁序无嵌套隐患。当前代码已满足 V1.0.0 正式发布条件，剩余问题都是可放到 V1.1 迭代的清洁项。

---

## 零、二轮问题修复验证

| 二轮项 | 定位 | 修复状态 | 关键改动 |
|---|---|---|---|
| R1 死代码 `bytes_written` 参数 | `fw_check_file_size_rotate_locked` | ✅ 修复 | 签名去掉参数（`FileWriter.c:552`），调用点同步简化（L636） |
| R2 LibVision 每次 Init 都打印 | `FileWriter.c:721` | ❌ 未修 | 仍为无条件 `printf`；多实例场景下打印 N 次 |
| R3 空写场景跨日检测未触发 | `fw_consumer_thread` L601-607 | ❌ 未修 | 仍在 `used > 0 \|\| r > 0` 分支内检测 |
| R4 消费线程持锁 fwrite 影响查询 | 查询接口 doxygen | ✅ 修复（文档） | `FileWriter.h:295-305` 加了整块 `@note` 说明潜在阻塞 |
| R5 Rotate 阻塞时长未文档化 | `FileWriterAPI_Rotate` 头文档 | ✅ 修复 | `FileWriter.h:266-269` 加 `@warning` 说明可能阻塞数十~数百 ms |
| R6 缺 Stats 查询 API | 全新增 | ✅ 修复 | `T_FileWriterStats` 结构 + `FileWriterAPI_StatsGet` 实现 + demo 使用 |
| R7 Flush 异步语义未文档化 | `FileWriterAPI_Flush` 头文档 | ✅ 修复 | `FileWriter.h:284-287` 加 `@warning` 明确异步语义 |
| R8 `fopen(..., "wb")` 覆盖打开 | `fw_open_new_file_locked` L346 | ❌ 未修 | 仍为 `"wb"`，理论上极端时间戳撞车会覆写 |
| R9 `fw_delete_oldest_locked` O(N²) | 未变更 | ❌ 未修 | 保留 while+opendir 每轮遍历 |
| R11 降级路径丢 stack size | Init L820-834 | ✅ 修复 | 改为 `eSetAttr=1 + istacksize_MB=2`，保留栈大小 |
| R12 SB 实例名硬编码 `"fw_sb"` | Init L765-778 | ✅ 修复 | 拼接为 `fw_<name>_sb`，多实例可辨识 |
| R13 命名精简 | 未变更 | ❌ 未修 | 纯风格，不改也 OK |
| C2 尾（bytes_lost 暴露给用户） | 通过 R6 落地 | ✅ 修复 | `StatsGet` 已能返回 bytes_lost 计数 |

**修复率**：中优先级 2/2（100%），低优先级 5/10（50%）；未修的 5 项均为低优先，无阻塞。

---

## 一、正确性问题（三轮新发现）

### N1. 统计字段用 `unsigned long`，32 位 ARM 下 4GB 就溢出 🟡

**位置**：`FileWriter.h:100-108` / `FileWriter_Main.h:98-101`

```c
typedef struct T_FILEWRITERSTATS
{
    unsigned long bytes_written;
    unsigned long bytes_lost;
    unsigned long rotate_count;
    unsigned long rotate_fail;
    ...
};
```

**问题**：IMX6ULL 是 32 位 ARM（armv7-a），`sizeof(unsigned long) == 4`，最大值 `4 294 967 295` ≈ **4 GB**。若设备做长期日志采集（比如 100 KB/s 连续写），累计到 4GB 只需约 **12 小时**就溢出。溢出后 `bytes_written` 回卷，用户看到的数据会**变小**，看起来像是丢失。

**触发场景**：任何长时间运行的传感器/日志采集程序，是 IMX6ULL 的典型应用场景。

**建议**：改为 `unsigned long long`（保证 ≥ 64 位）或明确使用 `uint64_t`：

```c
#include <stdint.h>

typedef struct T_FILEWRITERSTATS
{
    uint64_t bytes_written;   /**< 累计写盘字节数 */
    uint64_t bytes_lost;
    uint64_t rotate_count;
    uint64_t rotate_fail;
    ...
};
```

内部结构体 `T_FILEWRITER` 的 `stat_*` 同步改。demo 的 `%lu` 改为 `%llu` 或 `PRIu64`。改动约 15 行。

---

### N2. `fflush` 返回值未检查，短写统计偏乐观 🟢

**位置**：`FileWriter.c:618-624`（主循环）/ `L659-668`（drain）/ `L678`（关文件前）

```c
size_t w = fwrite(buf, 1, (size_t)n, fw->fp);
if(w > 0)
{
    fflush(fw->fp);                                /* ← 返回值丢弃 */
    fw->file_written        += (long)w;
    fw->stat_bytes_written  += (unsigned long)w;
}
```

**问题**：`fflush` 失败（磁盘满、fp 出错）返回 EOF 且置位 `ferror`。当前代码不检查，直接把 `w` 计入 `stat_bytes_written`，用户看到的写盘字节数比实际落到 kernel 的多。

**当前实际影响**：`fwrite` 短写基本会先于 `fflush` 失败反映出来，`fflush` 单独失败的概率低。但 `stat_bytes_written` 语义上应该表示"已落到 kernel"，宜精确。

**建议**：

```c
if(w > 0)
{
    if(fflush(fw->fp) != 0)
    {
        fw->stat_bytes_lost += (unsigned long)w;
        printf("fflush fail: %s ##%s->%d\n", strerror(errno), __FUNCTION__, __LINE__);
        clearerr(fw->fp);
        break;   /* 本轮放弃 */
    }
    fw->file_written       += (long)w;
    fw->stat_bytes_written += (unsigned long)w;
}
```

或至少把 fflush 返回值取出来赋 `stat_bytes_lost`。三处（主循环 / drain / 关闭前）都要改，共约 10 行。

---

### N3. `pt->name` 截断可能导致多实例撞名 🟢

**位置**：`FileWriter.c:735-740`

```c
strncpy(pt->name, nm, MAX_FILEWRITERNAME_LEN);   /* 32 */
pt->name[MAX_FILEWRITERNAME_LEN] = '\0';
```

**问题**：`MAX_FILEWRITERNAME_LEN = 32`。若两个实例的 `file_prefix` 前 32 字符相同（如 `"sensor/long_name_that_looks_similar_A"` 和 `"..._B"`），`pt->name` 都被截为同一字符串，导致：
1. SB 名字（`fw_<name>_sb`）撞名；
2. 消费线程 sThreadName 撞名（ThreadManage 的日志中难以区分）。

**触发概率**：极低（需要人为构造长前缀），但一旦发生排查困难。

**建议**：Init 中加撞名检查，或 name 后拼上一个自增序号（可用 static counter），或直接把 `MAX_FILEWRITERNAME_LEN` 扩到 63。

---

### N4. `StatsGet` 中 `GetLength` / `GetFileCount` 返回负值时被静默视为 0 🟢

**位置**：`FileWriter.c:1136-1137`

```c
out->sb_used    = (sb_used    >= 0) ? sb_used    : 0;
out->file_count = (file_count >= 0) ? file_count : 0;
```

**问题**：若 `StreamBufferAPI_GetLength` 或 `GetFileCount` 内部失败（opendir 失败等），返回 -1，被记为 0。用户无法通过 `StatsGet` 判断"值本身是 0"还是"取值失败"。

**建议（二选一）**：
- **A**：`StatsGet` 直接返回 -1 表示"部分字段获取失败"（改动小，但语义上有些绝对）；
- **B**：`T_FileWriterStats` 加两个字段 `sb_used_valid` / `file_count_valid`（改动稍大，用户可选择性检查）；
- **C**：接受当前行为但在头文件 `@note` 说明"sb_used / file_count 内部取值失败时置 0"。

推荐 **C**（改动最小）。

---

### N5. `demo_query` 里 `StatsGet` 打印用 `%lu`，未来切 `uint64_t` 时需同步改 🟢

**位置**：`debug/main.c:414-420`

```c
Debug_printx("Stats: written=%lu lost=%lu rotate_ok=%lu rotate_fail=%lu sb_used=%d files=%d",
             st.bytes_written, st.bytes_lost, ...);
```

**问题**：与 N1 联动——若采纳 N1 建议改成 `uint64_t`，`%lu` 变为不匹配（32 位 ARM 上 `unsigned long` 是 32 位，`uint64_t` 是 64 位）。

**建议**：改 `uint64_t` 时同步用 `PRIu64` 宏（`<inttypes.h>`）。

---

## 二、可维护性/风格（三轮新增）

### N6. `StatsGet` 内部注释"避免锁嵌套"表述不严谨 🟢

**位置**：`FileWriter.c:1122-1125`

```c
/* SB used 和 file_count 不持 file_lock 也能拿：
 * - SB 内部自持锁；
 * - GetFileCount 会自行处理并发（自己也拿 file_lock 快照目录路径）。
 * 先取这两个避免锁嵌套。 */
sb_used    = StreamBufferAPI_GetLength(fw->sb);
file_count = FileWriterAPI_GetFileCount(fw);
```

**问题**：注释里的"避免锁嵌套"逻辑不完全成立——即便把这两行放到 `pthread_mutex_lock(&fw->file_lock)` 之后，`GetFileCount` 内部**也会**尝试再次拿 `file_lock`，那才是真正的嵌套（同一线程重入非递归 mutex，行为未定义/死锁）。

**结论**：**当前代码是正确的**（就是必须先调 GetFileCount 再拿 file_lock），只是注释描述得不到位。

**建议**：改成：

```c
/* GetFileCount 内部会拿 file_lock，因此必须在本函数拿锁前调用，
 * 否则将造成同一线程重入（非递归 mutex 会死锁）。SB 内部有自己的锁，
 * 与 file_lock 无嵌套关系，顺序可任意。 */
```

---

### N7. `FileWriterAPI_StatsGet` 的头文档 `@date` 与其他 API 不一致 🟢

**位置**：`FileWriter.h:395`

```c
@date        2026-07-12
```

其他所有 API 均为 `2026-07-11`，这一个是 `07-12`。虽然日期确实是三轮加的，但既然文件顶部 `@date` 是 `2026-07-11`（V1.0.0 版本发布日），逐个 API 用不同日期反而混乱。

**建议**：统一为 V1.0.0 发布日期，或所有 API 都保持"最后修改日期"（相应大范围更新）。**优先级低**。

---

### N8. `fw_consumer_thread` 兜底 drain 不做 fflush 检查 🟢

**位置**：`FileWriter.c:653-673`

```c
while((n = StreamBufferAPI_GetData(fw->sb, buf, sizeof(buf))) > 0)
{
    if(fw->fp != NULL)
    {
        size_t w = fwrite(buf, 1, (size_t)n, fw->fp);
        if(w > 0) { fw->stat_bytes_written += (unsigned long)w; }
        if((int)w < n) { fw->stat_bytes_lost += (unsigned long)(n - (int)w); }
    }
    else { fw->stat_bytes_lost += (unsigned long)n; }
}

/* 关闭文件 */
if(fw->fp != NULL)
{
    fflush(fw->fp);
    fclose(fw->fp);
    fw->fp = NULL;
}
```

**问题**：
1. 兜底 drain 循环内**未 fflush**，只在 close 前统一 fflush。若 SB 有多轮数据（每次 GetData 2KB），累积在 stdio buffer，可能一次 fflush 就把几十 KB 交给 kernel，是 OK 的。但和主循环内每次 fwrite 后就 fflush 的策略不一致。
2. Close 前的 fflush 返回值未检查（同 N2）。

**建议**：兜底 drain 内部无需 fflush（成本考虑），但 close 前的 fflush 应检查返回值：

```c
if(fw->fp != NULL)
{
    if(fflush(fw->fp) != 0)
    {
        printf("final fflush fail: %s ##%s->%d\n", strerror(errno), __FUNCTION__, __LINE__);
    }
    fclose(fw->fp);
    fw->fp = NULL;
}
```

---

## 三、验证情况

| 需求文档条目 / 关键修复 | 实测结果 | 状态 |
|---|---|---|
| 多级目录自动创建（4.4.1） | `./fw_test/log/X.../sensor` 生成 | ✓ |
| 文件命名 `{prefix}_{seq3}_{datetime}.{ext}` | `demo_log_000_2026-07-12-...` | ✓ |
| max_files 删最老（4.4.3） | Part 5 rotate 6 次，保留 3 个 | ✓ |
| max_file_size 自动轮转（4.4.3） | Part 3 40 帧 * 24B 生成 2 个文件 | ✓ |
| 时间戳前缀（D5） | Part 7 格式正确 | ✓ |
| SCHED_RR + 可配优先级（D2） | ThreadManage 日志显示 SCHED_RR | ✓ |
| 多实例可重入（3.9） | Part 4 两实例独立 | ✓ |
| 优雅关闭（3.10） | Destroy 后 `*pp = NULL` | ✓ |
| 查询接口（4.6） | FileName/Path/DirPath/FileCount 全对 | ✓ |
| 工具函数（4.6） | GetTimeString 四种格式全对 | ✓ |
| **事务性 Rotate（一轮 C1）** | fopen 失败保持原 fp | ✓ |
| **Rotate 前 drain（一轮 D4）** | `fw_rotate_locked:513` 先 drain | ✓ |
| **snprintf 累加防溢出（一轮 C4）** | 三段都检 `n < 0 \|\| n >= remain` | ✓ |
| **Stats API（二轮 R6）** | Part 6 打印 6 项统计，数据合理 | ✓ |
| **降级路径保留 stack size（二轮 R11）** | 代码路径审阅确认 | ✓ |
| **SB 实例名带 pt->name（二轮 R12）** | Init 内拼接为 `fw_<name>_sb` | ✓ |

无回归；`StatsGet` 与消费线程并发的锁序审阅通过（先无锁调 SB / GetFileCount，再拿 file_lock 读 stat 字段）。

---

## 四、修复优先级建议（三轮）

| 优先级 | 项 | 影响面 | 改动量 |
|---|---|---|---|
| 🟡 中 | N1 stats 用 `unsigned long`（32 位平台 4GB 溢出） | 长期运行的 IoT 场景会看到误数据 | ~15 行 |
| 🟢 低 | N2 fflush 返回值未检查 | 磁盘异常时统计偏乐观 | ~10 行 |
| 🟢 低 | R2 LibVision 多次打印（二轮遗留） | 日志清洁 | 3 行 |
| 🟢 低 | R3 空写场景跨日检测（二轮遗留） | 病态时序下短暂错位 | 5 行 |
| 🟢 低 | R8 wb → wbx 独占打开（二轮遗留） | 极端时间戳撞车 | 1 行 |
| 🟢 低 | N3 pt->name 截断撞名 | 极端长前缀 | 5~30 行 |
| 🟢 低 | N4 StatsGet 负值静默为 0 | 用户区分不了 "0" 与 "取值失败" | 文档 3 行或结构体扩展 |
| 🟢 低 | N5 demo `%lu` 与 N1 联动 | 联动改动 | 1 行 |
| 🟢 低 | N6 StatsGet 注释表述不严谨 | 阅读理解 | 3 行 |
| 🟢 低 | N7 头文档 `@date` 不一致 | 一致性 | 若干 |
| 🟢 低 | N8 关闭前 fflush 未检查 | 最终 fflush 失败不知情 | 4 行 |
| 🟢 低 | R9 delete_oldest O(N²)（二轮遗留） | 目录数百文件时可感 | 40 行 |
| 🟢 低 | R13 命名精简（二轮遗留） | 风格 | 若干 |

---

## 五、总体评价

- **二轮问题修复**：中优先级 R6/R7 **100% 修复且实现质量高**，`StatsGet` 的锁序处理正确（先 SB / GetFileCount 无锁调用，再 file_lock 拷贝计数），无死锁隐患。低优先级 5 项未修但都是可延后项。
- **架构完备度**：至此三轮迭代后，模块具备了生产环境的诊断可观测性（bytes_lost / rotate_fail 均可被用户看到），事务性 Rotate + 前置 drain SB 保证了数据不错位，SCHED_RR 消费线程降级路径也补齐了 stack size。核心路径无已知正确性缺陷。
- **主要短板（三轮维度）**：
  - **N1 是本轮唯一"中优先级"新问题**——32 位 ARM 的 `unsigned long` 只有 4GB 上限，与 FileWriter 定位的"长时间日志采集"场景冲突，宜在 V1.0 发布前改为 `uint64_t`；
  - 其余 N2/N3/N4 / R2/R3/R8 均为低优先级健壮性/风格项，不阻塞发布。
- **验证情况**：demo 7 段 + 新增 Stats 打印全部通过，无回归。

**结论**：修复 N1（`uint64_t`）后可直接发布 V1.0.0；不修 N1 也能发布，但对典型的长跑场景要在 README 里注明"stats 字段 32 位平台每 4GB 会回卷"。其余问题列入 V1.1 迭代即可。
