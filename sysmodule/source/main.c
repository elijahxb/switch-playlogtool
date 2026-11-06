#include <switch.h>
#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../../limits_common.h"

#define INNER_HEAP_SIZE 0x400000
// MAX_LIMITS / MAX_USAGE / MAX_ACCOUNTS 与前台 NRO 共用 limits_common.h,
// 避免两端容量不一致(REFACTOR_SPEC §6 item 17)。
#define POLL_INTERVAL_SECONDS 1
#define DEFAULT_LIMIT_RELOAD_SECONDS 30
#define DEFAULT_CONFIG_SYNC_SECONDS 30
#define DEFAULT_UPLOAD_INTERVAL_SECONDS 900
#define MAX_CONFIG_SIZE 16384
#define MAX_UPLOAD_BODY_SIZE 65536
#define DEFAULT_SERVER_BASE_URL "https://your-server.example.com"
#define PUSH_PATH "/switch/statistics/daily"
#define CONFIG_FETCH_PATH "/switch/devices/config"
#define HEARTBEAT_PATH "/switch/devices/heartbeat"
#define PLAYLOG_DIR "sdmc:/PlayLog"
#define LIMITS_PATH PLAYLOG_DIR "/limits.json"
#define CONFIG_PATH PLAYLOG_DIR "/config.json"
#define STATE_PATH PLAYLOG_DIR "/limiter_state.csv"
// §11 周用量账本(未截断真实用量), 与日账本分开存放。
#define WEEK_STATE_PATH PLAYLOG_DIR "/limiter_week.csv"
#define GUARD_PATH PLAYLOG_DIR "/limiter_guard.json"
#define UPLOAD_STATE_PATH PLAYLOG_DIR "/limiter_upload_state.csv"
#define LOG_PATH PLAYLOG_DIR "/limiter.log"
// HTTPS 证书;缺失时明确报错而不是静默跳过校验(item 20)。
#define CAINFO_PATH PLAYLOG_DIR "/cacert.pem"
#define DEFAULT_HEARTBEAT_INTERVAL_SECONDS 300
#define HEARTBEAT_RETRY_INTERVAL_SECONDS 60
#define SPL_CONFIG_ITEM_EXOSPHERE_API_VERSION ((SplConfigItem)65000)
// 账号限额解析容量(系统最多 8 个账号)。
#define MAX_ACCOUNT_LIMITS (MAX_ACCOUNTS * MAX_LIMITS)
// §16 账号级"全部游戏合计"限额: 每账号最多一条(系统最多 8 个账号), 容量与
// 账号表一致; 服务端 account_total_limit 也是"每账号唯一一行"。
#define MAX_ACCOUNT_TOTAL_RULES MAX_ACCOUNTS
// 日切防回拨阈值(item 14)。两者都是 24 小时:
//   - 单调运行时长满 24h,或
//   - 墙钟相对"上一次接受新的一天"前进满 24h。
// 这样"把日期往回拨"永远不会重置配额,而"往前拨"每获得一天新配额至少
// 需要 24 小时的真实时间(或 24 小时的真实运行),无法快速刷配额。
#define DAY_GUARD_RUNTIME_SECONDS 86400ULL
#define DAY_GUARD_WALL_SECONDS 86400ULL
#define TITLE_NAME_CACHE_SIZE 64
// §9 前台焦点判定参数:
//   - 每次只扫描 PDM 事件日志末尾的 FOCUS_EVENT_SCAN_COUNT 条;
//   - 焦点快照每 FOCUS_POLL_SECONDS 秒刷新一次(1 秒轮询, 但不必每秒打 IPC);
//   - 焦点切换事件必须已"沉淀" FOCUS_BACKGROUND_MIN_AGE_SECONDS 秒, 且连续
//     FOCUS_BACKGROUND_CONFIRM_POLLS 次观测到同一 background 证据, 才敢暂停记账。
#define FOCUS_EVENT_SCAN_COUNT 16
#define FOCUS_POLL_SECONDS 5
#define FOCUS_BACKGROUND_MIN_AGE_SECONDS 60
#define FOCUS_BACKGROUND_CONFIRM_POLLS 2

// §11 单条限额规则(秒)。weekday_seconds 下标 0 = 周一 … 6 = 周日。
// §15: has_weekday = "按星期已配置"(服务端下发的数组恰好 7 个值)。已配置时该天
// 的值**原样生效, 0 也生效(= 当天封禁)**; 未配置(字段缺失 / `[]` / 长度不对)时
// 才回退 daily_seconds。`[]` 与 `[0]*7` 必须区分开, 这是 §15.1 的核心。
typedef struct {
    u32 daily_seconds;
    u32 weekly_seconds;                  ///< 0 = 不限制
    u32 weekday_seconds[WEEKDAY_COUNT];  ///< has_weekday 时生效(0 = 当天封禁), 否则忽略
    bool has_weekday;                    ///< §15: 数组长度恰为 7 = 按星期已配置
    // §15.2 时钟防抖: 服务端已经按**服务端本地日**算好的"今日生效秒数"
    // (GET /switch/devices/config 的 limits / account_limits 同步下发)。设备 RTC
    // 可能不准(服务端对签名时间戳默认只 warn), 按设备时钟算星期几会把
    // "周一到周五封禁" 算成周末限额 -> 限额静默失效; 所以同步成功后的信任窗口内
    // 优先用这个服务端值, 过期(长时间离线)才回退本地 weekday 计算。
    bool has_server_today;
    u32 server_today_seconds;
} LimitRule;

typedef struct {
    u64 title_id;
    u32 limit_seconds;
    // §11: 同一份 limits.json 里可能带 limit_rules(完整规则); 缺失时按老语义
    // (0 分钟 = 封禁)使用 limit_seconds。has_rule 标记该条目是否来自 limit_rules。
    u32 weekly_seconds;
    u32 weekday_seconds[WEEKDAY_COUNT];
    bool has_weekday;                    ///< §15: 按星期已配置(恰好 7 个值)
    bool has_rule;
    bool has_server_today;               ///< §15.2: limit_seconds 来自服务端今日生效值
    u32 server_today_seconds;
} LimitEntry;

// 账号级限额: (account_id, title_id) -> seconds(REFACTOR_SPEC §2.2)。
typedef struct {
    char account_id[33];
    u64 title_id;
    u32 limit_seconds;
    // §11 同上
    u32 weekly_seconds;
    u32 weekday_seconds[WEEKDAY_COUNT];
    bool has_weekday;                    ///< §15: 按星期已配置(恰好 7 个值)
    bool has_rule;
    bool has_server_today;               ///< §15.2 同上
    u32 server_today_seconds;
} AccountLimitEntry;

// §16 账号级"全部游戏合计"限额: account_id -> 当日生效上限。
// 与单游戏规则的 §15 语义完全一致(唯一真源):
//   - weekday_seconds 恰好 7 个值 = 按星期已配置, 当日生效值 = 该天的值(0 也生效
//     = 当天禁止游玩); `[]` / 字段缺失 / 长度 != 7 = 未配置, 回退 daily_seconds。
//   - daily_seconds == 0 且未配置按星期 = 当天禁止游玩(沿用"0 分钟 = 封禁")。
// 服务端只下发 enabled=true 的行, 所以"存在该条目"本身就表示限额已启用。
typedef struct {
    char account_id[33];
    u32 daily_seconds;
    u32 weekday_seconds[WEEKDAY_COUNT];
    bool has_weekday;
    // §15.2 服务端按自己的本地日算好的"今日生效合计上限"(account_total_limits)。
    bool has_server_today;
    u32 server_today_seconds;
} AccountTotalRuleEntry;

// §16 服务端权威的"今日该账号全部游戏合计"秒数, 作为本地合计的**下限**。
// 与 §14.3 同一原则: 只抬不降。带 day_key, 本地日键一变立即失效(见
// reset_account_total_floors / account_total_floor_seconds)。
typedef struct {
    char account_id[33];
    char day_key[16];
    u64 used_seconds;
} AccountTotalFloorEntry;

// 用量记账键: (账号, 游戏, 本地日)。account_id 为空串表示设备级。
typedef struct {
    char account_id[33];
    char day_key[16];
    u64 title_id;
    u32 used_seconds;
} UsageEntry;

// §11 周用量账本键: (账号, 游戏, 本周周一)。存的是**未截断**的真实用量。
//
// 为什么不复用 UsageEntry: 日账本只保留"当天"的条目(load_state 按 g_active_day
// 过滤), 无法用来求"本周"总和。§14 修复后日账本的 used_seconds 已按真实时长累加、
// 不再在限额处截断, 但"只留当天"这一点没变, 所以周用量仍需独立账本, 不动既有语义。
typedef struct {
    char account_id[33];
    char week_key[16];                  ///< 本周周一 YYYY-MM-DD
    u64 title_id;
    u32 uncapped_seconds;               ///< 未截断真实用量
} WeekUsageEntry;

// 待上传条目(键: 账号 + 游戏)。
typedef struct {
    char account_id[33];
    u64 title_id;
    u32 pending_seconds;
    u64 total_seconds;
    u32 launch_count;
    u32 pending_launches;
    // item 16: record_uid 在首次发送前生成并随状态持久化,重试复用同一个值。
    char record_uid[64];
} UploadEntry;

// 服务端响应签名头(item 20)。
typedef struct {
    char hex[129];
    bool present;
} ResponseSignature;

// 游戏名缓存: nsGetApplicationControlData 较重,同一 title 只解析一次。
typedef struct {
    u64 title_id;
    char name[0x201];
} TitleNameCacheEntry;

typedef struct {
    char server_base_url[256];
    char device_id[65];
    char device_id_type[20];
    char upload_token[129];
    u32 limit_reload_seconds;
    u32 config_sync_seconds;
    u32 upload_interval_seconds;
    u32 heartbeat_interval_seconds;
    // §10 限额执行方式(ENFORCE_MODE_*)。服务端 GET /switch/devices/config
    // 下发的值是权威值(会顺手写回 config.json); 本地 config.json 只是离线兜底。
    char enforce_mode[ENFORCE_MODE_MAX];
} AppConfig;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} MemoryBuffer;

//---------------------------------------------------------------------------------
// §9 前台焦点诊断(best-effort)
//
// 背景: `pm:dmnt GetApplicationProcessId` 只回答"应用进程是否存在", 不回答
// "它是否正在前台被玩" —— switchbrew 对该命令的说明就是 "Returns the pid of the
// application process"。游戏切到 HOME 菜单/其它 applet 后进程仍在, 于是旧逻辑
// 会继续计时, 甚至可能在后台把游戏杀掉。
//
// 已核实的可用信号(全部来自 /opt/devkitpro/libnx/include/switch/services/):
//   1. pdm.h: `pdmqryGetAvailablePlayEventRange()` + `pdmqryQueryAppletEvent()`
//      返回系统记录的 PdmAppletEvent(含 `PdmAppletEventType_InFocus` /
//      `OutOfFocus` / `OutOfFocus4`)。这是系统自己为"游玩时长统计"记录的
//      焦点切换日志, 也是唯一能区分前台/后台的已核实来源。
//   2. pm.h: `pmdmntGetApplicationProcessId()` —— 只作为"进程存在"信号。
//   3. apm.h: `apmGetPerformanceMode()` —— 只作为诊断信号, 不参与判定。
//
// 明确排除(核实过但不满足需求):
//   - `appletGetFocusState()`(applet.h): 返回的是**调用进程自己**的焦点状态,
//     由本进程的 appletMainLoop() 更新; sysmodule 是 AppletType_None, 拿不到
//     别的进程/标题的焦点。
//   - `pmshellGetApplicationProcessIdForShell()`(pm.h): switchbrew 说明它只是
//     "遍历进程链表找 mask 0x40 的第一个进程", 与 pm:dmnt 同样只看进程是否存在,
//     没有任何焦点信息。
//
// 安全规则(REFACTOR_SPEC §9): 只有**确证** background 才暂停记账与限额执行;
// unknown / suspect 一律维持旧行为(继续计时、继续执行限额)。任何读失败、时间戳
// 不可用、证据太新、信号互相矛盾都退化到 suspect/unknown。
//---------------------------------------------------------------------------------
typedef enum {
    FocusState_Unknown = 0,
    FocusState_Foreground,   ///< 应用在前台
    FocusState_Background,   ///< 确证应用不在前台(HOME/其它 applet 占焦)
    FocusState_Suspect,      ///< 信号不足或互相矛盾: 按 old behaviour 继续计时
} FocusState;

typedef struct {
    FocusState state;
    const char *source;          ///< §9 约定: "pm_shell"|"apm"|"heuristic"|"none"
    bool have_app_pid;
    u64 app_pid;
    bool pdm_ready;
    int newest_focus_event;      ///< 全局最新焦点事件类型, -1 = 未读到
    bool newest_focus_is_ours;   ///< 该事件是否属于当前标题
    int newest_ours_event;       ///< 当前标题最新事件(含 launch), -1 = 未读到
    long long evidence_age_seconds; ///< 证据事件距今秒数, -1 = 时间戳不可用
    bool have_apm;
    int apm_perf;
    unsigned background_streak;  ///< 连续确认到 background 证据的次数
} FocusStatus;

static FocusStatus g_focus = {
    .state = FocusState_Unknown,
    .source = "none",
    .newest_focus_event = -1,
    .newest_ours_event = -1,
    .evidence_age_seconds = -1,
    .apm_perf = -1,
};
// 只在焦点状态发生变化时写一行日志, 避免每 5 秒刷屏。
static bool g_focus_log_initialised = false;

static LimitEntry g_limits[MAX_LIMITS];
static size_t g_limit_count = 0;
static AccountLimitEntry g_account_limits[MAX_ACCOUNT_LIMITS];
static size_t g_account_limit_count = 0;
// §16 账号级"全部游戏合计"限额规则 + 服务端合计下限(每账号至多一条)。
static AccountTotalRuleEntry g_account_total_rules[MAX_ACCOUNT_TOTAL_RULES];
static size_t g_account_total_rule_count = 0;
static AccountTotalFloorEntry g_account_total_floors[MAX_ACCOUNT_TOTAL_RULES];
static size_t g_account_total_floor_count = 0;
// §19 服务端时间驱动: 最近一次成功同步服务端配置的时刻(单调秒, 0 = 本次启动还没同步过)
// 与最近一次同步到的**服务端本地日**(YYYY-MM-DD, 权威值)。
//
// 日账本 / 周账本 / 星期几 / §16 的"今日生效值"全部改为由服务端日驱动, 不再信任主机
// RTC(可能长期跑偏, 也可能被人为改). 同步成功后的信任窗口(1h, 远大于配置同步间隔)
// 内: g_active_day 直接采纳服务端日; 超时(长时间离线)才回退本地时钟 + 日切守卫,
// 行为与改造前一致(宁可不拦, 不可误杀)".
#define SERVER_TIME_TRUST_SECONDS 3600ULL
static u64 g_server_synced_at_mono = 0;
static char g_server_day[16] = "";
static UsageEntry g_usage[MAX_USAGE];
static size_t g_usage_count = 0;
// §11 周用量账本(未截断真实用量), 见 WeekUsageEntry 注释。
static WeekUsageEntry g_week_usage[MAX_USAGE];
static size_t g_week_usage_count = 0;
static bool g_week_state_dirty = false;
// 周用量是否可信: 时钟不可用 / 周账本读失败 / 当前周未知时为 false。
// 规则里配了周上限但这里为 false => 周用量算不出 => **不拦截**(宁可不拦不可误杀)。
static bool g_week_usage_known = false;
static UploadEntry g_upload[MAX_USAGE];
static size_t g_upload_count = 0;

// 当前前台游戏所在账号(32 位十六进制)。空串 = 无法确定,完全退回设备级。
static char g_current_account[33] = "";
// 生成 record_uid 用的单调序号。
static u64 g_uid_epoch = 1;
static bool g_uid_epoch_dirty = false;

// 日切守卫(item 14)。
// 已见最大日键序值(YYYYMMDD)与"上一次接受新的一天"时的墙钟/运行时长。
static u64 g_guard_day_seq = 0;
static u64 g_guard_runtime_seconds = 0;
static u64 g_guard_wall_max = 0;
static bool g_guard_dirty = false;
static bool g_guard_loaded = false;

static TitleNameCacheEntry g_title_names[TITLE_NAME_CACHE_SIZE];
static size_t g_title_name_count = 0;

// 排查过程:
// 之前用极小缓冲区(几KB级别)配合2MB堆,socket能建立会话、nifm状态也确认正常、
// NifmRequest也确认Available,但每次实际connect()都稳定卡满超时——
// bsd:u/bsd:s、IPv4强制、nifm request持有时间都排除了之后,
// 缓冲区大小是唯一还没做对照测试的变量,这里放大到接近官方默认值再测一次。
// 堆已经提到 INNER_HEAP_SIZE 0x400000(4MB)给这个留出空间。
static const SocketInitConfig g_socketInitConfig = {
    .tcp_tx_buf_size     = 0x8000,   // 32KB,和 socketGetDefaultInitConfig() 一致
    .tcp_rx_buf_size     = 0x1000,   // 4KB,和 socketGetDefaultInitConfig() 一致
    .tcp_tx_buf_max_size = 0x25000,  // 148KB,和 socketGetDefaultInitConfig() 一致
    .tcp_rx_buf_max_size = 0x25000,  // 148KB,和 socketGetDefaultInitConfig() 一致

    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0x2400,

    .sb_efficiency = 2,   // 官方默认是4,这里减半控制总内存占用

    .num_bsd_sessions = 1,   // 官方默认是3,我们只会有1个并发请求,没必要开3个
    // libnx 文档写明 bsd:u 才是"默认/推荐"的服务类型,bsd:s 是更特殊的系统级会话,
    // 之前测过两者行为完全一致,这里保留 bsd:u。
    .bsd_service_type = BsdServiceType_User,
};

static AppConfig g_config = {
    .server_base_url = DEFAULT_SERVER_BASE_URL,
    .limit_reload_seconds = DEFAULT_LIMIT_RELOAD_SECONDS,
    .config_sync_seconds = DEFAULT_CONFIG_SYNC_SECONDS,
    .upload_interval_seconds = DEFAULT_UPLOAD_INTERVAL_SECONDS,
    .heartbeat_interval_seconds = DEFAULT_HEARTBEAT_INTERVAL_SECONDS,
    .enforce_mode = ENFORCE_MODE_DEFAULT,
};

//---------------------------------------------------------------------------------
// §10 限额执行方式 enforce_mode
//
//   block_launch(默认): 配额用尽**不打断当前这局**(玩家正常存档退出),
//                        改为下次启动该游戏时在加载画面阶段立刻结束进程。
//   kill:                旧行为,达到限额即刻硬杀(可能丢进度)。
//
// 判定在"进入该 title 的那一刻"一次做完并**冻结**: 用进入时的已用秒数与进入时的限额
// 比较, 得到 over_limit_at_entry。此后无论用量怎么涨、限额怎么改, 这一局都不再改变结论
// (只有"进入时已超限"才拦), 否则"进入时未超限"的一局仍可能中途被误杀, 那就退化成 kill 了。
//
// 安全规则: 任何不确定(额度读不到、进程查询失败、模式取值无法识别)一律**不终止**。
//---------------------------------------------------------------------------------
typedef enum {
    EnforceMode_BlockLaunch = 0,  ///< 默认
    EnforceMode_Kill,             ///< 旧行为: 超限即杀
    EnforceMode_Unknown,          ///< 配置值无法识别: 一律不终止(fail-safe)
} EnforceMode;

// 当前这一次"前台 title 会话"的执行状态(进入 title 时由 decide_enforce_session() 冻结)。
//   entry_used     : 进入该 title 时 (账号, 游戏, 本地日) 的已用秒数(冻结值)
//   entry_known    : entry_used 是否可信(读不到用量记账时为 false)
//   decided        : 进入时是否拿到了额度并做出判定(拿不到 -> 本局永不终止)
//   over_at_entry  : 进入时已用 >= 当时限额(block_launch 唯一会拦的情况)
//   exceed_logged  : 本局是否已写过"超限但不杀"日志(避免刷屏)
static u64 g_session_entry_used = 0;
static bool g_session_entry_known = false;
static bool g_session_decided = false;
static bool g_session_over_limit_at_entry = false;
static bool g_session_exceed_logged = false;
// §16 账号合计限额的本局冻结状态(与单游戏的 g_session_* 分开, 不改变既有语义):
//   g_session_account_total_decided       : 进入本局时是否算出了账号合计规则
//   g_session_account_total_over_at_entry : 进入本局时合计是否已达/超上限(或 0 封禁)
//   g_account_total_exceed_logged         : 本局是否已写过 §16 日志(避免刷屏)
static bool g_session_account_total_decided = false;
static bool g_session_account_total_over_at_entry = false;
static bool g_account_total_exceed_logged = false;
// §16/§10: 进入本局时冻结的"当前账号"。游戏运行中主机如果切换了用户(账号变了),
// 视为新的一局重新做启动拦截判定 —— 否则 block_launch 会拿旧账号的判定放行整局。
// 空串 = 设备级(账号判定失败)。
static char g_session_account[33] = "";
// 上一次已经打过日志的执行方式, 用于在模式变化时只记一行。
static EnforceMode g_logged_enforce_mode = EnforceMode_BlockLaunch;
static bool g_enforce_mode_logged = false;

static char g_active_day[16] = "unknown";
// §11 当前生效的"周键" = 本周周一的 YYYY-MM-DD(本地时区)。"unknown" = 时钟不可用。
static char g_active_week[16] = "unknown";
static bool g_state_dirty = false;
static bool g_upload_dirty = false;
static bool g_fs_ready = false;
static bool g_sd_ready = false;
static bool g_time_ready = false;
static bool g_socket_ready = false;
static bool g_curl_ready = false;
static bool g_pmdmnt_ready = false;
static bool g_pminfo_ready = false;
static bool g_pmshell_ready = false;
static bool g_pdm_ready = false;
static bool g_apm_ready = false;
static bool g_nifm_ready = false;
static bool g_psm_ready = false;
static bool g_ns_ready = false;
static bool g_account_ready = false;
static bool g_spsm_ready = false;
static bool g_nifm_request_created = false;
static bool g_nifm_request_ready = false;
static NifmRequest g_nifm_request;

static void load_limits(void);
static void load_state(void);
static void save_state(void);
static void load_week_state(void);
static void save_week_state(void);
static void load_upload_state(void);
static void save_upload_state(void);
static void add_upload_seconds(const char *account_id, u64 title_id, u32 seconds);
static void add_upload_launch(const char *account_id, u64 title_id);
static u64 monotonic_seconds(void);
static void load_guard_state(void);
static void save_guard_state(void);
static const char *resolve_current_account(void);
static const char *get_title_name_cached(u64 title_id);
static bool read_whole_file(const char *path, char *out, size_t out_size, bool *exists, size_t *out_len);
static bool write_file_atomic(const char *path, const char *data, size_t len);
static void copy_fixed_string(char *out, size_t out_size, const char *in, size_t in_size);
static bool hex_value(char c, u8 *out);
static const char *json_find_value(const char *body, const char *key);
static bool parse_title_id(const char *text, size_t len, u64 *out);
static bool is_valid_account_id_hex(const char *text);
static UsageEntry *get_usage_entry(const char *account_id, u64 title_id);
// §11
static bool find_device_rule(u64 title_id, LimitRule *out_rule);
static bool find_effective_rule(const char *account_id, u64 title_id, LimitRule *out_rule);
static u32 rule_effective_daily(const LimitRule *rule, int weekday_index);
static bool rule_has_any_cap(const LimitRule *rule);
static int weekday_index_from_day_key(const char *day_key);
static bool week_start_key_from_day_key(const char *day_key, char *out, size_t out_size);
static void reset_week_state(void);
static void add_uncapped_seconds(const char *account_id, u64 title_id, u32 seconds);
static bool week_used_seconds(const char *account_id, u64 title_id, u32 *out_seconds);
static const char *evaluate_limit_rule(const LimitRule *rule, const char *account_id, u64 title_id,
                                       UsageEntry *usage, const char **out_reason);
// §16 账号级"全部游戏合计"限额。
static bool find_account_total_rule(const char *account_id, LimitRule *out_rule);
static u64 account_total_floor_seconds(const char *account_id);
static u64 account_total_used_today(const char *account_id);
static void reset_account_total_floors(void);
static bool account_total_should_terminate(const char *account_id, const char **out_source);
// §19 服务端时间驱动(时钟防抖)
static bool server_sync_recent(void);
static bool server_day_trusted(void);
static void apply_server_clock_hint(const char *body);
static void refresh_day_key(void);
static void append_focus_heartbeat_json(char *body, size_t body_size);
static void update_focus_status(bool have_app, u64 app_pid, u64 title_id);

#ifdef __cplusplus
extern "C" {
#endif

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 2;

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void)
{
    Result rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_SUCCEEDED(rc)) {
        g_fs_ready = true;
        rc = fsdevMountSdmc();
        g_sd_ready = R_SUCCEEDED(rc);
    }

    rc = timeInitialize();
    g_time_ready = R_SUCCEEDED(rc);

    // Keep sm open so the main loop can retry services that are not ready at boot2 time.
}

void __appExit(void)
{
    if (g_pmshell_ready) pmshellExit();
    if (g_pminfo_ready) pminfoExit();
    if (g_pmdmnt_ready) pmdmntExit();
    if (g_pdm_ready) pdmqryExit();
    if (g_apm_ready) apmExit();
    if (g_nifm_request_created) nifmRequestClose(&g_nifm_request);
    if (g_nifm_ready) nifmExit();
    if (g_account_ready) accountExit();
    if (g_ns_ready) nsExit();
    if (g_psm_ready) psmExit();
    if (g_spsm_ready) spsmExit();
    if (g_curl_ready) curl_global_cleanup();
    if (g_socket_ready) socketExit();
    if (g_time_ready) timeExit();
    if (g_sd_ready) fsdevUnmountAll();
    if (g_fs_ready) fsExit();
    smExit();
}

#ifdef __cplusplus
}
#endif

static void ensure_playlog_dir(void)
{
    if (!g_sd_ready)
        return;
    mkdir(PLAYLOG_DIR, 0777);
}

