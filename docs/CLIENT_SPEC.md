1. 下载安装 https://github.com/devkitPro/pacman/releases/tag/v6.0.2
2. 安装switch 开发工具链  sudo dkp-pacman -S switch-dev
3. 配置环境变量

```bash
# 在~/.zshrc中添加环境变量
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH
```

4. 验证安装效果

```bash
dkp-pacman -Q libnx
dkp-pacman -Q devkitA64
```

5. 添加网络模块

```bash
sudo dkp-pacman -S switch-curl switch-mbedtls switch-zlib
```


6. 编译

```
make clean
make

cd sysmodule
make package
```

7. 使用当前项目工具

前台管理工具

```
cp PlayLogTool.nro   SD:/switch/PlayLogTool.nro
```

后台管理工具

使用整个文件夹
sysmodule/package/atmosphere

把它合并复制到 SD 卡根目录，最终 SD 卡上应该有：
SD:/atmosphere/contents/4200000000504C54/exefs.nsp
SD:/atmosphere/contents/4200000000504C54/flags/boot2.flag
SD:/atmosphere/contents/4200000000504C54/toolbox.json


sysmodule 每 30 秒读取一次本地 `limits.json` 缓存，并且默认每 30 秒尝试从服务端拉取一次配置
（`config_sync_seconds`，可在前台工具里改，服务端也会下发）。服务端限额改完后会自动生效；
也可以打开前台工具按 `-` 立即同步。

8. 前台工具菜单

```
A      设置当前游戏本地每日可玩分钟数
Y      清除当前游戏本地限额
-      从服务端同步限额配置
R      生成报表并推送
X      配置服务端地址前缀
+      配置设备上传 token
Right  配置 sysmodule 定时参数
L      查看接口日志（§12.4；屏幕只画 ASCII，见 §13）
ZL/ZR  翻页
B      返回 / 退出（结果页里也是按 B 返回菜单）
```

前台工具会在 SD 卡写入：

```
SD:/PlayLog/config.json
SD:/PlayLog/limits.json
SD:/PlayLog/api-YYYYMMDD.log
```

两个二进制写 `config.json` / `limits.json` 都走"临时文件 + `rename()`"原子写；
读取失败时保留内存中的旧值，绝不会因为一次读失败就把 upload token 或限额重置掉。
HTTPS 需要 `SD:/PlayLog/cacert.pem`，缺失时会明确报错并跳过请求，而不是静默不校验证书。

`config.json` 示例：

```json
{
  "server_base_url": "https://your-server.example.com",
  "device_id": "自动生成的 UUID",
  "device_id_type": "hardware_hash",
  "upload_token": "服务端生成的设备上传 token",
  "limit_reload_seconds": 30,
  "config_sync_seconds": 30,
  "upload_interval_seconds": 900,
  "heartbeat_interval_seconds": 300
}
```

`device_id` 会显示在前台工具首页。PlayLogTool 会优先读取主机序列号并生成不可逆 SHA-256
硬件 hash；如果读取失败，则生成随机 UUID。`device_id_type` 会标记为
`hardware_hash` 或 `random_uuid`，并随上报数据一起发送到服务端。
先在服务端手动注册这个设备 ID，再把服务端生成的 token 输入到前台工具。
前台工具和 sysmodule 会自动把 `server_base_url` 拼成：

```
https://your-server.example.com/switch/statistics/daily
https://your-server.example.com/switch/devices/config
https://your-server.example.com/switch/devices/heartbeat
```

每款游戏每日可玩时长既可以在本地编辑，也可以由服务端配置覆盖；前台工具和 sysmodule 共用
`SD:/PlayLog/config.json` 与 `SD:/PlayLog/limits.json`。

## 按账号限额（仅 Switch）

前台工具会枚举主机上的用户账号（`accountListAllUsers` + `accountGetProfile`），并用
`pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId` 读取每账号每游戏的累计时长，
按增量上报到 `POST /switch/statistics/daily`，条目里附带可选的 `account_id` / `account_name`。
账号维度不可用（或该游戏没有任何账号记录）时，仍然按原来的设备级条目上报。

