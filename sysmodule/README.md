# PlayLogLimiter sysmodule

This is the background limiter half of PlayLogTool. It is an Atmosphere boot2 sysmodule, not a manually launched `.nro`.

## Build

```sh
export DEVKITPRO=/opt/devkitpro
make -C sysmodule
make -C sysmodule package
```

The package target creates:

```text
sysmodule/package/atmosphere/contents/4200000000504C54/exefs.nsp
sysmodule/package/atmosphere/contents/4200000000504C54/flags/boot2.flag
sysmodule/package/atmosphere/contents/4200000000504C54/toolbox.json
```

Copy the `atmosphere` folder to the SD card root and reboot.

## Limit config

The foreground PlayLogTool `.nro` can create and edit this file interactively. Select a game, press `A`, then enter the daily limit in minutes with the Switch keyboard.

The shared config file is:

```text
sdmc:/PlayLog/limits.json
```

Legacy (device-level only) format:

```json
{
  "0100D71004694000": 900
}
```

Format written by a cloud sync (`GET /switch/devices/config`), which also carries
per-account limits and the §11 weekly / per-weekday rules:

```json
{
  "device_id": "…",
  "limits": { "0100D71004694000": 900 },
  "limit_rules": {
    "0100D71004694000": { "daily_seconds": 900, "weekly_seconds": 28800,
                          "weekday_seconds": [0,0,0,0,0,900,900] }
  },
  "accounts": [ { "account_id": "1f2a…", "nickname": "熊仔", "is_active": true } ],
  "account_limits": { "1f2a…": { "0100D71004694000": 600 } },
  "account_limit_rules": { "1f2a…": { "0100D71004694000": { "daily_seconds": 600,
                                                            "weekly_seconds": 0,
                                                            "weekday_seconds": [] } } },
  "account_total_limit_rules": { "1f2a…": { "daily_seconds": 3600, "weekday_seconds": [] } },
  "account_used_today_total": { "1f2a…": 2699 },
  "enforce_mode": "block_launch"
}
```

Keys are game TitleIDs. Values are allowed seconds for the current local day. `900` means 15 minutes.

Effective limit precedence is `account_limit_rules[account][title]` →
`limit_rules[title]` → `account_limits[account][title]` → `limits[title]` → no limit.
Usage is accounted per **(account, title, local day)**, so one account's time is never
charged to another. If the current account cannot be determined the module falls back to
device-level limits and device-level accounting.

`enforce_mode` in this response is the server's authoritative value (see below); the
module also persists it back into `config.json` so the value survives an offline boot.

The sysmodule reloads this file every 30 seconds.

## Weekly total + per-weekday limits (REFACTOR_SPEC §11 / §15)

Both `limit_rules` and `account_limit_rules` carry the full rule per title (all values in
**seconds**):

| field | meaning |
|---|---|
| `daily_seconds` | base daily cap; `0` = block (the legacy "0 minutes" rule) |
| `weekday_seconds` | index **0 = Monday … 6 = Sunday** (matches Python `date.weekday()`). **Exactly 7 values** = per-weekday **configured**; **`[]` / missing / any other length** = **not configured** |
| `weekly_seconds` | weekly total cap; `0` = unlimited. **The week starts Monday 00:00 local time** |

The parser records a separate `has_weekday` flag (`weekday_seconds` present and exactly 7
elements). `[]` and `[0]*7` are **different things** and must never be conflated:

```
effective_daily = has_weekday ? weekday_seconds[today] : daily_seconds
```

- configured → the day's value applies **verbatim, including `0` (= no play that day)**;
- not configured → fall back to `daily_seconds`.

Missing field / wrong length / non-array → `has_weekday = false` and a fallback to
`daily_seconds`; the parser never crashes and never invents a limit. A rule with
`daily_seconds = 900, weekday_seconds = [0,0,0,0,0,900,900]` therefore gives
`effective_daily = 0` on a Wednesday → blocked, and `900` on Saturday.

**Block when EITHER** `effective_daily == 0` (blocked that day) or
`today_used >= effective_daily` **or** `week_used >= weekly_seconds` (when
`weekly_seconds > 0`) — whichever cap is reached first. This is an **OR, not an AND**.
The weekly cap is an extra ceiling; it is never subtracted from the daily allowance.
Title with no entry at all → unrestricted (unchanged).