//---------------------------------------------------------------------------------
// §12.3 接口日志(每天一份, 最多保留 7 天)
//
//   sdmc:/PlayLog/api-YYYYMMDD.log   (本地日期)
//
// 一行一次 API 调用(单行上限 API_LOG_LINE_MAX 字节, 超出截断):
//   YYYY-MM-DD HH:MM:SS [tag] METHOD /path -> HTTP <code> <detail|OK>
// tag ∈ config | push | heartbeat | write(写盘失败) | net(curl 层失败)
//
// 前台上报(NRO)与本 sysmodule 追加写**同一个**当天文件(短行, 交错时最多丢一行,
// 不会写坏别的行)。两个二进制互相独立, 所以实现在两边各留一份, 但路径/格式常量
// 共用 limits_common.h。
//
// 失败策略: 写日志失败绝不影响主流程, 所以所有函数都不返回错误、调用方也不需要
// 检查。保留策略按**文件名**计算(不列目录):
//   - 每次写入时 remove(今天-8);
//   - 启动时再扫一遍 今天-8 .. 今天-40, 清掉历史遗留。
//---------------------------------------------------------------------------------

/// HTTP 状态码的英文文案。服务端没给 detail 时用它兜底(§12.2)。
static const char *http_status_text(long code)
{
    switch (code) {
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "HTTP Error";
    }
}

/// 清理 detail 里的控制字符, 保证一条日志只占一行。
static void api_log_sanitize(const char *in, char *out, size_t out_size)
{
    if (out_size == 0)
        return;
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
    }
    out[o] = '\0';
}

/// 取"今天 - offset_days 天"的本地日历时间。时钟不可用/明显不合理时返回 false
/// (这时宁可不写日志, 也不要生成一个非法日期命名的文件)。
static bool api_log_calendar(long offset_days, TimeCalendarTime *out_cal)
{
    if (!g_time_ready || !out_cal)
        return false;

    u64 timestamp = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &timestamp)))
        return false;

    s64 shifted = (s64)timestamp - (s64)offset_days * 86400;
    if (shifted <= 0)
        return false;

    TimeCalendarTime cal;
    TimeCalendarAdditionalInfo info;
    if (R_FAILED(timeToCalendarTimeWithMyRule((u64)shifted, &cal, &info)))
        return false;
    if (cal.year < 2020 || cal.year > 2099)
        return false;

    *out_cal = cal;
    return true;
}

static bool api_log_path(long offset_days, char *out, size_t out_size)
{
    TimeCalendarTime cal;
    if (!api_log_calendar(offset_days, &cal))
        return false;

    char compact[16];
    snprintf(compact, sizeof(compact), "%04u%02u%02u", cal.year, cal.month, cal.day);
    snprintf(out, out_size, API_LOG_PATH_FMT, compact);
    return true;
}

/// 启动时清一次 今天-8 .. 今天-40 的历史日志。
static void api_log_sweep_old_files(void)
{
    for (long offset = API_LOG_SWEEP_FIRST_DAY; offset <= API_LOG_SWEEP_LAST_DAY; offset++) {
        char path[96];
        if (api_log_path(offset, path, sizeof(path)))
            remove(path);
    }
}

/// 追加一行接口日志。任何失败都静默跳过(不影响主流程)。
/// `code` 是服务端返回的 ASCII 机器码(可 NULL/空, 记作 `-`), `detail` 可为中文;
/// 两者一起写进 UTF-8 文件(§13: 前台日志查看器会自行把非 ASCII 净化为 '?')。
static void api_log_line(const char *tag, const char *method, const char *api_path,
                         long http_code, const char *code, const char *detail)
{
    if (!g_sd_ready)
        return;

    // 时钟不可用 => 无法按"每天一份"命名, 直接跳过。
    TimeCalendarTime cal;
    if (!api_log_calendar(0, &cal))
        return;

    // 每次写入顺手删掉今天-8 的文件(最多保留 7 天)。
    {
        char stale[96];
        if (api_log_path(API_LOG_SWEEP_FIRST_DAY, stale, sizeof(stale)))
            remove(stale);
    }

    // 惰性清理历史遗留: 只在本进程第一次写日志时扫一次(不在开机路径上做)。
    static bool sweep_done = false;
    if (!sweep_done) {
        sweep_done = true;
        api_log_sweep_old_files();
    }

    char file_path[96];
    if (!api_log_path(0, file_path, sizeof(file_path)))
        return;

    ensure_playlog_dir();
    FILE *f = fopen(file_path, "a");
    if (!f)
        return;

    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u",
             cal.year, cal.month, cal.day, cal.hour, cal.minute, cal.second);

    // detail 为空时: 2xx 记 OK, 其它按 HTTP 状态文案兜底。
    char reason[192];
    if (detail && detail[0]) {
        api_log_sanitize(detail, reason, sizeof(reason));
    } else if (http_code >= 200 && http_code < 300) {
        snprintf(reason, sizeof(reason), "OK");
    } else {
        snprintf(reason, sizeof(reason), "%s", http_status_text(http_code));
    }

    // §13: 机器码保证是 ASCII(缺失时 '-' 占位), 这样前台查看器净化后仍能一眼看出原因。
    const char *log_code = (code && code[0]) ? code : "-";

    char line[API_LOG_LINE_MAX + 4];
    int n = snprintf(line, sizeof(line), "%s [%s] %s %s -> HTTP %ld %s %s",
                     stamp, tag ? tag : "-", method ? method : "-",
                     api_path ? api_path : "-", http_code, log_code, reason);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len > API_LOG_LINE_MAX) len = API_LOG_LINE_MAX;          // 单行 240 字节上限
        if (len > sizeof(line) - 1) len = sizeof(line) - 1;
        // 截断时不要把 UTF-8 字符切成半个(否则前台日志查看器会显示乱码)。
        while (len > 0 && ((unsigned char)line[len] & 0xC0) == 0x80) len--;
        fwrite(line, 1, len, f);
        fputc('\n', f);
    }
    fclose(f);
}

//---------------------------------------------------------------------------------
// 文件读写: 一律 temp + rename 原子写(item 15)。
// 读取区分"文件不存在"(可用默认值)与"文件存在但读失败"(必须保留旧值)。
//---------------------------------------------------------------------------------
static bool read_whole_file(const char *path, char *out, size_t out_size, bool *exists, size_t *out_len)
{
    if (exists) *exists = false;
    if (out_len) *out_len = 0;
    if (out_size == 0)
        return false;

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    if (exists) *exists = true;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long file_size = ftell(f);
    if (file_size < 0 || (size_t)file_size >= out_size) {
        // 超长文件直接判定读失败,而不是截断后当成"内容就是这样"。
        fclose(f);
        return false;
    }
    rewind(f);

    size_t read = fread(out, 1, (size_t)file_size, f);
    bool failed = ferror(f) || read != (size_t)file_size;
    fclose(f);
    if (failed)
        return false;

    out[read] = '\0';
    if (out_len) *out_len = read;
    return true;
}

/// 真正干活的写盘函数; 失败由下面的 write_file_atomic 统一记一条 write 日志。
static bool write_file_atomic_raw(const char *path, const char *data, size_t len);

static bool write_file_atomic(const char *path, const char *data, size_t len)
{
    if (write_file_atomic_raw(path, data, len))
        return true;

    // §12.3: 写盘失败也要进接口日志, 并带上目标路径(write tag)。
    api_log_line(API_LOG_TAG_WRITE, "WRITE", path, 0, NULL, "写盘失败");
    return false;
}

static bool write_file_atomic_raw(const char *path, const char *data, size_t len)
{
    ensure_playlog_dir();

    char temp_path[320];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    FILE *f = fopen(temp_path, "wb");
    if (!f)
        return false;

    size_t written = len ? fwrite(data, 1, len, f) : 0;
    if (len && written != len) {
        fclose(f);
        remove(temp_path);
        return false;
    }
    if (fclose(f) != 0) {
        remove(temp_path);
        return false;
    }
    if (rename(temp_path, path) != 0) {
        // Switch 的 SD 卡是 FatFs, 它的 f_rename 在"目标已存在"时返回 FR_EXIST(不支持覆盖),
        // 于是 temp+rename 在文件已存在时必然失败。这里删掉旧文件后重试;
        // 其它文件系统(支持覆盖)仍走上面的原子路径。
        remove(path);
        if (rename(temp_path, path) != 0) {
            // 仍失败则退化为直接覆盖写: 牺牲原子性, 但保证能写进去。
            remove(temp_path);
            FILE *direct = fopen(path, "wb");
            if (!direct)
                return false;
            size_t direct_written = len ? fwrite(data, 1, len, direct) : 0;
            if ((len && direct_written != len) || fclose(direct) != 0)
                return false;
            return true;
        }
    }
    return true;
}

// 生成日志用的时间戳标签,精确到秒。系统时钟还没ready(比如刚开机那几秒)时,
// 退回到只有日期的 g_active_day,不让日志因为时间没准备好而写失败。
static void log_timestamp_tag(char *out, size_t out_size)
{
    if (g_time_ready) {
        u64 timestamp = 0;
        TimeCalendarTime cal;
        TimeCalendarAdditionalInfo info;

        if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &timestamp)) &&
            R_SUCCEEDED(timeToCalendarTimeWithMyRule(timestamp, &cal, &info)) &&
            cal.year >= 2020 && cal.year <= 2099) {
            snprintf(out, out_size, "%04u-%02u-%02u %02u:%02u:%02u",
                     cal.year, cal.month, cal.day, cal.hour, cal.minute, cal.second);
            return;
        }
    }
    snprintf(out, out_size, "%s --:--:--", g_active_day);
}

static void log_message(const char *fmt, ...)
{
    if (!g_sd_ready)
        return;

    ensure_playlog_dir();

    FILE *f = fopen(LOG_PATH, "a");
    if (!f)
        return;

    char tag[32];
    log_timestamp_tag(tag, sizeof(tag));
    fprintf(f, "[%s] ", tag);

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fputc('\n', f);
    fclose(f);
}

static bool read_json_string_field(const char *body, const char *field, char *out, size_t out_size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);

    char *key = strstr(body, pattern);
    if (!key)
        return false;
    char *colon = strchr(key, ':');
    if (!colon)
        return false;
    char *quote = strchr(colon, '"');
    if (!quote)
        return false;

    char *start = quote + 1;
    size_t len = 0;
    bool escape = false;
    for (char *p = start; *p && len < out_size - 1; p++) {
        if (escape) {
            out[len++] = *p;
            escape = false;
            continue;
        }
        if (*p == '\\') {
            escape = true;
            continue;
        }
        if (*p == '"') {
            out[len] = '\0';
            return true;
        }
        out[len++] = *p;
    }

    out[len] = '\0';
    return false;
}

static bool read_json_u32_field(const char *body, const char *field, u32 *out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);

    char *key = strstr(body, pattern);
    if (!key)
        return false;
    char *colon = strchr(key, ':');
    if (!colon)
        return false;

    char *num = colon + 1;
    while (*num == ' ' || *num == '\t' || *num == '\r' || *num == '\n')
        num++;

    char *end = NULL;
    unsigned long value = strtoul(num, &end, 10);
    if (end == num || value > 86400UL)
        return false;

    *out = (u32)value;
    return true;
}

//---------------------------------------------------------------------------------
// §12.2 服务端失败原因
//
// 面向设备的接口出错时统一返回 {"detail": "<中文原因>", "code": "<机器码>"}。
// sysmodule 没有屏幕, 所以这里取出的原因写进 limiter.log 与 api-*.log;
// 没有 detail 时退化为 HTTP 状态文案, 绝不让日志只留下一个状态码。
//---------------------------------------------------------------------------------
static void extract_server_failure_detail(const char *body, long http_code, char *out, size_t out_size)
{
    if (out_size == 0)
        return;
    out[0] = '\0';

    char raw[512];
    if (body && read_json_string_field(body, "detail", raw, sizeof(raw)) && raw[0]) {
        char clean[512];
        api_log_sanitize(raw, clean, sizeof(clean));
        snprintf(out, out_size, "%s", clean);
        return;
    }

    snprintf(out, out_size, "%s", http_status_text(http_code));
}

/// §13: 取出响应体里的 ASCII 机器码 `code`(只保留可见 ASCII 0x21..0x7E)。
/// 缺失/非法时输出空串, 由 api_log_line 记作 `-`。
/// sysmodule 没有屏幕, 所以这个码只用于让共享日志里带上可读的失败分类。
static void extract_server_failure_code(const char *body, char *out, size_t out_size)
{
    if (out_size == 0)
        return;
    out[0] = '\0';

    char raw[API_LOG_CODE_MAX];
    if (!body || !read_json_string_field(body, "code", raw, sizeof(raw)))
        return;

    size_t o = 0;
    for (size_t i = 0; raw[i] && o + 1 < out_size; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c >= 0x21 && c <= 0x7E)
            out[o++] = (char)c;
    }
    out[o] = '\0';
}

static void trim_server_base_url(void)
{
    size_t len = strlen(g_config.server_base_url);
    while (len > 0 && g_config.server_base_url[len - 1] == '/') {
        g_config.server_base_url[len - 1] = '\0';
        len--;
    }
}

static void build_endpoint_url(const char *path, char *out, size_t out_size)
{
    if (out_size == 0)
        return;

    size_t len = strlen(g_config.server_base_url);
    const char *slash = (len > 0 && g_config.server_base_url[len - 1] == '/') ? "" : "/";
    const char *clean_path = (path[0] == '/') ? path + 1 : path;
    snprintf(out, out_size, "%s%s%s", g_config.server_base_url, slash, clean_path);
}

static void write_app_config(void)
{
    if (!g_sd_ready)
        return;

    char out[2048];
    int needed = snprintf(out, sizeof(out),
            "{\n"
            "  \"server_base_url\": \"%s\",\n"
            "  \"device_id\": \"%s\",\n"
            "  \"device_id_type\": \"%s\",\n"
            "  \"upload_token\": \"%s\",\n"
            "  \"limit_reload_seconds\": %u,\n"
            "  \"config_sync_seconds\": %u,\n"
            "  \"upload_interval_seconds\": %u,\n"
            "  \"heartbeat_interval_seconds\": %u,\n"
            "  \"enforce_mode\": \"%s\"\n"
            "}\n",
            g_config.server_base_url,
            g_config.device_id,
            g_config.device_id_type,
            g_config.upload_token,
            g_config.limit_reload_seconds,
            g_config.config_sync_seconds,
            g_config.upload_interval_seconds,
            g_config.heartbeat_interval_seconds,
            g_config.enforce_mode);
    if (needed < 0 || (size_t)needed >= sizeof(out)) {
        log_message("refusing to write truncated config.json");
        return;
    }
    if (!write_file_atomic(CONFIG_PATH, out, (size_t)needed))
        log_message("failed to write config.json");
}

static void load_app_config(void)
{
    memset(&g_config, 0, sizeof(g_config));
    strncpy(g_config.server_base_url, DEFAULT_SERVER_BASE_URL, sizeof(g_config.server_base_url) - 1);
    g_config.limit_reload_seconds = DEFAULT_LIMIT_RELOAD_SECONDS;
    g_config.config_sync_seconds = DEFAULT_CONFIG_SYNC_SECONDS;
    g_config.upload_interval_seconds = DEFAULT_UPLOAD_INTERVAL_SECONDS;
    g_config.heartbeat_interval_seconds = DEFAULT_HEARTBEAT_INTERVAL_SECONDS;

    if (!g_sd_ready)
        return;

    char body[2048];
    bool exists = false;
    if (!read_whole_file(CONFIG_PATH, body, sizeof(body), &exists, NULL)) {
        // 文件存在但读失败: 绝不能重置为默认值,否则 upload_token 会被清空。
        if (exists)
            log_message("config.json unreadable; keeping in-memory config (no reset)");
        return;
    }

    read_json_string_field(body, "server_base_url", g_config.server_base_url, sizeof(g_config.server_base_url));
    read_json_string_field(body, "device_id", g_config.device_id, sizeof(g_config.device_id));
    read_json_string_field(body, "device_id_type", g_config.device_id_type, sizeof(g_config.device_id_type));
    read_json_string_field(body, "upload_token", g_config.upload_token, sizeof(g_config.upload_token));
    read_json_u32_field(body, "limit_reload_seconds", &g_config.limit_reload_seconds);
    read_json_u32_field(body, "config_sync_seconds", &g_config.config_sync_seconds);
    read_json_u32_field(body, "upload_interval_seconds", &g_config.upload_interval_seconds);
    read_json_u32_field(body, "heartbeat_interval_seconds", &g_config.heartbeat_interval_seconds);
    // §10: 字段缺失/为空 -> 默认 block_launch(老 SD 卡行为不变)。
    // 取值本身不在这里改写: 无法识别的值保持原样, 由 effective_enforce_mode()
    // 判为 Unknown 并选择"不终止"(fail-safe)。
    if (!read_json_string_field(body, "enforce_mode", g_config.enforce_mode, sizeof(g_config.enforce_mode)) ||
        !g_config.enforce_mode[0])
        strncpy(g_config.enforce_mode, ENFORCE_MODE_DEFAULT, sizeof(g_config.enforce_mode) - 1);

    if (g_config.limit_reload_seconds < 5)
        g_config.limit_reload_seconds = DEFAULT_LIMIT_RELOAD_SECONDS;
    if (g_config.config_sync_seconds < 5 || g_config.config_sync_seconds > 86400)
        g_config.config_sync_seconds = DEFAULT_CONFIG_SYNC_SECONDS;
    if (g_config.upload_interval_seconds < 30 || g_config.upload_interval_seconds > 86400)
        g_config.upload_interval_seconds = DEFAULT_UPLOAD_INTERVAL_SECONDS;
    if (g_config.heartbeat_interval_seconds < 60 || g_config.heartbeat_interval_seconds > 86400)
        g_config.heartbeat_interval_seconds = DEFAULT_HEARTBEAT_INTERVAL_SECONDS;
    trim_server_base_url();
}

static void apply_remote_app_settings(const char *body)
{
    u32 value = 0;
    bool changed = false;

    if (read_json_u32_field(body, "limit_reload_seconds", &value) && value >= 5 && value <= 3600) {
        g_config.limit_reload_seconds = value;
        changed = true;
    }
    if (read_json_u32_field(body, "config_sync_seconds", &value) && value >= 5 && value <= 86400) {
        g_config.config_sync_seconds = value;
        changed = true;
    }
    if (read_json_u32_field(body, "upload_interval_seconds", &value) && value >= 30 && value <= 86400) {
        g_config.upload_interval_seconds = value;
        changed = true;
    }
    if (read_json_u32_field(body, "heartbeat_interval_seconds", &value) && value >= 60 && value <= 86400) {
        g_config.heartbeat_interval_seconds = value;
        changed = true;
    }

    // §10 服务端下发的执行方式是**权威值**(本地 config.json 只是离线兜底)。
    // 原样保存、原样生效: 不认识的取值会走 Unknown -> 不终止。
    char mode[ENFORCE_MODE_MAX];
    mode[0] = '\0';
    if (read_json_string_field(body, "enforce_mode", mode, sizeof(mode)) && mode[0] &&
        strcmp(mode, g_config.enforce_mode) != 0) {
        strncpy(g_config.enforce_mode, mode, sizeof(g_config.enforce_mode) - 1);
        g_config.enforce_mode[sizeof(g_config.enforce_mode) - 1] = '\0';
        changed = true;
        log_message("enforce_mode updated by server: %s", g_config.enforce_mode);
    }

    if (changed) {
        write_app_config();
        log_message("updated intervals reload=%u sync=%u upload=%u heartbeat=%u enforce_mode=%s",
                    g_config.limit_reload_seconds, g_config.config_sync_seconds,
                    g_config.upload_interval_seconds, g_config.heartbeat_interval_seconds,
                    g_config.enforce_mode);
    }
}

static void bytes_to_hex(const u8 *in, size_t in_size, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    if (out_size < in_size * 2 + 1) {
        if (out_size > 0)
            out[0] = '\0';
        return;
    }

    for (size_t i = 0; i < in_size; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[in_size * 2] = '\0';
}

static void sha256_hex(const char *input, char *out, size_t out_size)
{
    u8 hash[SHA256_HASH_SIZE];
    sha256CalculateHash(hash, input ? input : "", strlen(input ? input : ""));
    bytes_to_hex(hash, sizeof(hash), out, out_size);
}

static void hmac_sha256_hex(const char *key, const char *message, char *out, size_t out_size)
{
    u8 mac[SHA256_HASH_SIZE];
    hmacSha256CalculateMac(mac, key, strlen(key), message, strlen(message));
    bytes_to_hex(mac, sizeof(mac), out, out_size);
}

static bool current_unix_timestamp(char *out, size_t out_size)
{
    if (!g_time_ready)
        return false;

    u64 timestamp = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &timestamp)))
        return false;

    snprintf(out, out_size, "%llu", (unsigned long long)timestamp);
    return true;
}

static void build_request_signature(const char *method, const char *path, const char *body,
                                    const char *timestamp, char *signature, size_t signature_size)
{
    char body_hash[SHA256_HASH_SIZE * 2 + 1];
    sha256_hex(body ? body : "", body_hash, sizeof(body_hash));

    char canonical[768];
    snprintf(canonical, sizeof(canonical), "%s\n%s\n%s\n%s\n%s",
             method, path, g_config.device_id, timestamp, body_hash);

    hmac_sha256_hex(g_config.upload_token, canonical, signature, signature_size);
}

static size_t write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t real_size = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;

    if (mem->size + real_size + 1 > MAX_CONFIG_SIZE)
        return 0;

    if (mem->size + real_size + 1 > mem->capacity) {
        size_t new_capacity = mem->capacity ? mem->capacity * 2 : 4096;
        while (new_capacity < mem->size + real_size + 1)
            new_capacity *= 2;
        if (new_capacity > MAX_CONFIG_SIZE)
            new_capacity = MAX_CONFIG_SIZE;

        char *ptr = realloc(mem->data, new_capacity);
        if (!ptr)
            return 0;
        mem->data = ptr;
        mem->capacity = new_capacity;
    }

    memcpy(mem->data + mem->size, contents, real_size);
    mem->size += real_size;
    mem->data[mem->size] = '\0';
    return real_size;
}

static void append_signed_headers(struct curl_slist **headers, const char *timestamp, const char *signature)
{
    char auth_header[256];
    char device_header[96];
    char timestamp_header[64];
    char signature_header[96];

    snprintf(auth_header, sizeof(auth_header),
             "Authorization: PlayLog-HMAC-SHA256 device_id=\"%s\",timestamp=\"%s\",signature=\"%s\"",
             g_config.device_id, timestamp, signature);
    snprintf(device_header, sizeof(device_header), "X-PlayLog-Device-Id: %s", g_config.device_id);
    snprintf(timestamp_header, sizeof(timestamp_header), "X-PlayLog-Timestamp: %s", timestamp);
    snprintf(signature_header, sizeof(signature_header), "X-PlayLog-Signature: %s", signature);

    *headers = curl_slist_append(*headers, "Accept: application/json");
    *headers = curl_slist_append(*headers, auth_header);
    *headers = curl_slist_append(*headers, device_header);
    *headers = curl_slist_append(*headers, timestamp_header);
    *headers = curl_slist_append(*headers, signature_header);
}

//---------------------------------------------------------------------------------
// CA 校验与响应签名校验(REFACTOR_SPEC §6 item 20)
//
// 响应签名约定(需服务端配合实现):
//   X-PlayLog-Response-Signature: <hex>
//   <hex> = hex(HMAC-SHA256(upload_token, <raw response body bytes>))
// 响应头带签名就必须校验通过;缺失时由调用方决定是否 fail closed。
//---------------------------------------------------------------------------------
static bool cainfo_available(void)
{
    FILE *ca = fopen(CAINFO_PATH, "rb");
    if (!ca) {
        log_message("missing %s; refusing to skip TLS certificate verification", CAINFO_PATH);
        return false;
    }
    fclose(ca);
    return true;
}

static void configure_tls_verification(CURL *curl)
{
    curl_easy_setopt(curl, CURLOPT_CAINFO, CAINFO_PATH);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

static size_t header_capture_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t real_size = size * nitems;
    ResponseSignature *sig = (ResponseSignature *)userdata;

    if (real_size > 29 && strncasecmp(buffer, "x-playlog-response-signature:", 29) == 0) {
        const char *value = buffer + 29;
        while (*value == ' ' || *value == '\t')
            value++;
        size_t len = 0;
        while (len < sizeof(sig->hex) - 1 && value[len] && value[len] != '\r' && value[len] != '\n') {
            sig->hex[len] = value[len];
            len++;
        }
        sig->hex[len] = '\0';
        sig->present = len > 0;
    } else if (real_size > 14 && strncasecmp(buffer, "authorization:", 14) == 0) {
        const char *marker = strstr(buffer, "signature=\"");
        if (marker) {
            marker += 11;
            size_t len = 0;
            while (len < sizeof(sig->hex) - 1 && marker[len] && marker[len] != '"') {
                sig->hex[len] = marker[len];
                len++;
            }
            sig->hex[len] = '\0';
            sig->present = len > 0;
        }
    }
    return real_size;
}