sysmodule 从服务端下发的 `accounts` / `account_limits` 里取当前账号的限额，优先级为
`account_limits[account][title]` → `limits[title]` → 不限额；配额按 **(账号, 游戏, 本地日)**
记账，一个账号的时长不会算到另一个账号头上。当前账号判定失败时退回设备级记账与设备级限额。

Minecraft 域不做账号维度。

## 上报来源标记（REFACTOR_SPEC §8）

两个二进制都会向 `POST /switch/statistics/daily` 上报，但口径不同：前台 NRO 的数字来自
PDM 系统统计，sysmodule 的数字来自它自己的前台计时。因此每条 game entry 都带上可选的
`"source"` 字段：

- 前台 NRO → `"source": "nro_pdm"`
- sysmodule → `"source": "sysmodule"`

服务端按 `(game, 本地日)` 对每个来源分别汇总后取 **max**（而不是求和），
从而消除同一段游玩时长的双计；旧客户端不带该字段时行为完全不变。

## 前台焦点判定（REFACTOR_SPEC §9）

sysmodule 只用 `pm:dmnt` 只能知道"应用进程存在"，游戏切到 HOME 菜单后仍会继续计时，
甚至可能在后台触发限额杀进程。因此 sysmodule 每 5 秒做一次尽力而为的焦点判定：

- 主要信号：`pdmqryGetAvailablePlayEventRange` + `pdmqryQueryAppletEvent`
  （`switch/services/pdm.h`）读取系统自己的 `in_focus` / `out_of_focus` 焦点日志；
- 辅助信号：`pmdmntGetApplicationProcessId`（进程存在）、`apmGetPerformanceMode`
  （仅诊断，不参与判定）。

判定结果为 `foreground` / `background` / `suspect` / `unknown`，只有**确证 background**
才暂停记账与限额执行；`unknown` / `suspect` 一律维持原有行为（继续计时、继续限额），
判定失败绝不会导致漏计时或误杀。心跳里新增诊断字段：

```json
"focus": { "state": "background",
           "signals": { "app_pid": 123, "pdm_ready": true, "pdm_focus_event": 3,
                        "pdm_focus_ours": false, "pdm_ours_event": 3,
                        "evidence_age_s": 120, "apm_perf": 0 },
           "source": "heuristic" }
```

详细状态机、被排除的 API 与仍需真机确认的点见 `sysmodule/README.md`。

## 限额执行方式 enforce_mode（REFACTOR_SPEC §10）

配额用尽时怎么处理游戏，由**设备级** `enforce_mode` 决定（服务端 `GET /switch/devices/config`
下发，权威值；本地 `SD:/PlayLog/config.json` 里的 `enforce_mode` 只是离线兜底，
字段缺失/为空时默认 `block_launch`）。两个二进制共用同一个 `config.json`，所以字段在两端都
会读入并原样写回。

| 取值 | 行为 | 丢进度风险 |
|---|---|---|
| `block_launch`（**默认**） | 配额用尽**不打断当前这局**，玩家正常存档退出；**下次启动该游戏时**在加载画面阶段（1 秒内）立刻结束进程 | 无 |
| `kill` | 达到限额立刻强制结束进程（旧行为） | 只在退出时存档的游戏会丢 |

sysmodule 的判定规则（严格按 §10）：

1. 记录当前前台 title，以及**进入该 title 那一刻**该 `(账号, 游戏, 本地日)` 的已用秒数（冻结值）。
2. `block_launch`：**进入时**已用 ≥ 限额 → 立即结束进程（启动拦截）；进入时未超限 →
   这一局**永不终止**，即使中途超限也只写日志、记账照常。
3. `kill`：超限即终止（与改造前一致）。
4. 任何不确定（限额读不到、前台进程查询失败、`enforce_mode` 取值无法识别）→ **一律不终止**。
   宁可不杀，不可误杀。

前台工具（NRO）不执行限额，只负责把服务端下发的值落进 `config.json`，供 sysmodule 离线启动时兜底。