The decision lives in one place, `evaluate_limit_rule()`, which returns the winning source
(`daily` / `weekday` / `weekly` / `none`). Precedence for picking the rule is account over
device, exactly as before. §10's `enforce_mode` still decides *how* the violation is
applied: `block_launch` only intercepts at launch when the rule was already violated at
session entry; `kill` terminates as soon as the rule is violated. The §15 change only makes
enforcement **stricter** (a configured `0` now blocks); `should_terminate_now()` /
`enforce_mode` semantics are untouched.

Effective limits are computed with the **local weekday derived from the day key that already
passed the anti-rollback day guard**, so the weekday can never disagree with the day the
quota is charged to.

### The daily ledger always counts real played time (REFACTOR_SPEC §14)

The daily ledger `limiter_state.csv` (`used_seconds`, keyed by account + title + local day)
accumulates the **real elapsed time** on every tick where the title is in the foreground and
the module is not focus-paused, and this is true whether or not a limit rule exists:

- no rule for this title → the ledger still grows (it used to be skipped entirely, so adding
  a limit mid-day restarted the counter at 0 and `block_launch` let a whole session through);
- a rule exists → the ledger is **not** clamped at the cap (`used_seconds += elapsed`); the
  comparison is still `used_seconds >= effective_daily`, so enforcement only becomes more
  accurate, never looser;
- the same real `elapsed` is queued for upload (`add_upload_seconds`) even after the cap is
  reached, so the dashboard's "today" no longer freezes at the moment the limit is hit and
  cannot diverge from the local ledger.

`add_uncapped_seconds()` / `limiter_week.csv` are unchanged (§11 weekly caps still use the
uncapped ledger). The ledger is still reset per local day by the day guard
(`load_state()` keeps only entries whose day key equals the accepted `g_active_day`, so a
new day starts from zero, and `save_state()` persists the current day's entries across
restarts). A `§14 session entry title=… used_today=… limit=… over=…` line is logged when a
title is entered.

### Weekly usage must be uncapped (why there is a second ledger)

`limiter_state.csv` cannot be used for the weekly sum: it only keeps entries for the
**current** day (`load_state` filters by the active day key). The module therefore keeps a
separate ledger holding **uncapped** usage:

```text
sdmc:/PlayLog/limiter_week.csv   # account_id,week,title_id,uncapped_seconds
```

- key `(account, title, Monday-of-this-week)`, value = full `elapsed` per tick, independent
  of the daily cap and of the daily ledger;
- only the current week is kept; the week key is derived from the day key that passed the
  day guard, so a clock rollback cannot resurrect last week's allowance;
- week change → counters reset and the file is reloaded;
- the table is bounded by `MAX_USAGE`; when full it evicts zero entries first and logs,
  never silently stops accounting.

### Uncertainty never terminates

`g_week_usage_known` is `false` whenever the clock is unavailable, the week ledger could not
be read, or the active week is unknown. If a rule has a weekly cap and weekly usage is not
trustworthy, `evaluate_limit_rule()` returns `none` (no block) and logs the reason —
"rather not block than block wrongly". The same applies to an unknown weekday and to a
missing usage entry.

> Contract note (§15 tightened this): the server classifies `enabled && effective daily == 0`
> as **block** and dispatches `limits[title] = 0`. A rule whose *today* value is 0 — whether the
> legacy daily `0`, or a configured `weekday_seconds[today] == 0` — is an explicit block, not
> "unrestricted". §15's "no cap" is expressed as "the title has no limit entry at all" (or a
> non-zero cap). Backwards-compatibility caveat: an **old** server that sends 7 zeros for
> "not configured" will now read as "blocked every day"; that trade-off is accepted by the
> §15.1 contract, since `[]` is the only encoding of "not configured".

## Account-wide "all games combined" limit (REFACTOR_SPEC §16)

Per-title limits alone let a child finish one game's quota and switch to another. §16 adds a
**per-account cap on the sum of playtime across all titles**, OR-ed with the per-title rule, so
enforcement can only become **stricter**.

`GET /switch/devices/config` gains two fields (all values in **seconds**):

```json
{
  "account_total_limit_rules": { "1f2a…": { "daily_seconds": 3600, "weekday_seconds": [] } },
  "account_used_today_total":  { "1f2a…": 2699 }
}
```