static bool constant_time_equals(const char *a, const char *b)
{
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    if (len_a == 0 || len_a != len_b)
        return false;

    unsigned char diff = 0;
    for (size_t i = 0; i < len_a; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

// -1 = 响应没有签名;0 = 签名不匹配;1 = 校验通过。
static int verify_response_signature(const char *body, size_t body_len, const ResponseSignature *sig)
{
    if (!sig->present || sig->hex[0] == '\0')
        return -1;
    if (!g_config.upload_token[0])
        return 0;

    u8 mac[SHA256_HASH_SIZE];
    hmacSha256CalculateMac(mac, g_config.upload_token, strlen(g_config.upload_token), body, body_len);

    char expected[SHA256_HASH_SIZE * 2 + 1];
    bytes_to_hex(mac, sizeof(mac), expected, sizeof(expected));

    return constant_time_equals(expected, sig->hex) ? 1 : 0;
}

static void retry_network_clients(void)
{
    if (!g_socket_ready) {
        // 注意: 这里不能用 socketInitializeDefault(),它的默认缓冲区配置是给
        // 普通应用准备的,会申请远超 INNER_HEAP_SIZE(4MB) 的传输内存,
        // 在 sysmodule 这种小堆场景下必定以 LibnxError_OutOfMemory 失败。
        Result rc = socketInitialize(&g_socketInitConfig);
        g_socket_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && !g_socket_ready)
            log_message("socketInitialize rc=0x%x", rc);
    }

    if (g_socket_ready && !g_curl_ready) {
        CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        g_curl_ready = (rc == CURLE_OK);
        if (g_sd_ready && !g_curl_ready)
            log_message("curl_global_init rc=%d", rc);
    }
}

// 统一的网络请求日志:
// - 发起前打一行 "-> METHOD url body=..."
// - 结束后打一行 "<- ..." 带上 dns/connect/pretransfer/total 耗时分段,
//   这样能直接看出10秒超时到底是卡在DNS解析、TCP连接、还是等服务端响应,
//   而不是只有一个笼统的 "Timeout was reached"。
static void log_nifm_status(const char *tag)
{
    if (!g_nifm_ready) {
        log_message("%s nifm not ready, skipping status check", tag);
        return;
    }
    NifmInternetConnectionType conn_type = 0;
    u32 wifi_strength = 0;
    NifmInternetConnectionStatus conn_status = 0;
    Result rc = nifmGetInternetConnectionStatus(&conn_type, &wifi_strength, &conn_status);
    log_message("%s nifm status rc=0x%x type=%u strength=%u status=%u",
                tag, rc, (unsigned)conn_type, wifi_strength, (unsigned)conn_status);
}

static void log_curl_request(const char *tag, const char *method, const char *url, const char *body)
{
    log_message("%s -> %s %s body=%.200s", tag, method, url, (body && body[0]) ? body : "(none)");
}

static void log_curl_result(const char *tag, CURL *curl, CURLcode res, long http_code,
                             const char *curl_err, MemoryBuffer *response)
{
    double dns_t = 0, connect_t = 0, pretransfer_t = 0, total_t = 0;
    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &dns_t);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect_t);
    curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME, &pretransfer_t);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_t);

    char *primary_ip = NULL;
    long primary_port = 0;
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &primary_ip);
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_PORT, &primary_port);

    if (res != CURLE_OK) {
        log_message("%s <- curl_error=%d(%s) detail=\"%s\" resolved_to=%s:%ld dns=%.3fs connect=%.3fs pretransfer=%.3fs total=%.3fs",
                     tag, (int)res, curl_easy_strerror(res),
                     (curl_err && curl_err[0]) ? curl_err : "(no detail)",
                     primary_ip ? primary_ip : "(none)", primary_port,
                     dns_t, connect_t, pretransfer_t, total_t);
    } else {
        const char *resp_body = (response && response->data) ? response->data : "(empty)";
        log_message("%s <- http=%ld resolved_to=%s:%ld dns=%.3fs connect=%.3fs pretransfer=%.3fs total=%.3fs body=%.300s",
                     tag, http_code, primary_ip ? primary_ip : "(none)", primary_port,
                     dns_t, connect_t, pretransfer_t, total_t, resp_body);
    }
}

//---------------------------------------------------------------------------------
// §14.3 采纳服务端权威的"今日已玩"作为本地日账本的**下限**
//
// 为什么需要: 本地日账本只从"模块运行期间"累计。若中途把模块从 SD 卡拿掉、或状态文件
// 丢失, 那段时间的游玩不会进入本地账本 -> 账本比真实值小 -> 超限判定偏松(线上 bug:
// 面板显示已超额, 主机还能玩)。服务端按 (游戏, 本地日) 对各上报来源取 max 去重, 是权威值。
//
// 铁律: **只许抬高, 绝不允许降低**。字段缺失/为 0/解析失败/数值异常时一律保持本地值不动,
// 否则就可能反过来把额度"解锁"。
//---------------------------------------------------------------------------------

// 从 open ('{') 开始找到与之配对的 '}', 返回 open; 失败返回 NULL。字符串内的括号不计数。
static const char *json_match_brace(const char *open, const char **out_end)
{
    if (!open || *open != '{')
        return NULL;

    int depth = 0;
    bool in_string = false;
    for (const char *q = open; *q; q++) {
        char c = *q;
        if (in_string) {
            if (c == '\\' && q[1]) { q++; continue; }
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) { *out_end = q; return open; }
        }
    }
    return NULL;
}

// 定位 "key" 之后的第一个对象, 返回 '{' 起始位置。找不到返回 NULL。
static const char *json_object_after_key(const char *body, const char *key, const char **out_end)
{
    if (!body || !key)
        return NULL;
    const char *p = strstr(body, key);
    if (!p)
        return NULL;
    p = strchr(p + strlen(key), '{');
    if (!p)
        return NULL;
    return json_match_brace(p, out_end);
}

typedef struct {
    const char *account_id;
} AdoptUsedTodayCtx;

// 遍历 {"name": <number>, ...}; 值不是纯数字(嵌套对象/null/字符串)就跳过该成员。
static void json_foreach_number(const char *start, const char *end,
                                void (*cb)(const char *name, unsigned long long value, void *ctx),
                                void *ctx)
{
    const char *p = start;
    while (p && p < end && cb) {
        const char *key_open = memchr(p, '"', (size_t)(end - p));
        if (!key_open) break;
        const char *key_close = memchr(key_open + 1, '"', (size_t)(end - key_open - 1));
        if (!key_close) break;

        size_t key_len = (size_t)(key_close - key_open - 1);
        const char *colon = memchr(key_close + 1, ':', (size_t)(end - key_close - 1));
        if (!colon) break;

        p = colon + 1;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p >= end) break;

        if (*p >= '0' && *p <= '9') {
            unsigned long long value = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                value = value * 10ULL + (unsigned long long)(*p - '0');
                p++;
            }
            if (key_len > 0 && key_len < 40) {
                char name[40];
                memcpy(name, key_open + 1, key_len);
                name[key_len] = '\0';
                cb(name, value, ctx);
            }
        } else {
            p++;   // 不是数字: 跳过, 继续找下一个成员
        }
    }
}

static void adopt_used_today_one(const char *account_id, const char *title_hex, unsigned long long seconds)
{
    if (!title_hex || strlen(title_hex) != 16)
        return;
    // 0 或异常大的值一律忽略(忽略 == 保持本地值, 不会因此放宽)。
    if (seconds == 0 || seconds > 86400ULL)
        return;

    u64 title_id = strtoull(title_hex, NULL, 16);
    if (title_id == 0)
        return;

    UsageEntry *usage = get_usage_entry(account_id, title_id);
    if (!usage)
        return;
    if ((unsigned long long)usage->used_seconds >= seconds)
        return;                                 // 本地已经更高: 不动

    log_message("§14.3 adopted server used_today title=%016lX account=%s local=%u server=%llu",
                title_id, (account_id && account_id[0]) ? account_id : "(device)",
                usage->used_seconds, seconds);
    usage->used_seconds = (u32)seconds;
    g_state_dirty = true;
}

static void adopt_used_today_cb(const char *name, unsigned long long value, void *ctx)
{
    AdoptUsedTodayCtx *c = (AdoptUsedTodayCtx *)ctx;
    adopt_used_today_one(c->account_id, name, value);
}

static void adopt_server_used_today(const char *body)
{
    if (!body || !g_active_day[0])
        return;                                 // 没有当天日期键时不能写入账本

    const char *end = NULL;
    const char *obj = json_object_after_key(body, "\"used_today\"", &end);
    if (obj) {
        AdoptUsedTodayCtx ctx;
        ctx.account_id = "";
        json_foreach_number(obj + 1, end, adopt_used_today_cb, &ctx);
    }

    const char *aend = NULL;
    const char *aobj = json_object_after_key(body, "\"account_used_today\"", &aend);
    if (!aobj)
        return;

    const char *p = aobj + 1;
    while (p < aend) {
        const char *key_open = memchr(p, '"', (size_t)(aend - p));
        if (!key_open) break;
        const char *key_close = memchr(key_open + 1, '"', (size_t)(aend - key_open - 1));
        if (!key_close) break;
        size_t key_len = (size_t)(key_close - key_open - 1);

        const char *inner_open = memchr(key_close + 1, '{', (size_t)(aend - key_close - 1));
        if (!inner_open) break;
        const char *inner_end = NULL;
        if (!json_match_brace(inner_open, &inner_end)) break;

        if (key_len > 0 && key_len < 33) {
            char account_id[33];
            memcpy(account_id, key_open + 1, key_len);
            account_id[key_len] = '\0';
            AdoptUsedTodayCtx ctx;
            ctx.account_id = account_id;
            json_foreach_number(inner_open + 1, inner_end, adopt_used_today_cb, &ctx);
        }
        p = inner_end + 1;
    }
}

static bool sync_limits_from_server(void)
{
    if (!g_sd_ready)
        return false;

    load_app_config();
    if (!g_config.device_id[0] || !g_config.upload_token[0]) {
        log_message("skip config sync: device id or token missing");
        return false;
    }

    retry_network_clients();
    if (!g_curl_ready)
        return false;

    if (!cainfo_available())
        return false;

    char timestamp[32];
    if (!current_unix_timestamp(timestamp, sizeof(timestamp))) {
        log_message("skip config sync: no system time");
        return false;
    }

    char signature[SHA256_HASH_SIZE * 2 + 1];
    build_request_signature("GET", CONFIG_FETCH_PATH, "", timestamp, signature, sizeof(signature));

    CURL *curl = curl_easy_init();
    if (!curl) {
        log_message("curl_easy_init failed");
        return false;
    }

    MemoryBuffer response = {0};
    ResponseSignature response_sig = {0};
    struct curl_slist *headers = NULL;
    append_signed_headers(&headers, timestamp, signature);

    char url[320];
    build_endpoint_url(CONFIG_FETCH_PATH, url, sizeof(url));
    log_nifm_status("config_sync");
    log_curl_request("config_sync", "GET", url, NULL);

    char curl_err[CURL_ERROR_SIZE];
    curl_err[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_capture_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_sig);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err);
    // Switch 系统对 IPv6 出网支持很有限,域名解析到 IPv6 地址后 connect 经常直接卡死到超时,
    // 而不是快速失败,强制只用 IPv4 可以避免这种情况。
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    configure_tls_verification(curl);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    log_curl_result("config_sync", curl, res, http_code, curl_err, &response);

    bool ok = false;
    if (res == CURLE_OK && http_code >= 200 && http_code < 300 && response.data && response.size > 0) {
        // limits.json 覆盖属于"可恢复"操作: 无签名时照常应用但要大声记录;
        // 有签名则必须校验通过才允许覆盖(item 20)。
        int sig_state = verify_response_signature(response.data, response.size, &response_sig);
        if (sig_state == 0) {
            log_message("CONFIG RESPONSE SIGNATURE MISMATCH; refusing to overwrite limits.json");
            api_log_line(API_LOG_TAG_CONFIG, "GET", CONFIG_FETCH_PATH, http_code,
                         NULL, "response signature mismatch; limits not applied");
        } else {
            if (sig_state < 0)
                log_message("WARNING: config response has no signature; limits.json update is unverified");
            if (write_file_atomic(LIMITS_PATH, response.data, response.size)) {
                apply_remote_app_settings(response.data);
                // §19: 先把服务端时间标记为可信并记下服务端本地日, 再刷新日/周账本。
                // 顺序很重要: 后面的 load_limits / adopt_server_used_today 都会把
                // "今日"写进本地账本/下限, 必须落在服务端日上(主机时钟不准也不受影响)。
                g_server_synced_at_mono = monotonic_seconds();
                apply_server_clock_hint(response.data);
                refresh_day_key();
                load_limits();
                // §14.3: 用服务端权威的"今日已玩"把本地日账本抬到不低于真实值
                // (只抬不降), 修复"中途新增限额/本地状态丢失 -> 超限判定偏松"。
                adopt_server_used_today(response.data);
                if (g_state_dirty)
                    save_state();
                log_message("synced server config bytes=%u", (unsigned)response.size);
                api_log_line(API_LOG_TAG_CONFIG, "GET", CONFIG_FETCH_PATH, http_code, NULL, "OK");
                ok = true;
            } else {
                // 写盘失败已由 write_file_atomic 记进接口日志(write tag)。
                log_message("failed to write limits.json");
            }
        }
    } else if (res != CURLE_OK) {
        // §12.3: curl 层失败记 net。
        log_message("config sync curl error=%d(%s)", (int)res, curl_easy_strerror(res));
        api_log_line(API_LOG_TAG_NET, "GET", CONFIG_FETCH_PATH, 0, NULL, curl_easy_strerror(res));
    } else if (http_code >= 200 && http_code < 300) {
        log_message("config sync got empty 2xx body; nothing to apply");
        api_log_line(API_LOG_TAG_CONFIG, "GET", CONFIG_FETCH_PATH, http_code, NULL, "empty response body");
    } else {
        // §12.2/§12.3/§13: detail(中文)写日志文件, ASCII 机器码一起带上。
        char reason[512];
        char code[API_LOG_CODE_MAX];
        extract_server_failure_detail(response.data, http_code, reason, sizeof(reason));
        extract_server_failure_code(response.data, code, sizeof(code));
        log_message("config sync failed http=%ld code=%s detail=%s", http_code,
                    code[0] ? code : "-", reason);
        api_log_line(API_LOG_TAG_CONFIG, "GET", CONFIG_FETCH_PATH, http_code, code, reason);
    }

    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

static void random_hex8(char *out, size_t out_size)
{
    u8 buf[4];
    randomGet(buf, sizeof(buf));
    bytes_to_hex(buf, sizeof(buf), out, out_size);
}

//---------------------------------------------------------------------------------
// 游戏名解析(REFACTOR_SPEC §4)
// 用 ns:GetApplicationControlData 取 NACP 语言名,失败才回退 "Title <TID>"。
// 这是"未知游戏"的根因修复: 以前只上报占位名,服务端 usable_game_name() 判为不可用。
//---------------------------------------------------------------------------------
static bool resolve_title_name(u64 title_id, char *out, size_t out_size)
{
    out[0] = '\0';
    if (!g_ns_ready)
        return false;

    NsApplicationControlData *control = malloc(sizeof(NsApplicationControlData));
    if (!control)
        return false;

    u64 actual_size = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, title_id,
                                            control, sizeof(NsApplicationControlData), &actual_size);
    bool ok = false;
    if (R_SUCCEEDED(rc)) {
        NacpLanguageEntry *lang = NULL;
        // nacpGetLanguageEntry 按系统语言优先并自动回退;仍为空时手工找第一个非空项。
        if (R_SUCCEEDED(nacpGetLanguageEntry(&control->nacp, &lang)) && lang && lang->name[0]) {
            copy_fixed_string(out, out_size, lang->name, sizeof(lang->name));
        } else {
            for (int i = 0; i < 16; i++) {
                if (control->nacp.lang[i].name[0]) {
                    copy_fixed_string(out, out_size, control->nacp.lang[i].name,
                                      sizeof(control->nacp.lang[i].name));
                    break;
                }
            }
        }
        ok = out[0] != '\0';
    }
    free(control);
    return ok;
}

// 带缓存的游戏名查询。四槽轮转,避免调用方同时持有多个结果时互相覆盖。
static const char *get_title_name_cached(u64 title_id)
{
    static char result[4][0x201];
    static size_t next_slot = 0;
    char *out = result[next_slot];
    next_slot = (next_slot + 1) % 4;

    for (size_t i = 0; i < g_title_name_count; i++) {
        if (g_title_names[i].title_id == title_id) {
            copy_fixed_string(out, 0x201, g_title_names[i].name, strlen(g_title_names[i].name));
            return out;
        }
    }

    char name[0x201];
    name[0] = '\0';
    if (!resolve_title_name(title_id, name, sizeof(name))) {
        snprintf(name, sizeof(name), "Title %016lX", title_id);
        log_message("title name unresolved title=%016lX, using placeholder", title_id);
    }

    if (g_title_name_count < TITLE_NAME_CACHE_SIZE) {
        TitleNameCacheEntry *entry = &g_title_names[g_title_name_count++];
        entry->title_id = title_id;
        copy_fixed_string(entry->name, sizeof(entry->name), name, strlen(name));
        log_message("resolved title name title=%016lX name=%s", title_id, entry->name);
    }

    copy_fixed_string(out, 0x201, name, strlen(name));
    return out;
}

//---------------------------------------------------------------------------------
// 待上传条目(键: 账号 + 游戏)。item 17: 表满时淘汰零值项,而不是静默停止记账。
//---------------------------------------------------------------------------------
static UploadEntry *get_upload_entry(const char *account_id, u64 title_id)
{
    if (!account_id)
        account_id = "";

    for (size_t i = 0; i < g_upload_count; i++) {
        if (g_upload[i].title_id == title_id && strcmp(g_upload[i].account_id, account_id) == 0)
            return &g_upload[i];
    }

    if (g_upload_count >= MAX_USAGE) {
        for (size_t i = 0; i < g_upload_count; i++) {
            UploadEntry *candidate = &g_upload[i];
            if (candidate->pending_seconds == 0 && candidate->pending_launches == 0 &&
                candidate->record_uid[0] == '\0') {
                memmove(&g_upload[i], &g_upload[i + 1], (g_upload_count - i - 1) * sizeof(UploadEntry));
                g_upload_count--;
                break;
            }
        }
        if (g_upload_count >= MAX_USAGE) {
            log_message("upload table full (%u entries); cannot record account=%s title=%016lX",
                        (unsigned)g_upload_count, account_id, title_id);
            return NULL;
        }
    }

    UploadEntry *entry = &g_upload[g_upload_count++];
    memset(entry, 0, sizeof(*entry));
    copy_fixed_string(entry->account_id, sizeof(entry->account_id), account_id, strlen(account_id));
    entry->title_id = title_id;
    g_upload_dirty = true;
    return entry;
}

static void add_upload_seconds(const char *account_id, u64 title_id, u32 seconds)
{
    if (seconds == 0)
        return;
    UploadEntry *entry = get_upload_entry(account_id, title_id);
    if (!entry)
        return;
    if (0xFFFFFFFFU - entry->pending_seconds < seconds)
        entry->pending_seconds = 0xFFFFFFFFU;
    else
        entry->pending_seconds += seconds;
    if (0xFFFFFFFFFFFFFFFFULL - entry->total_seconds < seconds)
        entry->total_seconds = 0xFFFFFFFFFFFFFFFFULL;
    else
        entry->total_seconds += seconds;
    g_upload_dirty = true;
}

static void add_upload_launch(const char *account_id, u64 title_id)
{
    UploadEntry *entry = get_upload_entry(account_id, title_id);
    if (!entry)
        return;
    if (entry->launch_count < 0xFFFFFFFFU)
        entry->launch_count++;
    if (entry->pending_launches < 0xFFFFFFFFU)
        entry->pending_launches++;
    g_upload_dirty = true;
}

// item 16: record_uid 在首次发送前生成并持久化,重试复用同一个值。
static void ensure_upload_record_uid(UploadEntry *entry)
{
    if (entry->record_uid[0])
        return;

    char nonce[9];
    random_hex8(nonce, sizeof(nonce));
    snprintf(entry->record_uid, sizeof(entry->record_uid), "sys-%016llX-%s-%016lX",
             (unsigned long long)g_uid_epoch, nonce, entry->title_id);
    if (g_uid_epoch < 0xFFFFFFFFFFFFFFFFULL)
        g_uid_epoch++;
    g_uid_epoch_dirty = true;
    g_upload_dirty = true;
}

// 本批次实际写入 body 的条目下标(item 18: 成功回调只清除这些)。
static size_t g_uploaded_indices[MAX_USAGE];
static size_t g_uploaded_count = 0;

// 把字符串转义成「JSON 字符串字面量内容」(不含首尾引号),便于用 snprintf 直接拼接。
// 游戏名现在来自 NACP 真实名称,可能包含 " 或 \ ,不转义会产出非法 JSON 导致整批上报被拒。
static void json_escape_into(const char *in, char *out, size_t out_size)
{
    if (out_size == 0)
        return;

    size_t o = 0;
    for (const char *p = in ? in : ""; *p != '\0' && o + 3 < out_size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static bool build_upload_body(char *body, size_t body_size, char *timestamp, size_t timestamp_size)
{
    if (!current_unix_timestamp(timestamp, timestamp_size)) {
        log_message("skip auto upload: no system time");
        return false;
    }

    int pn = snprintf(body, body_size,
             "{\"device_id\":\"%s\",\"device_id_type\":\"%s\",\"games\":[",
             g_config.device_id, g_config.device_id_type);
    if (pn < 0 || (size_t)pn >= body_size) {
        log_message("upload body prefix too long; aborting");
        return false;
    }

    size_t used = (size_t)pn;
    size_t entry_count = 0;
    g_uploaded_count = 0;

    for (size_t i = 0; i < g_upload_count; i++) {
        if (g_upload[i].pending_seconds == 0 && g_upload[i].pending_launches == 0)
            continue;

        ensure_upload_record_uid(&g_upload[i]);
        const char *name = get_title_name_cached(g_upload[i].title_id);

        // 真实游戏名可能含 " 或 \(例如带引号的标题),必须转义后再拼进 JSON。
        // 超长时安全截断(宁可名字短一点,也不能产出非法 JSON)。
        char safe_name[512];
        json_escape_into(name, safe_name, sizeof(safe_name));

        // §8: 标记来源。这里的数字来自 sysmodule 自己的前台计时(不是 PDM),
        // 服务端按 (game, 本地日) 对每个来源取 max, 消除与前台上报的双计。
        char entry[1024];
        int en;
        if (g_upload[i].account_id[0]) {
            en = snprintf(entry, sizeof(entry),
                     "%s{\"record_uid\":\"%s\",\"title_id\":\"%016lX\",\"name\":\"%s\","
                     "\"source\":\"" UPLOAD_SOURCE_SYSMODULE "\","
                     "\"account_id\":\"%s\",\"delta_seconds\":%u,"
                     "\"client_total_seconds\":%llu,\"client_launch_count\":%u,"
                     "\"launch_delta\":%u}",
                     entry_count == 0 ? "" : ",",
                     g_upload[i].record_uid,
                     g_upload[i].title_id,
                     safe_name,
                     g_upload[i].account_id,
                     g_upload[i].pending_seconds,
                     (unsigned long long)g_upload[i].total_seconds,
                     g_upload[i].launch_count,
                     g_upload[i].pending_launches);
        } else {
            en = snprintf(entry, sizeof(entry),
                     "%s{\"record_uid\":\"%s\",\"title_id\":\"%016lX\",\"name\":\"%s\","
                     "\"source\":\"" UPLOAD_SOURCE_SYSMODULE "\","
                     "\"delta_seconds\":%u,"
                     "\"client_total_seconds\":%llu,\"client_launch_count\":%u,"
                     "\"launch_delta\":%u}",
                     entry_count == 0 ? "" : ",",
                     g_upload[i].record_uid,
                     g_upload[i].title_id,
                     safe_name,
                     g_upload[i].pending_seconds,
                     (unsigned long long)g_upload[i].total_seconds,
                     g_upload[i].launch_count,
                     g_upload[i].pending_launches);
        }
        if (en < 0 || (size_t)en >= sizeof(entry)) {
            log_message("upload entry would be truncated title=%016lX; aborting batch", g_upload[i].title_id);
            return false;
        }
        if (used + (size_t)en + 3 > body_size) {
            log_message("auto upload body too large entries=%u; sending fewer entries", (unsigned)entry_count);
            break;
        }

        memcpy(body + used, entry, (size_t)en);
        used += (size_t)en;
        body[used] = '\0';

        if (g_uploaded_count < MAX_USAGE)
            g_uploaded_indices[g_uploaded_count++] = i;
        entry_count++;
    }

    if (entry_count == 0)
        return false;

    memcpy(body + used, "]}", 2);
    used += 2;
    body[used] = '\0';

    // record_uid 已生成,必须在发送前落盘,重试才能复用同一个 uid。
    save_upload_state();
    return true;
}

static bool upload_pending_usage(void)
{
    if (!g_sd_ready)
        return false;

    load_app_config();
    if (!g_config.device_id[0] || !g_config.upload_token[0]) {
        log_message("skip auto upload: device id or token missing");
        return false;
    }

    retry_network_clients();
    if (!g_curl_ready) {
        log_message("skip auto upload: network/curl not ready socket=%u curl=%u",
                    g_socket_ready ? 1 : 0, g_curl_ready ? 1 : 0);
        return false;
    }

    if (!cainfo_available())
        return false;

    save_upload_state();

    char *body = malloc(MAX_UPLOAD_BODY_SIZE);
    if (!body) {
        log_message("auto upload malloc failed");
        return false;
    }
    body[0] = '\0';

    char timestamp[32];
    if (!build_upload_body(body, MAX_UPLOAD_BODY_SIZE, timestamp, sizeof(timestamp))) {
        log_message("skip auto upload: no pending usage entries count=%u",
                    (unsigned)g_upload_count);
        free(body);
        return false;
    }

    char signature[SHA256_HASH_SIZE * 2 + 1];
    build_request_signature("POST", PUSH_PATH, body, timestamp, signature, sizeof(signature));

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(body);
        log_message("auto upload curl_easy_init failed");
        return false;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    append_signed_headers(&headers, timestamp, signature);
    MemoryBuffer response = {0};

    char url[320];
    build_endpoint_url(PUSH_PATH, url, sizeof(url));
    log_nifm_status("auto_upload");
    log_curl_request("auto_upload", "POST", url, body);

    char curl_err[CURL_ERROR_SIZE];
    curl_err[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    configure_tls_verification(curl);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    log_curl_result("auto_upload", curl, res, http_code, curl_err, &response);

    bool ok = false;
    if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
        // item 18: 只清除真正进入本批次 body 的条目,绝不清全部 pending。
        for (size_t k = 0; k < g_uploaded_count; k++) {
            size_t idx = g_uploaded_indices[k];
            if (idx >= g_upload_count)
                continue;
            g_upload[idx].pending_seconds = 0;
            g_upload[idx].pending_launches = 0;
            g_upload[idx].record_uid[0] = '\0';   // 已确认送达,下次重新生成
        }
        g_upload_dirty = true;
        save_upload_state();
        log_message("auto upload success http=%ld entries=%u",
                    http_code, (unsigned)g_uploaded_count);
        api_log_line(API_LOG_TAG_PUSH, "POST", PUSH_PATH, http_code, NULL, "OK");
        ok = true;
    } else if (res != CURLE_OK) {
        // §12.3: curl 层失败记 net。
        log_message("auto upload curl error=%d(%s)", (int)res, curl_easy_strerror(res));
        api_log_line(API_LOG_TAG_NET, "POST", PUSH_PATH, 0, NULL, curl_easy_strerror(res));
    } else {
        // §12.2/§12.3/§13: detail(中文)写日志文件, ASCII 机器码一起带上。
        char reason[512];
        char code[API_LOG_CODE_MAX];
        extract_server_failure_detail(response.data, http_code, reason, sizeof(reason));
        extract_server_failure_code(response.data, code, sizeof(code));
        log_message("auto upload failed http=%ld code=%s detail=%s", http_code,
                    code[0] ? code : "-", reason);
        api_log_line(API_LOG_TAG_PUSH, "POST", PUSH_PATH, http_code, code, reason);
    }

    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    return ok;
}

static void json_append(char *body, size_t body_size, const char *text)
{
    strncat(body, text, body_size - strlen(body) - 1);
}

static void json_append_escaped_string(char *body, size_t body_size, const char *text)
{
    json_append(body, body_size, "\"");
    for (const char *p = text ? text : ""; *p; p++) {
        char c = *p;
        char tmp[8];
        if (c == '"' || c == '\\') {
            snprintf(tmp, sizeof(tmp), "\\%c", c);
            json_append(body, body_size, tmp);
        } else if ((unsigned char)c >= 0x20) {
            tmp[0] = c;
            tmp[1] = '\0';
            json_append(body, body_size, tmp);
        }
    }
    json_append(body, body_size, "\"");
}

static void copy_fixed_string(char *out, size_t out_size, const char *in, size_t in_size)
{
    size_t len = 0;
    while (len < in_size && in[len] != '\0')
        len++;
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, in, len);
    out[len] = '\0';
}

static bool detect_atmosphere(void)
{
    u64 exosphere_api_version = 0;
    Result rc = splInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = splGetConfig(SPL_CONFIG_ITEM_EXOSPHERE_API_VERSION, &exosphere_api_version);
        splExit();
        if (R_SUCCEEDED(rc))
            return true;
    }

    return hosversionIsAtmosphere();
}