## 周总时长与按星期限额（REFACTOR_SPEC §11）

在「每游戏每日限额」之外，服务端还可以为同一款游戏配置**一周总时长上限**和**一周里每天不同的时长**。
服务端通过 `GET /switch/devices/config` 追加下发（单位一律**秒**）：

```json
{
  "limits": { "0100D71004694000": 900 },
  "limit_rules": {
    "0100D71004694000": { "daily_seconds": 900, "weekly_seconds": 28800,
                          "weekday_seconds": [0,0,0,0,0,900,900] }
  },
  "account_limits": { "1f2a…": { "0100D71004694000": 600 } },
  "account_limit_rules": { "1f2a…": { "0100D71004694000": { "daily_seconds": 600,
                                                            "weekly_seconds": 0,
                                                            "weekday_seconds": [] } } }
}
```

语义（与 `REFACTOR_SPEC.md` §15 完全一致，唯一真源）：

- `weekday_seconds`：**下标 0 = 周一 … 6 = 周日**（与 Python `date.weekday()` 一致），只有两种形态：
  - **恰好 7 个值 = 按星期已配置**：当日生效限额 = 该天的值，**值为 `0` 也照样生效（= 当天禁止游玩）**。
    例：`[0,0,0,0,0,900,900]` = 周一到周五不让玩、周末各 15 分钟。
  - **`[]`（或字段缺失、长度不是 7）= 按星期未配置**：当日生效限额回退 `daily_seconds`。
  - 客户端解析时单独记录 `has_weekday` 标志，**`[]` 与 `[0]*7` 必须区分开**：`[0]*7` 不是
    "未设置"，而是"每天都封禁"。
- `weekly_seconds`：周总上限，`0` = 不限制。**周 = 本地时区本周一 00:00 起算**（周日 23:59 仍属于本周）。
- 今日生效限额：`effective_daily = has_weekday ? weekday_seconds[today] : daily_seconds`。
- **拦截条件（任一满足即拦截，是 OR 不是 AND，即"封顶"）**：
  1. `effective_daily == 0`（当天封禁），或 `today_used >= effective_daily`；
  2. `week_used >= weekly_seconds`（且 `weekly_seconds > 0`）。

  哪个上限先到就封哪个。周额度只是**额外一层封顶**，不会从每日额度里扣减。
- 兼容：`limits` / `account_limits` 仍然下发**今日生效的日限额**，旧服务端没有 `limit_rules` 时
  新客户端自动回退到它们，行为与改造前完全一致；反过来新服务端下发的新字段旧客户端会直接忽略。
  **已知取舍**：老服务端若把"未配置"下发成 `[0]*7`，新客户端会读成"每天都封禁"——这是 §15.1
  接受的取舍（`has_weekday` 只看长度是否为 7，不猜意图）。
- 执行方式沿用 §10 的 `enforce_mode`（`block_launch` 启动时拦截 / `kill` 到点即杀）。
- **任何不确定一律不拦截**并写日志：规则读不到、周用量算不出、`enforce_mode` 无法识别。
  宁可不拦，不可误杀。唯一的例外是**已确定**的当天 0 上限：它一定拦截（只会更严，不会更松）。

### 客户端怎么记账（REFACTOR_SPEC §14：日账本按真实时长累加）

日账本 `SD:/PlayLog/limiter_state.csv`（键 `(账号, 游戏, 本地日)`，字段 `used_seconds`）
**每个前台 tick 都按真实 `elapsed` 累加**，与"是否配置了限额"无关：

- 该 title 没有限额条目 → 账本照样增长。修复前这个分支直接 `continue`，日账本从不累加，
  于是**游戏中途新增限额等于从 0 开始**：进入下一局时判定"未超限"，`block_launch` 放行整局；
- 有规则 → 账本**不再在日限额处截断**（`used_seconds += elapsed`），限额比较仍是
  `used_seconds >= effective_daily`，因此执法只会更准，绝不会更松；
- 同一份真实 `elapsed` 会**无论是否到顶都继续排队上报**（`add_upload_seconds`），
  面板"今日已玩"因此不会在到达限额的那一刻冻结、与本地账本分叉。