- `account_total_limit_rules` is keyed by `account_id` (one entry per account) and only
  contains `enabled=true` rows. `weekday_seconds` uses the **exact same §15 rule** as above:
  **exactly 7 values** = per-weekday configured (index 0 = Monday … 6 = Sunday, and a day's
  `0` means **no play that day**); `[]` / missing / any other length / non-numeric element =
  **not configured** → fall back to `daily_seconds`. `[0,0,0,0,0,3600,3600]` = no play
  Mon–Fri, 60 minutes per weekend day.
- `account_used_today_total` is the server-authoritative total of that account's playtime
  today across every title. It is adopted as a **floor** for the local sum:
  `total = max(local_sum, server_floor)` — **raise-only**, same principle as §14.3. Missing /
  empty / `0` / non-numeric values are ignored (they never lower the local value).

**Account today total (local)** = the sum of `usage->used_seconds` over every ledger entry
with the same `account_id` and `day_key == g_active_day`. §14 made those entries uncapped, so
this is real played time.

**Effective total limit for today** = the §15 rule verbatim:
`has_weekday ? weekday_seconds[today] : daily_seconds`; a value of `0` means "no play today".

**Enforcement is OR-ed with the existing per-title decision:**

- `enforce_mode = block_launch` (default): when **any** title is entered, if the account total
  is already at/over the limit → intercept the launch (same session-entry path as §10). A
  session that entered under the cap is allowed to finish.
- `enforce_mode = kill`: while a title is running, the moment the account total reaches the
  limit → terminate it (existing terminate helper).
- The §10/§11 fail-safe is unchanged: unknown account, missing rule, unknown weekday, no
  server floor and no ledger entry → **do not terminate**. The only exception is a
  positively-determined `0` limit, which always blocks (stricter, never looser).
- `should_terminate_now()` and its per-title semantics are untouched; the account-total
  decision is a separate evaluation OR-ed in by the caller.

Logging (rate-limited to one §16 line per session, like `g_session_exceed_logged`):

```text
§16 account total account=%s used=%llu limit=%u source=%s
§16 session entry account=%s total_used=%llu limit=%u source=%s over=%u
```

The server floor is stored per account together with the local day key and is discarded the
moment the day key changes (`reset_account_total_floors()`), exactly like the day/week ledgers
driven by the anti-rollback day guard — a clock rollback cannot resurrect yesterday's floor.

The foreground NRO does not enforce anything; it only parses `account_total_limit_rules` from
the same `limits.json` and prints one pure-ASCII line per configured account in the
"Selected" detail area (`Account total: today 60 min (per-day)` /
`Account total: today 0 min (blocked)`).

## Clock hardening (REFACTOR_SPEC §19)

Per-weekday rules used to be resolved against the **console's own clock**. That is not a
trustworthy clock: the server accepts skewed device timestamps by default
(`PLAYLOG_SIGNATURE_SKEW_MODE=warn`), and a user can change the console date/time. On
2026-10-19 a console whose RTC was 639 days behind made a Monday look like a Saturday, so
`[0,0,0,0,0,1800,1800]` ("no play Mon–Fri") silently became the 30-minute weekend quota.

`GET /switch/devices/config` therefore now also returns:

```json
{
  "server_local_date": "2026-10-19",
  "server_weekday": 0,
  "client_clock_skew_seconds": 55237344
}
```

and the sysmodule now drives the **whole day/week bookkeeping** from server time:

- After a successful, signature-verified config sync, `refresh_day_key()` adopts
  `server_local_date` as "today" for `SERVER_TIME_TRUST_SECONDS` (1 hour — far longer than
  the 90–300 s sync interval). The daily ledger (`limiter_state.csv`), weekly ledger
  (`limiter_week.csv`), the §16 server-usage floors and the `weekday_seconds[today]` index
  all follow the **server's** local date. Console RTC drift, manual date changes or a date
  jumping back and forth can no longer move the day/week window.
- `limits` / `account_limits` / `account_total_limits` already contain the server's
  today-effective seconds (the server resolves §15 against *its own* local date). Within the
  same window `rule_effective_daily()` returns those values instead of re-deriving them from
  the console weekday.
- Outside that window (long offline period) or when an old server omits
  `server_local_date`, the client falls back to the local clock + anti-rollback day guard —
  the previous behaviour, including the "weekday unknown → do not terminate" fail-safe.
