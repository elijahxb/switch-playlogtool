#include <switch.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdarg.h>

#include "../limits_common.h"
#include "i18n.h"
#include "ui.h"

u32 __nx_applet_type = AppletType_Application;

#undef printf
#define printf uiPrintf

static void boot_log(const char *msg)
{
    FILE *f = fopen("sdmc:/PlayLog/boot.log", "a");
    if (!f)
        return;
    fputs(msg, f);
    fputc('\n', f);
    fclose(f);
}

__attribute__((constructor(101))) static void boot_ctor_early(void) { boot_log("ctor101"); }
__attribute__((constructor(200))) static void boot_ctor_late(void) { boot_log("ctor200"); }

extern void __libnx_init_time(void);
extern void __nx_win_init(void) __attribute__((weak));
extern void userAppInit(void) __attribute__((weak));

static Result g_appinit_mount_rc = (Result)-1;

void __appInit(void)
{
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    if (hosversionGet() == 0) {
        rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            rc = setsysGetFirmwareVersion(&fw);
            if (R_SUCCEEDED(rc))
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            setsysExit();
        }
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    g_appinit_mount_rc = fsdevMountSdmc();

    rc = appletInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_AM));

    if (__nx_applet_type != AppletType_None) {
        rc = hidInitialize();
        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_HID));
    }

    rc = timeInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_Time));
    __libnx_init_time();

    /* 不调用 __libnx_init_cwd(): libnx 4.12 在固件 20.5.0 上会在 fsdev_chdir 崩溃。 */

    if (&__nx_win_init)
        __nx_win_init();
    if (&userAppInit)
        userAppInit();

    {
        char b[64];
        snprintf(b, sizeof(b), "appInit done mount=0x%x", g_appinit_mount_rc);
        boot_log(b);
    }
}

static void parseAccountsFromLimits(const char *body);
void copyBoundedString(char *out, size_t outSize, const char *in, size_t inSize);

#define MAX_GAMES 512
#define MAX_ACCOUNT_GAMES (MAX_GAMES * MAX_ACCOUNTS)
#define DEFAULT_SERVER_BASE_URL "https://your-server.example.com"
#define PUSH_PATH "/switch/statistics/daily"
#define CONFIG_FETCH_PATH "/switch/devices/config"
#define PLAYLOG_DIR "sdmc:/PlayLog"
#define LIMITS_PATH PLAYLOG_DIR "/limits.json"
#define CONFIG_PATH PLAYLOG_DIR "/config.json"
#define SNAPSHOT_PATH PLAYLOG_DIR "/snapshot.csv"
#define ACCOUNT_SNAPSHOT_PATH PLAYLOG_DIR "/account_snapshot.csv"
// 与 sysmodule 使用同一个 CA 文件;缺失时明文/不校验证书会静默降级,
// 因此这里改为明确报错(REFACTOR_SPEC §6 item 20)。
#define CAINFO_PATH PLAYLOG_DIR "/cacert.pem"

// 本地留档 CSV 的表头。它们是**写进文件**的中文(UTF-8), 不经过控制台;
// 单独抽成常量是为了让所有 printf 调用行都不含非 ASCII 字节(§13 静态检查)。
static const char CSV_DAILY_HEADER[] = "游戏名,TitleID,增量(秒),增量(分钟)";
static const char CSV_REPORT_HEADER[] = "TitleID,游戏名,累计时长(小时),启动次数";

typedef struct {
    u64 titleId;
    char name[0x201];
    char displayName[0x201];
    s64 totalPlaytimeSeconds;
    s32 launchCount;
} GameRecord;

typedef struct {
    u64 titleId;
    s64 snapshotPlaytimeSeconds;
} SnapshotRecord;

// 按账号的快照: (account_id, title_id) -> 上次已上报的累计秒数。
// 与设备级 SnapshotRecord 分开存放,因为 PDM 的按账号累计值口径不同。
typedef struct {
    char accountId[33];
    char accountName[0x21];
    u64 titleId;
    s64 snapshotPlaytimeSeconds;
} AccountSnapshotRecord;

typedef struct {
    char accountId[33];
    u64 titleId;
    s64 totalPlaytimeSeconds;
} AccountGameRecord;

typedef struct {
    char accountId[33];
    char accountName[0x21];
    bool isActive;
} SwitchAccountInfo;

SwitchAccountInfo g_accounts[MAX_ACCOUNTS];
int g_accountCount = 0;

typedef struct {
    u64 titleId;
    u32 limitSeconds;
    // §11 周总时长 + 按星期限额(秒)。weekdaySeconds 下标 0 = 周一 … 6 = 周日。
    // §15: hasWeekday = 按星期**已配置**(服务端下发的数组恰好 7 个值)。已配置时
    // 当日生效值 = weekdaySeconds[今天], 0 也生效(= 当天封禁); 未配置([] / 缺失 /
    // 长度不对)才回退 limitSeconds。weeklySeconds 0 = 不限制。
    // hasRule = 该条目是否来自服务端/本地的 limit_rules(有规则)而不是老的每日值。
    u32 weeklySeconds;
    u32 weekdaySeconds[WEEKDAY_COUNT];
    bool hasWeekday;
    bool hasRule;
} LimitRecord;

// §16 账号级"全部游戏合计"限额(前台 NRO 只读展示; 真正的执行在 sysmodule)。
// 语义与 LimitRecord 的 §15 判定完全一致:
//   weekdaySeconds 恰好 7 个值 = 按星期已配置, 当日值 = 该天数值(0 = 当天封禁);
//   `[]` / 缺失 / 长度不对 = 未配置, 回退 dailySeconds。
// 服务端只下发 enabled=true 的行, 所以"存在记录"即"该账号已启用合计限额"。
typedef struct {
    char accountId[33];
    u32 dailySeconds;
    u32 weekdaySeconds[WEEKDAY_COUNT];
    bool hasWeekday;
} AccountTotalRecord;

// 每账号至多一条合计限额(系统最多 8 个账号)。
AccountTotalRecord g_accountTotals[MAX_ACCOUNTS];
int g_accountTotalCount = 0;

// 账号 + 游戏 限额(account_limit_rules)。
typedef struct {
    char accountId[33];
    u64 titleId;
    u32 dailySeconds;
    u32 weeklySeconds;
    u32 weekdaySeconds[WEEKDAY_COUNT];
    bool hasWeekday;
} AccountGameLimitRecord;

#define MAX_ACCOUNT_GAME_LIMITS (MAX_ACCOUNTS * 128)
AccountGameLimitRecord g_accountGameLimits[MAX_ACCOUNT_GAME_LIMITS];
int g_accountGameLimitCount = 0;

int g_currentAccountIndex = 0;
bool g_online = false;
bool g_allow_offline_edit = false;
char g_server_time_raw[32] = "";
long long g_server_epoch = 0;
unsigned long long g_server_sync_mono = 0;
bool g_server_time_valid = false;

typedef struct {
    char serverBaseUrl[256];
    char deviceId[65];
    char deviceIdType[20];
    char uploadToken[129];
    u32 limitReloadSeconds;
    u32 configSyncSeconds;
    u32 uploadIntervalSeconds;
    u32 heartbeatIntervalSeconds;
    // §10 限额执行方式(block_launch | kill)。NRO 自己不执行限额,
    // 但和 sysmodule 共用 config.json: 必须原样读进来再写回去,
    // 否则一次"保存配置"就会把 sysmodule 依赖的兜底值擦掉。
    char enforceMode[ENFORCE_MODE_MAX];
} AppConfig;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} MemoryBuffer;

// 服务端响应签名头 (REFACTOR_SPEC §6 item 20)。
typedef struct {
    char hex[129];
    bool present;
} ResponseSignature;

// 下载配置/上传时的响应体上限,避免服务端返回超大 body 把内存吃光。
#define MAX_RESPONSE_SIZE 16384

AppConfig g_config = {
    .serverBaseUrl = DEFAULT_SERVER_BASE_URL,
    .limitReloadSeconds = 30,
    .configSyncSeconds = 30,
    .uploadIntervalSeconds = 900,
    .heartbeatIntervalSeconds = 300,
    // §10 默认值: 配额用尽不打断当前这局, 下次启动再拦截(不丢进度)。
    .enforceMode = ENFORCE_MODE_DEFAULT,
};

// config.json 是否存在且读取成功。只有为 true 时才允许回写,
// 避免一次读失败(或文件被截断)就把 upload_token/device_id 重置成默认值写回 SD 卡
// (REFACTOR_SPEC §6 item 15)。
static bool g_configWritable = false;

// limits.json 读取失败时为 true。此时禁止回写,避免用空表覆盖真实限额
// (REFACTOR_SPEC §6 item 15)。
static bool g_limitsUnknown = false;

//---------------------------------------------------------------------------------
// 文件读写工具: 全部走 temp + rename,避免掉电/并发写产生半截文件。
// 读文件时区分"文件不存在"(首次运行,可用默认值)与"文件存在但读失败"
// (必须保留内存中的旧值,不得重置)。
//---------------------------------------------------------------------------------
bool readWholeFile(const char *path, char *out, size_t outSize, bool *exists, size_t *outLen) {
    if (exists) *exists = false;
    if (outLen) *outLen = 0;
    if (outSize == 0) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (exists) *exists = true;

    // 用 fseek/ftell 先拿到真实长度: 只要文件比缓冲区大就直接判定读取失败,
    // 而不是把超出部分悄悄丢掉当成"内容就是这样"。
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long fileSize = ftell(f);
    if (fileSize < 0 || (size_t)fileSize >= outSize) {
        fclose(f);
        return false;
    }
    rewind(f);

    size_t read = fread(out, 1, (size_t)fileSize, f);
    bool failed = ferror(f) || read != (size_t)fileSize;
    fclose(f);
    if (failed) return false;

    out[read] = '\0';
    if (outLen) *outLen = read;
    return true;
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
// 前台上报与后台 sysmodule 追加写**同一个**当天文件(短行, 交错时最多丢一行,
// 不会写坏别的行)。这两个二进制各自 include 同一份 limits_common.h,
// 但实现是重复的(两个二进制互相独立, 不共享 .c)。
//
// 失败策略: 写日志失败绝不影响主流程, 所以所有函数都不返回错误、调用方
// 也不需要检查。保留策略按**文件名**计算(不列目录):
//   - 每次写入时 remove(今天-8);
//   - 启动时再扫一遍 今天-8 .. 今天-40, 清掉历史遗留。
//---------------------------------------------------------------------------------

/// HTTP 状态码的英文文案。服务端没给 detail 时用它兜底(§12.2)。
const char *httpStatusText(long code) {
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
/// 注意: >=0x80 的字节**原样保留**, 所以日志文件始终是完整 UTF-8。
void apiLogSanitize(const char *in, char *out, size_t outSize) {
    if (outSize == 0) return;
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < outSize; i++) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
    }
    out[o] = '\0';
}

//---------------------------------------------------------------------------------
// §13 屏幕只允许 ASCII
//
// libnx 的 console 是点阵字体(ConsoleFont{asciiOffset,numChars,...}), 完全
// 没有 UTF-8 处理: 一个汉字 3 字节会被当成 3 个无效字模画成乱码。所以:
//   - 屏幕上的字符串一律先过 asciiSanitize(>=0x80 -> '?');
//   - 服务端返回的中文 detail 只写日志文件(UTF-8), 绝不 printf 到屏幕;
//   - 屏幕上的失败原因用 ASCII 的 `code` + `hint`。
// 日志文件的内容(以及 report.csv / run_*.csv)不经过这里, 保持原始字节。
//---------------------------------------------------------------------------------

/// 把任意字节串净化成纯 ASCII(>=0x80 -> '?', 控制字符 -> 空格)。
void asciiSanitize(const char *in, char *out, size_t outSize) {
    if (outSize == 0) return;
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < outSize; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == 0x7F) out[o++] = ' ';
        else if (c >= 0x80) out[o++] = '?';
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

/// 把 ASCII 文本折行打印到控制台(最多 maxLines 行, 只在空格处断, 不切单词;
/// 放不下时最后一行用 `...` 收尾)。文本必须已经是纯 ASCII。
/// 取"今天 - dayOffset 天"的本地日期。时钟不可用/明显不合理时返回 false
/// (这时宁可不写日志, 也不要生成一个非法日期命名的文件)。
bool apiLogDateOffset(int dayOffset, char *compact, size_t compactSize, char *dashed, size_t dashedSize) {
    time_t now;
    time(&now);
    if (now <= 0) return false;

    time_t target = now - (time_t)dayOffset * 86400;
    struct tm *t = localtime(&target);
    if (!t) return false;

    int year = t->tm_year + 1900;
    if (year < 2020 || year > 2099) return false;

    if (compact) snprintf(compact, compactSize, "%04d%02d%02d", year, t->tm_mon + 1, t->tm_mday);
    if (dashed) snprintf(dashed, dashedSize, "%04d-%02d-%02d", year, t->tm_mon + 1, t->tm_mday);
    return true;
}

bool apiLogPathOffset(int dayOffset, char *out, size_t outSize) {
    char compact[16];
    if (!apiLogDateOffset(dayOffset, compact, sizeof(compact), NULL, 0)) return false;
    snprintf(out, outSize, API_LOG_PATH_FMT, compact);
    return true;
}

/// 启动时清一次 今天-8 .. 今天-40 的历史日志。
void apiLogSweepOldFiles(void) {
    for (int offset = API_LOG_SWEEP_FIRST_DAY; offset <= API_LOG_SWEEP_LAST_DAY; offset++) {
        char path[96];
        if (apiLogPathOffset(offset, path, sizeof(path)))
            remove(path);
    }
}

/// 追加一行接口日志。任何失败都静默跳过(不影响主流程)。
/// `code` 是服务端返回的 ASCII 机器码(可 NULL/空, 记作 `-`), `detail` 可为中文;
/// 两者一起写进 UTF-8 文件(§13: 屏幕查看器会自行把非 ASCII 净化为 '?')。
void apiLogLine(const char *tag, const char *method, const char *apiPath, long httpCode,
                const char *code, const char *detail) {
    // 时钟不可用 => 无法按"每天一份"命名, 直接跳过。
    char compact[16];
    if (!apiLogDateOffset(0, compact, sizeof(compact), NULL, 0)) return;

    // 每次写入顺手删掉今天-8 的文件(最多保留 7 天)。
    {
        char stale[96];
        if (apiLogPathOffset(API_LOG_SWEEP_FIRST_DAY, stale, sizeof(stale)))
            remove(stale);
    }

    char filePath[96];
    snprintf(filePath, sizeof(filePath), API_LOG_PATH_FMT, compact);

    mkdir(PLAYLOG_DIR, 0777);
    FILE *f = fopen(filePath, "a");
    if (!f) return;

    char stamp[32];
    {
        time_t now;
        time(&now);
        struct tm *t = localtime(&now);
        // strftime 的格式串长度固定(19 字节), 不会触发 -Wformat-truncation。
        if (!t || strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", t) == 0)
            snprintf(stamp, sizeof(stamp), "%s --:--:--", compact);
    }

    // detail 为空时: 2xx 记 OK, 其它按 HTTP 状态文案兜底。
    char reason[192];
    if (detail && detail[0]) {
        apiLogSanitize(detail, reason, sizeof(reason));
    } else if (httpCode >= 200 && httpCode < 300) {
        snprintf(reason, sizeof(reason), "OK");
    } else {
        snprintf(reason, sizeof(reason), "%s", httpStatusText(httpCode));
    }

    // §13: 机器码保证是 ASCII(缺失时 '-' 占位), 这样日志查看器净化后仍能一眼看出失败原因。
    const char *logCode = (code && code[0]) ? code : "-";

    char line[API_LOG_LINE_MAX + 4];
    int n = snprintf(line, sizeof(line), "%s [%s] %s %s -> HTTP %ld %s %s",
                     stamp, tag ? tag : "-", method ? method : "-",
                     apiPath ? apiPath : "-", httpCode, logCode, reason);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len > API_LOG_LINE_MAX) len = API_LOG_LINE_MAX;          // 单行 240 字节上限
        if (len > sizeof(line) - 1) len = sizeof(line) - 1;
        // 截断时不要把 UTF-8 字符切成半个(否则日志查看器会显示乱码)。
        while (len > 0 && ((unsigned char)line[len] & 0xC0) == 0x80) len--;
        fwrite(line, 1, len, f);
        fputc('\n', f);
    }
    fclose(f);
}

//---------------------------------------------------------------------------------
// 文件读写工具: 全部走 temp + rename,避免掉电/并发写产生半截文件。
// 读文件时区分"文件不存在"(首次运行,可用默认值)与"文件存在但读失败"
// (必须保留内存中的旧值,不得重置)。
//---------------------------------------------------------------------------------
/// 真正干活的写盘函数; 失败由下面的 writeFileAtomic 统一记一条 write 日志。
static bool writeFileAtomicRaw(const char *path, const char *data, size_t len);

bool writeFileAtomic(const char *path, const char *data, size_t len) {
    if (writeFileAtomicRaw(path, data, len))
        return true;

    // §12.3: 写盘失败也要进接口日志, 并带上目标路径(write tag)。
    apiLogLine(API_LOG_TAG_WRITE, "WRITE", path, 0, NULL, "写盘失败");
    return false;
}