static void append_system_heartbeat_json(char *body, size_t body_size)
{
    char firmware_version[32] = "";
    char firmware_display_version[64] = "";
    char firmware_display_title[160] = "";

    SetSysFirmwareVersion fw;
    Result rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = setsysGetFirmwareVersion(&fw);
        setsysExit();
        if (R_SUCCEEDED(rc)) {
            snprintf(firmware_version, sizeof(firmware_version), "%u.%u.%u",
                     fw.major, fw.minor, fw.micro);
            copy_fixed_string(firmware_display_version, sizeof(firmware_display_version),
                              fw.display_version, sizeof(fw.display_version));
            copy_fixed_string(firmware_display_title, sizeof(firmware_display_title),
                              fw.display_title, sizeof(fw.display_title));
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        }
    }

    json_append(body, body_size, ",\"system\":{\"firmware_version\":");
    json_append_escaped_string(body, body_size, firmware_version);
    json_append(body, body_size, ",\"firmware_display_version\":");
    json_append_escaped_string(body, body_size, firmware_display_version);
    json_append(body, body_size, ",\"firmware_display_title\":");
    json_append_escaped_string(body, body_size, firmware_display_title);
    json_append(body, body_size, ",\"atmosphere_detected\":");
    json_append(body, body_size, detect_atmosphere() ? "true" : "false");
    json_append(body, body_size, ",\"atmosphere_version\":\"\"}");
}

static void append_battery_heartbeat_json(char *body, size_t body_size)
{
    if (!g_psm_ready) {
        log_message("heartbeat battery unavailable: psm not ready");
        return;
    }

    bool first = true;
    char entry[96];
    json_append(body, body_size, ",\"battery\":{");

    u32 percent = 0;
    Result percent_rc = psmGetBatteryChargePercentage(&percent);
    if (R_SUCCEEDED(percent_rc)) {
        snprintf(entry, sizeof(entry), "\"percent\":%u", percent);
        json_append(body, body_size, entry);
        first = false;
    }

    double raw_percent = 0.0;
    Result raw_rc = psmGetRawBatteryChargePercentage(&raw_percent);
    if (R_SUCCEEDED(raw_rc)) {
        snprintf(entry, sizeof(entry), "%s\"raw_percent\":%.1f", first ? "" : ",", raw_percent);
        json_append(body, body_size, entry);
        first = false;
    }

    double age_percent = 0.0;
    Result age_rc = psmGetBatteryAgePercentage(&age_percent);
    if (R_SUCCEEDED(age_rc)) {
        snprintf(entry, sizeof(entry), "%s\"age_percent\":%.1f", first ? "" : ",", age_percent);
        json_append(body, body_size, entry);
        first = false;
    }

    PsmChargerType charger_type;
    Result charger_rc = psmGetChargerType(&charger_type);
    if (R_SUCCEEDED(charger_rc)) {
        snprintf(entry, sizeof(entry), "%s\"charger_type\":%u", first ? "" : ",", (unsigned)charger_type);
        json_append(body, body_size, entry);
        first = false;
    }

    PsmBatteryVoltageState voltage_state;
    Result voltage_state_rc = psmGetBatteryVoltageState(&voltage_state);
    if (R_SUCCEEDED(voltage_state_rc)) {
        snprintf(entry, sizeof(entry), "%s\"voltage_state\":%u", first ? "" : ",", (unsigned)voltage_state);
        json_append(body, body_size, entry);
        first = false;
    }

#ifdef PLAYLOG_HAVE_PSM_CHARGE_INFO
    PsmBatteryChargeInfoFields fields;
    Result charge_info_rc = psmGetBatteryChargeInfoFields(&fields);
    if (R_SUCCEEDED(charge_info_rc)) {
        snprintf(entry, sizeof(entry), "%s\"charging\":%s", first ? "" : ",", fields.battery_charging ? "true" : "false");
        json_append(body, body_size, entry);
        snprintf(entry, sizeof(entry), ",\"voltage_mv\":%u", fields.battery_charge_milli_voltage);
        json_append(body, body_size, entry);
        snprintf(entry, sizeof(entry), ",\"temperature_milli_celsius\":%u", fields.temperature_celcius);
        json_append(body, body_size, entry);
        if (R_FAILED(age_rc)) {
            snprintf(entry, sizeof(entry), ",\"age_percent\":%.1f", (double)fields.battery_age_percentage / 1000.0);
            json_append(body, body_size, entry);
        }
        first = false;
    }
#else
    Result charge_info_rc = 0;
#endif

    json_append(body, body_size, "}");

    if (R_FAILED(percent_rc) || R_FAILED(raw_rc) || R_FAILED(age_rc) ||
        R_FAILED(charger_rc) || R_FAILED(voltage_state_rc) || R_FAILED(charge_info_rc)) {
        log_message("heartbeat battery partial percent=0x%x raw=0x%x age=0x%x charger=0x%x voltage_state=0x%x charge_info=0x%x",
                    percent_rc, raw_rc, age_rc, charger_rc, voltage_state_rc, charge_info_rc);
    }
}

static void append_storage_volume_json(char *body, size_t body_size, const char *name, NcmStorageId storage_id, bool *first)
{
    if (!g_ns_ready)
        return;

    s64 total = 0;
    s64 free = 0;
    if (R_FAILED(nsGetStorageSize(storage_id, &total, &free)) || total < 0 || free < 0)
        return;

    char entry[160];
    snprintf(entry, sizeof(entry), "%s\"%s\":{\"total_bytes\":%lld,\"free_bytes\":%lld}",
             *first ? "" : ",", name, (long long)total, (long long)free);
    json_append(body, body_size, entry);
    *first = false;
}

static void append_storage_heartbeat_json(char *body, size_t body_size)
{
    bool first = true;
    json_append(body, body_size, ",\"storage\":{");
    append_storage_volume_json(body, body_size, "sd_card", NcmStorageId_SdCard, &first);
    append_storage_volume_json(body, body_size, "nand_user", NcmStorageId_BuiltInUser, &first);
    json_append(body, body_size, "}");
}

static void append_network_heartbeat_json(char *body, size_t body_size)
{
    if (!g_nifm_ready)
        return;

    NifmInternetConnectionType conn_type = 0;
    u32 wifi_strength = 0;
    NifmInternetConnectionStatus conn_status = 0;
    u32 current_ip = 0;
    Result status_rc = nifmGetInternetConnectionStatus(&conn_type, &wifi_strength, &conn_status);
    Result ip_rc = nifmGetCurrentIpAddress(&current_ip);
    if (R_FAILED(status_rc) && R_FAILED(ip_rc))
        return;

    char entry[160];
    json_append(body, body_size, ",\"network\":{");
    bool first = true;
    if (R_SUCCEEDED(status_rc)) {
        snprintf(entry, sizeof(entry), "\"internet_connection_type\":%u,\"internet_connection_status\":%u,\"wifi_strength\":%u",
                 (unsigned)conn_type, (unsigned)conn_status, wifi_strength);
        json_append(body, body_size, entry);
        first = false;
    }
    if (R_SUCCEEDED(ip_rc)) {
        snprintf(entry, sizeof(entry), "%s\"current_ip_address\":%u", first ? "" : ",", current_ip);
        json_append(body, body_size, entry);
    }
    json_append(body, body_size, "}");
}

static bool build_heartbeat_body(char *body, size_t body_size, char *timestamp, size_t timestamp_size, u64 current_title_id)
{
    if (!current_unix_timestamp(timestamp, timestamp_size)) {
        log_message("skip heartbeat: no system time");
        return false;
    }

    char foreground[17] = "";
    if (current_title_id)
        snprintf(foreground, sizeof(foreground), "%016lX", current_title_id);
    else
        log_message("heartbeat foreground title empty");

    // 附带当前判定到的账号,便于服务端核对"当前账号"判定是否准确(§5)。
    char account_field[64] = "";
    if (g_current_account[0])
        snprintf(account_field, sizeof(account_field), ",\"current_account_id\":\"%s\"", g_current_account);

    snprintf(body, body_size,
             "{\"device_id\":\"%s\",\"device_id_type\":\"%s\","
             "\"client_name\":\"PlayLogLimiter\",\"client_version\":\"1.0.0\","
             "\"uptime_seconds\":%llu,\"foreground_title_id\":\"%s\"%s",
             g_config.device_id,
             g_config.device_id_type,
             (unsigned long long)monotonic_seconds(),
             foreground,
             account_field);

    append_battery_heartbeat_json(body, body_size);
    append_system_heartbeat_json(body, body_size);
    append_storage_heartbeat_json(body, body_size);
    append_network_heartbeat_json(body, body_size);
    append_focus_heartbeat_json(body, body_size);   // §9 前台焦点诊断
    json_append(body, body_size, "}");
    return true;
}

// item 20: reboot/shutdown 属于破坏性动作,必须在响应签名校验通过后才执行。
// 服务端目前可能还没有对响应签名,因此这里 fail closed(拒绝执行并大声记日志),
// 而不是冒着伪造响应把主机重启/关机的风险。limits.json 这类可恢复的写操作
// 则在无签名时"大声警告后继续"(见 sync_limits_from_server)。
static void execute_device_command_response(const char *response_body, const ResponseSignature *sig)
{
    if (!response_body || !strstr(response_body, "\"command\""))
        return;

    char action[32] = "";
    if (!read_json_string_field(response_body, "action", action, sizeof(action)) || !action[0])
        return;

    if (strcmp(action, "reboot") != 0 && strcmp(action, "shutdown") != 0) {
        log_message("ignored unsupported command action=%s", action);
        return;
    }

    static char signed_body[MAX_CONFIG_SIZE + 1];
    copy_fixed_string(signed_body, sizeof(signed_body), response_body, strlen(response_body));

    {
        int sig_state = verify_response_signature(signed_body, strlen(signed_body), sig);
        if (sig_state < 0) {
            log_message("REFUSING destructive command action=%s: response is not signed", action);
            return;
        }
        if (sig_state == 0) {
            log_message("REFUSING destructive command action=%s: response signature mismatch", action);
            return;
        }
    }

    if (!g_spsm_ready) {
        Result rc = spsmInitialize();
        g_spsm_ready = R_SUCCEEDED(rc);
        if (!g_spsm_ready) {
            log_message("cannot execute command action=%s spsm rc=0x%x", action, rc);
            return;
        }
    }

    bool reboot = strcmp(action, "reboot") == 0;
    log_message("executing device command action=%s", action);
    Result rc = spsmShutdown(reboot);
    if (R_FAILED(rc))
        log_message("spsmShutdown action=%s failed rc=0x%x", action, rc);
}