- A trusted server date/value also counts as "weekday known": the old "can't determine the
  day → don't block" early-return is skipped when fresh server time exists.
- `server_local_date` and the clock skew are written to `limiter.log` (only when the date or
  the skew bucket changes) so a wrong console clock is visible in the logs:
  `server local date=2026-10-19 device local day=2025-01-18; day/week ledgers now follow server time`
  and `WARNING: device clock differs from server by 55237344 seconds; ...`.

### Mid-session account change (REFACTOR_SPEC §19.3)

The `block_launch` "never interrupt this session" freeze is scoped to one account. If the
current account changes while a title is running (`resolve_current_account()` goes from xb to
joey, or corrects a stale reading) the sysmodule treats it as a **new session**: it
re-freezes the §11/§16 entry decision for the new account and immediately runs
`enforce_limit_at_session_entry()`. So if the new account is already at/over its limit (or
blocked with a 0-minute day), `block_launch` intercepts right then instead of finishing the
whole session on the old account's decision. `kill` mode still terminates via the per-tick
decision. Log line: `mid-session account change <old> -> <new> ... re-evaluating limits`.

Known residual: the day rollover takes effect on the next successful config sync (≤ 5 minutes
with the current deployment), and a completely offline console with a wrong clock falls back
to the local-clock path (if its local date is behind the guard's server day, the day stays
`unknown` and per-day/per-weekday limits are skipped by fail-safe).

**This sysmodule never writes the system clock.** It only uses read-only libnx calls
(`timeInitialize()`, `timeGetCurrentTime()`, `timeToCalendarTimeWithMyRule()`) and never calls
`timeSetCurrentTime()` / `timeSetDeviceLocationName()` or any other setter; the server's
`server_local_date` is used only for this process's ledger day key / weekday decision / logs
and is never written back to the OS. (On Atmosphere, externally modifying the system time can
make the console reboot abnormally, so keeping the clock read-only is intentional.) Do **not**
sync/change the console clock as part of deploying this fix — it is not needed. Only the NRO's
on-screen "today effective" value and API-log file names still follow the console clock; they
do not affect enforcement.

## Push config

The foreground tool also writes:

```text
sdmc:/PlayLog/config.json
```

Format:

```json
{
  "server_base_url": "https://your-server.example.com",
  "device_id": "自动生成的 UUID 或硬件 hash",
  "device_id_type": "hardware_hash",
  "upload_token": "服务端生成的设备上传 token",
  "limit_reload_seconds": 30,
  "config_sync_seconds": 30,
  "upload_interval_seconds": 900,
  "heartbeat_interval_seconds": 300,
  "enforce_mode": "block_launch"
}
```

`config_sync_seconds` defaults to **30** seconds (the server sends the same default),
and is clamped to the 5–86400 range. Press `X` in the foreground tool to edit the
server base URL with the Switch keyboard.

`enforce_mode` is the §10 limit-enforcement mode (`block_launch` | `kill`). It is read from
this file at startup as a fallback, and overwritten with the server value on every config
sync (`GET /switch/devices/config` is authoritative). A missing or empty field means
`block_launch`, so older SD cards keep working unchanged.

Both binaries write `config.json` / `limits.json` with a temp-file + `rename()` so a
torn write can never leave a half-written file behind. If a read fails, the previous
in-memory values are kept — a failed read never resets the upload token or the limits.

## Runtime files

```text
sdmc:/PlayLog/limiter.log
sdmc:/PlayLog/api-YYYYMMDD.log           # §12.3 per-day API log (shared with the foreground NRO)
sdmc:/PlayLog/limiter_state.csv          # account_id,day,title_id,used_seconds
sdmc:/PlayLog/limiter_week.csv           # account_id,week,title_id,uncapped_seconds  (§11 uncapped weekly usage)
sdmc:/PlayLog/limiter_upload_state.csv   # …account_id,record_uid
sdmc:/PlayLog/limiter_guard.json         # day anti-rollback guard
sdmc:/PlayLog/cacert.pem                 # HTTPS CA bundle for the server certificate
```

`cacert.pem` must exist for any HTTPS sync/upload/heartbeat. When it is missing the
sysmodule logs a clear error and skips the request instead of silently continuing
without certificate verification.