static bool writeFileAtomicRaw(const char *path, const char *data, size_t len) {
    mkdir(PLAYLOG_DIR, 0777);

    char tempPath[320];
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);

    FILE *f = fopen(tempPath, "wb");
    if (!f) return false;

    size_t written = len ? fwrite(data, 1, len, f) : 0;
    if (len && written != len) {
        fclose(f);
        remove(tempPath);
        return false;
    }
    if (fclose(f) != 0) {
        remove(tempPath);
        return false;
    }

    if (rename(tempPath, path) != 0) {
        // Switch 的 SD 卡是 FatFs, 它的 f_rename 在"目标已存在"时返回 FR_EXIST(不支持覆盖),
        // 于是 temp+rename 在文件已存在时必然失败。这里删掉旧文件后重试;
        // 其它文件系统(支持覆盖)仍走上面的原子路径。
        remove(path);
        if (rename(tempPath, path) != 0) {
            // 仍失败则退化为直接覆盖写: 牺牲原子性, 但保证能写进去。
            remove(tempPath);
            FILE *direct = fopen(path, "wb");
            if (!direct)
                return false;
            size_t directWritten = len ? fwrite(data, 1, len, direct) : 0;
            if ((len && directWritten != len) || fclose(direct) != 0)
                return false;
            return true;
        }
    }
    return true;
}

int loadLimits(LimitRecord *out, int maxCount);
void saveLimits(LimitRecord *limits, int count);
void logPushError(const char *msg);

//---------------------------------------------------------------------------------
// 生成 UUID 格式的唯一ID(用 Switch 系统随机数服务)
//---------------------------------------------------------------------------------
void generateUuidString(char *out, size_t outSize) {
    u8 buf[16];
    randomGet(buf, sizeof(buf));

    buf[6] = (buf[6] & 0x0F) | 0x40; // version 4
    buf[8] = (buf[8] & 0x3F) | 0x80; // variant

    snprintf(out, outSize,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
        buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
}

void bytesToHex(const u8 *in, size_t inSize, char *out, size_t outSize) {
    static const char hex[] = "0123456789abcdef";
    size_t needed = inSize * 2 + 1;
    if (outSize < needed) {
        if (outSize > 0) out[0] = '\0';
        return;
    }

    for (size_t i = 0; i < inSize; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[inSize * 2] = '\0';
}

void sha256Hex(const char *input, char *out, size_t outSize) {
    u8 hash[SHA256_HASH_SIZE];
    sha256CalculateHash(hash, input, strlen(input));
    bytesToHex(hash, sizeof(hash), out, outSize);
}

void hmacSha256Hex(const char *key, const char *message, char *out, size_t outSize) {
    u8 mac[SHA256_HASH_SIZE];
    hmacSha256CalculateMac(mac, key, strlen(key), message, strlen(message));
    bytesToHex(mac, sizeof(mac), out, outSize);
}

//---------------------------------------------------------------------------------
Result queryPlayStatistics(u64 titleId, s64 *playtimeSec, s32 *launchCount) {
    PdmPlayStatistics stats;
    Result rc = pdmqryQueryPlayStatisticsByApplicationId(titleId, false, &stats);
    if (R_SUCCEEDED(rc)) {
        *playtimeSec = stats.playtime / 1000000000;
        *launchCount = stats.total_launches;
    }
    return rc;
}

Result getGameName(u64 titleId, char *outName, size_t outSize,
                   char *outDisplay, size_t outDisplaySize) {
    Result rc;
    NsApplicationControlData *controlData = malloc(sizeof(NsApplicationControlData));
    if (!controlData) return -1;

    size_t actualSize = 0;
    rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, titleId,
                                      controlData, sizeof(NsApplicationControlData), &actualSize);
    if (R_SUCCEEDED(rc)) {
        NacpLanguageEntry *langEntry = NULL;
        rc = nacpGetLanguageEntry(&controlData->nacp, &langEntry);
        if (R_SUCCEEDED(rc) && langEntry) {
            strncpy(outName, langEntry->name, outSize - 1);
            outName[outSize - 1] = '\0';
        } else {
            snprintf(outName, outSize, "Unknown_%016lX", titleId);
        }
        if (outDisplay && outDisplaySize) {
            // NRO 控制台只有 ASCII 字模: 屏幕优先显示 NACP 英文名(1=美式英语, 12=英式英语),
            // 没有英文条目时才回退到净化后的系统语言名。
            const char *en = controlData->nacp.lang[1].name[0]
                                 ? controlData->nacp.lang[1].name
                                 : (controlData->nacp.lang[12].name[0]
                                        ? controlData->nacp.lang[12].name
                                        : NULL);
            asciiSanitize(en ? en : outName, outDisplay, outDisplaySize);
        }
    } else {
        snprintf(outName, outSize, "Unknown_%016lX", titleId);
        if (outDisplay && outDisplaySize)
            asciiSanitize(outName, outDisplay, outDisplaySize);
    }

    free(controlData);
    return rc;
}

//---------------------------------------------------------------------------------
// JSON 字符串转义(防止游戏名里有引号/反斜杠导致JSON格式错误)
//---------------------------------------------------------------------------------
void escapeJsonString(const char *in, char *out, size_t outSize) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o < outSize - 2; i++) {
        unsigned char c = in[i];
        if (c == '"' || c == '\\') {
            if (o >= outSize - 3) break;
            out[o++] = '\\';
            out[o++] = c;
        } else if (c >= 0x20) {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

bool parseTitleIdString(const char *text, size_t len, u64 *out) {
    if (len == 18 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
        len -= 2;
    }
    if (len != 16) return false;

    u64 value = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        u8 n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else return false;
        value = (value << 4) | n;
    }

    *out = value;
    return true;
}

bool readJsonStringField(const char *body, const char *field, char *out, size_t outSize) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);

    char *key = strstr(body, pattern);
    if (!key) return false;
    char *colon = strchr(key, ':');
    if (!colon) return false;
    char *quote = strchr(colon, '"');
    if (!quote) return false;
    char *start = quote + 1;

    size_t len = 0;
    bool escape = false;
    for (char *p = start; *p && len < outSize - 1; p++) {
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

//---------------------------------------------------------------------------------
// §12.2 / §13 服务端失败原因
//
// 面向设备的接口出错时统一返回 {"detail": "<中文原因>", "code": "<机器码>",
// "hint": "<纯 ASCII 的一句话>"}。分工(§13):
//   - `detail`: 中文, **只写日志文件**(UTF-8, 拷到电脑上可读);
//   - `code`  : ASCII 机器码, 进屏幕与日志;
//   - `hint`  : 服务端给的纯 ASCII 解释, 进屏幕(无 hint 时退回 HTTP 状态文案)。
// libnx console 没有 UTF-8 字模, 所以 detail 绝不能 printf 到屏幕。
//---------------------------------------------------------------------------------
void extractServerFailureDetail(const char *body, long httpCode, char *out, size_t outSize) {
    if (outSize == 0) return;
    out[0] = '\0';

    char raw[512];
    if (body && readJsonStringField(body, "detail", raw, sizeof(raw)) && raw[0]) {
        char clean[512];
        apiLogSanitize(raw, clean, sizeof(clean));
        snprintf(out, outSize, "%s", clean);
        return;
    }

    snprintf(out, outSize, "%s", httpStatusText(httpCode));
}

/// §13: 取出响应体里的 ASCII 机器码 `code`。只保留可见 ASCII(0x21..0x7E),
/// 缺失/非法时输出空串(调用方打印 `-`)。
void extractServerFailureCode(const char *body, char *out, size_t outSize) {
    if (outSize == 0) return;
    out[0] = '\0';

    char raw[API_LOG_CODE_MAX];
    if (!body || !readJsonStringField(body, "code", raw, sizeof(raw))) return;

    size_t o = 0;
    for (size_t i = 0; raw[i] && o + 1 < outSize; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c >= 0x21 && c <= 0x7E) out[o++] = (char)c;
    }
    out[o] = '\0';
}

/// §13: 取出响应体里的纯 ASCII `hint`; 没有/为空时退回 HTTP 状态文案。
/// 输出保证纯 ASCII(>=0x80 会先被净化, 防止服务端漏放中文把屏幕搞乱)。
/// §13: 非 2xx 时在屏幕上打印的失败信息。**只输出 ASCII**:
///   <prefix> HTTP <status> <code>:
///     <ASCII hint, 按控制台宽度折行, 最多 CONSOLE_HINT_MAX_LINES 行>
/// 中文 detail 不在这里出现, 它只由调用方写进接口日志。
void printServerFailure(const char *prefix, long httpCode, const char *body) {
    char code[API_LOG_CODE_MAX];
    extractServerFailureCode(body, code, sizeof(code));
    printf("%s HTTP %ld %s:\n", prefix ? prefix : "-", httpCode, code[0] ? code : "-");

    char detail[512];
    bool detail_is_string = body && (strstr(body, "\"detail\":\"") || strstr(body, "\"detail\": \""));
    if (detail_is_string && readJsonStringField(body, "detail", detail, sizeof(detail)) && detail[0]) {
        printf("  %s\n", detail);
        return;
    }
    printf("  请求失败（HTTP %ld，详见接口日志）\n", httpCode);
}

bool readJsonU32FieldMax(const char *body, const char *field, unsigned long maxValue, u32 *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);

    char *key = strstr(body, pattern);
    if (!key) return false;
    char *colon = strchr(key, ':');
    if (!colon) return false;

    char *num = colon + 1;
    while (*num == ' ' || *num == '\t' || *num == '\r' || *num == '\n') num++;

    char *end = NULL;
    unsigned long value = strtoul(num, &end, 10);
    if (end == num || value > maxValue) return false;

    *out = (u32)value;
    return true;
}

bool readJsonU32Field(const char *body, const char *field, u32 *out) {
    return readJsonU32FieldMax(body, field, 86400UL, out);
}

//---------------------------------------------------------------------------------
// §11 limit_rules 解析(只读, 供前台显示与持久化)
//
// 形状(单位秒, 与后端 GET /switch/devices/config 一致):
//   "limit_rules": { "<title_id>": { "daily_seconds": 900,
//                                    "weekly_seconds": 28800,
//                                    "weekday_seconds": [0,0,0,0,0,900,900] } }
//
// §15 按星期语义(唯一真源):
//   - "weekday_seconds": [] / 字段缺失 / 长度 != 7 -> 按星期**未配置**,
//     当日显示值回退 daily_seconds;
//   - "weekday_seconds": [恰好 7 个值] -> 按星期**已配置**, 当日显示值 =
//     该天数值(含 0; 0 = 当天封禁)。
//
// 缺失 limit_rules(老服务端)时返回 false, 调用方保持"无规则"状态, 行为与改造前一致。
//---------------------------------------------------------------------------------

/// 把 value 指向的 JSON 对象(含花括号)整段拷进 out, 返回该对象之后的位置。
const char *copyJsonObjectAt(const char *value, char *out, size_t outSize) {
    if (!value || *value != '{') return NULL;

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t written = 0;

    for (const char *p = value; *p; p++) {
        char c = *p;
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
        } else if (c == '"') {
            inString = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
        }

        if (written + 1 < outSize) out[written++] = c;

        if (depth == 0 && !inString) {
            out[written < outSize ? written : outSize - 1] = '\0';
            return p + 1;
        }
    }
    return NULL;
}

/// 取出 field 对应的对象子树(含花括号)。找不到/不是对象返回 false。
bool extractJsonObjectField(const char *body, const char *field, char *out, size_t outSize) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);

    const char *key = strstr(body, pattern);
    if (!key) return false;
    const char *colon = strchr(key, ':');
    if (!colon) return false;

    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
    return copyJsonObjectAt(value, out, outSize) != NULL;
}

/// 解析 7 个按星期秒值(下标 0 = 周一)。
/// 返回 true **当且仅当** body 里的 "weekday_seconds" 是恰好 7 个数值的数组(§15 已配置)。
/// 字段缺失 / 不是数组 / 元素个数 != 7 / 含非数值 / 未闭合 -> 返回 false 且 out 全 0,
/// 调用方必须回退每日值(绝不臆造限额)。
bool parseWeekdaySeconds(const char *body, u32 out[WEEKDAY_COUNT]) {
    memset(out, 0, sizeof(u32) * WEEKDAY_COUNT);

    const char *p = strstr(body, "\"weekday_seconds\"");
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '[') return false;   // 非数组 = 未配置
    p++;

    int count = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p == ']') { p++; break; }
        if (*p == '\0') return false;           // 数组未闭合
        if (count >= WEEKDAY_COUNT) return false; // 多于 7 个 = 长度不对

        char *end = NULL;
        unsigned long seconds = strtoul(p, &end, 10);
        if (end == p) return false;             // 非数值
        if (seconds > WEEKDAY_SECONDS_MAX) seconds = WEEKDAY_SECONDS_MAX;
        out[count++] = (u32)seconds;
        p = end;
    }

    if (count != WEEKDAY_COUNT) {
        memset(out, 0, sizeof(u32) * WEEKDAY_COUNT);
        return false;                           // 少于 7 个 = 未配置
    }
    return true;
}

LimitRecord *findLimitRecord(LimitRecord *limits, int count, u64 titleId) {
    for (int i = 0; i < count; i++) {
        if (limits[i].titleId == titleId) return &limits[i];
    }
    return NULL;
}

bool limitRecordHasRule(const LimitRecord *record) {
    return record->hasRule;
}

/// 把 limit_rules 子树合并进 limits 表(已有的条目补齐字段, 没有的补建)。
void parseLimitRulesIntoRecords(const char *body, LimitRecord *records, int maxCount, int *count) {
    static char rules[8192];
    if (!extractJsonObjectField(body, "limit_rules", rules, sizeof(rules))) return;

    const char *p = rules;
    while (*p) {
        const char *keyStart = strchr(p, '"');
        if (!keyStart) break;
        keyStart++;
        const char *keyEnd = strchr(keyStart, '"');
        if (!keyEnd) break;

        u64 titleId = 0;
        if (!parseTitleIdString(keyStart, (size_t)(keyEnd - keyStart), &titleId)) {
            p = keyEnd + 1;
            continue;
        }

        const char *colon = strchr(keyEnd + 1, ':');
        if (!colon) break;
        const char *value = colon + 1;
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
        if (*value != '{') {
            p = keyEnd + 1;
            continue;
        }

        // 隔离这一个 title 的对象再解析, 否则 readJsonU32Field 会命中别的 title 的字段。
        char ruleObject[256];
        const char *after = copyJsonObjectAt(value, ruleObject, sizeof(ruleObject));
        if (!after) break;

        LimitRecord *record = findLimitRecord(records, *count, titleId);
        if (!record) {
            if (*count >= maxCount) break;
            record = &records[*count];
            memset(record, 0, sizeof(*record));
            record->titleId = titleId;
            (*count)++;
        }

        u32 daily = 0;
        u32 weekly = 0;
        if (readJsonU32FieldMax(ruleObject, "daily_seconds", WEEKDAY_SECONDS_MAX, &daily))
            record->limitSeconds = daily;
        if (readJsonU32FieldMax(ruleObject, "weekly_seconds", WEEKLY_SECONDS_MAX, &weekly))
            record->weeklySeconds = weekly;
        // §15: 数组恰好 7 个值才算"按星期已配置"; 否则 hasWeekday=false 并回退每日值。
        record->hasWeekday = parseWeekdaySeconds(ruleObject, record->weekdaySeconds);
        record->hasRule = true;

        p = after;
    }
}