static bool send_device_heartbeat(u64 current_title_id)
{
    if (!g_sd_ready)
        return false;

    load_app_config();
    if (!g_config.device_id[0] || !g_config.upload_token[0]) {
        log_message("skip heartbeat: device id or token missing");
        return false;
    }

    retry_network_clients();
    if (!g_curl_ready) {
        log_message("skip heartbeat: network/curl not ready socket=%u curl=%u",
                    g_socket_ready ? 1 : 0, g_curl_ready ? 1 : 0);
        return false;
    }

    if (!cainfo_available())
        return false;

    char body[4096];
    char timestamp[32];
    if (!build_heartbeat_body(body, sizeof(body), timestamp, sizeof(timestamp), current_title_id))
        return false;

    char signature[SHA256_HASH_SIZE * 2 + 1];
    build_request_signature("POST", HEARTBEAT_PATH, body, timestamp, signature, sizeof(signature));

    CURL *curl = curl_easy_init();
    if (!curl) {
        log_message("heartbeat curl_easy_init failed");
        return false;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    append_signed_headers(&headers, timestamp, signature);
    MemoryBuffer response = {0};
    ResponseSignature response_sig = {0};

    char url[320];
    build_endpoint_url(HEARTBEAT_PATH, url, sizeof(url));
    log_nifm_status("heartbeat");
    log_curl_request("heartbeat", "POST", url, body);

    char curl_err[CURL_ERROR_SIZE];
    curl_err[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_capture_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_sig);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    configure_tls_verification(curl);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    log_curl_result("heartbeat", curl, res, http_code, curl_err, &response);

    bool ok = (res == CURLE_OK && http_code >= 200 && http_code < 300);
    if (ok) {
        log_message("heartbeat success http=%ld", http_code);
        api_log_line(API_LOG_TAG_HEARTBEAT, "POST", HEARTBEAT_PATH, http_code, NULL, "OK");
        execute_device_command_response(response.data, &response_sig);
    } else if (res != CURLE_OK) {
        // §12.3: curl 层失败记 net。
        log_message("heartbeat curl error=%d(%s)", (int)res, curl_easy_strerror(res));
        api_log_line(API_LOG_TAG_NET, "POST", HEARTBEAT_PATH, 0, NULL, curl_easy_strerror(res));
    } else {
        // §12.2/§12.3/§13: detail(中文)写日志文件, ASCII 机器码一起带上。
        char reason[512];
        char code[API_LOG_CODE_MAX];
        extract_server_failure_detail(response.data, http_code, reason, sizeof(reason));
        extract_server_failure_code(response.data, code, sizeof(code));
        log_message("heartbeat failed http=%ld code=%s detail=%s", http_code,
                    code[0] ? code : "-", reason);
        api_log_line(API_LOG_TAG_HEARTBEAT, "POST", HEARTBEAT_PATH, http_code, code, reason);
    }

    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

static u64 monotonic_seconds(void)
{
    return armTicksToNs(armGetSystemTick()) / 1000000000ULL;
}

/// §15.2: 服务端"今日生效值"是否仍然可信(最近一次成功同步在信任窗口内)。
/// 刚同步成功时服务端值就是"服务端的今天"; 超过窗口(长时间离线)就回退本地时钟。
/// §19: 最近一次成功同步是否仍在信任窗口内。配置同步间隔通常 90–300 秒, 窗口取
/// 1 小时: 足够覆盖几次失败重试, 又不会在跨天之后继续拿昨天的服务端日/限额执法。
static bool server_sync_recent(void)
{
    if (g_server_synced_at_mono == 0)
        return false;
    u64 now = monotonic_seconds();
    return now >= g_server_synced_at_mono &&
           (now - g_server_synced_at_mono) <= SERVER_TIME_TRUST_SECONDS;
}

/// §19: 是否有可信的"服务端本地日"可以驱动日/周账本(日期格式合法 + 同步未过期)。
static bool server_day_trusted(void)
{
    return g_server_day[0] && weekday_index_from_day_key(g_server_day) >= 0 &&
           server_sync_recent();
}

/// §19: 把服务端下发的 server_local_date / server_weekday / client_clock_skew_seconds
/// 落到内存并记进 limiter.log, 便于排查"主机时钟不准 -> 星期几算错 -> 限额失效"。
/// 只在日期/偏差档位变化时写, 避免每 90 秒同步一次刷屏。
static void apply_server_clock_hint(const char *body)
{
    static char last_logged_server_day[16] = "";
    static unsigned long long last_skew_bucket = 0;

    char server_day[16] = "";
    if (read_json_string_field(body, "server_local_date", server_day, sizeof(server_day)) &&
        server_day[0] && weekday_index_from_day_key(server_day) >= 0) {
        copy_fixed_string(g_server_day, sizeof(g_server_day), server_day, strlen(server_day));
        if (strcmp(server_day, last_logged_server_day) != 0) {
            copy_fixed_string(last_logged_server_day, sizeof(last_logged_server_day),
                              server_day, strlen(server_day));
            log_message("server local date=%s device local day=%s; "
                        "day/week ledgers now follow server time",
                        server_day, g_active_day);
        }
    }

    // 偏差可能高达几十万天(秒数远超 u32 范围), 用 strtoull 而不是 read_json_u32_field。
    const char *skew_value = json_find_value(body, "client_clock_skew_seconds");
    if (skew_value && (*skew_value == '-' || (*skew_value >= '0' && *skew_value <= '9'))) {
        unsigned long long skew = strtoull(skew_value, NULL, 10);
        if (skew >= 300ULL) {
            unsigned long long bucket = skew / 600ULL;
            if (bucket != last_skew_bucket) {
                last_skew_bucket = bucket;
                log_message("WARNING: device clock differs from server by %llu seconds; "
                            "effective limits are evaluated with server time (server local date=%s)",
                            skew, last_logged_server_day[0] ? last_logged_server_day : "unknown");
            }
        }
    }
}

static u32 effective_config_sync_seconds(void)
{
    return g_config.config_sync_seconds;
}

static bool hex_value(char c, u8 *out)
{
    if (c >= '0' && c <= '9') {
        *out = (u8)(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (u8)(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (u8)(c - 'A' + 10);
        return true;
    }
    return false;
}

static bool parse_title_id(const char *text, size_t len, u64 *out)
{
    if (len == 18 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
        len -= 2;
    }

    if (len != 16)
        return false;

    u64 value = 0;
    for (size_t i = 0; i < len; i++) {
        u8 nibble;
        if (!hex_value(text[i], &nibble))
            return false;
        value = (value << 4) | nibble;
    }

    *out = value;
    return true;
}

//---------------------------------------------------------------------------------
// 轻量 JSON 扫描(沿用本文件既有的"字符串查找"风格,不引入完整解析器)。
//---------------------------------------------------------------------------------

static const char *json_skip_string(const char *p)
{
    if (*p != '"')
        return p;
    p++;
    while (*p) {
        if (*p == '\\' && p[1]) {
            p += 2;
            continue;
        }
        if (*p == '"')
            return p + 1;
        p++;
    }
    return p;
}

static const char *json_find_value(const char *body, const char *key)
{
    if (!body || !key)
        return NULL;

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t pattern_len = strlen(pattern);

    const char *p = body;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *after = p + pattern_len;
        while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n')
            after++;
        if (*after == ':') {
            after++;
            while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n')
                after++;
            return after;
        }
        p++;
    }
    return NULL;
}

// 取出 key 对应的对象/数组(含花括号),用于隔离 limits / account_limits 子树。
static bool json_extract_object(const char *body, const char *key, char *out, size_t out_size)
{
    const char *value = json_find_value(body, key);
    if (!value)
        return false;

    char open = *value;
    if (open != '{' && open != '[')
        return false;

    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    size_t written = 0;

    for (const char *p = value; *p; p++) {
        char c = *p;

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == open) {
            depth++;
        } else if (c == close) {
            depth--;
            if (depth == 0) {
                if (written < out_size)
                    out[written] = '\0';
                return true;
            }
        }

        if (written + 1 < out_size)
            out[written++] = c;
    }

    return false;
}

static const char *json_next_entry(const char *object_body, const char **cursor,
                                   char *key_out, size_t key_out_size,
                                   char *value_out, size_t value_out_size)
{
    const char *p = (*cursor && *cursor > object_body) ? *cursor : object_body;

    while (*p) {
        while (*p == ',' || *p == '{' || *p == '}' || *p == '[' || *p == ']' ||
               *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            return NULL;

        if (*p != '"')
            return NULL;

        {
            const char *key_start = p + 1;
            const char *after_key = json_skip_string(p);
            size_t key_len = (size_t)(after_key - key_start) - 1;
            if (key_len >= key_out_size)
                key_len = key_out_size - 1;
            memcpy(key_out, key_start, key_len);
            key_out[key_len] = '\0';

            {
                const char *colon = after_key;
                while (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n')
                    colon++;
                if (*colon != ':')
                    return NULL;
                colon++;
                while (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n')
                    colon++;

                if (*colon == '"') {
                    const char *after_value = json_skip_string(colon);
                    size_t value_len = (size_t)(after_value - (colon + 1)) - 1;
                    if (value_len >= value_out_size)
                        value_len = value_out_size - 1;
                    memcpy(value_out, colon + 1, value_len);
                    value_out[value_len] = '\0';
                    p = after_value;
                } else if (*colon == '{' || *colon == '[') {
                    char open = *colon;
                    char close = (open == '{') ? '}' : ']';
                    int depth = 0;
                    bool in_string = false;
                    bool escaped = false;
                    const char *q = colon;
                    for (; *q; q++) {
                        char c = *q;
                        if (in_string) {
                            if (escaped) escaped = false;
                            else if (c == '\\') escaped = true;
                            else if (c == '"') in_string = false;
                        } else if (c == '"') {
                            in_string = true;
                        } else if (c == open) {
                            depth++;
                        } else if (c == close) {
                            depth--;
                            if (depth == 0) { q++; break; }
                        }
                    }
                    {
                        size_t value_len = (size_t)(q - colon);
                        if (value_len >= value_out_size)
                            value_len = value_out_size - 1;
                        memcpy(value_out, colon, value_len);
                        value_out[value_len] = '\0';
                        p = q;
                    }
                } else {
                    const char *value_start = colon;
                    while (*colon && *colon != ',' && *colon != '\n' && *colon != '\r')
                        colon++;
                    {
                        size_t value_len = (size_t)(colon - value_start);
                        while (value_len > 0 &&
                               (value_start[value_len - 1] == ' ' || value_start[value_len - 1] == '\t'))
                            value_len--;
                        if (value_len >= value_out_size)
                            value_len = value_out_size - 1;
                        memcpy(value_out, value_start, value_len);
                        value_out[value_len] = '\0';
                        p = colon;
                    }
                }
            }
        }

        *cursor = p;
        return p;
    }

    return NULL;
}

static LimitEntry *find_device_limit_entry(u64 title_id)
{
    for (size_t i = 0; i < g_limit_count; i++) {
        if (g_limits[i].title_id == title_id)
            return &g_limits[i];
    }
    return NULL;
}

static AccountLimitEntry *find_account_limit_entry(const char *account_id, u64 title_id)
{
    for (size_t i = 0; i < g_account_limit_count; i++) {
        if (g_account_limits[i].title_id == title_id &&
            strcmp(g_account_limits[i].account_id, account_id) == 0)
            return &g_account_limits[i];
    }
    return NULL;
}

static void parse_limits_table(const char *object_body, const char *account_id)
{
    const char *cursor = object_body;
    char key[64];
    char value[64];

    while (json_next_entry(object_body, &cursor, key, sizeof(key), value, sizeof(value))) {
        u64 title_id = 0;
        if (!parse_title_id(key, strlen(key), &title_id))
            continue;

        char *end = NULL;
        unsigned long seconds = strtoul(value, &end, 10);
        if (end == value || seconds > 86400UL)
            continue;

        if (account_id) {
            if (g_account_limit_count >= MAX_ACCOUNT_LIMITS)
                break;
            AccountLimitEntry *entry = &g_account_limits[g_account_limit_count++];
            memset(entry, 0, sizeof(*entry));
            copy_fixed_string(entry->account_id, sizeof(entry->account_id), account_id, strlen(account_id));
            entry->title_id = title_id;
            entry->limit_seconds = (u32)seconds;
        } else {
            if (g_limit_count >= MAX_LIMITS)
                break;
            LimitEntry *entry = &g_limits[g_limit_count++];
            memset(entry, 0, sizeof(*entry));
            entry->title_id = title_id;
            entry->limit_seconds = (u32)seconds;
        }
    }
}

static void parse_account_limits_table(const char *object_body)
{
    const char *cursor = object_body;
    char account_key[64];
    static char nested[MAX_CONFIG_SIZE];

    while (json_next_entry(object_body, &cursor, account_key, sizeof(account_key), nested, sizeof(nested))) {
        if (!is_valid_account_id_hex(account_key))
            continue;
        if (nested[0] != '{')
            continue;
        parse_limits_table(nested, account_key);
    }
}

//---------------------------------------------------------------------------------
// §11 limit_rules / account_limit_rules 解析
//
// 形状(单位一律秒, 与后端 GET /switch/devices/config 一致):
//   "limit_rules": { "<title_id>": { "daily_seconds": 900,
//                                    "weekly_seconds": 28800,
//                                    "weekday_seconds": [0,0,0,0,0,900,900] } }
//   "account_limit_rules": { "<account_id>": { "<title_id>": { ...同上... } } }
//
// §15 按星期语义(唯一真源):
//   - "weekday_seconds": [] 或字段缺失或长度 != 7 -> 按星期**未配置**,
//     当日生效限额回退 daily_seconds;
//   - "weekday_seconds": [恰好 7 个值] -> 按星期**已配置**, 当日生效限额 =
//     该天数值(含 0; 0 = 当天禁止游玩)。
//
// 兼容: 老服务端没有这两个键 -> 完全走既有的 limits / account_limits,
// 此时 has_rule=false, 按旧语义(0 分钟 = 封禁)处理, 行为与改造前一致。
//---------------------------------------------------------------------------------

/// 解析 7 个按星期秒值(下标 0 = 周一)。
/// 返回 true **当且仅当** value 是恰好 7 个数值的 JSON 数组(§15.1: 已配置)。
/// 字段缺失 / 不是数组 / 元素个数 != 7 / 含非数值 / 未闭合 -> 返回 false,
/// out 全 0, 调用方必须回退 daily_seconds(绝不臆造限额)。
static bool parse_weekday_seconds_array(const char *value, u32 out[WEEKDAY_COUNT])
{
    memset(out, 0, sizeof(u32) * WEEKDAY_COUNT);
    if (!value)
        return false;

    const char *p = value;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != '[')
        return false;              // 非数组(含 null / 对象 / 数字) = 未配置
    p++;

    int count = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')
            p++;
        if (*p == ']') {
            p++;
            break;
        }
        if (*p == '\0')
            return false;          // 数组未闭合
        if (count >= WEEKDAY_COUNT)
            return false;          // 多于 7 个 -> 长度不对 = 未配置

        char *end = NULL;
        unsigned long seconds = strtoul(p, &end, 10);
        if (end == p)
            return false;          // 非数值元素
        if (seconds > WEEKDAY_SECONDS_MAX)
            seconds = WEEKDAY_SECONDS_MAX;
        out[count++] = (u32)seconds;
        p = end;
    }

    if (count != WEEKDAY_COUNT) {
        memset(out, 0, sizeof(u32) * WEEKDAY_COUNT);
        return false;              // 少于 7 个 -> 未配置
    }
    return true;
}

static void parse_limit_rule_object(const char *object_body, u32 *daily, u32 *weekly,
                                    u32 weekday[WEEKDAY_COUNT], bool *has_weekday)
{
    const char *v = json_find_value(object_body, "daily_seconds");
    if (v) {
        unsigned long seconds = strtoul(v, NULL, 10);
        *daily = (seconds > WEEKDAY_SECONDS_MAX) ? WEEKDAY_SECONDS_MAX : (u32)seconds;
    }

    v = json_find_value(object_body, "weekly_seconds");
    if (v) {
        unsigned long seconds = strtoul(v, NULL, 10);
        *weekly = (seconds > WEEKLY_SECONDS_MAX) ? WEEKLY_SECONDS_MAX : (u32)seconds;
    }

    *has_weekday = parse_weekday_seconds_array(json_find_value(object_body, "weekday_seconds"),
                                               weekday);
}

static void parse_limit_rules_table(const char *object_body, const char *account_id)
{
    const char *cursor = object_body;
    char key[64];
    static char rule_nested[1024];

    while (json_next_entry(object_body, &cursor, key, sizeof(key), rule_nested, sizeof(rule_nested))) {
        u64 title_id = 0;
        if (!parse_title_id(key, strlen(key), &title_id))
            continue;
        if (rule_nested[0] != '{')
            continue;

        u32 daily = 0;
        u32 weekly = 0;
        u32 weekday[WEEKDAY_COUNT];
        bool has_weekday = false;
        parse_limit_rule_object(rule_nested, &daily, &weekly, weekday, &has_weekday);

        // §15.2: limits / account_limits 里同一 title 的值是**服务端按它自己的本地日**
        // 算好的今日生效上限; 在解析 limit_rules 覆盖之前先把它抓下来。
        bool has_server_today = false;
        u32 server_today_seconds = 0;
        if (account_id) {
            AccountLimitEntry *server_entry = find_account_limit_entry(account_id, title_id);
            if (server_entry) {
                has_server_today = true;
                server_today_seconds = server_entry->limit_seconds;
            }
        } else {
            LimitEntry *server_entry = find_device_limit_entry(title_id);
            if (server_entry) {
                has_server_today = true;
                server_today_seconds = server_entry->limit_seconds;
            }
        }

        if (account_id) {
            AccountLimitEntry *entry = find_account_limit_entry(account_id, title_id);
            if (!entry) {
                if (g_account_limit_count >= MAX_ACCOUNT_LIMITS)
                    continue;
                entry = &g_account_limits[g_account_limit_count++];
                memset(entry, 0, sizeof(*entry));
                copy_fixed_string(entry->account_id, sizeof(entry->account_id), account_id, strlen(account_id));
                entry->title_id = title_id;
            }
            entry->limit_seconds = daily;
            entry->weekly_seconds = weekly;
            memcpy(entry->weekday_seconds, weekday, sizeof(weekday));
            entry->has_weekday = has_weekday;
            entry->has_rule = true;
            entry->has_server_today = has_server_today;
            entry->server_today_seconds = server_today_seconds;
        } else {
            LimitEntry *entry = find_device_limit_entry(title_id);
            if (!entry) {
                if (g_limit_count >= MAX_LIMITS)
                    continue;
                entry = &g_limits[g_limit_count++];
                memset(entry, 0, sizeof(*entry));
                entry->title_id = title_id;
            }
            entry->limit_seconds = daily;
            entry->weekly_seconds = weekly;
            memcpy(entry->weekday_seconds, weekday, sizeof(weekday));
            entry->has_weekday = has_weekday;
            entry->has_rule = true;
            entry->has_server_today = has_server_today;
            entry->server_today_seconds = server_today_seconds;
        }
    }
}

static void parse_account_limit_rules_table(const char *object_body)
{
    const char *cursor = object_body;
    char account_key[64];
    static char accounts_nested[MAX_CONFIG_SIZE];

    while (json_next_entry(object_body, &cursor, account_key, sizeof(account_key),
                           accounts_nested, sizeof(accounts_nested))) {
        if (!is_valid_account_id_hex(account_key))
            continue;
        if (accounts_nested[0] != '{')
            continue;
        parse_limit_rules_table(accounts_nested, account_key);
    }
}

//---------------------------------------------------------------------------------
// §16 账号级"全部游戏合计"限额解析
//
// 形状(单位一律秒, 与后端 GET /switch/devices/config 一致):
//   "account_total_limit_rules": { "<account_id>": { "daily_seconds": 3600,
//                                                     "weekday_seconds": [] | [7 个值] } }
//   "account_used_today_total":  { "<account_id>": 2699 }
//
// §15 按星期语义(唯一真源, 与单游戏规则共用同一条判定):
//   - "weekday_seconds": [] / 字段缺失 / 长度 != 7 -> 未配置, 回退 daily_seconds;
//   - 恰好 7 个值 -> 已配置, 当日上限 = 该天数值(含 0; 0 = 当天禁止游玩)。
//
// 健壮性: 字段缺失 / 不是对象 / 账号 id 不是 32 位十六进制 / daily 非数字 /
// 数组长度不对 -> 一律按"未配置"跳过, 绝不崩溃、绝不臆造限额。
//---------------------------------------------------------------------------------

static AccountTotalRuleEntry *find_account_total_rule_entry(const char *account_id)
{
    for (size_t i = 0; i < g_account_total_rule_count; i++) {
        if (strcmp(g_account_total_rules[i].account_id, account_id) == 0)
            return &g_account_total_rules[i];
    }
    return NULL;
}

static void parse_account_total_limit_rules_table(const char *object_body)
{
    const char *cursor = object_body;
    char account_key[64];
    static char nested[MAX_CONFIG_SIZE];

    while (json_next_entry(object_body, &cursor, account_key, sizeof(account_key),
                           nested, sizeof(nested))) {
        if (!is_valid_account_id_hex(account_key))
            continue;
        if (nested[0] != '{')
            continue;                          // 非对象(含 wrong type / null) -> 未配置

        // 先把两个字段解析到局部变量, 全部非法时整条跳过 —— 绝不因为"读不懂"
        // 就落一条 daily_seconds=0 (= 封禁) 的规则(那会凭空封掉一整天的游玩)。
        u32 daily = 0;
        bool have_daily = false;
        u32 weekday[WEEKDAY_COUNT];
        bool has_weekday = false;

        const char *v = json_find_value(nested, "daily_seconds");
        if (v && (*v == '-' || (*v >= '0' && *v <= '9'))) {
            // 只接受 JSON 数字; 字符串 / null / 对象等 wrong type 一律视为未配置。
            char *end = NULL;
            unsigned long seconds = strtoul(v, &end, 10);
            if (end != v) {
                daily = (seconds > WEEKDAY_SECONDS_MAX) ? WEEKDAY_SECONDS_MAX : (u32)seconds;
                have_daily = true;
            }
        }

        // §15: 恰好 7 个值才算"按星期已配置"; 其余(缺失 / [] / 长度不对 / 非数值)
        // 一律 has_weekday=false, 调用方回退 daily_seconds。
        has_weekday = parse_weekday_seconds_array(json_find_value(nested, "weekday_seconds"), weekday);

        if (!have_daily && !has_weekday)
            continue;                          // 两个字段都不可用 -> 未配置, 不建条目

        AccountTotalRuleEntry *entry = find_account_total_rule_entry(account_key);
        if (!entry) {
            if (g_account_total_rule_count >= MAX_ACCOUNT_TOTAL_RULES)
                continue;                      // 容量上限: 与既有账号限额表同样的处理
            entry = &g_account_total_rules[g_account_total_rule_count++];
            memset(entry, 0, sizeof(*entry));
            copy_fixed_string(entry->account_id, sizeof(entry->account_id),
                              account_key, strlen(account_key));
        }
        entry->daily_seconds = daily;
        memcpy(entry->weekday_seconds, weekday, sizeof(entry->weekday_seconds));
        entry->has_weekday = has_weekday;
    }
}

/// §15.2 解析 account_total_limits(account_id -> 服务端按本地日算好的今日生效秒数)。
/// 只给**已经有合计规则**的账号补上"今日生效上限"; 0 是合法值(= 当天封禁), 必须采信。
static void adopt_account_total_server_today_cb(const char *name, unsigned long long value, void *ctx)
{
    (void)ctx;
    if (!is_valid_account_id_hex(name))
        return;
    if (value > WEEKDAY_SECONDS_MAX)
        return;                                  // 异常值不采信 -> 回退本地星期几计算
    AccountTotalRuleEntry *entry = find_account_total_rule_entry(name);
    if (!entry)
        return;                                  // 没有规则行(未启用) -> 无需今日值
    entry->has_server_today = true;
    entry->server_today_seconds = (u32)value;
}

static void parse_account_total_limits_table(const char *object_body)
{
    const char *start = object_body;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (*start != '{')
        return;
    const char *end = NULL;
    if (!json_match_brace(start, &end))
        return;
    json_foreach_number(start + 1, end, adopt_account_total_server_today_cb, NULL);
}

/// §16 记录服务端合计下限(只抬不降)。0 / 非数字 / 异常大的值一律忽略 ——
/// 忽略等于保持本地值, 绝不会反过来放宽限制。
static void set_account_total_floor(const char *account_id, unsigned long long seconds)
{
    if (!is_valid_account_id_hex(account_id))
        return;
    if (seconds == 0 || seconds > 86400ULL)
        return;

    for (size_t i = 0; i < g_account_total_floor_count; i++) {
        if (strcmp(g_account_total_floors[i].account_id, account_id) == 0 &&
            strcmp(g_account_total_floors[i].day_key, g_active_day) == 0) {
            if (seconds > g_account_total_floors[i].used_seconds)
                g_account_total_floors[i].used_seconds = seconds;   // 只抬不降
            return;
        }
    }

    if (g_account_total_floor_count >= MAX_ACCOUNT_TOTAL_RULES)
        return;
    AccountTotalFloorEntry *entry = &g_account_total_floors[g_account_total_floor_count++];
    memset(entry, 0, sizeof(*entry));
    copy_fixed_string(entry->account_id, sizeof(entry->account_id), account_id, strlen(account_id));
    copy_fixed_string(entry->day_key, sizeof(entry->day_key), g_active_day, strlen(g_active_day));
    entry->used_seconds = seconds;
}

static void adopt_account_total_floor_cb(const char *name, unsigned long long value, void *ctx)
{
    (void)ctx;
    set_account_total_floor(name, value);
}

/// §16 解析 account_used_today_total(对象: account_id -> 秒数)。
/// 只接受**裸数字**成员: 字符串/null/对象/负数一律跳过(与 §14.3 的下限采纳同一风格)。
static void parse_account_used_today_total_table(const char *object_body)
{
    const char *start = object_body;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (*start != '{')
        return;

    const char *end = NULL;
    if (!json_match_brace(start, &end))
        return;

    json_foreach_number(start + 1, end, adopt_account_total_floor_cb, NULL);
}

//---------------------------------------------------------------------------------
// limits.json 解析(向后兼容旧格式 + 新格式,REFACTOR_SPEC §2.2)
//
// 旧格式: { "0100D71004694000": 900 }                -> 设备级限额
// 新格式: { "limits": {...}, "accounts": [...], "account_limits": {...} }
// "accounts" 里的昵称字段交给 NRO 使用,sysmodule 这里忽略。
//---------------------------------------------------------------------------------
static void load_limits(void)
{
    static char body[MAX_CONFIG_SIZE];
    static char limits_object[MAX_CONFIG_SIZE];
    static char account_limits_object[MAX_CONFIG_SIZE];
    static char limit_rules_object[MAX_CONFIG_SIZE];
    static char account_limit_rules_object[MAX_CONFIG_SIZE];
    static char account_total_object[MAX_CONFIG_SIZE];

    if (!g_sd_ready)
        return;

    bool exists = false;
    if (!read_whole_file(LIMITS_PATH, body, sizeof(body), &exists, NULL)) {
        // 读失败时保留上一次解析结果,不能"读不到就当成没限额"。
        if (exists)
            log_message("limits.json unreadable; keeping previous %u limit entries",
                        (unsigned)g_limit_count);
        return;
    }

    g_limit_count = 0;
    g_account_limit_count = 0;
    // §16: 规则与"服务端合计下限"都整表重建(下限带 day_key, 日切时另见 reset)。
    g_account_total_rule_count = 0;
    g_account_total_floor_count = 0;

    if (json_extract_object(body, "limits", limits_object, sizeof(limits_object)))
        parse_limits_table(limits_object, NULL);
    else
        parse_limits_table(body, NULL);   // 旧格式: 顶层就是 title -> seconds

    if (json_extract_object(body, "account_limits", account_limits_object, sizeof(account_limits_object)))
        parse_account_limits_table(account_limits_object);

    // §11: 完整规则(日 / 周 / 按星期)。放在 limits / account_limits 之后解析,
    // 这样同一 title 的规则会覆盖/补齐上面按老字段建立的条目。缺失时 has_rule 保持
    // false, 调用方退回旧语义(0 分钟 = 封禁), 老服务端行为不变。
    if (json_extract_object(body, "limit_rules", limit_rules_object, sizeof(limit_rules_object)))
        parse_limit_rules_table(limit_rules_object, NULL);

    if (json_extract_object(body, "account_limit_rules", account_limit_rules_object,
                            sizeof(account_limit_rules_object)))
        parse_account_limit_rules_table(account_limit_rules_object);

    // §16: 账号级"全部游戏合计"限额规则 + 服务端权威的今日合计(本地合计的下限)。
    // 缺少这些键(老服务端)时两张表保持为空 -> 完全不影响既有行为。
    if (json_extract_object(body, "account_total_limit_rules", account_total_object,
                            sizeof(account_total_object)))
        parse_account_total_limit_rules_table(account_total_object);

    // §15.2 服务端算好的"今日生效合计上限"。必须放在规则之后解析(给规则行补今日值);
    // 老服务端没有这个键时保持 has_server_today=false, 回退本地星期几计算。
    if (json_extract_object(body, "account_total_limits", account_total_object,
                            sizeof(account_total_object)))
        parse_account_total_limits_table(account_total_object);

    if (json_extract_object(body, "account_used_today_total", account_total_object,
                            sizeof(account_total_object)))
        parse_account_used_today_total_table(account_total_object);

    log_message("loaded limits device=%u account=%u total_rules=%u total_floors=%u",
                (unsigned)g_limit_count, (unsigned)g_account_limit_count,
                (unsigned)g_account_total_rule_count, (unsigned)g_account_total_floor_count);
}

//---------------------------------------------------------------------------------
// §11 规则查找与判定(唯一真源)
//
// 限额优先级沿用既有权口: account_limits[account][title] -> limits[title]
// (REFACTOR_SPEC §2.2)。规则同时携带 日 / 周 / 按星期 三个上限。
//---------------------------------------------------------------------------------

static bool find_device_rule(u64 title_id, LimitRule *out_rule)
{
    for (size_t i = 0; i < g_limit_count; i++) {
        if (g_limits[i].title_id != title_id)
            continue;
        out_rule->daily_seconds = g_limits[i].limit_seconds;
        out_rule->weekly_seconds = g_limits[i].weekly_seconds;
        memcpy(out_rule->weekday_seconds, g_limits[i].weekday_seconds, sizeof(out_rule->weekday_seconds));
        out_rule->has_weekday = g_limits[i].has_weekday;
        out_rule->has_server_today = g_limits[i].has_server_today;
        out_rule->server_today_seconds = g_limits[i].server_today_seconds;
        return true;
    }
    return false;
}

/// 生效规则优先级与既有口径一致: 账号级 > 设备级。
/// 返回 false = 该 title 完全没有限额条目 => 不限制(不记账、不拦截)。
static bool find_effective_rule(const char *account_id, u64 title_id, LimitRule *out_rule)
{
    if (account_id && account_id[0]) {
        for (size_t i = 0; i < g_account_limit_count; i++) {
            if (g_account_limits[i].title_id == title_id &&
                strcmp(g_account_limits[i].account_id, account_id) == 0) {
                out_rule->daily_seconds = g_account_limits[i].limit_seconds;
                out_rule->weekly_seconds = g_account_limits[i].weekly_seconds;
                memcpy(out_rule->weekday_seconds, g_account_limits[i].weekday_seconds,
                       sizeof(out_rule->weekday_seconds));
                out_rule->has_weekday = g_account_limits[i].has_weekday;
                out_rule->has_server_today = g_account_limits[i].has_server_today;
                out_rule->server_today_seconds = g_account_limits[i].server_today_seconds;
                return true;
            }
        }
    }
    return find_device_rule(title_id, out_rule);
}

/// §11/§15/§15.2 今日生效限额(秒):
///   - 同步成功后的信任窗口内: 优先用服务端按**服务端本地日**算好的今日生效值
///     (limits / account_limits 下发), 这样设备 RTC 不准/被改也不会把星期几算错;
///   - 否则(离线/老服务端)回退本地时钟: 按星期已配置(has_weekday) -> 该天的值,
///     **0 也是有效值**(当天封禁); 未配置 -> daily_seconds。
static u32 rule_effective_daily(const LimitRule *rule, int weekday_index)
{
    if (rule->has_server_today && server_sync_recent())
        return rule->server_today_seconds;
    // 唯一真源: limits_common.h 的纯函数(与前台 NRO 共用同一条 §15 判定)。
    return (u32)limits_effective_daily_seconds(rule->daily_seconds, rule->weekday_seconds,
                                               rule->has_weekday ? 1 : 0, weekday_index);
}

static bool rule_has_any_cap(const LimitRule *rule)
{
    if (rule->daily_seconds > 0 || rule->weekly_seconds > 0)
        return true;
    for (int i = 0; i < WEEKDAY_COUNT; i++) {
        if (rule->weekday_seconds[i] > 0)
            return true;
    }
    return false;
}

//---------------------------------------------------------------------------------
// §11 周键(周一)与星期几
//
// 用 mktime 对"日键"做日历归一化, 与"此刻"无关(日切守卫可能把 g_active_day 停在
// 较早的一天), 也不依赖时区设置的正确性: 这里只做"同一日键 -> 同一周键"的映射。
//---------------------------------------------------------------------------------

/// 解析 YYYY-MM-DD, 返回 0 = 周一 … 6 = 周日; 失败返回 -1。
static int weekday_index_from_day_key(const char *day_key)
{
    int year = 0, month = 0, day = 0;
    if (!day_key || sscanf(day_key, "%d-%d-%d", &year, &month, &day) != 3)
        return -1;
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31)
        return -1;

    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_isdst = -1;
    if (mktime(&t) == (time_t)-1)
        return -1;
    // tm_wday: 0 = 周日 … 6 = 周六 -> 转成 0 = 周一 … 6 = 周日
    return (t.tm_wday + 6) % 7;
}

/// 该日所在周的周一(YYYY-MM-DD)。失败返回 false。
static bool week_start_key_from_day_key(const char *day_key, char *out, size_t out_size)
{
    int index = weekday_index_from_day_key(day_key);
    if (index < 0)
        return false;

    int year = 0, month = 0, day = 0;
    if (sscanf(day_key, "%d-%d-%d", &year, &month, &day) != 3)
        return false;

    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day - index;   // mktime 负责跨月/跨年归一化
    t.tm_isdst = -1;
    if (mktime(&t) == (time_t)-1)
        return false;

    {
        // 先做范围校验再格式化: 直接把 tm 字段丢给 snprintf 会触发
        // -Wformat-truncation(这些字段的理论范围远大于输出缓冲区)。
        int out_year = t.tm_year + 1900;
        int out_month = t.tm_mon + 1;
        int out_day = t.tm_mday;
        if (out_year < 1970 || out_year > 9999 || out_month < 1 || out_month > 12 ||
            out_day < 1 || out_day > 31)
            return false;

        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", out_year, out_month, out_day);
        copy_fixed_string(out, out_size, buffer, strlen(buffer));
    }
    return true;
}

//---------------------------------------------------------------------------------
// §16 账号级"全部游戏合计"限额判定(唯一真源)
//
//   账号当日合计 = 本地账本里该账号 day_key == g_active_day 的所有条目之和,
//                  再取 max(本地合计, 服务端 account_used_today_total 下限)。
//                  —— 只抬不降(与 §14.3 同一原则)。
//   当日生效上限 = §15 规则: has_weekday ? weekday_seconds[今天] : daily_seconds;
//                  值 0 = 当天禁止游玩。
//   判定与单游戏限额是 OR; 任何不确定一律**不拦**(宁可不拦, 不可误杀)。
//---------------------------------------------------------------------------------

typedef struct {
    bool rule_known;      ///< 该账号是否有合计规则(enabled=true 的行)
    bool determinate;     ///< 今日生效上限是否算得出(账号已知 + 星期已知)
    bool over;            ///< 已达/超上限, 或显式 0 封禁
    u64 used;             ///< 账号当日合计(已取服务端下限)
    u32 limit;            ///< 今日生效合计上限
    const char *source;   ///< LIMIT_SOURCE_DAILY / LIMIT_SOURCE_WEEKDAY / NONE
    const char *reason;   ///< 判定说明(仅用于日志)
} AccountTotalDecision;

/// §16 查找账号的合计规则, 填充成 LimitRule(复用 §11/§15 的取值逻辑)。
/// 返回 false = 该账号没有合计规则 = 不限制(不拦截)。
static bool find_account_total_rule(const char *account_id, LimitRule *out_rule)
{
    if (!account_id || !account_id[0])
        return false;

    for (size_t i = 0; i < g_account_total_rule_count; i++) {
        if (strcmp(g_account_total_rules[i].account_id, account_id) != 0)
            continue;
        memset(out_rule, 0, sizeof(*out_rule));
        out_rule->daily_seconds = g_account_total_rules[i].daily_seconds;
        memcpy(out_rule->weekday_seconds, g_account_total_rules[i].weekday_seconds,
               sizeof(out_rule->weekday_seconds));
        out_rule->has_weekday = g_account_total_rules[i].has_weekday;
        out_rule->has_server_today = g_account_total_rules[i].has_server_today;
        out_rule->server_today_seconds = g_account_total_rules[i].server_today_seconds;
        return true;
    }
    return false;
}

/// §16 服务端合计下限(仅当记录的 day_key 与当前本地日一致时有效)。
static u64 account_total_floor_seconds(const char *account_id)
{
    if (!account_id || !account_id[0])
        return 0;

    for (size_t i = 0; i < g_account_total_floor_count; i++) {
        if (strcmp(g_account_total_floors[i].account_id, account_id) == 0 &&
            strcmp(g_account_total_floors[i].day_key, g_active_day) == 0)
            return g_account_total_floors[i].used_seconds;
    }
    return 0;
}

/// §16 账号当日合计 = 本地账本求和, 再抬到不低于服务端下限。
static u64 account_total_used_today(const char *account_id)
{
    if (!account_id)
        account_id = "";

    u64 local = 0;
    for (size_t i = 0; i < g_usage_count; i++) {
        // 只统计**同一账号 + 当前本地日**的条目(§14 修复后 used_seconds 已是真实时长)。
        if (g_usage[i].day_key[0] &&
            strcmp(g_usage[i].day_key, g_active_day) == 0 &&
            strcmp(g_usage[i].account_id, account_id) == 0)
            local += g_usage[i].used_seconds;
    }

    u64 floor = account_total_floor_seconds(account_id);
    // 唯一真源: limits_common.h 的纯函数(max, 只抬不降)。
    return limits_total_with_floor(local, floor);
}

/// §16 本地日键变化时作废服务端合计下限(下限只对"当天"有意义)。
static void reset_account_total_floors(void)
{
    g_account_total_floor_count = 0;
}

/// §16 算出账号合计判定。任何一步不确定时 over 保持 false(determinate 为 false)。
static void evaluate_account_total_limit(const char *account_id, AccountTotalDecision *out)
{
    memset(out, 0, sizeof(*out));
    out->source = LIMIT_SOURCE_NONE;

    if (!account_id || !account_id[0]) {
        out->reason = "account unknown";
        return;                                   // 账号未知 -> 不拦
    }

    LimitRule rule;
    memset(&rule, 0, sizeof(rule));
    if (!find_account_total_rule(account_id, &rule)) {
        out->reason = "no account total rule";
        return;                                   // 没有合计规则 -> 不限制
    }
    out->rule_known = true;

    int weekday_index = weekday_index_from_day_key(g_active_day);
    if (weekday_index < 0 && !(rule.has_server_today && server_sync_recent())) {
        // 星期几算不出(时钟/日键不可用), 且没有可信的服务端今日生效值 -> 不拦。
        // 与 §11 单游戏判定保持同一条 fail-safe(unknown weekday -> do not terminate)。
        out->reason = "weekday unknown (clock/day key unavailable)";
        return;
    }

    out->limit = rule_effective_daily(&rule, weekday_index);
    out->source = rule.has_weekday ? LIMIT_SOURCE_WEEKDAY : LIMIT_SOURCE_DAILY;
    out->used = account_total_used_today(account_id);
    out->determinate = true;

    // §15: 当日生效上限 0 = 禁止游玩(按星期配置里的 0 照样生效)。
    if (out->limit == 0) {
        out->over = true;
        out->reason = "account total blocked (0 = no play)";
        return;
    }
    if (limits_account_total_over(out->used, out->limit)) {
        out->over = true;
        out->reason = "account total cap reached";
        return;
    }
}

//---------------------------------------------------------------------------------
// 日切守卫(REFACTOR_SPEC §6 item 14)
//
//   - 观测到比"已见最大日"更早的日期 => 时间被回拨,沿用当前记账,不重置配额。
//   - 日期前进时需要足够的累计运行时长(或足够墙钟跨度 + 少量运行时长),否则同样不重置。
//   - 时钟读不出来时进入 unknown 模式: 继续在原窗口累加,不重置、不崩溃。
//---------------------------------------------------------------------------------
static u64 day_key_to_seq(int year, int month, int day)
{
    return (u64)year * 10000ULL + (u64)month * 100ULL + (u64)day;
}

static u64 wall_now(void)
{
    if (!g_time_ready)
        return 0;
    u64 value = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &value)))
        return 0;
    return value;
}

