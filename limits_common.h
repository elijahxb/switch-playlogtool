/**
 * limits_common.h - 两个二进制共享的编译期常量。
 *
 * 前台 NRO (`source/main.c`, 位于仓库根) 与后台 sysmodule
 * (`sysmodule/source/main.c`) 共享 SD 卡上的同一份
 * `sdmc:/PlayLog/config.json` 与 `sdmc:/PlayLog/limits.json`。
 * 这两个文件里能保存的限额条目数必须完全一致,否则一边写入的条目
 * 另一边读不出来(此前是 128 与 512 不一致,见 REFACTOR_SPEC §6 item 17)。
 *
 * 因此容量常量统一放在这个头文件里,两端各自 include 同一份定义。
 * 改动这里会同时影响两个二进制,属于接口变更,需同步检查另一端。
 */
#pragma once

/// 单个 limits 表能容纳的最大条目数(设备级或账号级各自计数)。
/// 两端必须保持一致: NRO 写入 limits.json 的上限 == sysmodule 读取的上限。
#define MAX_LIMITS 512

/// 状态表(用量记账 / 待上传条目)的条目数上限。
#define MAX_USAGE 512

/// 上报来源标记(REFACTOR_SPEC §8)。`POST /switch/statistics/daily` 的每个
/// game entry 都带可选 `"source"` 字段,服务端按 (game, 本地日) 对每个来源
/// 分别汇总后取 max,从而消除"前台 NRO(PDM 口径)"与"后台 sysmodule(自身
/// 前台计时口径)"对同一段游玩时长的双计。
///   - 前台 NRO: 数字来自 PDM 系统统计 -> "nro_pdm"
///   - sysmodule: 数字来自它自己的前台计时 -> "sysmodule"
/// 取值上限 32 字符;缺省 "" 表示历史/未知来源。
#define UPLOAD_SOURCE_NRO_PDM "nro_pdm"
#define UPLOAD_SOURCE_SYSMODULE "sysmodule"

/// 一台主机上最多缓存的 Switch 用户账号数(系统本身最多 8 个)。
#define MAX_ACCOUNTS 8

/// 限额执行方式(REFACTOR_SPEC §10),设备级配置 `enforce_mode`。
/// 两个二进制共享同一份 `sdmc:/PlayLog/config.json`,所以取值也必须共享,
/// 否则一边写入的字符串另一边认不出来。
///   - ENFORCE_MODE_BLOCK_LAUNCH(默认): 配额用尽时不打断当前这局(玩家正常存档),
///     改为**下次启动该游戏时**在加载画面阶段立刻结束进程,从而不丢进度。
///   - ENFORCE_MODE_KILL: 旧行为,达到限额即刻硬杀(严格,但只在退出时存档的游戏会丢进度)。
/// 服务端 `GET /switch/devices/config` 会下发 `enforce_mode`,那才是权威值;
/// 本地 `config.json` 里的值只是离线兜底。字段缺失/为空时按默认 block_launch;
/// 取值无法识别时客户端必须**不终止**(宁可少杀,不可误杀)。
#define ENFORCE_MODE_BLOCK_LAUNCH "block_launch"
#define ENFORCE_MODE_KILL "kill"
#define ENFORCE_MODE_DEFAULT ENFORCE_MODE_BLOCK_LAUNCH
/// enforce_mode 字符串缓冲区大小(服务端列为 CharField(20))。
#define ENFORCE_MODE_MAX 20

/// §11 周总时长 + 按星期限额, 两端共享的常量与语义(§15 起语义已明确化)。
///
///   - `weekday_seconds`: 长度恒为 7 的数组, **下标 0 = 周一 … 6 = 周日**
///     (与 Python `date.weekday()` 一致)。
///       * **恰好 7 个值** = "按星期已配置": 当日生效限额 = 该天的值, **值为 0
///         也照样生效(= 当天禁止游玩)**。
///       * **`[]` / 字段缺失 / 长度 != 7** = "按星期未配置": 回退 `daily_seconds`。
///     两端解析时都必须单独记录一个 `has_weekday` 标志: `[]` 与 `[0]*7` 必须区分开,
///     否则"周一到周五不让玩"会退化成"每天都能玩 daily 分钟"。
///   - `weekly_seconds`: 周总上限, `0` = 不限制。**周 = 本地时区本周一 00:00 起算**。
///   - 今日生效限额 `effective_daily = has_weekday ? weekday_seconds[today]
///     : daily_seconds`; `effective_daily == 0` 即**当天封禁**。
///   - **拦截条件(任一满足即拦截, 是 OR 不是 AND)**:
///       1) `effective_daily == 0`(当天封禁), 或 `today_used >= effective_daily`;
///       2) `week_used  >= weekly_seconds`   且 `weekly_seconds  > 0`。
///     哪个上限先到就封哪个; 周额度只是额外一层封顶, 不从每日额度里扣减。
///   - 周用量必须来自**未截断的真实用量**: 不得把已被每日限额截断的每日用量求和,
///     否则周上限永远算不到(见 sysmodule 的 limiter_week.csv)。
///   - 任一不确定(规则读不到、周用量算不出、模式无法识别)一律**不拦截**并写日志。
///   - 兼容: 老服务端只下发 `limits` / `account_limits`(今日生效日限额), 此时按
///     旧语义处理(`0` 分钟 = 封禁), 行为与改造前完全一致。老服务端若把"未配置"
///     下发成 `[0]*7`, 新客户端会读成"每天都封禁" —— 这是 §15.1 接受的取舍。
#define WEEKDAY_COUNT 7
/// 单日上限(秒): 24h。
#define WEEKDAY_SECONDS_MAX 86400U
/// 周上限(秒): 7 × 24h。
#define WEEKLY_SECONDS_MAX 604800U