bool isHexString(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

void inferDeviceIdType(void) {
    if (g_config.deviceIdType[0]) return;
    if (strlen(g_config.deviceId) == 64 && isHexString(g_config.deviceId, 64))
        strncpy(g_config.deviceIdType, "hardware_hash", sizeof(g_config.deviceIdType) - 1);
    else
        strncpy(g_config.deviceIdType, "random_uuid", sizeof(g_config.deviceIdType) - 1);
}

bool generateHardwareDeviceId(char *out, size_t outSize) {
    SetSysSerialNumber serial = {0};
    Result rc = setsysInitialize();
    if (R_FAILED(rc)) return false;

    rc = setsysGetSerialNumber(&serial);
    setsysExit();
    if (R_FAILED(rc) || !serial.number[0]) return false;

    char material[96];
    snprintf(material, sizeof(material), "PlayLogTool:device-id:v1:%s", serial.number);
    sha256Hex(material, out, outSize);
    return out[0] != '\0';
}

void ensureDeviceIdentity(void) {
    if (g_config.deviceId[0]) {
        inferDeviceIdType();
        return;
    }

    if (generateHardwareDeviceId(g_config.deviceId, sizeof(g_config.deviceId))) {
        strncpy(g_config.deviceIdType, "hardware_hash", sizeof(g_config.deviceIdType) - 1);
        return;
    }

    generateUuidString(g_config.deviceId, sizeof(g_config.deviceId));
    strncpy(g_config.deviceIdType, "random_uuid", sizeof(g_config.deviceIdType) - 1);
}

void buildEndpointUrl(const char *path, char *out, size_t outSize) {
    if (outSize == 0) return;
    size_t len = strlen(g_config.serverBaseUrl);
    const char *slash = (len > 0 && g_config.serverBaseUrl[len - 1] == '/') ? "" : "/";
    const char *cleanPath = (path[0] == '/') ? path + 1 : path;
    snprintf(out, outSize, "%s%s%s", g_config.serverBaseUrl, slash, cleanPath);
}

void loadAppConfig(void) {
    memset(&g_config, 0, sizeof(g_config));
    strncpy(g_config.serverBaseUrl, DEFAULT_SERVER_BASE_URL, sizeof(g_config.serverBaseUrl) - 1);
    g_config.limitReloadSeconds = 30;
    g_config.configSyncSeconds = 30;
    g_config.uploadIntervalSeconds = 900;
    g_config.heartbeatIntervalSeconds = 300;
    // §10: 字段缺失/为空 -> 默认 block_launch(老 SD 卡行为不变)。
    strncpy(g_config.enforceMode, ENFORCE_MODE_DEFAULT, sizeof(g_config.enforceMode) - 1);

    char body[2048];
    bool exists = false;
    if (readWholeFile(CONFIG_PATH, body, sizeof(body), &exists, NULL)) {
        readJsonStringField(body, "server_base_url", g_config.serverBaseUrl, sizeof(g_config.serverBaseUrl));
        readJsonStringField(body, "device_id", g_config.deviceId, sizeof(g_config.deviceId));
        readJsonStringField(body, "device_id_type", g_config.deviceIdType, sizeof(g_config.deviceIdType));
        readJsonStringField(body, "upload_token", g_config.uploadToken, sizeof(g_config.uploadToken));
        readJsonU32Field(body, "limit_reload_seconds", &g_config.limitReloadSeconds);
        readJsonU32Field(body, "config_sync_seconds", &g_config.configSyncSeconds);
        readJsonU32Field(body, "upload_interval_seconds", &g_config.uploadIntervalSeconds);
        readJsonU32Field(body, "heartbeat_interval_seconds", &g_config.heartbeatIntervalSeconds);
        if (!readJsonStringField(body, "enforce_mode", g_config.enforceMode, sizeof(g_config.enforceMode)) ||
            !g_config.enforceMode[0])
            strncpy(g_config.enforceMode, ENFORCE_MODE_DEFAULT, sizeof(g_config.enforceMode) - 1);
        {
            char ui_lang[8] = "";
            if (readJsonStringField(body, "ui_lang", ui_lang, sizeof(ui_lang)))
                i18n_set_en(strcmp(ui_lang, "en") == 0);
        }
        g_configWritable = true;
    } else if (!exists) {
        // 文件不存在(首次运行): 可以安全地写入默认值。
        g_configWritable = true;
    } else {
        // 文件存在但读不出来(截断/超长/IO错误): 绝不能用默认值覆盖它,
        // 否则会把 upload_token 清空。这里只保留内存里的默认值用于本次运行。
        g_configWritable = false;
        printf("[配置] " CONFIG_PATH " 存在但无法读取，保持原样。\n");
    }

    if (g_config.limitReloadSeconds < 5) g_config.limitReloadSeconds = 5;
    if (g_config.configSyncSeconds < 60) g_config.configSyncSeconds = 60;
    if (g_config.uploadIntervalSeconds < 60 || g_config.uploadIntervalSeconds > 86400) g_config.uploadIntervalSeconds = 900;
    if (g_config.heartbeatIntervalSeconds < 60 || g_config.heartbeatIntervalSeconds > 86400) g_config.heartbeatIntervalSeconds = 300;
    size_t len = strlen(g_config.serverBaseUrl);
    while (len > 0 && g_config.serverBaseUrl[len - 1] == '/') {
        g_config.serverBaseUrl[len - 1] = '\0';
        len--;
    }
    ensureDeviceIdentity();
}

void saveAppConfig(void) {
    if (!g_configWritable) {
        printf("[配置] 读取失败，拒绝覆盖 " CONFIG_PATH "。\n");
        return;
    }

    char safeBaseUrl[512];
    char safeDeviceId[80];
    char safeDeviceIdType[40];
    char safeToken[260];
    char safeEnforceMode[ENFORCE_MODE_MAX * 2];
    escapeJsonString(g_config.serverBaseUrl, safeBaseUrl, sizeof(safeBaseUrl));
    escapeJsonString(g_config.deviceId, safeDeviceId, sizeof(safeDeviceId));
    escapeJsonString(g_config.deviceIdType, safeDeviceIdType, sizeof(safeDeviceIdType));
    escapeJsonString(g_config.uploadToken, safeToken, sizeof(safeToken));
    escapeJsonString(g_config.enforceMode, safeEnforceMode, sizeof(safeEnforceMode));

    // §10 追加 enforce_mode 后把缓冲区放大一点: 各字符串按最坏转义长度估算
    // 已接近原来的 1024, 溢出时下面的检查会拒绝写文件(等于保存静默失败)。
    char out[1536];
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
            "  \"enforce_mode\": \"%s\",\n"
            "  \"ui_lang\": \"%s\"\n"
            "}\n",
            safeBaseUrl, safeDeviceId, safeDeviceIdType, safeToken,
            g_config.limitReloadSeconds, g_config.configSyncSeconds, g_config.uploadIntervalSeconds,
            g_config.heartbeatIntervalSeconds, safeEnforceMode, i18n_is_en() ? "en" : "zh");
    if (needed < 0 || (size_t)needed >= sizeof(out)) {
        printf("[配置] 拒绝写入被截断的 " CONFIG_PATH "。\n");
        return;
    }

    if (!writeFileAtomic(CONFIG_PATH, out, (size_t)needed))
        printf("[配置] 写入 " CONFIG_PATH " 失败。\n");
}

static unsigned long long monotonic_seconds(void)
{
    return (unsigned long long)(armTicksToNs(armGetSystemTick()) / 1000000000ULL);
}

static long long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, int *yy, unsigned *mm, unsigned *dd)
{
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y = (int)yoe + (int)era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : 9);
    *yy = y + (m <= 2);
    *mm = m;
    *dd = d;
}

/* server_local_time 是北京时间(YYYY-MM-DD HH:MM:SS)。把它当作"UTC 语义"解析、再按 UTC 格式化，
 * 往返一致；累加时间用单调时钟，不依赖设备 RTC。 */
static void format_server_now(char *out, size_t cap)
{
    if (!g_server_time_valid) {
        snprintf(out, cap, "-");
        return;
    }
    unsigned long long now = monotonic_seconds();
    long long elapsed = (long long)(now - g_server_sync_mono);
    if (elapsed < 0) elapsed = 0;
    long long ts = g_server_epoch + elapsed;
    long long days = ts / 86400;
    int sec = (int)(ts % 86400);
    if (sec < 0) { sec += 86400; days--; }
    int yy;
    unsigned mm, dd;
    civil_from_days(days, &yy, &mm, &dd);
    int h = sec / 3600, mi = (sec % 3600) / 60, ss = sec % 60;
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", yy, mm, dd, h, mi, ss);
}

int loadLimits(LimitRecord *out, int maxCount) {
    char body[8192];
    bool exists = false;
    if (!readWholeFile(LIMITS_PATH, body, sizeof(body), &exists, NULL)) {
        // 关键: 读失败时返回 -1 表示"未知",调用方必须保留当前内存里的限额,
        // 不能当成"没有限额"。文件不存在才返回 0(确实没有配置)。
        if (exists) {
            g_limitsUnknown = true;
            return -1;
        }
        g_limitsUnknown = false;
        return 0;
    }
    g_limitsUnknown = false;

    g_allow_offline_edit = strstr(body, "\"allow_offline_edit\":true") != NULL ||
                           strstr(body, "\"allow_offline_edit\": true") != NULL;

    {
        char st[32] = "";
        if (readJsonStringField(body, "server_local_time", st, sizeof(st)) && st[0] &&
            strcmp(st, g_server_time_raw) != 0) {
            int yy = 0, mo = 0, dd = 0, hh = 0, mi = 0, ss = 0;
            if (sscanf(st, "%d-%d-%d %d:%d:%d", &yy, &mo, &dd, &hh, &mi, &ss) == 6) {
                g_server_epoch = days_from_civil(yy, (unsigned)mo, (unsigned)dd) * 86400LL
                                 + hh * 3600 + mi * 60 + ss;
                g_server_sync_mono = monotonic_seconds();
                g_server_time_valid = true;
                copyBoundedString(g_server_time_raw, sizeof(g_server_time_raw), st, strlen(st));
            }
        }
    }

    parseAccountsFromLimits(body);

    int count = 0;
    char *p = body;
    while ((p = strchr(p, '"')) != NULL && count < maxCount) {
        char *keyStart = p + 1;
        char *keyEnd = strchr(keyStart, '"');
        if (!keyEnd) break;

        u64 titleId;
        if (!parseTitleIdString(keyStart, (size_t)(keyEnd - keyStart), &titleId)) {
            p = keyEnd + 1;
            continue;
        }

        char *colon = strchr(keyEnd + 1, ':');
        if (!colon) break;

        char *num = colon + 1;
        while (*num == ' ' || *num == '\t' || *num == '\r' || *num == '\n') num++;

        char *end = NULL;
        unsigned long seconds = strtoul(num, &end, 10);
        if (end != num && seconds <= 86400) {
            memset(&out[count], 0, sizeof(LimitRecord));
            out[count].titleId = titleId;
            out[count].limitSeconds = (u32)seconds;
            count++;
        }
        p = keyEnd + 1;
    }

    // §11: 追加的 limit_rules 子树(周总时长 + 按星期限额)。缺失时什么都不做,
    // 条目的 hasRule 保持 false, 于是使用老的"每日值"语义(0 = 封禁)。
    parseLimitRulesIntoRecords(body, out, maxCount, &count);

    return count;
}

bool getLimitSeconds(LimitRecord *limits, int count, u64 titleId, u32 *outSeconds) {
    for (int i = 0; i < count; i++) {
        if (limits[i].titleId == titleId) {
            *outSeconds = limits[i].limitSeconds;
            return true;
        }
    }
    return false;
}

void setLimitSeconds(LimitRecord *limits, int *count, u64 titleId, u32 seconds) {
    for (int i = 0; i < *count; i++) {
        if (limits[i].titleId == titleId) {
            // §11: 只改每日值, 保留同一 title 的周总时长与按星期限额。
            limits[i].limitSeconds = seconds;
            return;
        }
    }
    if (*count < MAX_LIMITS) {
        memset(&limits[*count], 0, sizeof(LimitRecord));
        limits[*count].titleId = titleId;
        limits[*count].limitSeconds = seconds;
        (*count)++;
    }
}

/// §15: "按星期已配置"(服务端下发的数组恰好 7 个值)。已配置时每天的值(含 0)
/// 都生效, 0 = 当天封禁; 未配置才回退每日值。注意这与"有没有非 0 覆盖值"不同:
/// `[0]*7` 是"每天都封禁", 不是tr("未配置")。
bool recordHasWeekdayOverride(const LimitRecord *record) {
    return record->hasWeekday;
}

// §11 三个上限全为 0 且未配置按星期 = 旧语义"0 分钟 = 封禁"。
bool recordBlocksAll(const LimitRecord *record) {
    return record->limitSeconds == 0 && record->weeklySeconds == 0 &&
           !recordHasWeekdayOverride(record);
}

// §15 今日生效限额(秒): 按星期已配置 -> weekdaySeconds[今天](0 也生效 = 当天封禁);
// 未配置 -> limitSeconds。只用于界面显示; 真正的执行在 sysmodule。
// 时钟算不出星期几时保守返回基础每日值。
u32 recordEffectiveDailySeconds(const LimitRecord *record) {
    if (!record) return 0;

    time_t now;
    time(&now);
    struct tm *t = localtime(&now);
    if (!t) return record->limitSeconds;

    int index = (t->tm_wday + 6) % 7;   // tm_wday: 0 = 周日 -> 0 = 周一 … 6 = 周日
    if (index < 0 || index >= WEEKDAY_COUNT) return record->limitSeconds;
    // 唯一真源: limits_common.h 的纯函数(与 sysmodule 共用同一条 §15 判定)。
    return (u32)limits_effective_daily_seconds(record->limitSeconds, record->weekdaySeconds,
                                               record->hasWeekday ? 1 : 0, index);
}

// §16 今日生效合计上限(秒): 按星期已配置 -> 该天值(0 = 当天封禁); 未配置 -> daily。
// 与 recordEffectiveDailySeconds 同一条 §15 判定, 只用于界面显示(执行在 sysmodule)。
// 时钟算不出星期几时保守返回基础每日值。
u32 accountTotalEffectiveDailySeconds(const AccountTotalRecord *record) {
    if (!record) return 0;

    time_t now;
    time(&now);
    struct tm *t = localtime(&now);
    if (!t) return record->dailySeconds;

    int index = (t->tm_wday + 6) % 7;
    if (index < 0 || index >= WEEKDAY_COUNT) return record->dailySeconds;
    // 唯一真源: limits_common.h 的纯函数(与 sysmodule 共用同一条 §15 判定)。
    return (u32)limits_effective_daily_seconds(record->dailySeconds, record->weekdaySeconds,
                                               record->hasWeekday ? 1 : 0, index);
}

void clearLimit(LimitRecord *limits, int *count, u64 titleId) {
    for (int i = 0; i < *count; i++) {
        if (limits[i].titleId == titleId) {
            for (int j = i; j < *count - 1; j++) limits[j] = limits[j + 1];
            (*count)--;
            return;
        }
    }
}

static bool sb_add(char *buf, size_t cap, size_t *used, bool *first, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;
    if (!*first) {
        if (*used + 2 >= cap) return false;
        buf[(*used)++] = ',';
        buf[(*used)++] = '\n';
    }
    *first = false;
    if (*used + (size_t)n >= cap) return false;
    memcpy(buf + *used, tmp, (size_t)n);
    *used += (size_t)n;
    return true;
}