## Heartbeat

The sysmodule posts a signed heartbeat to:

```text
/switch/devices/heartbeat
```

The interval is controlled by `heartbeat_interval_seconds` in `sdmc:/PlayLog/config.json`,
defaulting to 300 seconds. The heartbeat includes best-effort device status: battery and
charging fields, firmware display version, Atmosphere detection, SD/NAND user storage
space, network status, current foreground TitleID, the best-effort current account id,
the `focus` diagnostic object (see below), and sysmodule uptime.

The server can return one pending command in the heartbeat response. The sysmodule
supports `reboot` and `shutdown` via the power management service, but **only when the
response carries a valid HMAC signature**. An unsigned or mismatched response makes the
module refuse the destructive command and log it, rather than risking a forged reboot.
Screenshot capture is not implemented in the sysmodule.

Response signature scheme:

```text
X-PlayLog-Response-Signature: <hex>
<hex> = hex(HMAC-SHA256(upload_token, <raw response body bytes>))
```

## API log + server failure reasons (REFACTOR_SPEC §12; screen rule §13)

Device-facing endpoints (`/switch/*`, `/minecraft/*`) answer errors with a self-explaining body:

```json
{ "detail": "<human-readable reason, in Chinese, including what to do next>",
  "code": "<machine code>",
  "hint": "<one-sentence pure-ASCII explanation, <=110 chars>" }
```

The sysmodule has **no screen and no `printf` at all** — every message goes to a file. On every
non-2xx response it extracts `detail` (via `read_json_string_field`), writes it to **both**
`limiter.log` and the per-day API log, and also extracts the ASCII `code` (via
`extract_server_failure_code`) so the shared log line carries the machine-readable failure class.
When the body carries no `detail` the HTTP status text is used instead, so a failure can never
leave only a bare status code behind:

```text
config sync failed http=401 code=device.bad_signature detail=签名不匹配：设备 token 可能已重置，请在后台重置 token 后重新填入
auto upload failed http=422 code=request.invalid detail=…
heartbeat failed http=503 code=- detail=Service Unavailable
```

`hint` is not used by the sysmodule (nothing draws it); it exists for the foreground screen, which
must never print the Chinese `detail` because the libnx console bitmap font has no UTF-8 support.

Both binaries append to the **same** per-day file (short lines, so interleaving at worst
loses a line, never a whole record):

```text
sdmc:/PlayLog/api-YYYYMMDD.log
```

Line format (truncated at 240 bytes per line; `code` is ASCII, `-` when absent):

```text
YYYY-MM-DD HH:MM:SS [tag] METHOD /path -> HTTP <status> <code|-> <detail|OK>
```

| tag | source |
|---|---|
| `config` | `GET /switch/devices/config` (config sync) |
| `push` | `POST /switch/statistics/daily` (auto upload) |
| `heartbeat` | `POST /switch/devices/heartbeat` |
| `net` | curl-level failure (timeout/DNS/TLS; no HTTP code, logged as `HTTP 0`) |
| `write` | failed SD write — the `/path` field is the target file that failed |

Example (the file is UTF-8; the foreground viewer sanitizes `>= 0x80` to `?` when drawing):

```text
2026-10-14 08:12:20 [config] GET /switch/devices/config -> HTTP 200 - OK
2026-10-14 08:12:20 [config] GET /switch/devices/config -> HTTP 401 device.unknown_or_disabled 设备未注册或已停用：请在后台用该 device_id 绑定设备并填入新 token
2026-10-14 08:12:20 [net] POST /switch/statistics/daily -> HTTP 0 - Timeout was reached
2026-10-14 08:12:20 [write] WRITE sdmc:/PlayLog/limits.json -> HTTP 0 - 写盘失败
```

**Retention (7 days).** Every write removes "today-8"; at startup the module also sweeps
"today-8 … today-40" by computing the file names — no directory listing is needed. On the
very first run there is no log file at all; nothing is created until an API call (or a failed
write) actually happens, and the foreground viewer then reports `No log yet`. A logging failure
(a removed SD card, a full filesystem) is swallowed and can never break sync/upload/heartbeat.

`write_file_atomic()` is the single choke point for SD writes, so any failed config / limits /
state write is logged once with the `write` tag and the target path.

## Upload source tag (REFACTOR_SPEC §8)