static void load_guard_state(void)
{
    if (g_guard_loaded)
        return;
    g_guard_loaded = true;

    static char body[1024];
    bool exists = false;
    if (!read_whole_file(GUARD_PATH, body, sizeof(body), &exists, NULL)) {
        if (exists)
            log_message("limiter_guard.json unreadable; keeping previous day guard in memory");
        return;
    }

    unsigned long long seq = 0, runtime = 0, wall = 0;
    const char *v = json_find_value(body, "day_seq");
    if (v) seq = strtoull(v, NULL, 10);
    v = json_find_value(body, "runtime_seconds");
    if (v) runtime = strtoull(v, NULL, 10);
    v = json_find_value(body, "wall_max");
    if (v) wall = strtoull(v, NULL, 10);

    g_guard_day_seq = (u64)seq;
    g_guard_runtime_seconds = (u64)runtime;
    g_guard_wall_max = (u64)wall;

    if (g_guard_day_seq)
        log_message("loaded day guard seq=%llu runtime=%llu wall_max=%llu",
                    (unsigned long long)g_guard_day_seq,
                    (unsigned long long)g_guard_runtime_seconds,
                    (unsigned long long)g_guard_wall_max);
}

static void save_guard_state(void)
{
    if (!g_guard_dirty || !g_sd_ready)
        return;

    char out[256];
    int needed = snprintf(out, sizeof(out),
                          "{\n  \"day_seq\": %llu,\n  \"runtime_seconds\": %llu,\n  \"wall_max\": %llu\n}\n",
                          (unsigned long long)g_guard_day_seq,
                          (unsigned long long)g_guard_runtime_seconds,
                          (unsigned long long)g_guard_wall_max);
    if (needed < 0 || (size_t)needed >= sizeof(out)) {
        log_message("refusing to write truncated limiter_guard.json");
        return;
    }
    if (write_file_atomic(GUARD_PATH, out, (size_t)needed))
        g_guard_dirty = false;
}

static bool read_day_key(char *out, size_t out_size)
{
    if (!g_time_ready)
        return false;

    u64 timestamp = 0;
    TimeCalendarTime cal;
    TimeCalendarAdditionalInfo info;

    if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &timestamp)))
        return false;

    if (R_FAILED(timeToCalendarTimeWithMyRule(timestamp, &cal, &info)))
        return false;

    if (cal.year < 2020 || cal.year > 2099)
        return false;

    snprintf(out, out_size, "%04u-%02u-%02u", cal.year, cal.month, cal.day);
    return true;
}

//---------------------------------------------------------------------------------
// 用量记账: 以 (账号, 游戏, 本地日) 为键(REFACTOR_SPEC §5),
// 保证一个账号的时长不会被算到另一个账号头上。
//---------------------------------------------------------------------------------
static UsageEntry *get_usage_entry(const char *account_id, u64 title_id)
{
    if (!account_id)
        account_id = "";

    for (size_t i = 0; i < g_usage_count; i++) {
        if (g_usage[i].title_id == title_id &&
            strcmp(g_usage[i].day_key, g_active_day) == 0 &&
            strcmp(g_usage[i].account_id, account_id) == 0)
            return &g_usage[i];
    }

    if (g_usage_count >= MAX_USAGE) {
        // 淘汰零值项,而不是静默停止记账(item 17)。
        for (size_t i = 0; i < g_usage_count; i++) {
            if (g_usage[i].used_seconds == 0) {
                memmove(&g_usage[i], &g_usage[i + 1], (g_usage_count - i - 1) * sizeof(UsageEntry));
                g_usage_count--;
                break;
            }
        }
        if (g_usage_count >= MAX_USAGE) {
            log_message("usage table full (%u entries); cannot record account=%s title=%016lX",
                        (unsigned)g_usage_count, account_id, title_id);
            return NULL;
        }
    }

    {
        UsageEntry *entry = &g_usage[g_usage_count++];
        memset(entry, 0, sizeof(*entry));
        copy_fixed_string(entry->account_id, sizeof(entry->account_id), account_id, strlen(account_id));
        copy_fixed_string(entry->day_key, sizeof(entry->day_key), g_active_day, strlen(g_active_day));
        entry->title_id = title_id;
        g_state_dirty = true;
        return entry;
    }
}