static void weekdays_json(const u32 w[WEEKDAY_COUNT], bool has, char *out, size_t cap)
{
    if (!has) {
        snprintf(out, cap, "[]");
        return;
    }
    snprintf(out, cap, "[%u, %u, %u, %u, %u, %u, %u]",
             w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
}

void saveLimits(LimitRecord *limits, int count) {
    if (g_limitsUnknown) {
        printf("[限额] 读取失败，拒绝覆盖 " LIMITS_PATH "。\n");
        return;
    }

    static char out[32768];
    static char sec[16384];
    static char al[16384], ar[16384], at[8192];
    static char a1[8192], a2[8192];

    size_t used = 0;
    bool first = true;
    out[used++] = '{';
    out[used++] = '\n';
    bool ok = true;

    for (int i = 0; i < count && ok; i++)
        ok = sb_add(out, sizeof(out), &used, &first, "  \"%016lX\": %u",
                    limits[i].titleId, limits[i].limitSeconds);

    if (ok) {
        size_t su = 0;
        bool sf = true;
        sec[su++] = '{';
        for (int i = 0; i < count; i++) {
            if (!limitRecordHasRule(&limits[i])) continue;
            char wk[160];
            weekdays_json(limits[i].weekdaySeconds, limits[i].hasWeekday, wk, sizeof(wk));
            if (!sb_add(sec, sizeof(sec), &su, &sf,
                        "    \"%016lX\": {\"daily_seconds\": %u, \"weekly_seconds\": %u, \"weekday_seconds\": %s}",
                        limits[i].titleId, limits[i].limitSeconds, limits[i].weeklySeconds, wk)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            if (su + 2 >= sizeof(sec)) ok = false;
            else { sec[su++] = '\n'; sec[su++] = '}'; sec[su] = '\0'; }
        }
        if (ok) ok = sb_add(out, sizeof(out), &used, &first, "  \"limit_rules\": %s", sec);
    }

    if (ok) {
        size_t alu = 0, aru = 0;
        bool alf = true, arf = true;
        al[alu++] = '{';
        ar[aru++] = '{';
        for (int ai = 0; ai < g_accountCount && ok; ai++) {
            const char *aid = g_accounts[ai].accountId;
            bool any = false;
            for (int i = 0; i < g_accountGameLimitCount; i++) {
                if (strcmp(g_accountGameLimits[i].accountId, aid) == 0) { any = true; break; }
            }
            if (!any) continue;

            size_t u1 = 0, u2 = 0;
            bool f1 = true, f2 = true;
            a1[u1++] = '{';
            a2[u2++] = '{';
            for (int i = 0; i < g_accountGameLimitCount; i++) {
                AccountGameLimitRecord *r = &g_accountGameLimits[i];
                if (strcmp(r->accountId, aid) != 0) continue;
                if (!sb_add(a1, sizeof(a1), &u1, &f1, "      \"%016lX\": %u", r->titleId, r->dailySeconds)) {
                    ok = false;
                    break;
                }
                char wk[160];
                weekdays_json(r->weekdaySeconds, r->hasWeekday, wk, sizeof(wk));
                if (!sb_add(a2, sizeof(a2), &u2, &f2,
                            "      \"%016lX\": {\"daily_seconds\": %u, \"weekly_seconds\": %u, \"weekday_seconds\": %s}",
                            r->titleId, r->dailySeconds, r->weeklySeconds, wk)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
            if (u1 + 2 >= sizeof(a1) || u2 + 2 >= sizeof(a2)) { ok = false; break; }
            a1[u1++] = '\n'; a1[u1++] = '}'; a1[u1] = '\0';
            a2[u2++] = '\n'; a2[u2++] = '}'; a2[u2] = '\0';
            if (!sb_add(al, sizeof(al), &alu, &alf, "    \"%s\": %s", aid, a1)) { ok = false; break; }
            if (!sb_add(ar, sizeof(ar), &aru, &arf, "    \"%s\": %s", aid, a2)) { ok = false; break; }
        }
        if (ok) {
            if (alu + 2 >= sizeof(al) || aru + 2 >= sizeof(ar)) ok = false;
            else {
                al[alu++] = '\n'; al[alu++] = '}'; al[alu] = '\0';
                ar[aru++] = '\n'; ar[aru++] = '}'; ar[aru] = '\0';
            }
        }
        if (ok) ok = sb_add(out, sizeof(out), &used, &first, "  \"account_limits\": %s", al);
        if (ok) ok = sb_add(out, sizeof(out), &used, &first, "  \"account_limit_rules\": %s", ar);
    }

    if (ok) {
        size_t au = 0;
        bool af = true;
        at[au++] = '{';
        for (int i = 0; i < g_accountTotalCount; i++) {
            AccountTotalRecord *r = &g_accountTotals[i];
            char wk[160];
            weekdays_json(r->weekdaySeconds, r->hasWeekday, wk, sizeof(wk));
            if (!sb_add(at, sizeof(at), &au, &af,
                        "    \"%s\": {\"daily_seconds\": %u, \"weekday_seconds\": %s}",
                        r->accountId, r->dailySeconds, wk)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            if (au + 2 >= sizeof(at)) ok = false;
            else { at[au++] = '\n'; at[au++] = '}'; at[au] = '\0'; }
        }
        if (ok) ok = sb_add(out, sizeof(out), &used, &first, "  \"account_total_limit_rules\": %s", at);
    }

    if (!ok || used + 3 >= sizeof(out)) {
        printf("[限额] 拒绝写入被截断的 " LIMITS_PATH "。\n");
        return;
    }
    out[used++] = '\n';
    out[used++] = '}';
    out[used++] = '\n';

    if (!writeFileAtomic(LIMITS_PATH, out, used))
        printf("[限额] 写入 " LIMITS_PATH " 失败。\n");
}

static AccountGameLimitRecord *findAccountGameLimit(const char *aid, u64 tid) {
    if (!aid) return NULL;
    for (int i = 0; i < g_accountGameLimitCount; i++) {
        if (strcmp(g_accountGameLimits[i].accountId, aid) == 0 &&
            g_accountGameLimits[i].titleId == tid)
            return &g_accountGameLimits[i];
    }
    return NULL;
}

static void setAccountGameDaily(const char *aid, u64 tid, u32 seconds) {
    if (!aid) return;
    AccountGameLimitRecord *r = findAccountGameLimit(aid, tid);
    if (!r) {
        if (g_accountGameLimitCount >= MAX_ACCOUNT_GAME_LIMITS) return;
        r = &g_accountGameLimits[g_accountGameLimitCount++];
        memset(r, 0, sizeof(*r));
        copyBoundedString(r->accountId, sizeof(r->accountId), aid, strlen(aid));
        r->titleId = tid;
    }
    r->dailySeconds = seconds;
    r->hasWeekday = false;
    memset(r->weekdaySeconds, 0, sizeof(r->weekdaySeconds));
    r->weeklySeconds = 0;
}

static void clearAccountGameLimit(const char *aid, u64 tid) {
    for (int i = 0; i < g_accountGameLimitCount; i++) {
        if (strcmp(g_accountGameLimits[i].accountId, aid) == 0 &&
            g_accountGameLimits[i].titleId == tid) {
            for (int j = i; j < g_accountGameLimitCount - 1; j++)
                g_accountGameLimits[j] = g_accountGameLimits[j + 1];
            g_accountGameLimitCount--;
            return;
        }
    }
}

static AccountTotalRecord *findAccountTotal(const char *aid) {
    if (!aid) return NULL;
    for (int i = 0; i < g_accountTotalCount; i++) {
        if (strcmp(g_accountTotals[i].accountId, aid) == 0)
            return &g_accountTotals[i];
    }
    return NULL;
}

static void setAccountTotalDaily(const char *aid, u32 seconds) {
    if (!aid) return;
    AccountTotalRecord *r = findAccountTotal(aid);
    if (!r) {
        if (g_accountTotalCount >= MAX_ACCOUNTS) return;
        r = &g_accountTotals[g_accountTotalCount++];
        memset(r, 0, sizeof(*r));
        copyBoundedString(r->accountId, sizeof(r->accountId), aid, strlen(aid));
    }
    r->dailySeconds = seconds;
    r->hasWeekday = false;
    memset(r->weekdaySeconds, 0, sizeof(r->weekdaySeconds));
}

static void clearAccountTotal(const char *aid) {
    for (int i = 0; i < g_accountTotalCount; i++) {
        if (strcmp(g_accountTotals[i].accountId, aid) == 0) {
            for (int j = i; j < g_accountTotalCount - 1; j++)
                g_accountTotals[j] = g_accountTotals[j + 1];
            g_accountTotalCount--;
            return;
        }
    }
}

static void setDeviceDaily(LimitRecord *limits, int *count, u64 tid, u32 seconds) {
    LimitRecord *r = findLimitRecord(limits, *count, tid);
    if (!r) {
        if (*count >= MAX_LIMITS) return;
        r = &limits[(*count)++];
        memset(r, 0, sizeof(*r));
        r->titleId = tid;
    }
    r->limitSeconds = seconds;
    r->hasRule = true;
    r->hasWeekday = false;
    memset(r->weekdaySeconds, 0, sizeof(r->weekdaySeconds));
    r->weeklySeconds = 0;
}

int chooseMenu(PadState *pad, const char *title, const char *const *items, int n) {
    if (n <= 0) return -1;
    int sel = 0;
    while (uiMainLoop()) {
        uiFrameBegin(title, NULL);
        uiSetFooter(tr("↑↓ 选择    A 确认    B 取消"));
        for (int i = 0; i < n; i++) {
            int y = 120 + i * 42;
            if (i == sel)
                uiFill(100, y - 6, 1080, 36, UI_SELECTED);
            uiTextAt(116, y, 1040, tr(items[i]), i == sel ? UI_TITLE : UI_NORMAL);
        }
        uiFrameEnd();
        padUpdate(pad);
        u64 k = padGetButtonsDown(pad);
        if ((k & HidNpadButton_AnyUp) && sel > 0) sel--;
        if ((k & HidNpadButton_AnyDown) && sel < n - 1) sel++;
        if (k & HidNpadButton_A) return sel;
        if (k & HidNpadButton_B) return -1;
    }
    return -1;
}

bool showKeyboard(const char *header, const char *guide, const char *initial, SwkbdType type,
                  char *out, size_t outSize) {
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) return false;

    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, type);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetOkButtonText(&kbd, "OK");
    swkbdConfigSetStringLenMax(&kbd, outSize > 1 ? (u32)outSize - 1 : 1);
    swkbdConfigSetStringLenMin(&kbd, 1);
    if (initial && initial[0]) swkbdConfigSetInitialText(&kbd, initial);

    rc = swkbdShow(&kbd, out, outSize);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc);
}

bool promptLimitMinutes(u32 currentSeconds, u32 *outSeconds) {
    char initial[16];
    if (currentSeconds > 0) snprintf(initial, sizeof(initial), "%u", currentSeconds / 60);
    else initial[0] = '\0';

    char input[16] = {0};
    if (!showKeyboard("Daily limit", "Minutes, 0-1440; 0 blocks play", initial, SwkbdType_NumPad, input, sizeof(input)))
        return false;

    char *end = NULL;
    unsigned long minutes = strtoul(input, &end, 10);
    if (end == input || minutes > 1440)
        return false;

    *outSeconds = (u32)(minutes * 60);
    return true;
}

bool promptServerBaseUrl(void) {
    char input[256] = {0};
    if (!showKeyboard("Server URL", "Example: https://your-server.example.com", g_config.serverBaseUrl,
                      SwkbdType_QWERTY, input, sizeof(input)))
        return false;

    if (strncmp(input, "http://", 7) != 0 && strncmp(input, "https://", 8) != 0)
        return false;

    strncpy(g_config.serverBaseUrl, input, sizeof(g_config.serverBaseUrl) - 1);
    g_config.serverBaseUrl[sizeof(g_config.serverBaseUrl) - 1] = '\0';
    size_t len = strlen(g_config.serverBaseUrl);
    while (len > 0 && g_config.serverBaseUrl[len - 1] == '/') {
        g_config.serverBaseUrl[len - 1] = '\0';
        len--;
    }
    saveAppConfig();
    return true;
}

bool promptUploadToken(void) {
    char input[129] = {0};
    if (!showKeyboard("Upload token", "Shared secret from server", g_config.uploadToken,
                      SwkbdType_QWERTY, input, sizeof(input)))
        return false;

    if (strlen(input) < 16)
        return false;

    strncpy(g_config.uploadToken, input, sizeof(g_config.uploadToken) - 1);
    g_config.uploadToken[sizeof(g_config.uploadToken) - 1] = '\0';
    saveAppConfig();
    return true;
}

bool promptIntervalSettings(void) {
    char initial[16];
    char input[16] = {0};

    snprintf(initial, sizeof(initial), "%u", g_config.limitReloadSeconds);
    if (!showKeyboard("Local reload", "Seconds, 5-3600", initial, SwkbdType_NumPad,
                      input, sizeof(input)))
        return false;
    char *end = NULL;
    unsigned long reloadSeconds = strtoul(input, &end, 10);
    if (end == input || reloadSeconds < 5 || reloadSeconds > 3600)
        return false;

    memset(input, 0, sizeof(input));
    snprintf(initial, sizeof(initial), "%u", g_config.configSyncSeconds);
    if (!showKeyboard("Cloud sync", "Seconds, 60-86400", initial, SwkbdType_NumPad,
                      input, sizeof(input)))
        return false;
    end = NULL;
    unsigned long syncSeconds = strtoul(input, &end, 10);
    if (end == input || syncSeconds < 60 || syncSeconds > 86400)
        return false;

    memset(input, 0, sizeof(input));
    snprintf(initial, sizeof(initial), "%u", g_config.uploadIntervalSeconds);
    if (!showKeyboard("Auto upload", "Seconds, 60-86400", initial, SwkbdType_NumPad,
                      input, sizeof(input)))
        return false;
    end = NULL;
    unsigned long uploadSeconds = strtoul(input, &end, 10);
    if (end == input || uploadSeconds < 60 || uploadSeconds > 86400)
        return false;

    memset(input, 0, sizeof(input));
    snprintf(initial, sizeof(initial), "%u", g_config.heartbeatIntervalSeconds);
    if (!showKeyboard("Heartbeat", "Seconds, 60-86400", initial, SwkbdType_NumPad,
                      input, sizeof(input)))
        return false;
    end = NULL;
    unsigned long heartbeatSeconds = strtoul(input, &end, 10);
    if (end == input || heartbeatSeconds < 60 || heartbeatSeconds > 86400)
        return false;

    g_config.limitReloadSeconds = (u32)reloadSeconds;
    g_config.configSyncSeconds = (u32)syncSeconds;
    g_config.uploadIntervalSeconds = (u32)uploadSeconds;
    g_config.heartbeatIntervalSeconds = (u32)heartbeatSeconds;
    saveAppConfig();
    return true;
}

void applyRemoteAppSettings(const char *body) {
    u32 value = 0;
    bool changed = false;

    if (readJsonU32Field(body, "limit_reload_seconds", &value) && value >= 5 && value <= 3600) {
        g_config.limitReloadSeconds = value;
        changed = true;
    }
    if (readJsonU32Field(body, "config_sync_seconds", &value) && value >= 60 && value <= 86400) {
        g_config.configSyncSeconds = value;
        changed = true;
    }
    if (readJsonU32Field(body, "upload_interval_seconds", &value) && value >= 60 && value <= 86400) {
        g_config.uploadIntervalSeconds = value;
        changed = true;
    }
    if (readJsonU32Field(body, "heartbeat_interval_seconds", &value) && value >= 60 && value <= 86400) {
        g_config.heartbeatIntervalSeconds = value;
        changed = true;
    }
    // §10 服务端下发的执行方式是权威值; NRO 不执行限额, 但负责把它落进
    // config.json, 这样 sysmodule 离线启动时也有正确的兜底值。
    // 取值按原样保存(不在此处校验): 无法识别的取值交给 sysmodule 判为
    // unknown 并选择"不终止", 比悄悄改写成默认值更安全、也更好排查。
    {
        char mode[ENFORCE_MODE_MAX];
        mode[0] = '\0';
        if (readJsonStringField(body, "enforce_mode", mode, sizeof(mode)) && mode[0] &&
            strcmp(mode, g_config.enforceMode) != 0) {
            strncpy(g_config.enforceMode, mode, sizeof(g_config.enforceMode) - 1);
            g_config.enforceMode[sizeof(g_config.enforceMode) - 1] = '\0';
            changed = true;
        }
    }

    if (changed)
        saveAppConfig();
}

bool getCurrentUnixTimestamp(char *out, size_t outSize) {
    time_t now;
    time(&now);
    if (now <= 0) return false;
    snprintf(out, outSize, "%ld", (long)now);
    return true;
}

void buildRequestSignature(const char *method, const char *path, const char *body,
                           const char *timestamp, char *signature, size_t signatureSize) {
    char bodyHash[SHA256_HASH_SIZE * 2 + 1];
    sha256Hex(body ? body : "", bodyHash, sizeof(bodyHash));

    char canonical[768];
    snprintf(canonical, sizeof(canonical),
             "%s\n"
             "%s\n"
             "%s\n"
             "%s\n"
             "%s",
             method,
             path,
             g_config.deviceId,
             timestamp,
             bodyHash);

    hmacSha256Hex(g_config.uploadToken, canonical, signature, signatureSize);
}

size_t writeMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realSize = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;

    if (mem->size + realSize + 1 > MAX_RESPONSE_SIZE)
        return 0;

    if (mem->size + realSize + 1 > mem->capacity) {
        size_t newCapacity = mem->capacity ? mem->capacity * 2 : 4096;
        while (newCapacity < mem->size + realSize + 1) newCapacity *= 2;
        char *ptr = realloc(mem->data, newCapacity);
        if (!ptr) return 0;
        mem->data = ptr;
        mem->capacity = newCapacity;
    }

    memcpy(mem->data + mem->size, contents, realSize);
    mem->size += realSize;
    mem->data[mem->size] = '\0';
    return realSize;
}

//---------------------------------------------------------------------------------
// 服务端响应签名校验(REFACTOR_SPEC §6 item 20)
//
// 约定的响应签名(需要服务端配合实现):
//   X-PlayLog-Response-Signature: <hex>
//   <hex> = hex(HMAC-SHA256(upload_token, <raw response body bytes>))
//   即直接对响应体原始字节做 HMAC,不做任何 JSON 规范化(避免两端解析差异)。
// 也接受 Authorization 里带 signature="..." 的形态,方便服务端复用请求签名代码。
//
// 只要响应头里带了签名,就必须校验通过;签名缺失时由调用方决定是否 fail closed。
//---------------------------------------------------------------------------------
size_t headerCaptureCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t realSize = size * nitems;
    ResponseSignature *sig = (ResponseSignature *)userdata;

    if (realSize > 13 && strncasecmp(buffer, "x-playlog-response-signature:", 29) == 0) {
        const char *value = buffer + 29;
        while (*value == ' ' || *value == '\t') value++;
        size_t len = 0;
        while (len < sizeof(sig->hex) - 1 && value[len] && value[len] != '\r' && value[len] != '\n') {
            sig->hex[len] = value[len];
            len++;
        }
        sig->hex[len] = '\0';
        sig->present = len > 0;
    } else if (realSize > 13 && strncasecmp(buffer, "authorization:", 14) == 0) {
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
    return realSize;
}

bool constantTimeEquals(const char *a, const char *b) {
    size_t lenA = strlen(a);
    size_t lenB = strlen(b);
    if (lenA == 0 || lenA != lenB) return false;

    unsigned char diff = 0;
    for (size_t i = 0; i < lenA; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

bool verifyResponseSignature(const char *body, size_t bodyLen, ResponseSignature *sig) {
    if (!sig->present || sig->hex[0] == '\0')
        return false;

    // 对响应体原始字节直接做 HMAC,不做任何 JSON 规范化(避免两端解析差异)。
    u8 mac[SHA256_HASH_SIZE];
    hmacSha256CalculateMac(mac, g_config.uploadToken, strlen(g_config.uploadToken), body, bodyLen);
    char expected[SHA256_HASH_SIZE * 2 + 1];
    bytesToHex(mac, sizeof(mac), expected, sizeof(expected));
    return constantTimeEquals(expected, sig->hex);
}

// CA 文件缺失时明确报错,而不是静默退化成"不校验证书"。
bool cainfoAvailable(void) {
    FILE *ca = fopen(CAINFO_PATH, "rb");
    if (!ca) {
        printf("[TLS] 缺少 " CAINFO_PATH "，拒绝跳过证书校验。\n");
        logPushError("missing " CAINFO_PATH);
        return false;
    }
    fclose(ca);
    return true;
}

void configureTlsVerification(CURL *curl) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, CAINFO_PATH);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

void appendSignedHeaders(struct curl_slist **headers, const char *method, const char *path,
                         const char *body, const char *timestamp, const char *signature) {
    (void)method;
    (void)path;
    (void)body;

    char authHeader[256];
    char deviceHeader[96];
    char timestampHeader[64];
    char signatureHeader[96];

    snprintf(authHeader, sizeof(authHeader),
             "Authorization: PlayLog-HMAC-SHA256 device_id=\"%s\",timestamp=\"%s\",signature=\"%s\"",
             g_config.deviceId, timestamp, signature);
    snprintf(deviceHeader, sizeof(deviceHeader), "X-PlayLog-Device-Id: %s", g_config.deviceId);
    snprintf(timestampHeader, sizeof(timestampHeader), "X-PlayLog-Timestamp: %s", timestamp);
    snprintf(signatureHeader, sizeof(signatureHeader), "X-PlayLog-Signature: %s", signature);

    *headers = curl_slist_append(*headers, authHeader);
    *headers = curl_slist_append(*headers, deviceHeader);
    *headers = curl_slist_append(*headers, timestampHeader);
    *headers = curl_slist_append(*headers, signatureHeader);
}

bool syncLimitsFromServer(void) {
    if (g_limitsUnknown) {
        // 本地限额读失败时不接受云端覆盖,否则空表会把真实限额冲掉。
        printf("[配置] 本地 " LIMITS_PATH " 无法读取，拒绝覆盖。\n");
        return false;
    }

    if (!g_config.uploadToken[0]) {
        printf(tr("[配置] 未配置上传 Token。\n"));
        return false;
    }

    if (!cainfoAvailable()) {
        printf(tr("[配置] 未配置 TLS 校验，已中止同步。\n"));
        return false;
    }

    char timestamp[32];
    if (!getCurrentUnixTimestamp(timestamp, sizeof(timestamp))) {
        printf(tr("[配置] 无法读取系统时间。\n"));
        return false;
    }

    char signature[SHA256_HASH_SIZE * 2 + 1];
    buildRequestSignature("GET", CONFIG_FETCH_PATH, "", timestamp, signature, sizeof(signature));

    CURL *curl = curl_easy_init();
    if (!curl) {
        printf(tr("[配置] curl 初始化失败\n"));
        return false;
    }

    MemoryBuffer response = {0};
    ResponseSignature responseSig = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    appendSignedHeaders(&headers, "GET", CONFIG_FETCH_PATH, "", timestamp, signature);

    char url[320];
    buildEndpointUrl(CONFIG_FETCH_PATH, url, sizeof(url));
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCaptureCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseSig);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    configureTlsVerification(curl);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    bool ok = false;
    if (res == CURLE_OK && httpCode >= 200 && httpCode < 300 && response.data && response.size > 0) {
        // limits.json 覆盖属于"可恢复"的写操作: 无签名时照常应用,但必须大声记录,
        // 因为一份伪造的响应可以把限额清空(REFACTOR_SPEC §6 item 20)。
        if (responseSig.present) {
            if (verifyResponseSignature(response.data, response.size, &responseSig)) {
                printf(tr("[配置] 服务端响应签名校验通过。\n"));
            } else {
                printf("[配置] 服务端响应签名不匹配，拒绝覆盖 " LIMITS_PATH "。\n");
                logPushError("config response signature mismatch");
                apiLogLine(API_LOG_TAG_CONFIG, "GET", CONFIG_FETCH_PATH, httpCode,
                           NULL, "response signature mismatch; limits not applied");
                goto done;
            }
        } else {
            printf(tr("[配置] 警告：服务端未对配置响应签名，limits.json 更新未经验证。\n"));
            logPushError("config response has no signature (unverified limits update)");
        }

        if (writeFileAtomic(LIMITS_PATH, response.data, response.size)) {
            applyRemoteAppSettings(response.data);
            ok = true;
            printf(tr("[配置] 已同步服务端配置。\n"));
            apiLogLine(API_LOG_TAG_CONFIG, "GET", CONFIG_FETCH_PATH, httpCode, NULL, "OK");
        } else {
            printf("[配置] 写入 " LIMITS_PATH " 失败。\n");
            // 写盘失败已由 writeFileAtomic 记进接口日志(write tag)。
        }
    } else if (res != CURLE_OK) {
        printf(tr("[配置] 失败: %s\n"), curl_easy_strerror(res));
        apiLogLine(API_LOG_TAG_NET, "GET", CONFIG_FETCH_PATH, 0, NULL, curl_easy_strerror(res));
    } else if (httpCode >= 200 && httpCode < 300) {
        // 2xx 但响应体为空: 调用方按失败处理(没有可应用的 limits), 但仍如实记一行。
        printf(tr("[配置] 服务端返回 HTTP %ld（空响应体）\n"), httpCode);
        apiLogLine(API_LOG_TAG_CONFIG, "GET", CONFIG_FETCH_PATH, httpCode, NULL, "empty response body");
    } else {
        // §13: 屏幕只打印 ASCII 的 `code` + `hint`(中文 detail 只写日志文件)。
        char reason[512];
        char code[API_LOG_CODE_MAX];
        extractServerFailureDetail(response.data, httpCode, reason, sizeof(reason));
        extractServerFailureCode(response.data, code, sizeof(code));
        printServerFailure(tr("[配置] 服务端返回"), httpCode, response.data);
        logPushError(reason);
        apiLogLine(API_LOG_TAG_CONFIG, "GET", CONFIG_FETCH_PATH, httpCode, code, reason);
    }

done:
    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

//---------------------------------------------------------------------------------
// 全局游戏列表(账号采集会遍历它,所以放在这里做前置声明)
//---------------------------------------------------------------------------------
GameRecord *g_games = NULL;
int g_gameCount = 0;

//---------------------------------------------------------------------------------
// 账号枚举(REFACTOR_SPEC §5,仅 Switch 域)
//---------------------------------------------------------------------------------
bool g_accountReady = false;

void accountUidToHex(const AccountUid *uid, char *out, size_t outSize) {
    bytesToHex((const u8 *)uid->uid, sizeof(uid->uid), out, outSize);
}

// 定长字符串拷贝: 用 memcpy + 手工截断,避免 GCC 对 "%s" 的
// -Wformat-truncation 误报(源数组长度并不保证以 NUL 结尾)。
void copyBoundedString(char *out, size_t outSize, const char *in, size_t inSize) {
    if (outSize == 0) return;
    size_t len = 0;
    while (len < inSize && in[len] != '\0') len++;
    if (len >= outSize) len = outSize - 1;
    memcpy(out, in, len);
    out[len] = '\0';
}

bool accountHexEquals(const char *a, const char *b) {
    return a[0] != '\0' && strcmp(a, b) == 0;
}

/* limits.json 里的 accounts 带服务端昵称(alias), 用它补全/覆盖 Switch 端可能为空的昵称。 */
static void parseAccountsFromLimits(const char *body)
{
    const char *a = strstr(body, "\"accounts\"");
    if (!a)
        return;
    a = strchr(a, '[');
    if (!a)
        return;
    const char *end = strchr(a, ']');
    if (!end)
        return;

    const char *p = a;
    while (p < end) {
        const char *os = strchr(p, '{');
        if (!os || os > end)
            break;
        const char *oe = strchr(os, '}');
        if (!oe || oe > end)
            break;

        char obj[512];
        size_t len = (size_t)(oe - os + 1);
        if (len >= sizeof(obj))
            len = sizeof(obj) - 1;
        memcpy(obj, os, len);
        obj[len] = '\0';

        char aid[64] = "";
        if (readJsonStringField(obj, "account_id", aid, sizeof(aid)) && aid[0]) {
            char nick[96] = "";
            readJsonStringField(obj, "nickname", nick, sizeof(nick));
            bool active = strstr(obj, "\"is_active\":true") != NULL ||
                          strstr(obj, "\"is_active\": true") != NULL;

            int idx = -1;
            for (int i = 0; i < g_accountCount; i++) {
                if (accountHexEquals(g_accounts[i].accountId, aid)) { idx = i; break; }
            }
            if (idx < 0 && g_accountCount < MAX_ACCOUNTS) {
                idx = g_accountCount++;
                memset(&g_accounts[idx], 0, sizeof(g_accounts[idx]));
                copyBoundedString(g_accounts[idx].accountId, sizeof(g_accounts[idx].accountId), aid, strlen(aid));
                g_accounts[idx].isActive = true;
            }
            if (idx >= 0) {
                if (nick[0]) {
                    strncpy(g_accounts[idx].accountName, nick, sizeof(g_accounts[idx].accountName) - 1);
                    g_accounts[idx].accountName[sizeof(g_accounts[idx].accountName) - 1] = '\0';
                }
                g_accounts[idx].isActive = active;
            }
        }
        p = oe + 1;
    }
}

bool isValidAccountIdHex(const char *text) {
    return strlen(text) == 32 && isHexString(text, 32);
}

bool parseAccountIdHex(const char *text, AccountUid *out) {
    if (!isValidAccountIdHex(text)) return false;

    memset(out, 0, sizeof(*out));
    for (int word = 0; word < 2; word++) {
        u64 value = 0;
        for (int i = 0; i < 16; i++) {
            char c = text[word * 16 + i];
            u8 n;
            if (c >= '0' && c <= '9') n = (u8)(c - '0');
            else if (c >= 'a' && c <= 'f') n = (u8)(c - 'a' + 10);
            else n = (u8)(c - 'A' + 10);
            value = (value << 4) | n;
        }
        out->uid[word] = value;
    }
    return true;
}

//---------------------------------------------------------------------------------
// §16 account_total_limit_rules 解析(只读展示)
//
//   "account_total_limit_rules": { "<account_id>": { "daily_seconds": 3600,
//                                                     "weekday_seconds": [] | [7 个值] } }
//
// §15 语义(与 limit_rules 共用同一条判定): 恰好 7 个值 = 按星期已配置(0 生效 =
// 当天封禁); []/缺失/长度不对 = 未配置, 回退 daily_seconds。
// 字段缺失 / 账号 id 非法 / 值不是对象 / daily 非数字 -> 一律按未配置跳过, 不崩溃。
// 前台工具不执行限额, 这里只解析用于显示。
//---------------------------------------------------------------------------------
void parseAccountTotalRules(const char *body, AccountTotalRecord *records, int *count, int maxCount) {
    static char table[8192];
    if (!extractJsonObjectField(body, "account_total_limit_rules", table, sizeof(table))) return;

    const char *p = table;
    while (*p) {
        const char *keyStart = strchr(p, '"');
        if (!keyStart) break;
        keyStart++;
        const char *keyEnd = strchr(keyStart, '"');
        if (!keyEnd) break;

        if ((size_t)(keyEnd - keyStart) != 32) { p = keyEnd + 1; continue; }
        char accountId[33];
        copyBoundedString(accountId, sizeof(accountId), keyStart, (size_t)(keyEnd - keyStart));
        if (!isValidAccountIdHex(accountId)) { p = keyEnd + 1; continue; }

        const char *colon = strchr(keyEnd + 1, ':');
        if (!colon) break;
        const char *value = colon + 1;
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
        if (*value != '{') { p = keyEnd + 1; continue; }

        // 隔离这一个账号的对象再解析, 否则会命中别的账号的字段。
        char ruleObject[256];
        const char *after = copyJsonObjectAt(value, ruleObject, sizeof(ruleObject));
        if (!after) break;

        // 先把两个字段解析到局部变量: 都不可用时整条跳过, 绝不因为"读不懂"就显示
        // "today 0 min (blocked)" —— 那是在屏幕上凭空造出一条封禁。
        u32 daily = 0;
        bool haveDaily = readJsonU32FieldMax(ruleObject, "daily_seconds", WEEKDAY_SECONDS_MAX, &daily);
        u32 weekday[WEEKDAY_COUNT];
        bool hasWeekday = parseWeekdaySeconds(ruleObject, weekday);
        if (!haveDaily && !hasWeekday) { p = after; continue; }

        AccountTotalRecord *record = NULL;
        for (int i = 0; i < *count; i++) {
            if (strcmp(records[i].accountId, accountId) == 0) { record = &records[i]; break; }
        }
        if (!record) {
            if (*count >= maxCount) break;
            record = &records[*count];
            memset(record, 0, sizeof(*record));
            copyBoundedString(record->accountId, sizeof(record->accountId), accountId, strlen(accountId));
            (*count)++;
        }

        record->dailySeconds = daily;
        memcpy(record->weekdaySeconds, weekday, sizeof(record->weekdaySeconds));
        // §15: 数组恰好 7 个值才算"按星期已配置"; 否则 hasWeekday=false 并回退每日值。
        record->hasWeekday = hasWeekday;

        p = after;
    }
}

/// §16 从 sdmc:/PlayLog/limits.json(服务端下发的完整配置)读取账号合计限额。
/// 读失败/文件不存在 -> 清空列表(只影响展示, 不影响任何执行逻辑)。
void loadAccountTotalLimits(void) {
    g_accountTotalCount = 0;
    char body[8192];
    bool exists = false;
    if (!readWholeFile(LIMITS_PATH, body, sizeof(body), &exists, NULL)) return;
    parseAccountTotalRules(body, g_accountTotals, &g_accountTotalCount, MAX_ACCOUNTS);
}

/* account_limit_rules: { "<account>": { "<title>": {daily_seconds, weekly_seconds, weekday_seconds} } } */
void parseAccountGameRules(const char *body, AccountGameLimitRecord *records, int *count, int maxCount) {
    static char table[8192];
    if (!extractJsonObjectField(body, "account_limit_rules", table, sizeof(table))) return;

    const char *p = table;
    while (*p) {
        const char *keyStart = strchr(p, '"');
        if (!keyStart) break;
        keyStart++;
        const char *keyEnd = strchr(keyStart, '"');
        if (!keyEnd) break;
        if ((size_t)(keyEnd - keyStart) != 32) { p = keyEnd + 1; continue; }
        char accountId[33];
        copyBoundedString(accountId, sizeof(accountId), keyStart, (size_t)(keyEnd - keyStart));
        if (!isValidAccountIdHex(accountId)) { p = keyEnd + 1; continue; }

        const char *colon = strchr(keyEnd + 1, ':');
        if (!colon) break;
        const char *value = colon + 1;
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
        if (*value != '{') { p = keyEnd + 1; continue; }

        char acctObj[4096];
        const char *after = copyJsonObjectAt(value, acctObj, sizeof(acctObj));
        if (!after) break;

        const char *q = acctObj;
        while (*q) {
            const char *tks = strchr(q, '"');
            if (!tks) break;
            tks++;
            const char *tke = strchr(tks, '"');
            if (!tke) break;
            u64 titleId;
            if (!parseTitleIdString(tks, (size_t)(tke - tks), &titleId)) { q = tke + 1; continue; }

            const char *c2 = strchr(tke + 1, ':');
            if (!c2) break;
            const char *v2 = c2 + 1;
            while (*v2 == ' ' || *v2 == '\t' || *v2 == '\r' || *v2 == '\n') v2++;
            if (*v2 != '{') { q = tke + 1; continue; }

            char ruleObj[256];
            const char *after2 = copyJsonObjectAt(v2, ruleObj, sizeof(ruleObj));
            if (!after2) break;

            u32 daily = 0, weekly = 0;
            bool haveDaily = readJsonU32FieldMax(ruleObj, "daily_seconds", WEEKDAY_SECONDS_MAX, &daily);
            readJsonU32FieldMax(ruleObj, "weekly_seconds", WEEKLY_SECONDS_MAX, &weekly);
            u32 weekday[WEEKDAY_COUNT];
            bool hasWeekday = parseWeekdaySeconds(ruleObj, weekday);
            if (haveDaily || hasWeekday) {
                AccountGameLimitRecord *rec = NULL;
                for (int i = 0; i < *count; i++) {
                    if (strcmp(records[i].accountId, accountId) == 0 && records[i].titleId == titleId) {
                        rec = &records[i];
                        break;
                    }
                }
                if (!rec) {
                    if (*count >= maxCount) break;
                    rec = &records[(*count)++];
                    memset(rec, 0, sizeof(*rec));
                    copyBoundedString(rec->accountId, sizeof(rec->accountId), accountId, strlen(accountId));
                    rec->titleId = titleId;
                }
                rec->dailySeconds = daily;
                rec->weeklySeconds = weekly;
                memcpy(rec->weekdaySeconds, weekday, sizeof(rec->weekdaySeconds));
                rec->hasWeekday = hasWeekday;
            }
            q = after2;
        }
        p = after;
    }
}

void loadAccountGameLimits(void) {
    g_accountGameLimitCount = 0;
    char body[8192];
    bool exists = false;
    if (!readWholeFile(LIMITS_PATH, body, sizeof(body), &exists, NULL)) return;
    parseAccountGameRules(body, g_accountGameLimits, &g_accountGameLimitCount, MAX_ACCOUNT_GAME_LIMITS);
}

// 枚举主机上的用户账号与昵称。失败时账号列表为空,调用方退回设备级上报。
void loadAccounts(void) {
    g_accountCount = 0;
    if (!g_accountReady) return;

    AccountUid uids[MAX_ACCOUNTS];
    s32 total = 0;
    memset(uids, 0, sizeof(uids));

    Result rc = accountListAllUsers(uids, MAX_ACCOUNTS, &total);
    if (R_FAILED(rc)) {
        printf(tr("[账号] 枚举账号失败: 0x%x\n"), rc);
        return;
    }
    if (total > MAX_ACCOUNTS) total = MAX_ACCOUNTS;
    if (total < 0) total = 0;

    for (s32 i = 0; i < total; i++) {
        if (!accountUidIsValid(&uids[i])) continue;

        SwitchAccountInfo *info = &g_accounts[g_accountCount];
        accountUidToHex(&uids[i], info->accountId, sizeof(info->accountId));
        info->accountName[0] = '\0';
        info->isActive = true;

        AccountProfile profile;
        memset(&profile, 0, sizeof(profile));
        Result prc = accountGetProfile(&profile, uids[i]);
        if (R_SUCCEEDED(prc)) {
            AccountProfileBase base;
            memset(&base, 0, sizeof(base));
            if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &base))) {
                strncpy(info->accountName, base.nickname, sizeof(info->accountName) - 1);
                info->accountName[sizeof(info->accountName) - 1] = '\0';
            }
            accountProfileClose(&profile);
        }
        g_accountCount++;
    }

    printf(tr("[账号] 找到 %d 个账号\n"), g_accountCount);
    for (int i = 0; i < g_accountCount; i++) {
        // §13: 昵称可能是中文, 屏幕只画 ASCII(上报到服务端时仍用原始 UTF-8)。
        printf("  %s  %s\n", g_accounts[i].accountId, g_accounts[i].accountName);
    }
    uiUpdate();
}