`add_uncapped_seconds()` / `limiter_week.csv` 语义不变（§11 周上限仍以未截断用量为准）。
账本仍按本地日重置：`load_state()` 只保留日键等于已通过日切守卫的 `g_active_day` 的条目
（新的一天从 0 开始），`save_state()` 把当天条目落盘，跨重启可续上。进入某 title 时会写一行
`§14 session entry title=… used_today=… limit=… over=…`。

### 周用量为什么必须是"未截断"的

日账本 `SD:/PlayLog/limiter_state.csv` **只保留"当天"的条目**（`load_state` 按当前日键过滤），
所以不能用来算周总量。因此 sysmodule 另外维护一份**独立的周账本**：

```
SD:/PlayLog/limiter_week.csv            # account_id,week,title_id,uncapped_seconds
```

- 键：`(账号, 游戏, 本周周一)`，值是**未截断的真实用量**（每 tick 的完整 `elapsed`，
  与每日限额无关，也与日账本互不影响）；
- 只保留当前周，跨周时整体重置。周键由**已经通过日切守卫的** `g_active_day` 推导，
  所以回拨时钟无法"复活"上一周的额度，也无法刷出新的周额度；
- 时钟不可用 / 周账本读失败 / 当前周未知 → 周用量标记为不可信，配了周上限时**不拦截**并写日志；
- 容量上限复用 `MAX_USAGE`（与日账本一致），满时先淘汰零值项，绝不静默停止记账。

前台工具（NRO）不记账、不执行限额，只负责把 `limit_rules` 解析并持久化。它在本地编辑每日限额后
重写 `limits.json` 时，会保留同一 title 的周总时长与按星期限额，并把它写回到**同一个键名**
`limit_rules` 下，同时**原样保留原有的顶层 `"<TITLEID>": <秒>` 键**（旧客户端只扫顶层，
`limit_rules` 既不是合法 TitleID、其值又是对象，会被原样跳过 → 向后兼容）。

§15 起按星期是否"已配置"用 `has_weekday` 标志记录：**未配置时重写出来的是 `"weekday_seconds": []`，
绝不写成 `[0]*7`**（否则 sysmodule 会读成"每天都封禁"，等于凭空造出一条限额）；已配置时才写 7 个值。
NRO 的「Selected」详情会对已配置按星期的规则逐天列出周一到周日，**0 的那天明确标注 `0 (blocked)`**
（全部纯 ASCII，见 §13），列表行则显示 `CUSTOM today …`。

`limits.json` 的体量仍然受既有上限约束：条目数封顶 `MAX_LIMITS`(512)，写文件时若缓冲区不够
**整体拒绝写入**（绝不写出截断的 JSON），因此不会把已有配置写坏。一条带规则的记录约 150 字节，
16 KB 的缓冲区足够容纳数十款受限游戏；超出时请精简限额条目。

> 判定说明（需要 Lead 拍板的取舍）：服务端至今把 `enabled && daily_limit_seconds == 0` 归类为
> **block**，`limits[title]` 也照旧下发 `0`。因此客户端对"显式配置的、三个上限全为 0"的规则
> **仍然按旧的「0 分钟 = 封禁」处理**（`evaluate_limit_rule` 的全 0 分支），
> 以免静默放行 —— §11 那句"全 0 = 完全不限制"在客户端按"该 title 没有任何限额条目"实现。
> 若 Lead 想让全 0 表示不限制，需要给"封禁"另加一个显式字段。

## 账号级「全部游戏合计」限额（REFACTOR_SPEC §16）

只按游戏配限额时，孩子玩完一个游戏的额度后换另一个游戏就能继续。§16 增加一层**按账号**的
「全部游戏加起来」上限，与单游戏限额是 **OR**（哪个先到封哪个），因此执法只会更严、绝不会更松。

服务端通过 `GET /switch/devices/config` 追加下发（单位一律**秒**）：