/// §15/§16 纯判定函数(无 libnx 依赖, 可在主机上单独编译测试)。
///
/// 前两个二进制(前台 NRO / 后台 sysmodule)必须用**同一条**判定, 否则
/// "未配置按星期"与"显式 7 个 0"会在两端解释不一致(REFACTOR_SPEC §15.1)。
/// 把这些判定抽到这里, 两端 include 同一份实现 —— 主机单测也直接编译这一份。
///
/// 注意: 参数用普通 unsigned/unsigned long long, 不依赖 u32 等 libnx 类型,
/// 这样 limits_common.h 可以脱离 switch.h 在主机上编译。

/// §15: 恰好 7 个值 = "按星期已配置"(**不看值**, 全 0 也算已配置)。
/// `[]` / 字段缺失 / 长度 != 7 => 未配置, 回退 daily。
static inline int limits_has_weekday_override(int weekday_count)
{
    return weekday_count == WEEKDAY_COUNT;
}

/// §15 今日生效限额(秒):
///   已配置按星期(恰好 7 个值) -> weekday[今天], **0 也原样生效**(0 = 当天封禁);
///   未配置 -> daily; 下标非法时回退 daily(相当于"星期几算不出")。
static inline unsigned limits_effective_daily_seconds(unsigned daily_seconds,
                                                      const unsigned weekday_seconds[WEEKDAY_COUNT],
                                                      int has_weekday, int weekday_index)
{
    if (has_weekday && weekday_index >= 0 && weekday_index < WEEKDAY_COUNT)
        return weekday_seconds[weekday_index];
    return daily_seconds;
}

/// §16 账号当日合计 = max(本地账本合计, 服务端 account_used_today_total 下限)。
/// **只抬不降**: 服务端值更大时采用它, 更小时绝不下调。
static inline unsigned long long limits_total_with_floor(unsigned long long local_sum,
                                                         unsigned long long server_floor)
{
    return (local_sum > server_floor) ? local_sum : server_floor;
}

/// §16 是否已达/超当日合计上限。limit == 0 = 当天封禁(§15), 因此 used(0) >= 0 即拦。
static inline int limits_account_total_over(unsigned long long used_seconds, unsigned limit_seconds)
{
    return used_seconds >= (unsigned long long)limit_seconds;
}

/// §11 判定结果: 哪个上限触发/生效(与后端 `limit_source` 取值一致)。
#define LIMIT_SOURCE_NONE "none"
#define LIMIT_SOURCE_DAILY "daily"
#define LIMIT_SOURCE_WEEKDAY "weekday"
#define LIMIT_SOURCE_WEEKLY "weekly"

/// §12.3 接口日志(每天一份, 最多保留 7 天)。两端写的是**同一个**当天文件,
/// 所以路径与格式也放在共享头里, 避免两边改歪。
///
///   sdmc:/PlayLog/api-YYYYMMDD.log   (本地日期, YYYYMMDD)
///
/// 每次 API 调用追加一行(REFACTOR_SPEC §13 起带上 ASCII 机器码):
///   YYYY-MM-DD HH:MM:SS [tag] METHOD /path -> HTTP <status> <code|-> <detail|OK>
///   - `code` 来自响应体的 `"code"` 字段, 只保留可见 ASCII; 缺失时打印 `-`;
///   - `detail` 可能是中文, 但**文件保持 UTF-8**(拷到电脑上看完整原因);
///     屏幕(日志查看器)会自行把 >=0x80 净化为 `?`。
/// tag ∈ config | push | heartbeat | write(写盘失败) | net(curl 层失败)
///
/// 注意: 这里直接写死 `sdmc:/PlayLog`(而不是复用各文件里的 PLAYLOG_DIR),
/// 因为宏展开时 PLAYLOG_DIR 还不存在; 两端都必须用这一份路径, 否则日志会分家。
#define API_LOG_PATH_FMT "sdmc:/PlayLog/api-%s.log"
/// 单行上限(字节), 超出截断(不截半个 UTF-8 字符)。
#define API_LOG_LINE_MAX 240
/// 机器码(`code`)缓冲区上限。服务端契约为短 ASCII 串(如 device.bad_signature)。
#define API_LOG_CODE_MAX 64

/// §13: libnx 的 console 是点阵字体(ConsoleFont), 只覆盖 ASCII, 没有任何
/// UTF-8 处理。因此**屏幕输出必须是纯 ASCII**: 中文 detail/hint 只能进日志文件,
/// 屏幕上的失败原因用 ASCII 的 `code` + `hint`(见客户端 printServerFailure)。
/// 控制台一行 80 列, 折行宽度留出缩进。
#define CONSOLE_COLUMNS 80
/// 屏幕上 hint 的折行宽度(扣除缩进后 <= 78 列)与最大行数。
#define CONSOLE_HINT_WIDTH 76
#define CONSOLE_HINT_MAX_LINES 3
/// 保留天数: 每次写入时 remove(今天-7), 于是最多 7 份(今天 0 .. 今天-6)。
#define API_LOG_RETENTION_DAYS 7
/// 启动时清理的历史区间: 今天-7 .. 今天-40(按文件名计算, 不列目录)。
#define API_LOG_SWEEP_FIRST_DAY API_LOG_RETENTION_DAYS
#define API_LOG_SWEEP_LAST_DAY 40
/// 日志 tag(REFACTOR_SPEC §12.3)。
#define API_LOG_TAG_CONFIG "config"
#define API_LOG_TAG_PUSH "push"
#define API_LOG_TAG_HEARTBEAT "heartbeat"
#define API_LOG_TAG_WRITE "write"
#define API_LOG_TAG_NET "net"