// 查询某账号在某游戏上的累计时长(与设备级 pdmqryQueryPlayStatisticsByApplicationId 口径不同)。
Result queryAccountPlayStatistics(u64 titleId, const char *accountIdHex, s64 *outSeconds, s32 *outLaunches) {
    AccountUid uid;
    if (!parseAccountIdHex(accountIdHex, &uid))
        return -1;

    PdmPlayStatistics stats;
    memset(&stats, 0, sizeof(stats));
    Result rc = pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(titleId, uid, false, &stats);
    if (R_SUCCEEDED(rc)) {
        if (outSeconds) *outSeconds = stats.playtime / 1000000000;
        if (outLaunches) *outLaunches = stats.total_launches;
    }
    return rc;
}

// 采集所有 (账号, 游戏) 的累计时长。返回条目数(0 表示账号维度不可用)。
int collectAccountGames(AccountGameRecord *out, int maxCount) {
    if (g_accountCount <= 0) return 0;

    int count = 0;
    for (int a = 0; a < g_accountCount && count < maxCount; a++) {
        for (int g = 0; g < g_gameCount && count < maxCount; g++) {
            s64 seconds = 0;
            Result rc = queryAccountPlayStatistics(g_games[g].titleId, g_accounts[a].accountId, &seconds, NULL);
            if (R_FAILED(rc)) continue;
            if (seconds <= 0) continue;

            AccountGameRecord *rec = &out[count];
            copyBoundedString(rec->accountId, sizeof(rec->accountId),
                              g_accounts[a].accountId, sizeof(g_accounts[a].accountId));
            rec->titleId = g_games[g].titleId;
            rec->totalPlaytimeSeconds = seconds;
            count++;
        }
        uiUpdate();
    }
    return count;
}