```json
{
  "account_total_limit_rules": {
    "1f2a…": { "daily_seconds": 3600, "weekday_seconds": [] }
  },
  "account_used_today_total": { "1f2a…": 2699 }
}
```

语义（与单游戏限额**完全同一条** §15 判定，唯一真源）：

- `account_total_limit_rules` 只包含 `enabled=true` 的行，按 `account_id` 键控；每账号至多一条。
- `weekday_seconds`：**恰好 7 个值 = 按星期已配置**（下标 0 = 周一 … 6 = 周日，值 `0` 照样生效
  = 那天不让玩）；`[]` / 字段缺失 / 长度 ≠ 7 / 含非数值 = **未配置**，回退 `daily_seconds`。
  例：`[0,0,0,0,0,3600,3600]` = 周一到周五不让玩、周末各 60 分钟。
- `account_used_today_total`：服务端权威的"该账号今日全部游戏合计秒数"，作为**本地合计的下限**
  （`合计 = max(本地账本合计, 服务端合计)`，**只抬不降**，与 §14.3 同一原则）。
  字段缺失/为空/为 0/非数字一律忽略，等于保持本地值，绝不会反过来放宽限制。
- **账号当日合计** = 本地账本 `limiter_state.csv` 里该账号 `day_key == 当前本地日` 的所有条目
  之和（§14 修复后 `used_seconds` 已是真实时长，不再被单游戏限额截断）。
- **判定（与单游戏限额 OR）**：
  - `enforce_mode = block_launch`（默认）：进入**任何**一局时若合计已到上限 → 启动拦截；
    进入时未超限则本局不再打断（与 §10 一致，避免丢进度）。
  - `enforce_mode = kill`：运行中合计到上限 → 立即终止。
- **fail-safe 不变**：账号未知、没有合计规则、星期几算不出、账本拿不到一律**不终止**并写日志。
  唯一的例外是**已确定**的当日 0 上限：它一定拦截（只会更严）。
- 日志：`§16 account total account=%s used=%llu limit=%u source=%s`，每个会话最多一条避免刷屏；
  另在每次进入 title 时写一行 `§16 session entry account=… total_used=… limit=… source=… over=…`。
- 日切：服务端合计下限带本地日键，**本地日一变立即作废**（`reset_account_total_floors()`），
  与日账本/周账本一起由已通过日切守卫的 `g_active_day` 驱动，回拨时钟无法复活旧下限。
- 前台工具（NRO）不执行限额，只在「Selected」详情里对配置了合计限额的账号只读展示一行纯 ASCII：
  `Account total: today 60 min (per-day)` 或 `Account total: today 0 min (blocked)`。

## 时钟防抖：按星期限额不依赖主机本地时钟（REFACTOR_SPEC §19）

**线上事故（2026-10-19 周一）**：joey 的合计限额配了 `[0,0,0,0,0,1800,1800]`
（周一到周五不让玩），当天却能用 joey 账号启动 Minecraft Dungeons。

原因：sysmodule 用**主机本地日**算"今天星期几"。这台 Switch 的 RTC 比真实时间差了
**639 天**（服务端日志 `device.auth.warn ... skew_human=639.3d`，
`PLAYLOG_SIGNATURE_SKEW_MODE=warn` 只告警不拒绝），设备认为当天是
`2025-01-18`（周六）→ 取到周末的 `1800` 秒，而不是周一的 `0`；joey 当天只玩了
607 秒，于是"封禁"被静默降级成"周末 30 分钟"。

因此 sysmodule 改成**统一由服务端时间驱动**日账本 / 周账本 / 按星期判定：

1. 服务端在 `GET /switch/devices/config` 里额外下发
   `server_local_date` / `server_weekday` / `client_clock_skew_seconds`；
2. 同步成功后的 `SERVER_TIME_TRUST_SECONDS`（1 小时，远大于 90–300 秒的同步间隔）
   窗口内，`refresh_day_key()` 直接采纳 **服务端本地日**作为"今天"：日账本
   `limiter_state.csv`、周账本 `limiter_week.csv`、§16 服务端合计下限、以及
   `weekday_seconds[今天]` 的下标全部跟着服务端日期走。主机 RTC 跑偏/被改/来回跳
   都不再影响日/周窗口；