static void load_state(void)
{
    static char body[MAX_CONFIG_SIZE];

    if (!g_sd_ready)
        return;

    bool exists = false;
    if (!read_whole_file(STATE_PATH, body, sizeof(body), &exists, NULL)) {
        if (exists)
            log_message("limiter_state.csv unreadable; keeping previous %u usage entries",
                        (unsigned)g_usage_count);
        return;
    }

    g_usage_count = 0;
    size_t matched = 0;
    char *saveptr = NULL;
    char *line = strtok_r(body, "\n", &saveptr);
    while (line && g_usage_count < MAX_USAGE) {
        char *field[5];
        int n = 0;
        char *p = line;
        while (n < 5) {
            field[n++] = p;
            char *comma = strchr(p, ',');
            if (!comma) break;
            *comma = '\0';
            p = comma + 1;
        }
        for (int i = 0; i < n; i++) {
            size_t len = strlen(field[i]);
            while (len > 0 && (field[i][len - 1] == '\n' || field[i][len - 1] == '\r'))
                field[i][--len] = '\0';
        }

        {
            const char *account_id = "";
            const char *day = NULL;
            const char *title_text = NULL;
            const char *used_text = NULL;

            if (n >= 4 && is_valid_account_id_hex(field[0])) {
                // 新格式: account_id,day,title_id,used_seconds
                account_id = field[0];
                day = field[1];
                title_text = field[2];
                used_text = field[3];
            } else if (n >= 3) {
                // 旧格式(向后兼容): day,title_id,used_seconds
                day = field[0];
                title_text = field[1];
                used_text = field[2];
            } else {
                line = strtok_r(NULL, "\n", &saveptr);
                continue;
            }

            {
                u64 title_id = 0;
                if (parse_title_id(title_text, strlen(title_text), &title_id) &&
                    strcmp(day, g_active_day) == 0) {
                    unsigned long used = strtoul(used_text, NULL, 10);
                    UsageEntry *entry = &g_usage[g_usage_count++];
                    memset(entry, 0, sizeof(*entry));
                    copy_fixed_string(entry->account_id, sizeof(entry->account_id), account_id, strlen(account_id));
                    copy_fixed_string(entry->day_key, sizeof(entry->day_key), day, strlen(day));
                    entry->title_id = title_id;
                    entry->used_seconds = (u32)used;
                    matched++;
                }
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    g_state_dirty = false;
    log_message("loaded %u usage entries for day %s", (unsigned)matched, g_active_day);
}

static void save_state(void)
{
    if (!g_sd_ready || !g_state_dirty)
        return;

    static char body[MAX_CONFIG_SIZE];
    size_t used = 0;
    int needed = snprintf(body, sizeof(body), "account_id,day,title_id,used_seconds\n");
    if (needed < 0 || (size_t)needed >= sizeof(body))
        return;
    used = (size_t)needed;

    for (size_t i = 0; i < g_usage_count; i++) {
        needed = snprintf(body + used, sizeof(body) - used, "%s,%s,%016lX,%u\n",
                          g_usage[i].account_id,
                          g_usage[i].day_key,
                          g_usage[i].title_id,
                          g_usage[i].used_seconds);
        if (needed < 0 || (size_t)needed >= sizeof(body) - used) {
            log_message("limiter_state.csv too large; not writing (keeping previous file)");
            return;
        }
        used += (size_t)needed;
    }

    if (write_file_atomic(STATE_PATH, body, used))
        g_state_dirty = false;
    else
        log_message("failed to write limiter_state.csv");
}

//---------------------------------------------------------------------------------
// §11 周用量账本(未截断真实用量)
//
// 与日账本完全分开:
//   - 日账本 limiter_state.csv 只保留"当天"条目(§14 修复后 used_seconds 已按真实
//     时长累加、不再在限额处截断, 但"只留当天"仍使它不能用来算周总量);
//   - 本账本 limiter_week.csv 按 (账号, 游戏, 本周周一) 记录**未截断**秒数,
//     只保留当前周, 跨周时整体重置(重置由已通过日切守卫的 g_active_day 驱动,
//     所以回拨时钟无法"复活"上一周的额度)。
//
// 任何一步不确定(时钟不可用、文件读失败、当前周未知)都把 g_week_usage_known
// 置 false: 规则里配了周上限时不拦截, 并写日志(REFACTOR_SPEC §11: 宁可不拦不可误杀)。
//---------------------------------------------------------------------------------

static void reset_week_state(void)
{
    g_week_usage_count = 0;
    g_week_state_dirty = false;
}

static void load_week_state(void)
{
    if (!g_sd_ready) {
        g_week_usage_known = false;
        return;
    }

    if (strcmp(g_active_week, "unknown") == 0) {
        // 时钟不可用: 周键未知 -> 周用量不可信。保留内存里已累计的值, 不清零。
        g_week_usage_known = false;
        log_message("week usage unknown (active week unavailable); weekly cap will not block");
        return;
    }

    static char body[MAX_CONFIG_SIZE];
    bool exists = false;
    if (!read_whole_file(WEEK_STATE_PATH, body, sizeof(body), &exists, NULL)) {
        if (exists) {
            // 文件存在但读失败: 保留内存中的累计值, 标记不可信(周上限不拦截)。
            g_week_usage_known = false;
            log_message("limiter_week.csv unreadable; keeping previous %u week entries",
                        (unsigned)g_week_usage_count);
        } else {
            // 首次运行: 确实是空的, 可以信。
            g_week_usage_count = 0;
            g_week_state_dirty = false;
            g_week_usage_known = true;
        }
        return;
    }

    g_week_usage_count = 0;
    size_t matched = 0;
    char *saveptr = NULL;
    char *line = strtok_r(body, "\n", &saveptr);
    while (line && g_week_usage_count < MAX_USAGE) {
        char *field[4];
        int n = 0;
        char *p = line;
        while (n < 4) {
            field[n++] = p;
            char *comma = strchr(p, ',');
            if (!comma)
                break;
            *comma = '\0';
            p = comma + 1;
        }
        for (int i = 0; i < n; i++) {
            size_t len = strlen(field[i]);
            while (len > 0 && (field[i][len - 1] == '\n' || field[i][len - 1] == '\r'))
                field[i][--len] = '\0';
        }

        if (n >= 4) {
            // 格式: account_id,week_key,title_id,uncapped_seconds
            const char *account_id = is_valid_account_id_hex(field[0]) ? field[0] : "";
            u64 title_id = 0;
            if (strcmp(field[1], g_active_week) == 0 &&
                parse_title_id(field[2], strlen(field[2]), &title_id)) {
                WeekUsageEntry *entry = &g_week_usage[g_week_usage_count++];
                memset(entry, 0, sizeof(*entry));
                copy_fixed_string(entry->account_id, sizeof(entry->account_id),
                                  account_id, strlen(account_id));
                copy_fixed_string(entry->week_key, sizeof(entry->week_key),
                                  field[1], strlen(field[1]));
                entry->title_id = title_id;
                entry->uncapped_seconds = (u32)strtoul(field[3], NULL, 10);
                matched++;
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    g_week_state_dirty = false;
    g_week_usage_known = true;
    log_message("loaded %u week usage entries for week %s", (unsigned)matched, g_active_week);
}

static void save_week_state(void)
{
    if (!g_sd_ready || !g_week_state_dirty)
        return;

    static char body[MAX_CONFIG_SIZE];
    size_t used = 0;
    int needed = snprintf(body, sizeof(body), "account_id,week,title_id,uncapped_seconds\n");
    if (needed < 0 || (size_t)needed >= sizeof(body))
        return;
    used = (size_t)needed;

    for (size_t i = 0; i < g_week_usage_count; i++) {
        needed = snprintf(body + used, sizeof(body) - used, "%s,%s,%016lX,%u\n",
                          g_week_usage[i].account_id,
                          g_week_usage[i].week_key,
                          g_week_usage[i].title_id,
                          g_week_usage[i].uncapped_seconds);
        if (needed < 0 || (size_t)needed >= sizeof(body) - used) {
            log_message("limiter_week.csv too large; not writing (keeping previous file)");
            return;
        }
        used += (size_t)needed;
    }

    if (write_file_atomic(WEEK_STATE_PATH, body, used))
        g_week_state_dirty = false;
    else
        log_message("failed to write limiter_week.csv");
}

static WeekUsageEntry *get_week_usage_entry(const char *account_id, u64 title_id)
{
    if (!account_id)
        account_id = "";

    for (size_t i = 0; i < g_week_usage_count; i++) {
        if (g_week_usage[i].title_id == title_id &&
            strcmp(g_week_usage[i].week_key, g_active_week) == 0 &&
            strcmp(g_week_usage[i].account_id, account_id) == 0)
            return &g_week_usage[i];
    }

    if (g_week_usage_count >= MAX_USAGE) {
        // 与日账本一致: 淘汰零值项而不是静默停止记账(item 17)。
        for (size_t i = 0; i < g_week_usage_count; i++) {
            if (g_week_usage[i].uncapped_seconds == 0) {
                memmove(&g_week_usage[i], &g_week_usage[i + 1],
                        (g_week_usage_count - i - 1) * sizeof(WeekUsageEntry));
                g_week_usage_count--;
                break;
            }
        }
        if (g_week_usage_count >= MAX_USAGE) {
            log_message("week usage table full (%u entries); cannot record account=%s title=%016lX",
                        (unsigned)g_week_usage_count, account_id, title_id);
            return NULL;
        }
    }

    {
        WeekUsageEntry *entry = &g_week_usage[g_week_usage_count++];
        memset(entry, 0, sizeof(*entry));
        copy_fixed_string(entry->account_id, sizeof(entry->account_id), account_id, strlen(account_id));
        copy_fixed_string(entry->week_key, sizeof(entry->week_key), g_active_week, strlen(g_active_week));
        entry->title_id = title_id;
        g_week_state_dirty = true;
        return entry;
    }
}

/// §11 累加**未截断**的真实用量(不受每日限额影响)。调用方只在"确实在计时"时调用。
static void add_uncapped_seconds(const char *account_id, u64 title_id, u32 seconds)
{
    if (seconds == 0)
        return;

    if (strcmp(g_active_week, "unknown") == 0) {
        // 周键未知: 无法归属到某一周, 记为不可信并放弃本次累加(绝不猜)。
        if (g_week_usage_known) {
            log_message("week key unknown; uncapped usage not recorded (weekly cap will not block)");
            g_week_usage_known = false;
        }
        return;
    }

    WeekUsageEntry *entry = get_week_usage_entry(account_id, title_id);
    if (!entry)
        return;

    u32 updated = entry->uncapped_seconds + seconds;
    if (updated < entry->uncapped_seconds)
        updated = 0xFFFFFFFFu;   // 饱和, 不回绕
    entry->uncapped_seconds = updated;
    g_week_state_dirty = true;
}

/// §11 本周已用秒数。返回 false = 算不出来(时钟/账本不可信), 调用方必须按"不拦截"处理。
static bool week_used_seconds(const char *account_id, u64 title_id, u32 *out_seconds)
{
    if (out_seconds)
        *out_seconds = 0;

    if (!g_week_usage_known || strcmp(g_active_week, "unknown") == 0)
        return false;
    if (!account_id)
        account_id = "";

    u32 total = 0;
    for (size_t i = 0; i < g_week_usage_count; i++) {
        if (g_week_usage[i].title_id != title_id)
            continue;
        if (strcmp(g_week_usage[i].week_key, g_active_week) != 0)
            continue;
        if (strcmp(g_week_usage[i].account_id, account_id) != 0)
            continue;
        u32 updated = total + g_week_usage[i].uncapped_seconds;
        total = (updated < total) ? 0xFFFFFFFFu : updated;
    }

    if (out_seconds)
        *out_seconds = total;
    return true;
}

/// §11: 让周键跟随已通过日切守卫的 g_active_day。
///
/// 只在这里推进周键 —— 日切守卫已经拒绝了回拨和可疑的向前跳, 因此"周"同样无法被
/// 时钟回拨复活。跨周时整体重置周账本; 同一周内保留已累计的未截断用量。
static void apply_active_week_for_day(const char *day_key)
{
    char week[16];
    if (!week_start_key_from_day_key(day_key, week, sizeof(week))) {
        if (strcmp(g_active_week, "unknown") != 0) {
            copy_fixed_string(g_active_week, sizeof(g_active_week), "unknown", 7);
            log_message("cannot derive week key from day %s; weekly usage unknown", day_key);
        }
        g_week_usage_known = false;
        return;
    }

    if (strcmp(week, g_active_week) == 0)
        return;   // 同一周: 保留已累计的周用量

    copy_fixed_string(g_active_week, sizeof(g_active_week), week, strlen(week));
    reset_week_state();
    load_week_state();
    log_message("active week changed to %s", g_active_week);
}

/// §19: 采纳一个新的"当天"(重置日账本 / 周账本 / §16 服务端合计下限)。
/// 同时把防回拨守卫的基准同步到该日, 这样之后回退到"本地时钟兜底"时,
/// 把时钟往回拨仍然无法复活旧的配额。
static void accept_active_day(const char *new_day, u64 day_seq, const char *reason)
{
    u64 now_wall = wall_now();
    copy_fixed_string(g_active_day, sizeof(g_active_day), new_day, strlen(new_day));
    g_guard_day_seq = day_seq;
    g_guard_runtime_seconds = 0;
    if (now_wall > g_guard_wall_max)
        g_guard_wall_max = now_wall;
    g_guard_dirty = true;
    g_usage_count = 0;
    g_state_dirty = false;
    load_state();
    reset_account_total_floors();          // §16: 新的一天, 昨天的服务端合计下限作废
    apply_active_week_for_day(g_active_day);   // §11: 周键跟随(跨周会重置周账本)
    log_message("active day changed to %s (%s)", g_active_day, reason);
}

/// §19: 决定"今天"是哪一天。优先级:
///   1) 最近一次成功同步的服务端本地日(权威值, 允许正常跨日/管理员校时);
///   2) 服务端不可用(长时间离线) -> 主机本地时钟 + 既有日切防回拨守卫(旧行为)。
/// 服务端日驱动意味着: 主机 RTC 跑偏/被改都不会再影响日账本、周账本和"周一到周五封禁".
static void refresh_day_key(void)
{
    load_guard_state();

    char day[16];
    bool from_server = false;

    if (server_day_trusted()) {
        copy_fixed_string(day, sizeof(day), g_server_day, strlen(g_server_day));
        from_server = true;
    } else if (!read_day_key(day, sizeof(day))) {
        // 服务端时间不可用且本地时钟也读不出来: 进入 unknown 模式,
        // 但绝不重置配额、绝不崩溃(与既有行为一致)。
        if (strcmp(g_active_day, "unknown") != 0) {
            copy_fixed_string(g_active_day, sizeof(g_active_day), "unknown", 7);
            g_usage_count = 0;
            load_state();
            reset_account_total_floors();   // §16: 日键失效 -> 服务端合计下限作废
            // §11 周键同样变成未知: 周用量不可信, 配了周上限时一律不拦截。
            copy_fixed_string(g_active_week, sizeof(g_active_week), "unknown", 7);
            g_week_usage_known = false;
            log_message("active day unknown (no server time and clock unavailable); keeping accumulated quota");
        }
        return;
    }

    if (strcmp(day, g_active_day) == 0)
        return;

    int year = 0, month = 0, dom = 0;
    if (sscanf(day, "%d-%d-%d", &year, &month, &dom) != 3)
        return;
    u64 day_seq = day_key_to_seq(year, month, dom);

    if (from_server) {
        // §19: 服务端日是权威值 —— 正常跨日直接采纳; 即使主机时钟被改坏、
        // 甚至在两个不同日期之间来回跳, 也以服务端日为准(不会给出额外配额:
        // 服务端同时下发"今日已玩"下限, 且周账本按服务端周键累计)。
        accept_active_day(day, day_seq, "server time");
        return;
    }

    // ===== 以下为本地时钟兜底(与改造前完全一致)=====
    static char last_rejected_day[16] = "";
    if (g_guard_day_seq == 0) {
        // 首次运行(没有守卫文件): 建立基准并接受当前日期。
        accept_active_day(day, day_seq, "day guard initialised");
        return;
    }

    if (day_seq < g_guard_day_seq) {
        // 观测日期早于已见最大日 => 时间被回拨,绝不重置配额。
        // 长时间离线时每 tick 都会走到这里, 所以只在日期变化时记一行, 避免刷屏。
        if (strcmp(day, last_rejected_day) != 0) {
            copy_fixed_string(last_rejected_day, sizeof(last_rejected_day), day, strlen(day));
            log_message("day rollback rejected: seen=%s now=%s; keeping quota", g_active_day, day);
        }
        return;
    }

    if (day_seq > g_guard_day_seq) {
        bool allow = false;
        u64 now_wall = wall_now();

        if (g_guard_runtime_seconds >= DAY_GUARD_RUNTIME_SECONDS) {
            // 真实运行满 24h: 一定经历了完整的一天。
            allow = true;
        } else if (now_wall > g_guard_wall_max &&
                   (now_wall - g_guard_wall_max) >= DAY_GUARD_WALL_SECONDS) {
            // 墙钟相对"上次接受新的一天"前进满 24h: 正常跨日/关机多天后开机。
            allow = true;
        }

        if (!allow) {
            // 墙钟被瞬时往前拨(不足 24h 就跳到下一天): 视为刷配额,
            // 停在当前窗口,等真实时间走满 24h 再推进。
            if (strcmp(day, last_rejected_day) != 0) {
                copy_fixed_string(last_rejected_day, sizeof(last_rejected_day), day, strlen(day));
                log_message("suspicious day jump %s -> %s ignored (runtime=%llus wall_delta=%llds); keeping quota",
                            g_active_day, day,
                            (unsigned long long)g_guard_runtime_seconds,
                            (long long)((now_wall > g_guard_wall_max) ? (now_wall - g_guard_wall_max) : 0));
            }
            return;
        }
    }

    accept_active_day(day, day_seq, "device time");
}

static bool is_valid_account_id_hex(const char *text)
{
    if (!text || strlen(text) != 32)
        return false;
    for (size_t i = 0; i < 32; i++) {
        u8 nibble;
        if (!hex_value(text[i], &nibble))
            return false;
    }
    return true;
}

//---------------------------------------------------------------------------------
// 账号维度(REFACTOR_SPEC §5,仅 Switch 域)
//
// "当前账号"判定说明(需要真机确认):
//   pdm 的按账号统计只在标题退出/场景切换时落盘,无法用它判断"此刻正在玩的是谁"。
//   因此这里按 best-effort 顺序解析:
//     1) accountGetPreselectedUser(): 最近一次用户选择器选中的账号(最接近"谁启动了这个游戏")。
//     2) accountGetLastOpenedUser(): 系统记录的最近打开过的账号。
//   两者都拿不到就返回空串 => 调用方完全退回设备级记账与设备级限额,
//   绝不会因为"拿不到账号"而变成没有限额。全零 UID 视为无效。
//---------------------------------------------------------------------------------
static const char *account_uid_to_hex(const AccountUid *uid, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    if (!out || out_size < 33)
        return out;

    for (int i = 0; i < 2; i++) {
        for (int b = 0; b < 8; b++) {
            u8 byte = (u8)((uid->uid[i] >> (8 * (7 - b))) & 0xFF);
            out[i * 16 + b * 2] = hex[(byte >> 4) & 0xF];
            out[i * 16 + b * 2 + 1] = hex[byte & 0xF];
        }
    }
    out[32] = '\0';
    return out;
}

static const char *resolve_current_account(void)
{
    if (!g_account_ready)
        return g_current_account;

    AccountUid last_uid;
    AccountUid pre_uid;
    memset(&last_uid, 0, sizeof(last_uid));
    memset(&pre_uid, 0, sizeof(pre_uid));

    Result last_rc = accountGetLastOpenedUser(&last_uid);
    Result pre_rc = accountGetPreselectedUser(&pre_uid);

    char last_hex[33] = "";
    char pre_hex[33] = "";
    if (R_SUCCEEDED(last_rc) && accountUidIsValid(&last_uid))
        account_uid_to_hex(&last_uid, last_hex, sizeof(last_hex));
    if (R_SUCCEEDED(pre_rc) && accountUidIsValid(&pre_uid))
        account_uid_to_hex(&pre_uid, pre_hex, sizeof(pre_hex));

    bool has_last = last_hex[0] && strcmp(last_hex, "0000000000000000") != 0;
    bool has_pre = pre_hex[0] && strcmp(pre_hex, "0000000000000000") != 0;
    const char *picked = has_last ? last_hex : (has_pre ? pre_hex : NULL);

    if (picked) {
        if (strcmp(picked, g_current_account) != 0) {
            copy_fixed_string(g_current_account, sizeof(g_current_account), picked, strlen(picked));
            log_message("current account resolved account=%s source=%s last_opened=%s preselected=%s",
                        g_current_account, has_last ? "last_opened" : "preselected",
                        has_last ? last_hex : "(none)", has_pre ? pre_hex : "(none)");
        }
        return g_current_account;
    }

    if (g_current_account[0]) {
        log_message("current account unresolved; falling back to device-level accounting");
        g_current_account[0] = '\0';
    }
    return g_current_account;
}

//---------------------------------------------------------------------------------
// 上传状态: (账号, 游戏) -> pending/total/launches + record_uid。
// record_uid 随状态一起持久化,保证重试复用(item 16)。
//---------------------------------------------------------------------------------
static void load_upload_state(void)
{
    static char body[MAX_CONFIG_SIZE];

    if (!g_sd_ready)
        return;

    bool exists = false;
    if (!read_whole_file(UPLOAD_STATE_PATH, body, sizeof(body), &exists, NULL)) {
        // 读失败绝不能让这些记账清零,否则 pending 时长会静默丢失。
        if (exists)
            log_message("limiter_upload_state.csv unreadable; keeping %u in-memory upload entries",
                        (unsigned)g_upload_count);
        return;
    }

    g_upload_count = 0;
    char *saveptr = NULL;
    char *line = strtok_r(body, "\n", &saveptr);
    while (line && g_upload_count < MAX_USAGE) {
        char *field[8];
        int n = 0;
        char *p = line;
        while (n < 8) {
            field[n++] = p;
            char *comma = strchr(p, ',');
            if (!comma) break;
            *comma = '\0';
            p = comma + 1;
        }
        for (int i = 0; i < n; i++) {
            size_t len = strlen(field[i]);
            while (len > 0 && (field[i][len - 1] == '\n' || field[i][len - 1] == '\r'))
                field[i][--len] = '\0';
        }

        if (n < 2 || strcmp(field[0], "title_id") == 0)
            goto next_line;   // 空行或表头

        const char *account_id = "";
        const char *record_uid = "";
        for (int i = 1; i < n; i++) {
            if (is_valid_account_id_hex(field[i]))
                account_id = field[i];
            else if (strncmp(field[i], "sys-", 4) == 0)
                record_uid = field[i];
        }

        u64 title_id = 0;
        if (!parse_title_id(field[0], strlen(field[0]), &title_id))
            goto next_line;

        {
            unsigned long pending = strtoul(field[1], NULL, 10);
            unsigned long long total = n > 2 ? strtoull(field[2], NULL, 10) : pending;
            unsigned long launches = n > 3 ? strtoul(field[3], NULL, 10) : 0;
            unsigned long pending_launches = n > 4 ? strtoul(field[4], NULL, 10) : 0;

            if (pending == 0 && total == 0 && launches == 0 && pending_launches == 0 &&
                record_uid[0] == '\0')
                goto next_line;

            UploadEntry *entry = &g_upload[g_upload_count++];
            memset(entry, 0, sizeof(*entry));
            copy_fixed_string(entry->account_id, sizeof(entry->account_id), account_id, strlen(account_id));
            entry->title_id = title_id;
            entry->pending_seconds = (u32)pending;
            entry->total_seconds = (u64)total;
            entry->launch_count = (u32)launches;
            entry->pending_launches = (u32)pending_launches;
            copy_fixed_string(entry->record_uid, sizeof(entry->record_uid), record_uid, strlen(record_uid));
        }

next_line:
        line = strtok_r(NULL, "\n", &saveptr);
    }

    g_upload_dirty = false;
    log_message("loaded %u upload entries", (unsigned)g_upload_count);
}

static void save_upload_state(void)
{
    if (!g_sd_ready)
        return;

    if (!g_upload_dirty && !g_uid_epoch_dirty)
        return;

    static char body[MAX_CONFIG_SIZE];
    size_t used = 0;
    int needed = snprintf(body, sizeof(body),
                          "title_id,pending_seconds,total_seconds,launch_count,pending_launches,account_id,record_uid\n");
    if (needed < 0 || (size_t)needed >= sizeof(body))
        return;
    used = (size_t)needed;

    for (size_t i = 0; i < g_upload_count; i++) {
        if (g_upload[i].pending_seconds == 0 && g_upload[i].pending_launches == 0 &&
            g_upload[i].record_uid[0] == '\0')
            continue;   // 已确认送达的零值项不落盘

        needed = snprintf(body + used, sizeof(body) - used, "%016lX,%u,%llu,%u,%u,%s,%s\n",
                          g_upload[i].title_id,
                          g_upload[i].pending_seconds,
                          (unsigned long long)g_upload[i].total_seconds,
                          g_upload[i].launch_count,
                          g_upload[i].pending_launches,
                          g_upload[i].account_id,
                          g_upload[i].record_uid);
        if (needed < 0 || (size_t)needed >= sizeof(body) - used) {
            log_message("limiter_upload_state.csv too large; not writing (keeping previous file)");
            return;
        }
        used += (size_t)needed;
    }

    if (write_file_atomic(UPLOAD_STATE_PATH, body, used)) {
        g_upload_dirty = false;
        g_uid_epoch_dirty = false;
    } else {
        log_message("failed to write limiter_upload_state.csv");
    }
}

//---------------------------------------------------------------------------------
// §9 前台焦点判定实现
//---------------------------------------------------------------------------------
static const char *focus_state_name(FocusState state)
{
    switch (state) {
    case FocusState_Foreground: return "foreground";
    case FocusState_Background: return "background";
    case FocusState_Suspect:    return "suspect";
    case FocusState_Unknown:
    default:                    return "unknown";
    }
}

// PdmAppletEvent.timestamp_user 在 [16.0.0+] 是 PosixTime; 旧固件上由 libnx 从
// PdmAppletEventV1 转换而来, 可能仍是"1999-12-31 起算的分钟数"(< 1e9)。
// 两种都识别; 认不出来(0 / 异常值)就返回 0 = 时间未知, 调用方会退回 suspect,
// 绝不会因为"时间读不出来"就停止计时。
static u64 applet_event_wall_time(const PdmAppletEvent *event)
{
    u64 stamp = event->timestamp_user;
    if (stamp == 0)
        return 0;
    if (stamp < 1000000000ULL)
        return pdmPlayTimestampToPosix((u32)stamp);
    return stamp;
}

// 扫描 PDM applet 事件日志末尾若干条, 取"全局最新焦点事件"与"当前标题最新事件"。
// 返回 false = 读取失败/服务不可用(调用方维持 old behaviour)。
static bool read_applet_focus_events(u64 title_id, FocusStatus *out, u64 *focus_wall, u64 *ours_wall)
{
    s32 total_entries = 0;
    s32 start_index = 0;
    s32 end_index = 0;

    if (R_FAILED(pdmqryGetAvailablePlayEventRange(&total_entries, &start_index, &end_index)))
        return false;
    if (end_index < 0 || start_index < 0 || end_index < start_index)
        return false;

    s32 first = end_index - (FOCUS_EVENT_SCAN_COUNT - 1);
    if (first < start_index)
        first = start_index;
    s32 count = end_index - first + 1;
    if (count <= 0)
        return false;
    if (count > FOCUS_EVENT_SCAN_COUNT)
        count = FOCUS_EVENT_SCAN_COUNT;

    static PdmAppletEvent events[FOCUS_EVENT_SCAN_COUNT];
    s32 got = 0;
    if (R_FAILED(pdmqryQueryAppletEvent(first, true, events, count, &got)))
        return false;
    if (got > FOCUS_EVENT_SCAN_COUNT)
        got = FOCUS_EVENT_SCAN_COUNT;
    if (got <= 0)
        return true;   // 读成功但窗口内没有事件: 调用方按 unknown 处理

    s32 newest_focus_index = -1;
    s32 newest_ours_index = -1;

    for (s32 i = 0; i < got; i++) {
        const PdmAppletEvent *event = &events[i];
        int type = (int)event->event_type;
        bool ours = (event->program_id == title_id);
        s32 index = (s32)event->entry_index;

        bool is_focus_transition = (type == PdmAppletEventType_InFocus ||
                                    type == PdmAppletEventType_OutOfFocus ||
                                    type == PdmAppletEventType_OutOfFocus4);
        // Launch 也算"我们刚被启动"的弱前台证据; Exit 系列不参与判定。
        bool is_focusish_ours = is_focus_transition || (type == PdmAppletEventType_Launch);

        if (is_focus_transition && index >= newest_focus_index) {
            newest_focus_index = index;
            out->newest_focus_event = type;
            out->newest_focus_is_ours = ours;
            *focus_wall = applet_event_wall_time(event);
        }

        if (ours && is_focusish_ours && index >= newest_ours_index) {
            newest_ours_index = index;
            out->newest_ours_event = type;
            *ours_wall = applet_event_wall_time(event);
        }
    }

    return true;
}

static void update_focus_status(bool have_app, u64 app_pid, u64 title_id)
{
    FocusStatus next;
    memset(&next, 0, sizeof(next));
    next.state = FocusState_Unknown;
    next.source = "none";
    next.have_app_pid = have_app;
    next.app_pid = app_pid;
    next.pdm_ready = g_pdm_ready;
    next.newest_focus_event = -1;
    next.newest_ours_event = -1;
    next.evidence_age_seconds = -1;
    next.apm_perf = -1;

    // apm 只作为诊断信号上报, 不参与状态判定(性能模式与"是否被玩"不是一回事)。
    if (g_apm_ready) {
        ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
        if (R_SUCCEEDED(apmGetPerformanceMode(&mode))) {
            next.have_apm = true;
            next.apm_perf = (int)mode;
        }
    }

    if (have_app && g_pdm_ready) {
        u64 focus_wall = 0;
        u64 ours_wall = 0;

        if (read_applet_focus_events(title_id, &next, &focus_wall, &ours_wall)) {
            // §9 冻结枚举里没有 "pdm", 而我们所有判定都建立在 PDM 焦点事件这条
            // 间接证据上, 因此按契约取 "heuristic"(原始信号在 signals 里逐项给出)。
            next.source = "heuristic";

            u64 evidence_wall = 0;
            FocusState raw = FocusState_Unknown;

            if (next.newest_focus_event == PdmAppletEventType_InFocus) {
                // 全局最新一次焦点切换就是 in_focus: 属于我们 => 前台;
                // 属于别的 applet/标题 => 我们被切到后台(确证)。
                raw = next.newest_focus_is_ours ? FocusState_Foreground : FocusState_Background;
                evidence_wall = focus_wall;
            } else if (next.newest_ours_event == PdmAppletEventType_InFocus) {
                raw = FocusState_Foreground;
                evidence_wall = ours_wall;
            } else if (next.newest_ours_event == PdmAppletEventType_OutOfFocus ||
                       next.newest_ours_event == PdmAppletEventType_OutOfFocus4) {
                // 我们最后一次焦点切换是"失焦", 且此后没有任何 in_focus => 后台。
                raw = FocusState_Background;
                evidence_wall = ours_wall;
            } else if (next.newest_ours_event == PdmAppletEventType_Launch) {
                // 刚启动、焦点事件还没落盘 => 前台(绝不因此漏计时)。
                raw = FocusState_Foreground;
                evidence_wall = ours_wall;
            } else if (next.newest_focus_event >= 0) {
                // 只看到"别的 applet 失焦"这类无法定位焦点的记录 => suspect(继续计时)。
                raw = FocusState_Suspect;
                evidence_wall = focus_wall;
            }

            if (evidence_wall > 0) {
                u64 now_wall = wall_now();
                next.evidence_age_seconds = (now_wall >= evidence_wall)
                                                ? (long long)(now_wall - evidence_wall)
                                                : 0;   // 时钟异常 => 视为"刚发生"
            }

            bool settled = (next.evidence_age_seconds >= FOCUS_BACKGROUND_MIN_AGE_SECONDS);
            if (raw == FocusState_Background && settled)
                next.background_streak = g_focus.background_streak + 1;

            if (raw == FocusState_Background) {
                // 只有"证据已沉淀 + 连续确认"才允许暂停记账; 否则 suspect = 继续计时。
                next.state = (settled && next.background_streak >= FOCUS_BACKGROUND_CONFIRM_POLLS)
                                 ? FocusState_Background
                                 : FocusState_Suspect;
            } else {
                next.state = raw;
            }
        }
        // read 失败: 保持 FocusState_Unknown / source "none"(old behaviour)。
    }

    if (!g_focus_log_initialised || next.state != g_focus.state) {
        log_message("focus %s -> %s app_pid=%llu have_app=%u pdm_ready=%u "
                    "pdm_focus_event=%d pdm_focus_ours=%d pdm_ours_event=%d "
                    "evidence_age=%llds apm_perf=%d streak=%u",
                    focus_state_name(g_focus.state), focus_state_name(next.state),
                    (unsigned long long)next.app_pid, next.have_app_pid ? 1u : 0u,
                    next.pdm_ready ? 1u : 0u,
                    next.newest_focus_event, next.newest_focus_is_ours ? 1 : 0,
                    next.newest_ours_event, next.evidence_age_seconds,
                    next.have_apm ? next.apm_perf : -1,
                    next.background_streak);
        g_focus_log_initialised = true;
    }

    g_focus = next;
}

// 心跳里的 §9 诊断字段。只包含真实读到的信号, 保持很小(心跳每几分钟一次)。
static void append_focus_heartbeat_json(char *body, size_t body_size)
{
    char entry[256];

    json_append(body, body_size, ",\"focus\":{\"state\":\"");
    json_append(body, body_size, focus_state_name(g_focus.state));
    json_append(body, body_size, "\",\"signals\":{");

    snprintf(entry, sizeof(entry),
             "\"app_pid\":%llu,\"pdm_ready\":%s,\"pdm_focus_event\":%d,"
             "\"pdm_focus_ours\":%s,\"pdm_ours_event\":%d,\"evidence_age_s\":%lld",
             (unsigned long long)(g_focus.have_app_pid ? g_focus.app_pid : 0),
             g_focus.pdm_ready ? "true" : "false",
             g_focus.newest_focus_event,
             g_focus.newest_focus_is_ours ? "true" : "false",
             g_focus.newest_ours_event,
             g_focus.evidence_age_seconds);
    json_append(body, body_size, entry);

    if (g_focus.have_apm) {
        snprintf(entry, sizeof(entry), ",\"apm_perf\":%d", g_focus.apm_perf);
        json_append(body, body_size, entry);
    }

    json_append(body, body_size, "},\"source\":\"");
    json_append(body, body_size, g_focus.source);
    json_append(body, body_size, "\"}");
}

static bool query_foreground_application(u64 *pid_out, u64 *title_id_out)
{
    if (!g_pmdmnt_ready || !g_pminfo_ready)
        return false;

    u64 pid = 0;
    Result rc = pmdmntGetApplicationProcessId(&pid);
    if (R_FAILED(rc))
        return false;

    u64 title_id = 0;
    rc = pminfoGetProgramId(&title_id, pid);
    if (R_FAILED(rc))
        rc = pmdmntGetProgramId(&title_id, pid);
    if (R_FAILED(rc))
        return false;

    *pid_out = pid;
    *title_id_out = title_id;
    return true;
}

static void enforce_limit(u64 pid, const char *account_id, u64 title_id, UsageEntry *usage,
                          const LimitRule *rule, const char *source)
{
    int weekday_index = weekday_index_from_day_key(g_active_day);
    u32 effective_daily = (weekday_index >= 0) ? rule_effective_daily(rule, weekday_index) : 0;
    u32 week_used = 0;
    bool week_known = week_used_seconds(account_id, title_id, &week_used);

    log_message("limit reached title=%016lX pid=%016lX account=%s source=%s used=%u effective_daily=%u "
                "weekly_limit=%u weekly_used=%u(known=%d)",
                title_id, pid, (account_id && account_id[0]) ? account_id : "(device)",
                source, usage ? usage->used_seconds : 0, effective_daily,
                rule->weekly_seconds, week_used, week_known ? 1 : 0);

    if (!g_pmshell_ready) {
        log_message("pm:shell not ready, cannot terminate title=%016lX", title_id);
        return;
    }

    Result rc = pmshellTerminateProcess(pid);
    if (R_FAILED(rc)) {
        log_message("pmshellTerminateProcess failed rc=0x%x, trying program terminate", rc);
        rc = pmshellTerminateProgram(title_id);
    }

    if (R_SUCCEEDED(rc))
        log_message("terminated title=%016lX", title_id);
    else
        log_message("terminate failed title=%016lX rc=0x%x", title_id, rc);
}

//---------------------------------------------------------------------------------
// §11 规则判定(唯一真源)
//---------------------------------------------------------------------------------

/// §11 核心判定: 返回哪个上限触发/生效(LIMIT_SOURCE_*), LIMIT_SOURCE_NONE = 未触发。
///
/// 拦截条件(**任一满足即拦截, OR 不是 AND**):
///   1) today_used >= effective_daily 且 effective_daily > 0;
///   2) week_used  >= weekly_seconds   且 weekly_seconds   > 0。
/// §15: effective_daily = has_weekday ? weekday_seconds[today] : daily_seconds,
/// 且 effective_daily == 0 本身即"当天禁止游玩"(按星期配置里的 0 也照样生效)。
///
/// 任何不确定一律返回 NONE(不拦截)并在 out_reason 里说明:
///   - 星期几算不出(时钟/日键不可用);
///   - 配了周上限但周用量算不出(周账本不可信);
///   - 用量记账项拿不到(表满)。
/// 兼容: 整条规则三个上限全为 0 时, 沿用既有"0 分钟 = 封禁"语义(immediate block),
/// 因为服务端仍把 `enabled && daily==0` 归类为 block 并下发 `limits[title]=0`。
static const char *evaluate_limit_rule(const LimitRule *rule, const char *account_id, u64 title_id,
                                       UsageEntry *usage, const char **out_reason)
{
    static const char *no_reason = "";
    if (out_reason)
        *out_reason = no_reason;

    if (!usage) {
        if (out_reason)
            *out_reason = "usage ledger unavailable";
        return LIMIT_SOURCE_NONE;
    }

    int weekday_index = weekday_index_from_day_key(g_active_day);
    // §15.2: 有可信的服务端"今日生效值"时不需要本地星期几(设备时钟可能不准);
    // 只有两者都没有时才按 fail-safe 不拦。
    if (weekday_index < 0 && !(rule->has_server_today && server_sync_recent())) {
        if (out_reason)
            *out_reason = "weekday unknown (clock/day key unavailable)";
        return LIMIT_SOURCE_NONE;
    }

    u32 effective_daily = rule_effective_daily(rule, weekday_index);

    // §15.1: 当日生效上限 = 0 -> 禁止游玩(block)。按星期已配置时为该天显式 0;
    // 未配置时即既有的"0 分钟 = 封禁"旧语义。绝不因为 0 就当成"没限额"。
    if (effective_daily == 0) {
        if (out_reason)
            *out_reason = rule->has_weekday ? "weekday blocked (0 = no play)"
                                            : "daily blocked (0 minutes)";
        return rule->has_weekday ? LIMIT_SOURCE_WEEKDAY : LIMIT_SOURCE_DAILY;
    }

    if (usage->used_seconds >= effective_daily) {
        if (out_reason)
            *out_reason = rule->has_weekday ? "weekday cap reached" : "daily cap reached";
        return rule->has_weekday ? LIMIT_SOURCE_WEEKDAY : LIMIT_SOURCE_DAILY;
    }

    if (rule->weekly_seconds > 0) {
        u32 week_used = 0;
        if (!week_used_seconds(account_id, title_id, &week_used)) {
            // 周用量算不出来 -> 绝不因为"可能超了"而杀。
            if (out_reason)
                *out_reason = "weekly usage unknown";
            return LIMIT_SOURCE_NONE;
        }
        if (week_used >= rule->weekly_seconds) {
            if (out_reason)
                *out_reason = "weekly cap reached";
            return LIMIT_SOURCE_WEEKLY;
        }
    }

    if (!rule_has_any_cap(rule)) {
        // 全部上限为 0 的显式规则 = 旧语义"0 分钟封禁"(已被上面的
        // effective_daily == 0 分支覆盖, 这里仅作防御性兜底)。
        if (out_reason)
            *out_reason = "explicit zero-minute block rule";
        return LIMIT_SOURCE_DAILY;
    }

    return LIMIT_SOURCE_NONE;
}

//---------------------------------------------------------------------------------
// §10 执行方式判定
//---------------------------------------------------------------------------------
static const char *enforce_mode_name(EnforceMode mode)
{
    switch (mode) {
    case EnforceMode_BlockLaunch: return ENFORCE_MODE_BLOCK_LAUNCH;
    case EnforceMode_Kill:        return ENFORCE_MODE_KILL;
    default:                      return "unknown";
    }
}

/// 把配置里的字符串解析成执行方式。空串(字段缺失)按契约默认 block_launch;
/// 取值无法识别 -> Unknown(不终止), 不猜、不改写。
static EnforceMode parse_enforce_mode(const char *value)
{
    if (!value || !value[0])
        return EnforceMode_BlockLaunch;
    if (strcmp(value, ENFORCE_MODE_BLOCK_LAUNCH) == 0)
        return EnforceMode_BlockLaunch;
    if (strcmp(value, ENFORCE_MODE_KILL) == 0)
        return EnforceMode_Kill;
    return EnforceMode_Unknown;
}

static EnforceMode effective_enforce_mode(void)
{
    EnforceMode mode = parse_enforce_mode(g_config.enforce_mode);
    if (!g_enforce_mode_logged || mode != g_logged_enforce_mode) {
        log_message("enforce_mode effective=%s (config=\"%s\")",
                    enforce_mode_name(mode), g_config.enforce_mode);
        g_logged_enforce_mode = mode;
        g_enforce_mode_logged = true;
    }
    return mode;
}

//---------------------------------------------------------------------------------
// §16 账号级"全部游戏合计"限额的强制(与单游戏规则 OR)
//
//   block_launch: 进入**任何**一局时若合计已到上限 -> 启动拦截(与 §10 同语义:
//                 只需在进入时判定一次并冻结; 中途才到上限不打断当前这局)。
//   kill:        运行中合计到上限 -> 立即终止。
//
// 这里**不动** should_terminate_now 的既有语义: 单游戏判定与账号合计判定各自
// 独立求值, 调用方取 OR。任何不确定(账号未知 / 没有合计规则 / 星期算不出 /
// 账本拿不到)一律返回 false —— 宁可不拦, 不可误杀。
//---------------------------------------------------------------------------------

/// 进入本局时冻结账号合计判定, 并写一条便于排查的 §16 日志。
static void decide_account_total_session(const char *account_id)
{
    g_session_account_total_decided = false;
    g_session_account_total_over_at_entry = false;
    g_account_total_exceed_logged = false;

    if (effective_enforce_mode() == EnforceMode_Unknown)
        return;                                   // 模式无法识别 -> 不判定、不终止

    AccountTotalDecision decision;
    evaluate_account_total_limit(account_id, &decision);

    g_session_account_total_decided = decision.determinate;
    g_session_account_total_over_at_entry = decision.over;

    log_message("§16 session entry account=%s total_used=%llu limit=%u source=%s over=%u",
                (account_id && account_id[0]) ? account_id : "(device)",
                (unsigned long long)decision.used, decision.limit, decision.source,
                decision.over ? 1u : 0u);
}

/// §16 现在是否应当因"账号合计超限"而终止/拦截。
/// *out_source 返回触发来源(LIMIT_SOURCE_DAILY / WEEKDAY / NONE)。
static bool account_total_should_terminate(const char *account_id, const char **out_source)
{
    if (out_source)
        *out_source = LIMIT_SOURCE_NONE;

    AccountTotalDecision decision;
    evaluate_account_total_limit(account_id, &decision);
    if (out_source)
        *out_source = decision.source;

    if (!decision.over)
        return false;                             // 未到上限或不判定 -> 不拦

    // 到达/封禁: 写 §16 决策日志(每局最多一条, 防止刷屏)。
    if (!g_account_total_exceed_logged) {
        log_message("§16 account total account=%s used=%llu limit=%u source=%s",
                    (account_id && account_id[0]) ? account_id : "(device)",
                    (unsigned long long)decision.used, decision.limit, decision.source);
        g_account_total_exceed_logged = true;
    }

    EnforceMode mode = effective_enforce_mode();
    if (mode == EnforceMode_Kill)
        return true;                              // 旧行为: 超限即杀

    if (mode == EnforceMode_Unknown)
        return false;                             // 模式无法识别: 宁可不杀

    // block_launch: 只认"进入本局时"冻结下来的判定。
    if (g_session_account_total_decided && g_session_account_total_over_at_entry)
        return true;                              // 启动拦截(反复重试直到进程退出)

    if (!g_session_exceed_logged) {
        log_message("§10 block_launch: account total exceeded mid-session but session is allowed to finish "
                    "account=%s used=%llu limit=%u source=%s",
                    (account_id && account_id[0]) ? account_id : "(device)",
                    (unsigned long long)decision.used, decision.limit, decision.source);
        g_session_exceed_logged = true;
    }
    return false;
}

/// 进入当前前台 title 时调用一次: 冻结"进入时已用秒数", 并用**进入时的规则**做出
/// 本局唯一一次 block_launch 判定(kill 模式不使用该判定)。
///   rule_known=false 表示进入时该游戏没有任何限额条目 -> 不做判定, 本局永不终止
///   (与轮询路径"没有额度就不执行限额"的既有行为一致)。
/// §11: 判定同时考虑 日/按星期/周 三个上限(OR)。
static void decide_enforce_session(const char *account_id, u64 title_id, bool rule_known,
                                   const LimitRule *rule)
{
    g_session_entry_used = 0;
    g_session_entry_known = false;
    g_session_decided = false;
    g_session_over_limit_at_entry = false;
    g_session_exceed_logged = false;

    const char *resolved_account = account_id ? account_id : "";

    if (!rule_known || !rule) {
        // §14: 没有限额条目时也要把"真实今日用量"写进日志 —— 这正对应线上 bug 中
        // "没有限额期间玩掉的时长从未进入日账本"的场景, 是排查"中途新增限额从 0
        // 起算"的关键证据(修复后账本始终按真实时长累加, 与是否存在限额无关)。
        UsageEntry *usage = get_usage_entry(resolved_account, title_id);
        log_message("§14 session entry title=%016lX used_today=%llu limit=%u over=%u",
                    title_id, usage ? (unsigned long long)usage->used_seconds : 0ULL, 0u, 0u);
        return;
    }

    UsageEntry *usage = get_usage_entry(resolved_account, title_id);
    if (!usage) {
        // 用量记账表写不进去(容量满) -> 无法判断进入时是否已超限 -> 本局不杀。
        log_message("§11 session entry usage unavailable title=%016lX; will not terminate this session",
                    title_id);
        return;
    }

    g_session_entry_used = usage->used_seconds;
    g_session_entry_known = true;
    g_session_decided = true;

    {
        const char *reason = "";
        const char *source = evaluate_limit_rule(rule, resolved_account, title_id, usage, &reason);
        g_session_over_limit_at_entry = (strcmp(source, LIMIT_SOURCE_NONE) != 0);
        // §14: 进入本局时的真实今日用量 vs 今日生效限额(修复后日账本不再在限额处
        // 截断, 所以这个值就是真实已玩时长)。over 直接取 evaluate_limit_rule 的判定,
        // 因此周上限先到 / 全 0 封禁 也会正确记为 1。
        log_message("§14 session entry title=%016lX used_today=%llu limit=%u over=%u",
                    title_id, (unsigned long long)g_session_entry_used,
                    rule_effective_daily(rule, weekday_index_from_day_key(g_active_day)),
                    g_session_over_limit_at_entry ? 1u : 0u);
        log_message("§11 session entry title=%016lX used=%llu weekly_limit=%u source=%s%s%s",
                    title_id, (unsigned long long)g_session_entry_used, rule->weekly_seconds,
                    source, reason[0] ? " (" : "", reason[0] ? reason : "");
    }
}

static void close_enforce_session(void)
{
    g_session_entry_used = 0;
    g_session_entry_known = false;
    g_session_decided = false;
    g_session_over_limit_at_entry = false;
    g_session_exceed_logged = false;
    // §16 账号合计限额的本局冻结状态一并清空(下一局重新在进入时判定)。
    g_session_account_total_decided = false;
    g_session_account_total_over_at_entry = false;
    g_account_total_exceed_logged = false;
    // §16/§10: 本局账号也一并清空(下一局进入时重新记录)。
    g_session_account[0] = '\0';
}

/// §10 核心判定: 返回 true 表示"现在应当终止该进程"。
///
/// 只在 §11 的 OR 规则**确实**判定为已超限时才可能返回 true; 任何不确定一律返回 false。
static bool should_terminate_now(u64 title_id, const char *account_id, UsageEntry *usage,
                                 const LimitRule *rule)
{
    EnforceMode mode = effective_enforce_mode();
    const char *reason = "";
    const char *source = evaluate_limit_rule(rule, account_id ? account_id : "", title_id, usage, &reason);

    // 注意: 这里不能用"限额为 0"当作"读不到额度" —— 0 分钟是合法配置("0 blocks play",
    // 见 evaluate_limit_rule 的全 0 规则分支), 必须继续拦截。
    if (strcmp(source, LIMIT_SOURCE_NONE) == 0) {
        // 未触发, 或"算不出来"(周用量未知/星期几未知/记账项拿不到) -> 不终止。
        if (reason[0] && !g_session_exceed_logged) {
            log_message("§11 not terminating (indeterminate) title=%016lX reason=%s", title_id, reason);
            g_session_exceed_logged = true;
        }
        return false;
    }

    if (mode == EnforceMode_Kill)
        return true;                       // 旧行为: 超限即杀

    if (mode == EnforceMode_Unknown) {
        // 配置取值无法识别: 宁可不杀, 不可误杀。
        if (!g_session_exceed_logged) {
            log_message("§10 enforce_mode unknown (\"%s\"); not terminating title=%016lX source=%s reason=%s",
                        g_config.enforce_mode, title_id, source, reason);
            g_session_exceed_logged = true;
        }
        return false;
    }

    // block_launch: 只认"进入本局时"冻结下来的判定。
    if (!g_session_decided || !g_session_entry_known) {
        // 进入时没有可判定的状态(当时没额度 / 记账项拿不到) -> 本局不终止。
        if (!g_session_exceed_logged) {
            log_message("§10 block_launch: session entry not decidable; not terminating "
                        "title=%016lX source=%s reason=%s",
                        title_id, source, reason);
            g_session_exceed_logged = true;
        }
        return false;
    }
    if (g_session_over_limit_at_entry) {
        // 进入时已超限 -> 启动拦截(反复重试直到进程真的退出)。
        log_message("§10 block_launch launch interception title=%016lX entry_used=%llu source=%s reason=%s",
                    title_id, (unsigned long long)g_session_entry_used, source, reason);
        return true;
    }

    // 进入时未超限 -> 本局永不终止(即使中途超限、即使限额被改小)。记账照常。
    if (!g_session_exceed_logged) {
        log_message("§10 block_launch: quota exceeded mid-session but session is allowed to finish "
                    "title=%016lX entry_used=%llu source=%s reason=%s",
                    title_id, (unsigned long long)g_session_entry_used, source, reason);
        g_session_exceed_logged = true;
    }
    return false;
}

/// §10 启动拦截: 进入 title 的第一个轮询就判定一次(加载画面阶段结束进程)。
/// 返回 true 表示已终止(调用方需要重置 current_pid/current_title_id)。
static bool enforce_limit_at_session_entry(u64 pid, const char *account_id, u64 title_id,
                                           bool rule_known, const LimitRule *rule)
{
    // §9: 只有确证 background 才暂停限额执行。刚启动的标题本不该被判成 background,
    // 但这里保持一致, 任何非 foreground 的疑点都退回轮询路径去处理。
    if (g_focus.state == FocusState_Background) {
        log_message("§10 session entry interception skipped (focus=background) title=%016lX", title_id);
        return false;
    }

    const char *resolved_account = account_id ? account_id : "";

    log_message("§10 session entry check title=%016lX pid=%016lX account=%s rule_known=%d focus_state=%d",
                title_id, pid, resolved_account[0] ? resolved_account : "(device)",
                rule_known ? 1 : 0, (int)g_focus.state);

    // §16: 账号级"全部游戏合计"限额与单游戏限额是 OR —— 即使该 title 没有任何
    // 单游戏限额条目(rule_known=false), 只要账号合计在进入时已达上限就要拦截。
    // 判定已在 decide_account_total_session() 里冻结; 这里与 §10 一致地只做拦截。
    {
        const char *total_source = LIMIT_SOURCE_NONE;
        if (account_total_should_terminate(resolved_account, &total_source)) {
            UsageEntry *total_usage = get_usage_entry(resolved_account, title_id);
            LimitRule total_rule;
            memset(&total_rule, 0, sizeof(total_rule));
            save_state();
            save_week_state();
            log_message("§10 session entry terminate reason=account_total title=%016lX account=%s source=%s used=%u",
                        title_id, resolved_account[0] ? resolved_account : "(device)", total_source,
                        total_usage ? total_usage->used_seconds : 0);
            enforce_limit(pid, resolved_account, title_id, total_usage, &total_rule, total_source);
            return true;
        }
    }

    if (!rule_known || !rule) {
        log_message("§10 session entry pass title=%016lX reason=no_rule", title_id);
        return false;                      // 没有任何单游戏限额条目 -> 不拦截(与轮询路径一致)
    }

    UsageEntry *usage = get_usage_entry(resolved_account, title_id);
    if (!usage) {
        log_message("§10 session entry pass title=%016lX reason=usage_unavailable", title_id);
        return false;
    }

    const char *reason = "";
    const char *source = evaluate_limit_rule(rule, resolved_account, title_id, usage, &reason);
    if (!should_terminate_now(title_id, resolved_account, usage, rule)) {
        log_message("§10 session entry pass title=%016lX reason=under_limit source=%s used=%u",
                    title_id, source, usage ? usage->used_seconds : 0);
        return false;
    }

    save_state();
    save_week_state();
    log_message("§10 session entry terminate reason=single_game title=%016lX account=%s source=%s used=%u reason=%s",
                title_id, resolved_account[0] ? resolved_account : "(device)", source,
                usage ? usage->used_seconds : 0, reason ? reason : "");
    enforce_limit(pid, resolved_account, title_id, usage, rule, source);
    return true;
}

static void retry_runtime_services(void)
{
    static u32 attempts = 0;
    attempts++;

    if (g_fs_ready && !g_sd_ready) {
        Result rc = fsdevMountSdmc();
        g_sd_ready = R_SUCCEEDED(rc);
    }

    if (!g_time_ready) {
        Result rc = timeInitialize();
        g_time_ready = R_SUCCEEDED(rc);
    }

    if (!g_pmdmnt_ready) {
        Result rc = pmdmntInitialize();
        g_pmdmnt_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && (attempts <= 5 || attempts % 12 == 0))
            log_message("pmdmntInitialize rc=0x%x ready=%u", rc, g_pmdmnt_ready ? 1 : 0);
    }

    if (!g_pminfo_ready) {
        Result rc = pminfoInitialize();
        g_pminfo_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && (attempts <= 5 || attempts % 12 == 0))
            log_message("pminfoInitialize rc=0x%x ready=%u", rc, g_pminfo_ready ? 1 : 0);
    }

    if (!g_pmshell_ready) {
        Result rc = pmshellInitialize();
        g_pmshell_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && (attempts <= 5 || attempts % 12 == 0))
            log_message("pmshellInitialize rc=0x%x ready=%u", rc, g_pmshell_ready ? 1 : 0);
    }

    if (!g_pdm_ready) {
        // §9: 前台焦点判定依赖 pdm:qry 的 applet 焦点事件日志。
        // 拿不到就只是退化为 unknown(继续按旧行为计时), 不影响其它功能。
        Result rc = pdmqryInitialize();
        g_pdm_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && (attempts <= 5 || attempts % 12 == 0))
            log_message("pdmqryInitialize rc=0x%x ready=%u", rc, g_pdm_ready ? 1 : 0);
    }

    if (!g_apm_ready) {
        // apm 仅用于诊断信号(apm_perf), 不参与焦点判定。
        Result rc = apmInitialize();
        g_apm_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && (attempts <= 5 || attempts % 12 == 0))
            log_message("apmInitialize rc=0x%x ready=%u", rc, g_apm_ready ? 1 : 0);
    }

    if (!g_psm_ready) {
        Result rc = psmInitialize();
        g_psm_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && (attempts <= 5 || attempts % 12 == 0))
            log_message("psmInitialize rc=0x%x ready=%u", rc, g_psm_ready ? 1 : 0);
    }

    if (!g_ns_ready) {
        Result rc = nsInitialize();
        g_ns_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && (attempts <= 5 || attempts % 12 == 0))
            log_message("nsInitialize rc=0x%x ready=%u", rc, g_ns_ready ? 1 : 0);
    }

    if (!g_spsm_ready) {
        Result rc = spsmInitialize();
        g_spsm_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && (attempts <= 5 || attempts % 12 == 0))
            log_message("spsmInitialize rc=0x%x ready=%u", rc, g_spsm_ready ? 1 : 0);
    }

    if (!g_account_ready) {
        // 账号服务在 boot2 阶段可能还没就绪,和 ns 一样放进重试循环初始化。
        Result rc = accountInitialize(AccountServiceType_System);
        g_account_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && (attempts <= 5 || attempts % 12 == 0))
            log_message("accountInitialize rc=0x%x ready=%u", rc, g_account_ready ? 1 : 0);
    }

    if (!g_nifm_ready) {
        // 新版 libnx 里 socketInitialize() 不再顺带初始化 nifm 了(这点和老版本不一样),
        // 必须自己显式初始化,否则 bsd socket 会话建立没问题,但拿不到网络路由,
        // 表现就是每次请求都卡到 CURLOPT_TIMEOUT 才超时。
        // sysmodule 用 NifmServiceType_System,跟 bsd:s 保持同一个"系统级"身份。
        Result rc = nifmInitialize(NifmServiceType_System);
        g_nifm_ready = R_SUCCEEDED(rc);
        if (g_sd_ready && (attempts <= 5 || attempts % 12 == 0))
            log_message("nifmInitialize rc=0x%x ready=%u", rc, g_nifm_ready ? 1 : 0);
    }

    if (g_nifm_ready && !g_nifm_request_ready) {
        // nifmInitialize() 只是打开了一个能查询网络状态的会话,并不会替这个进程去
        // "申请"一条真正被路由的网络连接——应用启动时系统会自动替它做这件事,
        // sysmodule 没有这个自动待遇,必须自己创建并提交一个 NifmRequest,
        // 之后 bsd socket 的流量才会被正确路由。
        //
        // 注意: 这里只用非阻塞的 nifmRequestSubmit() + 轮询 nifmGetRequestState(),
        // 不用 nifmRequestSubmitAndWait()——后者内部会阻塞等待网络就绪,
        // 网络一直连不上时会卡住整个 sysmodule 主循环,导致限额监控/前台游戏检测
        // 全部停摆,甚至拖慢开机流程。轮询的方式即使永远等不到网络,
        // 也只是这一小段 if 每次直接跳过,不影响其它逻辑正常运行,只在日志里持续体现。
        if (!g_nifm_request_created) {
            Result rc = nifmCreateRequest(&g_nifm_request, false);
            g_nifm_request_created = R_SUCCEEDED(rc);
            if (g_sd_ready)
                log_message("nifmCreateRequest rc=0x%x created=%u", rc, g_nifm_request_created ? 1 : 0);
            if (g_nifm_request_created) {
                Result submit_rc = nifmRequestSubmit(&g_nifm_request);
                if (g_sd_ready)
                    log_message("nifmRequestSubmit rc=0x%x", submit_rc);
            }
        }
        if (g_nifm_request_created) {
            NifmRequestState state = NifmRequestState_Invalid;
            Result rc = nifmGetRequestState(&g_nifm_request, &state);
            if (R_SUCCEEDED(rc) && state == NifmRequestState_Available) {
                g_nifm_request_ready = true;
                log_message("nifm request available, network routing ready");
            } else if (g_sd_ready && (attempts <= 10 || attempts % 30 == 0)) {
                log_message("nifm request not ready yet: rc=0x%x state=%u", rc, (unsigned)state);
            }
        }
    }

    retry_network_clients();
}