//---------------------------------------------------------------------------------
// 快照相关(本地CSV记录用,不影响上报去重)
// 写入一律走 temp + rename(item 15)。
//---------------------------------------------------------------------------------

// 小型的带边界检查的缓冲写入器: 一旦写满就置 overflow,由调用方放弃写入。
typedef struct {
    char *buf;
    size_t capacity;
    size_t used;
    bool overflow;
} CsvBuilder;

void csv_reset(CsvBuilder *b, char *buf, size_t capacity) {
    b->buf = buf;
    b->capacity = capacity;
    b->used = 0;
    b->overflow = false;
    if (capacity > 0) buf[0] = '\0';
}

void csv_appendf(CsvBuilder *b, const char *fmt, ...) {
    if (b->overflow || b->used >= b->capacity) {
        b->overflow = true;
        return;
    }

    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(b->buf + b->used, b->capacity - b->used, fmt, args);
    va_end(args);

    if (needed < 0 || (size_t)needed >= b->capacity - b->used) {
        b->overflow = true;
        return;
    }
    b->used += (size_t)needed;
}

// 解析一行 CSV。返回字段数。字段内不含逗号(写出的字段已过滤)。
int splitCsvLine(char *line, char **fields, int maxFields) {
    int count = 0;
    char *p = line;
    while (count < maxFields) {
        fields[count++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }
    for (int i = 0; i < count; i++) {
        size_t len = strlen(fields[i]);
        while (len > 0 && (fields[i][len - 1] == '\n' || fields[i][len - 1] == '\r'))
            fields[i][--len] = '\0';
    }
    return count;
}

int loadSnapshot(SnapshotRecord *out, int maxCount) {
    char body[65536];
    bool exists = false;
    if (!readWholeFile(SNAPSHOT_PATH, body, sizeof(body), &exists, NULL)) {
        if (exists) printf("[快照] " SNAPSHOT_PATH " 无法读取，按空处理。\n");
        return 0;
    }

    int count = 0;
    char *saveptr = NULL;
    char *line = strtok_r(body, "\n", &saveptr);
    while (line && count < maxCount) {
        u64 tid = 0;
        long long playtime = 0;
        if (sscanf(line, "%lX,%lld", &tid, &playtime) == 2) {
            out[count].titleId = tid;
            out[count].snapshotPlaytimeSeconds = (s64)playtime;
            count++;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return count;
}

s64 findSnapshotValue(SnapshotRecord *snap, int count, u64 titleId) {
    for (int i = 0; i < count; i++) {
        if (snap[i].titleId == titleId) return snap[i].snapshotPlaytimeSeconds;
    }
    return 0;
}

void saveSnapshot(GameRecord *games, int count) {
    static char body[65536];
    CsvBuilder b;
    csv_reset(&b, body, sizeof(body));
    csv_appendf(&b, "TitleID,累计秒数\n");
    for (int i = 0; i < count; i++)
        csv_appendf(&b, "%016lX,%lld\n", games[i].titleId, (long long)games[i].totalPlaytimeSeconds);

    if (b.overflow) {
        printf("[快照] " SNAPSHOT_PATH " 过大，保留旧文件。\n");
        return;
    }
    writeFileAtomic(SNAPSHOT_PATH, body, b.used);
}

// (账号, 游戏) -> 上次已上报的累计秒数
int loadAccountSnapshot(AccountSnapshotRecord *out, int maxCount) {
    char body[131072];
    bool exists = false;
    if (!readWholeFile(ACCOUNT_SNAPSHOT_PATH, body, sizeof(body), &exists, NULL)) {
        if (exists) printf("[快照] " ACCOUNT_SNAPSHOT_PATH " 无法读取，按空处理。\n");
        return 0;
    }

    int count = 0;
    char *saveptr = NULL;
    char *line = strtok_r(body, "\n", &saveptr);
    while (line && count < maxCount) {
        char *fields[4];
        int n = splitCsvLine(line, fields, 4);
        if (n >= 3 && isValidAccountIdHex(fields[0])) {
            u64 tid = 0;
            if (parseTitleIdString(fields[1], strlen(fields[1]), &tid)) {
                copyBoundedString(out[count].accountId, sizeof(out[count].accountId), fields[0], 32);
                out[count].titleId = tid;
                out[count].snapshotPlaytimeSeconds = (s64)strtoll(fields[2], NULL, 10);
                count++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return count;
}

s64 findAccountSnapshotValue(AccountSnapshotRecord *snap, int count, const char *accountId, u64 titleId) {
    for (int i = 0; i < count; i++) {
        if (snap[i].titleId == titleId && accountHexEquals(snap[i].accountId, accountId))
            return snap[i].snapshotPlaytimeSeconds;
    }
    return 0;
}

void saveAccountSnapshot(AccountGameRecord *games, int count) {
    static char body[131072];
    CsvBuilder b;
    csv_reset(&b, body, sizeof(body));
    csv_appendf(&b, "account_id,TitleID,累计秒数\n");
    for (int i = 0; i < count; i++)
        csv_appendf(&b, "%s,%016lX,%lld\n",
                    games[i].accountId, games[i].titleId,
                    (long long)games[i].totalPlaytimeSeconds);

    if (b.overflow) {
        printf("[快照] " ACCOUNT_SNAPSHOT_PATH " 过大，保留旧文件。\n");
        return;
    }
    writeFileAtomic(ACCOUNT_SNAPSHOT_PATH, body, b.used);
}

void getLocalTag(char *out, size_t size) {
    // 仅用于本地文件命名,系统时钟不可靠,只是给人看的参考标签
    time_t now;
    time(&now);
    struct tm *t = localtime(&now);
    snprintf(out, size, "%04d-%02d-%02d_%02d%02d%02d",
              t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
              t->tm_hour, t->tm_min, t->tm_sec);
}

void getIsoTimestamp(time_t t, char *out, size_t size) {
    struct tm *tm_ = localtime(&t);
    snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02d",
              tm_->tm_year + 1900, tm_->tm_mon + 1, tm_->tm_mday,
              tm_->tm_hour, tm_->tm_min, tm_->tm_sec);
}

// 本地留档,方便你自己在SD卡上翻查,不参与服务端去重逻辑
void appendLocalLog(GameRecord *games, SnapshotRecord *snap, int snapCount, int gameCount, s64 *outDelta) {
    mkdir(PLAYLOG_DIR, 0777);
    mkdir(PLAYLOG_DIR "/daily", 0777);

    char tag[32];
    getLocalTag(tag, sizeof(tag));

    char path[256];
    snprintf(path, sizeof(path), PLAYLOG_DIR "/daily/run_%s.csv", tag);

    FILE *f = fopen(path, "w");
    s64 dailyTotalDelta = 0;
    if (f) {
        fprintf(f, "%s\n", CSV_DAILY_HEADER);
        for (int i = 0; i < gameCount; i++) {
            s64 prevValue = findSnapshotValue(snap, snapCount, games[i].titleId);
            s64 delta = games[i].totalPlaytimeSeconds - prevValue;
            if (delta <= 0) continue;
            fprintf(f, "%s,%016lX,%ld,%.1f\n", games[i].name, games[i].titleId, delta, delta / 60.0);
            dailyTotalDelta += delta;
        }
        fclose(f);
    } else {
        // §12.3: 写盘失败也要进接口日志(带目标路径)。
        apiLogLine(API_LOG_TAG_WRITE, "WRITE", path, 0, NULL, "写盘失败");
    }
    *outDelta = dailyTotalDelta;
}

//---------------------------------------------------------------------------------
// 推送相关: 生成待推送批次 -> 先落盘(pending) -> 尝试发送 -> 成功则删除pending
//---------------------------------------------------------------------------------
void logPushError(const char *msg) {
    mkdir(PLAYLOG_DIR, 0777);
    FILE *ferr = fopen(PLAYLOG_DIR "/push_errors.log", "a");
    if (ferr) {
        char tag[32];
        getLocalTag(tag, sizeof(tag));
        fprintf(ferr, "[%s] %s\n", tag, msg);
        fclose(ferr);
    }
}

//---------------------------------------------------------------------------------
// 构建本次增量的 JSON body。
//
// 账号维度(REFACTOR_SPEC §2.1 / §5):
//   - 账号维度可用时,按 (账号, 游戏) 的累计增量上报,附带 account_id/account_name;
//   - 某款游戏一条账号条目都没有(账号维度不可用,或所有账号都没玩过它)时,
//     仍按设备级上报,与改动前行为一致;
//   - 同一款游戏不会既报账号条目又报设备条目,避免服务端重复计费。
//
// 截断保护(item 19): 全过程按 snprintf 返回值做边界校验,任何一条放不下就整体放弃,
// 绝不上传半截 JSON。
//---------------------------------------------------------------------------------
#define MAX_BATCH_ENTRIES (MAX_ACCOUNT_GAMES + MAX_GAMES)

typedef struct {
    int accountIndex;  // -1 表示设备级条目
    int gameIndex;
} BatchEntryRef;

static BatchEntryRef g_batchEntries[MAX_BATCH_ENTRIES];

int buildBatchJson(GameRecord *games, int gameCount,
                   AccountGameRecord *accountGames, int accountGameCount,
                   SnapshotRecord *snap, int snapCount,
                   AccountSnapshotRecord *acctSnap, int acctSnapCount,
                   char *outBody, size_t bodySize, size_t *outLen) {
    time_t now;
    time(&now);

    if (bodySize == 0) return 0;

    int pn = snprintf(outBody, bodySize,
             "{\"device_id\":\"%s\",\"device_id_type\":\"%s\",\"games\":[",
             g_config.deviceId, g_config.deviceIdType);
    if (pn < 0 || (size_t)pn >= bodySize) {
        printf(tr("[推送] 设备标识过长, 已取消上传。\n"));
        return 0;
    }
    size_t used = (size_t)pn;

    // 被账号条目覆盖的设备级游戏: 不再生成设备级条目,避免服务端重复计费。
    bool covered[MAX_GAMES];
    memset(covered, 0, sizeof(covered));

    int entryCount = 0;
    for (int a = 0; a < accountGameCount && entryCount < MAX_BATCH_ENTRIES; a++) {
        int gi = -1;
        for (int i = 0; i < gameCount; i++) {
            if (games[i].titleId == accountGames[a].titleId) { gi = i; break; }
        }
        if (gi < 0) continue;

        s64 prev = findAccountSnapshotValue(acctSnap, acctSnapCount, accountGames[a].accountId, accountGames[a].titleId);
        if (accountGames[a].totalPlaytimeSeconds <= prev) continue;

        int ai = -1;
        for (int k = 0; k < g_accountCount; k++) {
            if (accountHexEquals(g_accounts[k].accountId, accountGames[a].accountId)) { ai = k; break; }
        }
        if (ai < 0) continue;

        g_batchEntries[entryCount].accountIndex = ai;
        g_batchEntries[entryCount].gameIndex = gi;
        entryCount++;
        covered[gi] = true;
    }

    for (int i = 0; i < gameCount && entryCount < MAX_BATCH_ENTRIES; i++) {
        if (covered[i]) continue;
        s64 delta = games[i].totalPlaytimeSeconds - findSnapshotValue(snap, snapCount, games[i].titleId);
        if (delta <= 0) continue;

        g_batchEntries[entryCount].accountIndex = -1;
        g_batchEntries[entryCount].gameIndex = i;
        entryCount++;
    }

    int emitted = 0;
    for (int e = 0; e < entryCount; e++) {
        int ai = g_batchEntries[e].accountIndex;
        int gi = g_batchEntries[e].gameIndex;
        u64 titleId = games[gi].titleId;

        s64 delta;
        const char *accountId = "";
        const char *accountName = "";
        if (ai >= 0) {
            s64 prev = findAccountSnapshotValue(acctSnap, acctSnapCount, g_accounts[ai].accountId, titleId);
            delta = -1;
            for (int a = 0; a < accountGameCount; a++) {
                if (accountGames[a].titleId == titleId &&
                    accountHexEquals(accountGames[a].accountId, g_accounts[ai].accountId)) {
                    delta = accountGames[a].totalPlaytimeSeconds - prev;
                    break;
                }
            }
            accountId = g_accounts[ai].accountId;
            accountName = g_accounts[ai].accountName;
        } else {
            delta = games[gi].totalPlaytimeSeconds - findSnapshotValue(snap, snapCount, titleId);
        }
        if (delta <= 0) continue;

        char uuid[40];
        generateUuidString(uuid, sizeof(uuid));

        char safeName[0x300];
        char safeAccountName[0x60];
        escapeJsonString(games[gi].name, safeName, sizeof(safeName));
        escapeJsonString(accountName, safeAccountName, sizeof(safeAccountName));

        char startedIso[32], endedIso[32];
        getIsoTimestamp(now - delta, startedIso, sizeof(startedIso));
        getIsoTimestamp(now, endedIso, sizeof(endedIso));

        char entry[1400];
        int rn;
        // §8: 标记来源。前台 NRO 的数字来自 PDM 系统统计, 服务端据此与
        // sysmodule 自身的计时去重(对每个 (game, 本地日) 取各来源 max)。
        if (accountId[0]) {
            rn = snprintf(entry, sizeof(entry),
                "%s{\"record_uid\":\"%s\",\"title_id\":\"%016lX\",\"name\":\"%s\","
                "\"source\":\"" UPLOAD_SOURCE_NRO_PDM "\","
                "\"account_id\":\"%s\",\"account_name\":\"%s\","
                "\"delta_seconds\":%ld,\"started_at\":\"%s\",\"ended_at\":\"%s\"}",
                emitted == 0 ? "" : ",", uuid, titleId, safeName, accountId, safeAccountName,
                (long)delta, startedIso, endedIso);
        } else {
            rn = snprintf(entry, sizeof(entry),
                "%s{\"record_uid\":\"%s\",\"title_id\":\"%016lX\",\"name\":\"%s\","
                "\"source\":\"" UPLOAD_SOURCE_NRO_PDM "\","
                "\"delta_seconds\":%ld,\"started_at\":\"%s\",\"ended_at\":\"%s\"}",
                emitted == 0 ? "" : ",", uuid, titleId, safeName,
                (long)delta, startedIso, endedIso);
        }
        if (rn < 0 || (size_t)rn >= sizeof(entry)) {
            printf(tr("[推送] 单条记录过长, 已丢弃整批。\n"));
            logPushError("entry JSON would be truncated; batch discarded");
            return 0;
        }

        if (used + (size_t)rn + 3 > bodySize) {
            printf(tr("[推送] 上传内容会被截断, 已丢弃整批。\n"));
            logPushError("upload body truncated; batch discarded");
            return 0;
        }

        memcpy(outBody + used, entry, (size_t)rn);
        used += (size_t)rn;
        outBody[used] = '\0';
        emitted++;
    }

    if (emitted == 0)
        return 0;

    memcpy(outBody + used, "]}", 2);
    used += 2;
    outBody[used] = '\0';

    if (outLen) *outLen = used;
    return emitted;
}

void savePendingPush(const char *batchId, const char *body, size_t bodyLen) {
    mkdir(PLAYLOG_DIR, 0777);
    mkdir(PLAYLOG_DIR "/pending", 0777);
    char path[256];
    snprintf(path, sizeof(path), PLAYLOG_DIR "/pending/%s.json", batchId);
    if (!writeFileAtomic(path, body, bodyLen))
        printf(tr("[推送] 待上传批次保存失败 %s\n"), batchId);
}

void deletePendingPush(const char *batchId) {
    char path[256];
    snprintf(path, sizeof(path), PLAYLOG_DIR "/pending/%s.json", batchId);
    remove(path);
}

// 实际发送,成功(2xx)返回true
bool sendPushBody(const char *body) {
    if (!g_config.uploadToken[0]) {
        printf(tr("[推送] 未配置上传 Token。\n"));
        logPushError("upload token is not configured");
        return false;
    }

    char timestamp[32];
    if (!getCurrentUnixTimestamp(timestamp, sizeof(timestamp))) {
        printf(tr("[推送] 无法读取系统时间用于签名。\n"));
        logPushError("failed to read system time for signature");
        return false;
    }

    char signature[SHA256_HASH_SIZE * 2 + 1];
    buildRequestSignature("POST", PUSH_PATH, body, timestamp, signature, sizeof(signature));

    CURL *curl = curl_easy_init();
    if (!curl) {
        printf(tr("[推送] curl 初始化失败\n"));
        logPushError("curl_easy_init failed");
        return false;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    appendSignedHeaders(&headers, "POST", PUSH_PATH, body, timestamp, signature);

    if (!cainfoAvailable()) {
        printf(tr("[推送] 未配置 TLS 校验, 已中止上传。\n"));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return false;
    }

    char url[320];
    buildEndpointUrl(PUSH_PATH, url, sizeof(url));
    // §12.2: 失败响应体里带 detail, 需要收集(上限 MAX_RESPONSE_SIZE)。
    MemoryBuffer response = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    configureTlsVerification(curl);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    bool ok = false;
    if (res != CURLE_OK) {
        printf(tr("[推送] 失败: %s\n"), curl_easy_strerror(res));
        char msg[256];
        snprintf(msg, sizeof(msg), "curl error: %s", curl_easy_strerror(res));
        logPushError(msg);
        apiLogLine(API_LOG_TAG_NET, "POST", PUSH_PATH, 0, NULL, curl_easy_strerror(res));
    } else if (httpCode >= 200 && httpCode < 300) {
        printf(tr("[推送] 成功, HTTP %ld\n"), httpCode);
        apiLogLine(API_LOG_TAG_PUSH, "POST", PUSH_PATH, httpCode, NULL, "OK");
        ok = true;
    } else {
        // §13: 屏幕只打印 ASCII 的 `code` + `hint`(中文 detail 只写日志文件)。
        char reason[512];
        char code[API_LOG_CODE_MAX];
        extractServerFailureDetail(response.data, httpCode, reason, sizeof(reason));
        extractServerFailureCode(response.data, code, sizeof(code));
        printServerFailure(tr("[推送] 服务端返回"), httpCode, response.data);
        logPushError(reason);
        apiLogLine(API_LOG_TAG_PUSH, "POST", PUSH_PATH, httpCode, code, reason);
    }

    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

// 重试之前失败、还留在pending目录里的批次
void retryPendingPushes(void) {
    DIR *d = opendir(PLAYLOG_DIR "/pending");
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char path[320];
        snprintf(path, sizeof(path), PLAYLOG_DIR "/pending/%s", entry->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        char body[4096];
        size_t len = fread(body, 1, sizeof(body) - 1, f);
        body[len] = '\0';
        fclose(f);

        printf(tr("[推送] 重试待上传批次: %s\n"), entry->d_name);
        uiUpdate();

        if (sendPushBody(body)) {
            remove(path);
        }
    }
    closedir(d);
}

int scanInstalledGames(GameRecord **outGames, bool showProgress) {
    NsApplicationRecord records[MAX_GAMES];
    s32 outCount = 0;
    Result rc = nsListApplicationRecord(records, MAX_GAMES, 0, &outCount);
    if (R_FAILED(rc)) {
        if (showProgress) printf(tr("获取游戏列表失败: 0x%x\n"), rc);
        *outGames = NULL;
        return 0;
    }

    GameRecord *games = calloc(outCount > 0 ? outCount : 1, sizeof(GameRecord));
    if (!games) {
        *outGames = NULL;
        return 0;
    }

    int validCount = 0;
    if (showProgress) {
        printf(tr("找到 %d 个已安装游戏，正在扫描...\n\n"), outCount);
        uiUpdate();
    }

    for (s32 i = 0; i < outCount; i++) {
        u64 titleId = records[i].application_id;
        s64 playtime = 0;
        s32 launches = 0;

        Result r1 = queryPlayStatistics(titleId, &playtime, &launches);
        if (R_FAILED(r1)) continue;

        char name[0x201];
        char displayName[0x201];
        getGameName(titleId, name, sizeof(name), displayName, sizeof(displayName));

        games[validCount].titleId = titleId;
        games[validCount].totalPlaytimeSeconds = playtime;
        games[validCount].launchCount = launches;
        strncpy(games[validCount].name, name, sizeof(games[validCount].name) - 1);
        games[validCount].name[sizeof(games[validCount].name) - 1] = '\0';
        strncpy(games[validCount].displayName, displayName, sizeof(games[validCount].displayName) - 1);
        games[validCount].displayName[sizeof(games[validCount].displayName) - 1] = '\0';
        validCount++;

        if (showProgress) {
            printf(tr("%s: %.1f 小时 (%d 次启动)\n"), name, playtime / 3600.0, launches);
            uiUpdate();
        }
    }

    *outGames = games;
    return validCount;
}

void runReportAndPush(void) {
    uiClear();
    printf(tr("正在生成报告并推送...\n"));
    printf(tr("服务器: %s\n\n"), g_config.serverBaseUrl);
    uiUpdate();

    retryPendingPushes();

    GameRecord *games = NULL;
    int validCount = scanInstalledGames(&games, true);
    if (!games) {
        printf(tr("内存不足。\n"));
        uiUpdate();
        return;
    }

    // 把扫描结果暴露给账号采集逻辑使用。
    g_games = games;
    g_gameCount = validCount;

    mkdir(PLAYLOG_DIR, 0777);
    FILE *f = fopen(PLAYLOG_DIR "/report.csv", "w");
    if (f) {
        fprintf(f, "%s\n", CSV_REPORT_HEADER);
        for (int i = 0; i < validCount; i++) {
            fprintf(f, "%016lX,%s,%.2f,%d\n",
                    games[i].titleId, games[i].name,
                    games[i].totalPlaytimeSeconds / 3600.0,
                    games[i].launchCount);
        }
        fclose(f);
        printf("\n报告已保存: " PLAYLOG_DIR "/report.csv\n");
    } else {
        // §12.3: 写盘失败也要进接口日志(带目标路径)。
        apiLogLine(API_LOG_TAG_WRITE, "WRITE", PLAYLOG_DIR "/report.csv", 0, NULL, "写盘失败");
    }

    SnapshotRecord snap[MAX_GAMES];
    int snapCount = loadSnapshot(snap, MAX_GAMES);

    s64 localDelta = 0;
    appendLocalLog(games, snap, snapCount, validCount, &localDelta);
    printf(tr("本次新增游玩时长: %.1f 分钟\n"), localDelta / 60.0);
    uiUpdate();

    // 账号维度(REFACTOR_SPEC §5): 账号枚举/查询失败时条目数为 0,
    // buildBatchJson 会自动退回设备级上报,行为与改动前一致。
    static AccountGameRecord accountGames[MAX_ACCOUNT_GAMES];
    int accountGameCount = 0;
    if (g_accountCount > 0) {
        printf(tr("正在统计 %d 个账号的游玩时长...\n"), g_accountCount);
        uiUpdate();
        accountGameCount = collectAccountGames(accountGames, MAX_ACCOUNT_GAMES);
        printf(tr("按账号条目: %d\n"), accountGameCount);
        uiUpdate();
    }

    static AccountSnapshotRecord acctSnap[MAX_ACCOUNT_GAMES];
    int acctSnapCount = loadAccountSnapshot(acctSnap, MAX_ACCOUNT_GAMES);

    // body 放在 static 区,避免 64KB 的批次体占用主线程栈。
    static char body[65536];
    size_t bodyLen = 0;
    int entryCount = buildBatchJson(games, validCount, accountGames, accountGameCount,
                                    snap, snapCount, acctSnap, acctSnapCount,
                                    body, sizeof(body), &bodyLen);

    // 只有"增量确实已经交给服务端"(发送成功,或本来就没有增量)时才推进快照基线。
    // 发送失败会把批次留在 pending 目录里下次重试,基线必须保持不变,
    // 否则重试成功后下一轮会把同一段时长再算一遍。
    bool snapshotCommitted = true;
    char batchPath[256];
    batchPath[0] = '\0';

    if (entryCount > 0) {
        char batchId[40];
        generateUuidString(batchId, sizeof(batchId));
        snprintf(batchPath, sizeof(batchPath), PLAYLOG_DIR "/pending/%s.json", batchId);
        savePendingPush(batchId, body, bodyLen);

        if (sendPushBody(body)) {
            deletePendingPush(batchId);
        } else {
            snapshotCommitted = false;
            printf(tr("[推送] 已保留在待上传队列, 下次重试。\n"));
        }
    } else {
        printf(tr("[推送] 没有新增游玩时长, 跳过推送。\n"));
    }
    uiUpdate();

    if (!snapshotCommitted) {
        FILE *pending = fopen(batchPath, "rb");
        if (!pending) {
            // 批次文件不存在(写失败等): 没有可重试的数据,允许推进基线,
            // 否则这段时长会被永久重复计算。
            snapshotCommitted = true;
        } else {
            fclose(pending);
        }
    }

    if (snapshotCommitted) {
        saveSnapshot(games, validCount);
        saveAccountSnapshot(accountGames, accountGameCount);
    } else {
        printf(tr("[推送] 快照未推进(待上传批次保留以便重试)。\n"));
    }

    free(games);
    g_games = NULL;
    g_gameCount = 0;

    printf(tr("\n按 B 返回主菜单。\n"));
    uiUpdate();
}

/// §16 账号级"全部游戏合计"限额的只读展示(纯 ASCII, §13)。
/// 每个配置了合计限额的账号打印一行; 账号不止一个时带上昵称/id, 避免多行无法区分。
/// 前台工具**不执行**该限额, 这里只显示今日生效值; 真正拦截在 sysmodule。
static int draw_week_table(int x, int y, int w, const char *title,
                           u32 daily, u32 weekly, const u32 *wd,
                           bool hasWeekday, bool hasRule)
{
    static const char *dayLabels[WEEKDAY_COUNT] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    uiTextAt(x, y, w, title, UI_ACCENT);
    if (weekly > 0) {
        char rb[64];
        snprintf(rb, sizeof(rb), tr("周总 %.1f 小时"), weekly / 3600.0);
        int bw = uiTextW(rb, UI_DIM);
        if (bw < w)
            uiTextAt(x + w - bw, y, bw + 4, rb, UI_DIM);
    }
    int cw = w / WEEKDAY_COUNT;
    int hy = y + 24;
    for (int d = 0; d < WEEKDAY_COUNT; d++) {
        int lw = uiTextW(dayLabels[d], UI_DIM);
        uiTextAt(x + d * cw + (cw - lw) / 2, hy, cw, tr(dayLabels[d]), UI_DIM);
    }
    int vy = hy + 24;
    for (int d = 0; d < WEEKDAY_COUNT; d++) {
        if (!hasRule) {
            uiTextAt(x + d * cw, vy, cw, tr("未设置"), UI_DIM);
            continue;
        }
        u32 v = hasWeekday ? wd[d] : daily;
        if (v == 0) {
            uiTextAt(x + d * cw, vy, cw, tr("禁止"), UI_BAD);
        } else {
            char b[32];
            snprintf(b, sizeof(b), "%u", v / 60);
            uiTextAt(x + d * cw, vy, cw, b, UI_GOOD);
        }
    }
    return vy + 28;
}

void drawMenu(GameRecord *games, int gameCount, int selected, LimitRecord *limits, int limitCount)
{
    const char *mode;
    if (g_online)
        mode = tr("在线（只读）");
    else if (g_allow_offline_edit)
        mode = tr("离线（可编辑）");
    else
        mode = tr("离线（只读 · 服务端禁止编辑）");
    uiFrameBegin(tr("PlayLog 家长控制台"), mode);
    if (!g_online && g_allow_offline_edit)
        uiSetFooter(tr("↑↓ 游戏  ←→ 账号  A 设置  Y 清除  ZL/ZR 翻页  R 推送  L 日志  - 同步  X 设置  + Token  B 退出"));
    else
        uiSetFooter(tr("↑↓ 游戏  ←→ 账号  ZL/ZR 翻页  R 推送  L 日志  - 同步  X 设置  + Token  B 退出"));

    const int LX = 24, GY = 64, LW = 760, GH = 360;
    const int SY = GY + 368, SH = 228;
    const int RX = 800, RW = 456;
    uiPanel(LX, GY, LW, GH);
    uiPanel(LX, SY, LW, SH);
    uiPanel(RX, GY, RW, 596);

    uiTextAt(LX + 16, GY + 10, 500, tr("游戏"), UI_ACCENT);
    uiTextAt(LX + 620, GY + 10, 120, tr("累计"), UI_ACCENT);
    uiFill(LX + 12, GY + 40, LW - 24, 1, UI_BORDER);

    if (gameCount <= 0) {
        uiTextAt(LX + 16, GY + 52, LW - 32, tr("没有找到游戏"), UI_DIM);
    } else {
        int rows = 8;
        int start = selected - rows / 2;
        if (start > gameCount - rows) start = gameCount - rows;
        if (start < 0) start = 0;
        int end = start + rows;
        if (end > gameCount) end = gameCount;

        for (int i = start; i < end; i++) {
            int y = GY + 50 + (i - start) * 32;
            if (i == selected)
                uiFill(LX + 8, y - 5, LW - 16, 30, UI_SELECTED);
            uiTextAt(LX + 16, y, 590, games[i].name, i == selected ? UI_TITLE : UI_NORMAL);
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f h", games[i].totalPlaytimeSeconds / 3600.0);
            uiTextAt(LX + 620, y, 120, buf, UI_DIM);
        }

        char page[64];
        snprintf(page, sizeof(page), "%d / %d", selected + 1, gameCount);
        uiTextAt(LX + LW - 150, GY + GH - 26, 130, page, UI_DIM);
    }

    uiTextAt(LX + 16, SY + 10, 300, tr("系统信息"), UI_ACCENT);
    uiFill(LX + 12, SY + 40, LW - 24, 1, UI_BORDER);
    {
        int sy = SY + 52;
        char b[256];
        uiTextAt(LX + 16, sy, 100, tr("设备编码"), UI_DIM);
        uiTextAt(LX + 110, sy, LW - 126, g_config.deviceId, UI_NORMAL);
        sy += 22;
        snprintf(b, sizeof(b), "Token  %s", g_config.uploadToken[0] ? tr("已配置") : tr("未配置"));
        uiTextAt(LX + 16, sy, LW - 32, b, UI_NORMAL);
        sy += 22;
        snprintf(b, sizeof(b), tr("服务器  %.70s"), g_config.serverBaseUrl);
        uiTextAt(LX + 16, sy, LW - 32, b, UI_NORMAL);
        sy += 22;
        snprintf(b, sizeof(b), tr("间隔  重载 %u / 同步 %u / 上传 %u / 心跳 %u"),
                 g_config.limitReloadSeconds, g_config.configSyncSeconds,
                 g_config.uploadIntervalSeconds, g_config.heartbeatIntervalSeconds);
        uiTextAt(LX + 16, sy, LW - 32, b, UI_NORMAL);
        sy += 22;
        snprintf(b, sizeof(b), tr("执行模式  %.19s"), g_config.enforceMode);
        uiTextAt(LX + 16, sy, LW - 32, b, UI_NORMAL);
        sy += 22;
        snprintf(b, sizeof(b), tr("离线编辑  %s"), g_allow_offline_edit ? tr("允许") : tr("禁止"));
        uiTextAt(LX + 16, sy, LW - 32, b, UI_NORMAL);
        sy += 22;
        {
            char nowbuf[32];
            format_server_now(nowbuf, sizeof(nowbuf));
            snprintf(b, sizeof(b), tr("服务端时间  %s"), nowbuf);
            uiTextAt(LX + 16, sy, LW - 32, b, UI_ACCENT);
            sy += 22;
        }
        if (g_accountCount > 0) {
            int ai = g_currentAccountIndex;
            if (ai < 0) ai = 0;
            if (ai >= g_accountCount) ai = g_accountCount - 1;
            snprintf(b, sizeof(b), tr("当前账号  %s"),
                     g_accounts[ai].accountName[0] ? g_accounts[ai].accountName : g_accounts[ai].accountId);
            uiTextAt(LX + 16, sy, LW - 32, b, UI_ACCENT);
        }
    }

    if (gameCount > 0) {
        GameRecord *cur = &games[selected];
        LimitRecord *rec = findLimitRecord(limits, limitCount, cur->titleId);
        char buf[160];

        uiTextAt(RX + 16, GY + 14, RW - 32, cur->name, UI_TITLE);
        snprintf(buf, sizeof(buf), "TitleID  %016lX", cur->titleId);
        uiTextAt(RX + 16, GY + 46, RW - 32, buf, UI_DIM);
        snprintf(buf, sizeof(buf), tr("累计 %.1f 小时    启动 %d 次"),
                 cur->totalPlaytimeSeconds / 3600.0, cur->launchCount);
        uiTextAt(RX + 16, GY + 72, RW - 32, buf, UI_NORMAL);

        const char *aid = NULL;
        if (g_accountCount > 0) {
            int ai = g_currentAccountIndex;
            if (ai < 0) ai = 0;
            if (ai >= g_accountCount) ai = g_accountCount - 1;
            aid = g_accounts[ai].accountId;
            snprintf(buf, sizeof(buf), tr("账号  %s   ←/→ 切换"),
                     g_accounts[ai].accountName[0] ? g_accounts[ai].accountName : g_accounts[ai].accountId);
            uiTextAt(RX + 16, GY + 96, RW - 32, buf, UI_ACCENT);
        } else {
            uiTextAt(RX + 16, GY + 96, RW - 32, tr("账号  （未加载）"), UI_BAD);
        }

        int ty = GY + 126;
        ty = draw_week_table(RX + 16, ty, RW - 32, tr("游戏限额（所有账号，各自独立计算）"),
                             rec ? rec->limitSeconds : 0,
                             rec ? rec->weeklySeconds : 0,
                             rec ? rec->weekdaySeconds : NULL,
                             rec && rec->hasWeekday,
                             rec && limitRecordHasRule(rec));
        uiFill(RX + 12, ty, RW - 24, 1, UI_BORDER);
        ty += 12;

        AccountGameLimitRecord *ag = findAccountGameLimit(aid, cur->titleId);
        ty = draw_week_table(RX + 16, ty, RW - 32, tr("账号 + 游戏限额（当前账号 · 这个游戏）"),
                             ag ? ag->dailySeconds : 0,
                             ag ? ag->weeklySeconds : 0,
                             ag ? ag->weekdaySeconds : NULL,
                             ag && ag->hasWeekday,
                             ag != NULL);
        uiFill(RX + 12, ty, RW - 24, 1, UI_BORDER);
        ty += 12;

        AccountTotalRecord *at = findAccountTotal(aid);
        draw_week_table(RX + 16, ty, RW - 32, tr("账号合计限额（当前账号 · 所有游戏）"),
                        at ? at->dailySeconds : 0,
                        0,
                        at ? at->weekdaySeconds : NULL,
                        at && at->hasWeekday,
                        at != NULL);
    }

    uiFrameEnd();
}

void waitForB(PadState *pad) {
    while (uiMainLoop()) {
        padUpdate(pad);
        if (padGetButtonsDown(pad) & HidNpadButton_B) break;
        uiUpdate();
    }
}

//---------------------------------------------------------------------------------
// §12.4 接口日志查看器(主菜单按 L 进入)
//
// 显示最新一天可用的 sdmc:/PlayLog/api-YYYYMMDD.log(今天没有就回退到今天-1、
// 今天-2 … 今天-40 里最近存在的一天)。
//
// 内存边界(有界读取, 绝不整份吃进内存):
//   - 最多读文件**尾部** 64 KB(更早的内容不载入, 界面上会说明);
//   - 最多保留最后 512 行(更早的行不载入, 界面上会说明);
//   - 一行最多显示 79 个字符, 避免长行折行把"一行一屏"的翻页搞乱。
//   读取失败/文件不存在/文件为空都只显示 "No log yet", 不会崩溃。
//   界面文字与日志行一律净化成 ASCII(§13), 但文件内容保持 UTF-8。
//---------------------------------------------------------------------------------
#define LOG_VIEW_MAX_BYTES 65536
#define LOG_VIEW_MAX_LINES 512
#define LOG_VIEW_ROWS 24          // 图形内容区约 27 行, 表头 + 24 行 + 提示仍在屏内

typedef struct {
    char name[32];                // 文件名(只用于显示)
    char *buffer;                 // 文件内容(最多 LOG_VIEW_MAX_BYTES + 1 字节)
    char *lines[LOG_VIEW_MAX_LINES];
    int shownLines;               // 可滚动的行数(<= LOG_VIEW_MAX_LINES)
    int totalLines;               // 扫描到的总行数(可能大于 shownLines)
    bool truncated;               // 文件大于 64KB, 只读了尾部
} ApiLogView;

/// 从今天往回找最近存在的一天日志并读进内存。返回 false 仅表示内存不足。
bool loadApiLogView(ApiLogView *view) {
    memset(view, 0, sizeof(*view));
    view->buffer = malloc(LOG_VIEW_MAX_BYTES + 1);
    if (!view->buffer) return false;

    FILE *f = NULL;
    for (int offset = 0; offset <= API_LOG_SWEEP_LAST_DAY; offset++) {
        char path[96];
        if (!apiLogPathOffset(offset, path, sizeof(path))) break;
        f = fopen(path, "rb");
        if (!f) continue;

        const char *slash = strrchr(path, '/');
        const char *base = slash ? slash + 1 : path;
        size_t nameLen = strlen(base);
        if (nameLen >= sizeof(view->name)) nameLen = sizeof(view->name) - 1;
        memcpy(view->name, base, nameLen);
        view->name[nameLen] = '\0';
        break;
    }
    if (!f) return true;   // 一份日志都没有: 显示 "No log yet"

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return true; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return true; }

    long toRead = size;
    if (toRead > LOG_VIEW_MAX_BYTES) {
        // 只读尾部 64KB(最新内容), 更早的行不载入。
        toRead = LOG_VIEW_MAX_BYTES;
        view->truncated = true;
        if (fseek(f, size - toRead, SEEK_SET) != 0) { fclose(f); return true; }
    } else {
        rewind(f);
    }

    size_t read = fread(view->buffer, 1, (size_t)toRead, f);
    fclose(f);
    view->buffer[read] = '\0';

    if (view->truncated) {
        // 从中间截断时第一行可能是残行, 丢掉。
        char *nl = strchr(view->buffer, '\n');
        if (nl) memmove(view->buffer, nl + 1, strlen(nl + 1) + 1);
        else view->buffer[0] = '\0';
    }

    // 第一遍数总行数(文件末尾的换行不算新的一行)。
    int total = 0;
    for (char *p = view->buffer; *p; ) {
        char *nl = strchr(p, '\n');
        total++;
        if (!nl) break;
        p = nl + 1;
        if (!*p) break;
    }
    view->totalLines = total;

    // 第二遍只保留最后 LOG_VIEW_MAX_LINES 行(最新), 更早的行不载入。
    int keep = total > LOG_VIEW_MAX_LINES ? LOG_VIEW_MAX_LINES : total;
    int skip = total - keep;

    int index = 0;
    char *p = view->buffer;
    while (*p && index < total) {
        char *nl = strchr(p, '\n');
        char *next = nl ? nl + 1 : NULL;
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && p[len - 1] == '\r') p[len - 1] = '\0';
        if (nl) *nl = '\0';
        if (index >= skip && view->shownLines < LOG_VIEW_MAX_LINES)
            view->lines[view->shownLines++] = p;
        index++;
        if (!next || !*next) break;
        p = next;
    }

    return true;
}

/// 一行在屏上最多显示 maxBytes 个字节。
/// §13: 屏幕只允许 ASCII —— 先把这一行净化进 out(>=0x80 -> '?'), 再按宽度截断。
/// 日志文件里的原始 UTF-8 字节不受影响, 拷到电脑上仍是完整中文。


void drawLogView(const ApiLogView *view, int top) {
    uiClear();
    printf("\x1b[36;1m接口日志\x1b[0m  \x1b[2m↑↓ 行   ZL/ZR 翻页   B 返回\x1b[0m\n");
    printf("\x1b[2m------------------------------------------------------------\x1b[0m\n");

    if (view->name[0] == '\0') {
        printf("\n\x1b[33m暂无日志\x1b[0m  还没有记录任何接口调用。\n");
        printf("\x1b[2m预期路径 " PLAYLOG_DIR "/api-YYYYMMDD.log\x1b[0m\n");
        return;
    }
    if (view->shownLines == 0) {
        printf("\n\x1b[33m暂无日志\x1b[0m  %s 为空。\n", view->name);
        return;
    }

    int end = top + LOG_VIEW_ROWS;
    if (end > view->shownLines) end = view->shownLines;

    printf("\x1b[36m%s\x1b[0m  \x1b[2m共 %d 行, 显示 %d-%d%s\x1b[0m\n",
           view->name, view->totalLines, top + 1, end,
           view->truncated ? " (仅尾部 64KB)" : "");
    if (view->shownLines < view->totalLines)
        printf("\x1b[2m窗口 %d 行, 更早的 %d 行未载入\x1b[0m\n",
               LOG_VIEW_MAX_LINES, view->totalLines - view->shownLines);
    printf("\x1b[2m------------------------------------------------------------\x1b[0m\n");

    for (int i = top; i < end; i++)
        printf("%s\n", view->lines[i]);

    uiUpdate();
}

void runLogViewer(PadState *pad) {
    // 静态存放: ApiLogView 里有 512 个行指针, 不放到已经很紧张的栈上。
    static ApiLogView view;
    if (!loadApiLogView(&view)) {
        uiClear();
        printf(tr("内存不足。\n\n按 B 返回主菜单。\n"));
        uiUpdate();
        waitForB(pad);
        return;
    }

    int top = 0;
    int maxTop = view.shownLines > LOG_VIEW_ROWS ? view.shownLines - LOG_VIEW_ROWS : 0;

    while (uiMainLoop()) {
        drawLogView(&view, top);

        padUpdate(pad);
        u64 kDown = padGetButtonsDown(pad);

        if (kDown & HidNpadButton_B) break;                      // B = 返回主菜单
        if (kDown & HidNpadButton_Up) { if (top > 0) top--; }
        if (kDown & HidNpadButton_Down) { if (top < maxTop) top++; }
        if (kDown & HidNpadButton_ZL) { top -= LOG_VIEW_ROWS; if (top < 0) top = 0; }
        if (kDown & HidNpadButton_ZR) { top += LOG_VIEW_ROWS; if (top > maxTop) top = maxTop; }
    }

    free(view.buffer);
    view.buffer = NULL;
}

void interactiveMenu(PadState *pad, GameRecord *games, int gameCount) {
    LimitRecord limits[MAX_LIMITS];
    int limitCount = loadLimits(limits, MAX_LIMITS);
    if (limitCount < 0) limitCount = 0;
    loadAccountTotalLimits();
    loadAccountGameLimits();

    g_currentAccountIndex = 0;
    for (int i = 0; i < g_accountCount; i++) {
        if (g_accounts[i].isActive) { g_currentAccountIndex = i; break; }
    }

    g_online = syncLimitsFromServer();
    if (g_online) {
        int reloaded = loadLimits(limits, MAX_LIMITS);
        if (reloaded >= 0) limitCount = reloaded;
        loadAccountTotalLimits();
        loadAccountGameLimits();
    }

    {
        char dbg[160];
        snprintf(dbg, sizeof(dbg), "menu accounts=%d online=%d total=%d accgame=%d",
                 g_accountCount, g_online ? 1 : 0, g_accountTotalCount, g_accountGameLimitCount);
        boot_log(dbg);
    }

    int selected = 0;
    const char *scope_items[3] = {
        "游戏限额（设备级 / 所有账号共享）",
        tr("账号 + 游戏限额（当前账号 · 这个游戏）"),
        "账号合计限额（该账号全部游戏）",
    };

    while (uiMainLoop()) {
        drawMenu(games, gameCount, selected, limits, limitCount);

        padUpdate(pad);
        u64 kDown = padGetButtonsDown(pad);

        if (kDown & HidNpadButton_B) break;
        if ((kDown & HidNpadButton_AnyUp) && selected > 0) selected--;
        if ((kDown & HidNpadButton_AnyDown) && selected < gameCount - 1) selected++;
        if ((kDown & HidNpadButton_AnyLeft) && g_accountCount > 0) {
            if (g_currentAccountIndex <= 0) g_currentAccountIndex = g_accountCount - 1;
            else g_currentAccountIndex--;
        }
        if ((kDown & HidNpadButton_AnyRight) && g_accountCount > 0) {
            if (g_currentAccountIndex + 1 >= g_accountCount) g_currentAccountIndex = 0;
            else g_currentAccountIndex++;
        }
        if ((kDown & HidNpadButton_ZL) && selected > 0) {
            selected -= 10;
            if (selected < 0) selected = 0;
        }
        if ((kDown & HidNpadButton_ZR) && selected < gameCount - 1) {
            selected += 10;
            if (selected >= gameCount) selected = gameCount - 1;
        }

        if (kDown & HidNpadButton_R) {
            runReportAndPush();
            waitForB(pad);
            int reloaded = loadLimits(limits, MAX_LIMITS);
            if (reloaded >= 0) limitCount = reloaded;
            loadAccountTotalLimits();
            loadAccountGameLimits();
        }

        if (kDown & HidNpadButton_L)
            runLogViewer(pad);

        if (kDown & HidNpadButton_Minus) {
            uiClear();
            printf(tr("正在同步服务端配置...\n\n"));
            uiUpdate();
            g_online = syncLimitsFromServer();
            int reloaded = loadLimits(limits, MAX_LIMITS);
            if (reloaded >= 0) limitCount = reloaded;
            loadAccountTotalLimits();
            loadAccountGameLimits();
            if (g_online)
                printf(tr("在线：限额只读。\n"));
            else
                printf(tr("离线：允许本地编辑。\n"));
            printf(tr("\n按 B 返回。\n"));
            uiUpdate();
            waitForB(pad);
        }

        if (kDown & HidNpadButton_X) {
            const char *settings_items[4] = {"服务器地址", "上传 Token", "同步/上传间隔", "界面语言"};
            int sel = chooseMenu(pad, tr("设置"), settings_items, 4);
            if (sel == 0)
                promptServerBaseUrl();
            else if (sel == 1)
                promptUploadToken();
            else if (sel == 2)
                promptIntervalSettings();
            else if (sel == 3) {
                const char *lang_items[2] = {"中文", "English"};
                int l = chooseMenu(pad, tr("界面语言"), lang_items, 2);
                if (l >= 0) {
                    i18n_set_en(l == 1);
                    saveAppConfig();
                }
            }
        }

        if (kDown & HidNpadButton_Plus)
            promptUploadToken();

        if (!g_online && g_allow_offline_edit && (kDown & HidNpadButton_A) && gameCount > 0) {
            int scope = chooseMenu(pad, tr("选择限额维度"), scope_items, 3);
            if (scope < 0)
                continue;

            int acc = g_currentAccountIndex;
            if (scope != 0) {
                const char *acc_items[MAX_ACCOUNTS];
                char acc_bufs[MAX_ACCOUNTS][0x40];
                for (int i = 0; i < g_accountCount; i++) {
                    snprintf(acc_bufs[i], sizeof(acc_bufs[i]), "%s",
                             g_accounts[i].accountName[0] ? g_accounts[i].accountName : g_accounts[i].accountId);
                    acc_items[i] = acc_bufs[i];
                }
                acc = chooseMenu(pad, tr("选择账号"), acc_items, g_accountCount);
                if (acc < 0)
                    continue;
            }

            const char *aid = (g_accountCount > 0) ? g_accounts[acc].accountId : NULL;
            u32 current = 0;
            if (scope == 0) {
                LimitRecord *r = findLimitRecord(limits, limitCount, games[selected].titleId);
                current = r ? r->limitSeconds : 0;
            } else if (scope == 1) {
                AccountGameLimitRecord *r = findAccountGameLimit(aid, games[selected].titleId);
                current = r ? r->dailySeconds : 0;
            } else {
                AccountTotalRecord *r = findAccountTotal(aid);
                current = r ? r->dailySeconds : 0;
            }

            u32 seconds = 0;
            if (promptLimitMinutes(current, &seconds)) {
                if (scope == 0)
                    setDeviceDaily(limits, &limitCount, games[selected].titleId, seconds);
                else if (scope == 1)
                    setAccountGameDaily(aid, games[selected].titleId, seconds);
                else
                    setAccountTotalDaily(aid, seconds);
                saveLimits(limits, limitCount);
            }
        }

        if (!g_online && g_allow_offline_edit && (kDown & HidNpadButton_Y) && gameCount > 0) {
            int scope = chooseMenu(pad, tr("清除限额维度"), scope_items, 3);
            if (scope < 0)
                continue;

            int acc = g_currentAccountIndex;
            if (scope != 0) {
                const char *acc_items[MAX_ACCOUNTS];
                char acc_bufs[MAX_ACCOUNTS][0x40];
                for (int i = 0; i < g_accountCount; i++) {
                    snprintf(acc_bufs[i], sizeof(acc_bufs[i]), "%s",
                             g_accounts[i].accountName[0] ? g_accounts[i].accountName : g_accounts[i].accountId);
                    acc_items[i] = acc_bufs[i];
                }
                acc = chooseMenu(pad, tr("选择账号"), acc_items, g_accountCount);
                if (acc < 0)
                    continue;
            }

            const char *aid = (g_accountCount > 0) ? g_accounts[acc].accountId : NULL;
            if (scope == 0)
                clearLimit(limits, &limitCount, games[selected].titleId);
            else if (scope == 1)
                clearAccountGameLimit(aid, games[selected].titleId);
            else
                clearAccountTotal(aid);
            saveLimits(limits, limitCount);
        }
    }
}

//---------------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    boot_log("main enter");
    uiInit();
    timeInitialize();
    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    mkdir(PLAYLOG_DIR, 0777);
    // §12.3: 启动时清一次 今天-8 .. 今天-40 的历史接口日志(按文件名计算)。
    apiLogSweepOldFiles();
    loadAppConfig();
    saveAppConfig();

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    Result rc;
    bool nsReady = false;
    bool pdmReady = false;
    bool accountReady = false;
    bool needWait = false;

    rc = nsInitialize();
    if (R_FAILED(rc)) {
        printf(tr("nsInitialize 初始化失败: 0x%x\n"), rc);
        needWait = true;
        goto wait_exit;
    }
    nsReady = true;

    rc = pdmqryInitialize();
    if (R_FAILED(rc)) {
        printf(tr("pdmqryInitialize 初始化失败: 0x%x\n"), rc);
        needWait = true;
        goto wait_exit;
    }
    pdmReady = true;

    // 账号服务是可选的: 拿不到就退回设备级上报,不影响原有功能(REFACTOR_SPEC §5)。
    rc = accountInitialize(AccountServiceType_Application);
    if (R_SUCCEEDED(rc)) {
        accountReady = true;
        g_accountReady = true;
        loadAccounts();
    } else {
        g_accountReady = false;
        printf(tr("[账号] 账号服务初始化失败: 0x%x，仅按设备级处理。\n"), rc);
        uiUpdate();
    }

    printf(tr("PlayLogTool 正在加载游戏...\n"));
    uiUpdate();

    GameRecord *games = NULL;
    int gameCount = scanInstalledGames(&games, false);
    if (games) {
        interactiveMenu(&pad, games, gameCount);
        free(games);
    }

wait_exit:
    if (needWait) {
        printf(tr("\n按 B 退出...\n"));
        uiUpdate();
        while (uiMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_B) break;
            uiUpdate();
        }
    }

    g_accountReady = false;
    if (accountReady) accountExit();
    if (pdmReady) pdmqryExit();
    if (nsReady) nsExit();

    curl_global_cleanup();
    socketExit();
    timeExit();
    uiExit();
    return 0;
}