Every game entry the sysmodule posts to `/switch/statistics/daily` carries
`"source": "sysmodule"`; the foreground `.nro` sends `"source": "nro_pdm"` (its numbers come
from the PDM system statistics). The server collapses the two sources per
`(game, local day)` by taking the **maximum** instead of the sum, which removes the
double-counting without regressing clients that omit the optional field.

## Foreground focus detection (REFACTOR_SPEC §9)

`pm:dmnt GetApplicationProcessId` only answers *"does the application process exist"*, so on
its own the module keeps charging playtime while the game sits on the HOME menu. Every 5
seconds the module therefore takes a best-effort focus snapshot:

- `pdmqryGetAvailablePlayEventRange()` + `pdmqryQueryAppletEvent()`
  (`switch/services/pdm.h`) — the system's own `in_focus` / `out_of_focus` transition log,
  i.e. the same log PDM uses for playtime. This is the only verified API that distinguishes
  "focused and being played" from "process merely exists".
- `pmdmntGetApplicationProcessId()` / `pminfoGetProgramId()` — process existence + TitleID.
- `apmGetPerformanceMode()` (`switch/services/apm.h`) — diagnostic signal only, it never
  decides the state.

Rejected APIs (checked in the installed headers, `applet.h` / `pm.h`):
`appletGetFocusState()` reports the **calling** process's focus state (the sysmodule is
`AppletType_None`, it never receives applet focus messages), and
`pmshellGetApplicationProcessIdForShell()` merely walks PM's process list, exactly like
`pm:dmnt` — neither says anything about focus.

| state | meaning | behaviour |
|---|---|---|
| `foreground` | newest PDM focus transition is our title's `in_focus`, or our title just launched | keep counting, enforce the limit |
| `background` | newest focus transition is another applet's `in_focus`, or our title's `out_of_focus` with no later `in_focus`, **and** the evidence is ≥ 60 s old and was confirmed twice | pause accounting and skip `pmshellTerminateProcess` |
| `suspect` | background evidence still too fresh, timestamp unusable, or only "another applet lost focus" records | keep counting (old behaviour) |
| `unknown` | no foreground application, `pdm:qry` unavailable, or the log read failed | keep counting (old behaviour) |

`unknown` and `suspect` are deliberately identical to the pre-§9 behaviour: a failed
detection can never silently stop accounting and can never trigger — or dodge — a kill on its
own. Only a confirmed `background` pauses, and it is re-evaluated every 5 seconds.

Heartbeat field:

```json
"focus": { "state": "background",
           "signals": { "app_pid": 123, "pdm_ready": true, "pdm_focus_event": 3,
                        "pdm_focus_ours": false, "pdm_ours_event": 3,
                        "evidence_age_s": 120, "apm_perf": 0 },
           "source": "heuristic" }
```

`source` follows the §9 enum (`pm_shell|apm|heuristic|none`): it is `heuristic` whenever the
verdict is derived from the PDM focus log (the frozen enum has no `pdm` value; the raw
evidence is itemised in `signals`) and `none` when no signal could be read. `signals` only
contains values that were actually read. Focus state changes are logged to
`limiter.log` (only on change, so the log stays quiet).

Still needs a real console to confirm:

- how quickly PDM flushes `in_focus` after a game regains focus (this bounds how long a
  return to the game can stay uncounted),
- whether `timestamp_user` really is PosixTime on the installed firmware (the code accepts
  both PosixTime and the legacy "minutes since 1999" form and falls back to
  `suspect`/`unknown` when it cannot interpret it),
- that `pdm:qry` and `apm` can actually be opened from a boot2 sysmodule on the target FW.

If the sysmodule causes boot problems, remove this folder from the SD card:

```text
sdmc:/atmosphere/contents/4200000000504C54
```

## Limit enforcement mode (REFACTOR_SPEC §10)

Hard-killing a game gives it no chance to save, so titles that only save on exit lose
progress. libnx offers no "please quit gracefully" API to a sysmodule, so the only real fix is
to **never kill mid-session** and intercept the *next launch* instead. That is what the
device-level `enforce_mode` selects:

| value | behaviour | progress loss |
|---|---|---|
| `block_launch` (**default**) | when the quota is already used up, the current session is **never interrupted** (the player can save and quit normally); the next launch of that title is terminated immediately, during the loading screen (~1 s) | none |
| `kill` | terminate as soon as the quota is reached (the pre-§10 behaviour) | possible for save-on-exit games |