3. `limits` / `account_limits` / `account_total_limits` 里本来就是服务端按本地日算好的
   **今日生效值**，同步后的窗口内 `rule_effective_daily` 也优先用它们，双保险；
4. 超出窗口（长时间离线）或老服务端没有这些字段 → 回退原来的本地时钟 + 日切防回拨
   守卫，行为不变（fail-safe：两者都算不出时仍然不拦）；
5. 每次同步成功把 `server_local_date` 与时钟偏差写进 `limiter.log`
   （`server local date=...` / `WARNING: device clock differs from server by ...`），
   方便定位"主机时间不对导致星期几算错"。

另外补上**运行中账号变化的重判定**：`block_launch` 的"本局不打断"只针对同一账号。
如果游戏运行中主机切换了用户（例如 xb → joey），sysmodule 会把这一刻当作新的一局，
用新账号重新冻结 §11/§16 判定并立即做一次启动拦截——新账号如果当天 0 分钟/已超限，
`block_launch` 也会当场拦截，而不是拿旧账号的"未超限"判定放行整局。日志：
`mid-session account change <old> -> <new> ... re-evaluating limits`。

**重要：本方案不需要、也不要改主机（Switch）系统时间。** sysmodule 只调用
`timeInitialize()` / `timeGetCurrentTime()` / `timeToCalendarTimeWithMyRule()` 这类
**只读**接口，从不调用 `timeSetCurrentTime()` 等写时间 API；服务端下发的
`server_local_date` 只用于本程序内部的账本日键、星期几判定和日志，**不会写回系统时钟**。
（Atmosphere 下外部修改系统时间可能触发主机异常重启，所以这里刻意只读。）
主机 RTC 不准没关系：日/周账本以服务端日为准。只有 NRO 的"今日生效"显示、接口日志
文件名等纯本地功能仍按主机时钟命名，不影响限额执行。

## 失败原因与接口日志（REFACTOR_SPEC §12；屏幕 ASCII 规则见 §13）

面向设备的接口（`/switch/*`）出错时，服务端统一返回：

```json
{ "detail": "<可读的中文原因，含下一步怎么做>",
  "code": "<机器码>",
  "hint": "<一句话纯 ASCII 解释，<=110 字符>" }
```

- **前台 NRO**：`GET /switch/devices/config` 与 `POST /switch/statistics/daily` 只要拿到非 2xx，
  就在屏幕上打印 `HTTP <状态码> <code>:` 加服务端给的 **ASCII `hint`**（按 76 列折行，最多 3 行）；
  `hint` 缺失时退化为 HTTP 状态文案（`Unauthorized` 之类），`code` 缺失时显示 `-`。
  例如：

  ```text
  [Push] Server returned HTTP 401 device.bad_signature:
    Signature mismatch (token changed?). Reset the device token in the web
    console, then re-enter it here.
  ```

- **中文字段 `detail` 只写日志文件，绝不打屏**：libnx 的 console 是点阵字体（`ConsoleFont`），
  **没有任何 UTF-8 处理**，把 UTF-8 中文 `printf` 到屏幕必然乱码（一个汉字 3 字节被当成 3 个无效字模）。
- **sysmodule** 没有屏幕，所以把 `detail` 写进 `limiter.log` 与接口日志；它本身不做任何 console 输出。
- 同理，NRO 屏幕上显示的一切（游戏名、账号昵称、日志行）都先做 ASCII 净化：`>=0x80` 的字节显示为 `?`，
  但**磁盘上的文件内容保持原始 UTF-8**（日志、`report.csv`、`run_*.csv` 都不净化）。

两个二进制都会把每次 API 调用追加写进**同一个**当天文件（短行写入，交错时最多丢一行）：

```text
sdmc:/PlayLog/api-YYYYMMDD.log        # 本地日期，每天一份
```

单行格式（单行上限 240 字节，超出截断；`code` 是 ASCII 机器码，缺失记 `-`）：