static void schedule_next_heartbeat(u64 now, u64 *next_heartbeat, bool success)
{
    *next_heartbeat = now + (success ? g_config.heartbeat_interval_seconds : HEARTBEAT_RETRY_INTERVAL_SECONDS);
}

static void run_periodic_network_tasks(u64 now, u64 current_title_id, u64 *last_config_sync, u64 *last_auto_upload, u64 *next_heartbeat)
{
    if (now - *last_config_sync >= effective_config_sync_seconds()) {
        sync_limits_from_server();
        *last_config_sync = now;
    }

    if (*next_heartbeat == 0 || now >= *next_heartbeat) {
        bool heartbeat_ok = send_device_heartbeat(current_title_id);
        schedule_next_heartbeat(now, next_heartbeat, heartbeat_ok);
    }

    if (now - *last_auto_upload >= g_config.upload_interval_seconds) {
        log_message("auto upload due pending_entries=%u interval=%u",
                    (unsigned)g_upload_count, g_config.upload_interval_seconds);
        sync_limits_from_server();
        if (upload_pending_usage()) {
            bool heartbeat_ok = send_device_heartbeat(current_title_id);
            schedule_next_heartbeat(now, next_heartbeat, heartbeat_ok);
        }
        *last_auto_upload = now;
        *last_config_sync = now;
    }
}

int main(int argc, char* argv[])
{
    svcSleepThread(10000000000LL);
    retry_runtime_services();
    ensure_playlog_dir();
    // 注意: 日志清理【不放在开机路径】——boot2 阶段尽量少做事, 避免拖慢/干扰开机。
    // 历史文件改为第一次写日志时惰性清理(见 api_log_line)。
    load_guard_state();
    refresh_day_key();
    load_app_config();
    load_limits();
    load_state();
    load_upload_state();
    sync_limits_from_server();
    (void)effective_enforce_mode();   // §10: 启动时就把生效的执行方式写进日志
    log_message("PlayLogLimiter started reload=%u sync=%u upload=%u heartbeat=%u enforce_mode=%s",
                g_config.limit_reload_seconds,
                g_config.config_sync_seconds,
                g_config.upload_interval_seconds,
                g_config.heartbeat_interval_seconds,
                g_config.enforce_mode);

    u64 current_pid = 0;
    u64 current_title_id = 0;
    u64 last_tick_seconds = monotonic_seconds();
    u64 last_limit_reload = last_tick_seconds;
    u64 last_state_save = last_tick_seconds;
    u64 last_config_sync = last_tick_seconds;
    u64 last_auto_upload = last_tick_seconds;
    u64 next_heartbeat = 0;
    u64 next_focus_check = 0;

    while (true) {
        svcSleepThread((s64)POLL_INTERVAL_SECONDS * 1000000000LL);

        u64 now = monotonic_seconds();
        u32 elapsed = (now > last_tick_seconds) ? (u32)(now - last_tick_seconds) : POLL_INTERVAL_SECONDS;
        if (elapsed > 60)
            elapsed = POLL_INTERVAL_SECONDS;
        last_tick_seconds = now;

        retry_runtime_services();

        // 单调运行时长累加: 供日切守卫判断"日期前进"是否可信(item 14)。
        g_guard_runtime_seconds += elapsed;
        load_guard_state();

        refresh_day_key();

        if (now - last_limit_reload >= g_config.limit_reload_seconds) {
            load_app_config();
            load_limits();
            last_limit_reload = now;
        }

        u64 pid = 0;
        u64 title_id = 0;
        bool have_app = query_foreground_application(&pid, &title_id);

        // §9: 前台焦点快照。按 FOCUS_POLL_SECONDS 刷新(有 IPC 开销),
        // 前台进程/标题变化或没有前台进程时立即刷新。
        if (!have_app || pid != current_pid || title_id != current_title_id || now >= next_focus_check) {
            update_focus_status(have_app, pid, title_id);
            next_focus_check = now + FOCUS_POLL_SECONDS;
        }

        if (!have_app) {
            current_pid = 0;
            current_title_id = 0;
            close_enforce_session();
            run_periodic_network_tasks(now, current_title_id, &last_config_sync, &last_auto_upload, &next_heartbeat);
            continue;
        }

        // 当前账号判定(§5)。拿不到就退回设备级记账与设备级限额,
        // 不会比改动前更宽松,也不会崩溃。
        {
            const char *account_id = resolve_current_account();

            if (pid != current_pid || title_id != current_title_id) {
                current_pid = pid;
                current_title_id = title_id;
                add_upload_launch(account_id, title_id);
                save_upload_state();
                log_message("foreground title=%016lX pid=%016lX account=%s",
                            title_id, pid, account_id[0] ? account_id : "(device)");
                // §10/§11: 冻结"进入本局时已用秒数 + 当时的完整规则(日/按星期/周)",
                // 并在加载画面阶段做一次启动拦截。
                LimitRule entry_rule;
                memset(&entry_rule, 0, sizeof(entry_rule));
                bool entry_rule_known = find_effective_rule(account_id, title_id, &entry_rule);
                decide_enforce_session(account_id, title_id, entry_rule_known, &entry_rule);
                // §16: 账号合计限额同样在"进入本局"时冻结一次(block_launch 只认此时刻)。
                decide_account_total_session(account_id);
                // §16/§10: 记下本局进入时的账号, 供运行中"账号被切换"时重新判定。
                copy_fixed_string(g_session_account, sizeof(g_session_account),
                                  account_id ? account_id : "",
                                  account_id ? strlen(account_id) : 0);
                if (enforce_limit_at_session_entry(pid, account_id, title_id,
                                                   entry_rule_known, &entry_rule)) {
                    current_pid = 0;
                    current_title_id = 0;
                }
                run_periodic_network_tasks(now, current_title_id, &last_config_sync, &last_auto_upload, &next_heartbeat);
                continue;
            }

            if (now - last_state_save >= g_config.limit_reload_seconds) {
                g_guard_dirty = true;
                save_state();
                save_week_state();
                save_guard_state();
                save_upload_state();
                last_state_save = now;
            }

            {
                LimitRule rule;
                memset(&rule, 0, sizeof(rule));
                // §9: 只有 focus 明确判定为 background 时才暂停记账与限额执行。
                // unknown / suspect / foreground 一律维持旧行为。
                bool focus_paused = (g_focus.state == FocusState_Background);

                // §16/§10: 运行中"当前账号"变了(主机切换用户/账号判定从 xb 校正到 joey)。
                // 视为新的一局: 用新账号重新冻结 §11/§16 判定; 如果新账号进入时就已
                // 超限/被封禁(例如周一到周五 0 分钟), block_launch 也必须立即拦截,
                // 否则会拿旧账号的"未超限"判定把整局放行。kill 模式仍由下面每 tick 的
                // 判定兜底, 这里只是把"启动拦截"的时机修正到账号变化的那一刻。
                if (!focus_paused &&
                    strcmp(account_id ? account_id : "", g_session_account) != 0) {
                    log_message("mid-session account change %s -> %s title=%016lX; re-evaluating limits",
                                g_session_account[0] ? g_session_account : "(device)",
                                (account_id && account_id[0]) ? account_id : "(device)",
                                title_id);
                    copy_fixed_string(g_session_account, sizeof(g_session_account),
                                      account_id ? account_id : "",
                                      account_id ? strlen(account_id) : 0);
                    LimitRule changed_rule;
                    memset(&changed_rule, 0, sizeof(changed_rule));
                    bool changed_rule_known = find_effective_rule(account_id, title_id, &changed_rule);
                    decide_enforce_session(account_id, title_id, changed_rule_known, &changed_rule);
                    decide_account_total_session(account_id);
                    if (enforce_limit_at_session_entry(pid, account_id, title_id,
                                                       changed_rule_known, &changed_rule)) {
                        current_pid = 0;
                        current_title_id = 0;
                        run_periodic_network_tasks(now, current_title_id, &last_config_sync,
                                                   &last_auto_upload, &next_heartbeat);
                        continue;
                    }
                }

                bool rule_known = find_effective_rule(account_id, title_id, &rule);

                // §14: 日账本与上报一律按**真实经过时长**累加, 与是否配置了限额无关。
                // 修复前"没有限额条目"的分支从不累加 usage->used_seconds, 且账本到顶后
                // 停止上报, 导致 (1) 游戏中途新增限额时账本仍从 0 起算 -> block_launch
                // 在下一局放行整局; (2) 面板"今日已玩"到顶后冻结, 与服务端累加分叉。
                // 记账项拿不到(表满)时不上报也不累加日账本, 但周账本/上报各有独立
                // 账本, 仍按真实时长累加, 绝不静默漏记(item 17)。
                UsageEntry *usage = NULL;
                if (!focus_paused) {
                    usage = get_usage_entry(account_id, title_id);
                    if (usage) {
                        usage->used_seconds += elapsed;   // 真实时长, 不再在限额处截断
                        g_state_dirty = true;
                    }
                    add_upload_seconds(account_id, title_id, elapsed);
                    // §11: 未截断真实用量必须按完整 elapsed 累加 —— 周上限只认它,
                    // 而且无论日限额是否已到都要累加(REFACTOR_SPEC §11 警告)。
                    add_uncapped_seconds(account_id, title_id, elapsed);
                }

                // §16: 账号级"全部游戏合计"限额与单游戏限额是 OR —— 即使该游戏没有
                // 任何单游戏限额条目(rule_known=false), 账号合计上限仍然生效。
                // 与 §10 一致: kill 到点即杀; block_launch 只认进入本局时的冻结判定。
                // 任何不确定(账号未知 / 无合计规则 / 星期算不出 / 账本拿不到)一律不终止。
                if (!focus_paused && usage) {
                    const char *total_source = LIMIT_SOURCE_NONE;
                    if (account_total_should_terminate(account_id, &total_source)) {
                        LimitRule total_rule;
                        memset(&total_rule, 0, sizeof(total_rule));
                        save_state();
                        save_week_state();
                        log_message("§16 runtime terminate title=%016lX account=%s source=%s used=%u",
                                    title_id, (account_id && account_id[0]) ? account_id : "(device)",
                                    total_source, usage ? usage->used_seconds : 0);
                        enforce_limit(pid, account_id, title_id, usage,
                                      rule_known ? &rule : &total_rule, total_source);
                        current_pid = 0;
                        current_title_id = 0;
                        run_periodic_network_tasks(now, current_title_id, &last_config_sync,
                                                   &last_auto_upload, &next_heartbeat);
                        continue;
                    }
                }

                if (!rule_known) {
                    // 没有任何限额条目 -> 该游戏不限制(执行行为与改造前一致)。
                    // §14: 记账(日账本/上报/周账本)已在上面完成, 这里不需要再做什么。
                    run_periodic_network_tasks(now, current_title_id, &last_config_sync, &last_auto_upload, &next_heartbeat);
                    continue;
                }

                if (!usage) {
                    // focus=background 或记账项拿不到(表满) -> 本 tick 无法判定限额,
                    // 按 fail-safe 不终止, 也不动账本, 等下一个 tick 再说。
                    run_periodic_network_tasks(now, current_title_id, &last_config_sync, &last_auto_upload, &next_heartbeat);
                    continue;
                }

                // 后台暂停期间既不计时也不杀进程: 等它回到前台再按剩余额度处理。
                // §10/§11: 是否终止交给 should_terminate_now() —— kill 超限即杀;
                // block_launch 只有"进入本局时就已超限"才杀(启动拦截, 通常已在
                // enforce_limit_at_session_entry 里处理), 中途超限只记日志、不打断。
                // 注意: 这里每个 tick 都要判定, 因为周上限可能在"当日还没到日限额"
                // 的情况下先到(封顶语义)。任何不确定一律不终止。
                if (!focus_paused && should_terminate_now(title_id, account_id, usage, &rule)) {
                    const char *reason = "";
                    const char *source = evaluate_limit_rule(&rule, account_id, title_id, usage, &reason);
                    save_state();
                    save_week_state();
                    log_message("§11 runtime terminate title=%016lX account=%s source=%s used=%u reason=%s",
                                title_id, (account_id && account_id[0]) ? account_id : "(device)",
                                source, usage ? usage->used_seconds : 0, reason ? reason : "");
                    enforce_limit(pid, account_id, title_id, usage, &rule, source);
                    current_pid = 0;
                    current_title_id = 0;
                }
            }

            run_periodic_network_tasks(now, current_title_id, &last_config_sync, &last_auto_upload, &next_heartbeat);
        }
    }

    return 0;
}