How the module decides (1 s foreground poll):

1. When the foreground title changes it records the **used seconds at the moment the title
   became foreground** for that `(account, title, local day)` entry. This frozen value is the
   *only* `block_launch` criterion — using the live, growing counter would kill a session that
   was legitimately allowed to start, i.e. silently degrade to `kill`.
2. `block_launch`:
   - the §11 OR rule (daily / weekday / weekly) is already violated at entry → terminate right
     away (launch interception; the check runs on the first poll after the title becomes
     foreground, normally still inside the loading screen).
   - not violated at entry → **this session is never terminated**, even if the quota (daily or
     weekly) is exceeded mid-session. The overrun is logged once and accounting continues untouched.
3. `kill`: unchanged — violation of the §11 OR rule terminates immediately, exactly as before.
   Note that with a weekly cap this can now happen even while the day's own cap is not reached.
4. **Anything uncertain means do not terminate**: the effective rule cannot be read, the
   foreground process query fails, the ledger entry cannot be obtained, the local weekday cannot
   be derived, the weekly usage cannot be computed when the rule has a weekly cap, or the
   configured `enforce_mode` string is not one of the two known values (`unknown` → never kill).
   The rule is "rather miss a kill than kill by mistake".
5. The §16 account-wide "all games combined" limit is OR-ed into the same flow: session entry
   freezes whether the account total was already at/over its limit, and the per-tick path also
   evaluates it. `block_launch` intercepts at entry, `kill` terminates live — but only for a
   **positively determined** violation. If the account is unknown, has no total rule, the
   weekday cannot be derived, or neither the server floor nor a ledger entry exists, the module
   does not terminate. This never loosens the per-title decision; the two are independent and
   the caller terminates when **either** says so. `should_terminate_now()` itself is unchanged.

Details worth knowing:

- A `0`-second limit is a *valid* limit ("0 blocks play") and is still intercepted, both in
  `block_launch` (at launch, because `entry_used = 0 >= 0`) and in `kill`. Never treat
  "limit == 0" as "no limit"/"unreadable". An explicitly configured all-zero rule keeps this
  legacy meaning (see the §11 section above and the contract note there).
- The rule is evaluated every tick (not only once the daily cap is reached), because the weekly
  cap can be hit while the day's own cap is still untouched ("whichever cap is reached first").
- The mode is taken from `GET /switch/devices/config` (authoritative) and persisted back into
  `config.json`; on an offline boot the `config.json` value is the fallback, and a missing or
  empty field means `block_launch`.
- Mode changes are logged (`enforce_mode effective=…`), as is every launch interception
  (`block_launch launch interception …`, with the winning `source` = `daily`/`weekday`/`weekly`),
  every allowed mid-session overrun, and every "indeterminate → not terminating" decision.
- Sessions are closed when no application is in the foreground; running out of the
  `limiter_state.csv` ledger (unreadable file) can only make the module *more* lenient, never
  trigger a kill. The same applies to the §11 `limiter_week.csv` ledger.

## Current behavior

The module polls the foreground Application every 1 second with `pm:dmnt`, resolves the
TitleID with `pm:info`, and terminates the process with `pm:shell` according to the configured
`enforce_mode` (see above): `kill` terminates as soon as the limit is reached, `block_launch`
only intercepts a launch whose quota was already exhausted. A best-effort focus snapshot (see
above) decides whether the game is actually in the foreground: only a confirmed `background`
pauses the accounting and the limit enforcement, everything else keeps the old behaviour.

If the local system date is invalid, the day key becomes `unknown`; in that mode it keeps
accumulating instead of resetting unexpectedly.

Daily-quota reset is guarded against clock tampering (the guard lives in
`limiter_guard.json`):

- Moving the date **backwards** never resets the quota — the module keeps charging the
  already-seen day.
- Accepting a **later** date requires either 24 h of monotonic running time, or a 24 h
  wall-clock advance since the last accepted day. So winding the clock forward a few hours
  to reach "tomorrow" does not hand out a fresh quota; each extra day costs a real day.
- A device that was switched off for several days still resets normally, because the wall
  clock has genuinely advanced by more than 24 h.