```text
YYYY-MM-DD HH:MM:SS [tag] METHOD /path -> HTTP <status> <code|-> <detail|OK>
```

`tag` 取值：

| tag | 含义 |
|---|---|
| `config` | `GET /switch/devices/config` 配置同步 |
| `push` | `POST /switch/statistics/daily` 时长上报 |
| `heartbeat` | sysmodule 心跳 |
| `net` | curl 层失败（超时、DNS、TLS 等，没有 HTTP 状态码，记为 `HTTP 0`） |
| `write` | 写 SD 卡失败，`/path` 位置就是失败的目标文件 |

示例（文件是 UTF-8；拷到电脑上能读到完整中文）：

```text
2026-10-14 08:12:20 [config] GET /switch/devices/config -> HTTP 200 - OK
2026-10-14 08:12:20 [push] POST /switch/statistics/daily -> HTTP 401 device.bad_signature 签名不匹配：设备 token 可能已重置，请在后台重置 token 后重新填入
2026-10-14 08:12:20 [net] POST /switch/statistics/daily -> HTTP 0 - Timeout was reached
2026-10-14 08:12:20 [write] WRITE sdmc:/PlayLog/limits.json -> HTTP 0 - 写盘失败
```

`code` 放在 `detail` 前面，是为了让日志查看器把中文净化成 `?` 之后，
仍能一眼看出失败分类（`device.bad_signature` 等）。

**保留 7 天**：每次写日志时 `remove()` 掉"今天-8"的文件（所以只会留下今天 … 今天-7）；
进程启动时再按文件名扫一遍"今天-8 .. 今天-40"，清掉历史遗留文件。全程按文件名计算，
**不需要列目录**。首次运行（SD 卡上还没有任何 `api-*.log`）时不会创建空文件，
只有真的发生一次 API 调用或写盘失败才会生成当天文件；此时按 `L` 查看日志会显示 `No log yet`。
写日志失败（比如 SD 卡拔了）只会静默跳过，绝不影响上报/同步主流程。

### 日志查看器（`L` 键）

主菜单按 **`L`** 进入"接口日志"全屏界面（界面文字全部是 ASCII，避免点阵字模乱码）：

```text
Up/Down  逐行滚动
ZL/ZR    整页滚动
B        返回主菜单
```

界面显示文件名、总行数、当前可见行范围。默认显示**今天**的日志；今天没有就回退到最近存在的一天
（今天-1、今天-2 … 今天-40）。文件不存在或为空时显示 `No log yet`，按 `B` 返回。

内存有界：最多读文件**尾部 64 KB**、最多保留**最后 512 行**，界面会标注只载入了多少；
一行最多显示 79 个字符（避免长行折行打乱翻页）。更早的内容不载入，但**日志文件本身不会被修改**。
绘制时会把 `>=0x80` 的字节显示为 `?`（§13），所以中英混排的日志行不会把界面搞乱。

推送请求会携带 HMAC-SHA256 签名：

```
Authorization: PlayLog-HMAC-SHA256 device_id="...",timestamp="...",signature="..."
X-PlayLog-Device-Id: ...
X-PlayLog-Timestamp: ...
X-PlayLog-Signature: ...
```

服务端设计见 `SERVER_DESIGN.md`。

sysmodule 会按 `heartbeat_interval_seconds` 定时向服务端发送设备心跳。当前会尽量采集：
电量/充电状态/电池健康与电压、系统固件版本、是否运行 Atmosphere、SD 卡与机身用户区存储容量、
网络连接状态、当前前台 TitleID、`focus` 焦点诊断对象（见上）、sysmodule uptime。
Atmosphere 版本号目前保留字段但不保证能采集。
服务端可以在心跳响应里下发 `reboot` 或 `shutdown` 指令，但**只有在响应带有效 HMAC 签名时**
才会执行；无签名或签名不匹配时 sysmodule 会拒绝执行并记录日志。响应签名约定为
`X-PlayLog-Response-Signature: hex(HMAC-SHA256(upload_token, 原始响应体字节))`。
截图上传暂未实现。
