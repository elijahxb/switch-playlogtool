# PlayLog Server Design

## Upload Authentication

Use HMAC-SHA256, not reversible encryption.

Client config:

```json
{
  "server_base_url": "https://your-server.example.com",
  "device_id": "client-generated-uuid",
  "device_id_type": "random_uuid",
  "upload_token": "server-generated-shared-secret"
}
```

`server_base_url` is the origin only; the client appends `/switch/statistics/daily`,
`/switch/devices/config` and `/switch/devices/heartbeat` itself. (There is no
`push_url` key — it was removed and must not be reintroduced.)

Request headers:

```text
Authorization: PlayLog-HMAC-SHA256 device_id="<device_id>",timestamp="<unix_seconds>",signature="<hex_hmac>"
X-PlayLog-Device-Id: <device_id>
X-PlayLog-Timestamp: <unix_seconds>
X-PlayLog-Signature: <hex_hmac>
Content-Type: application/json
```

Canonical string:

```text
POST
/switch/statistics/daily
<device_id>
<timestamp>
<sha256_hex(request_body)>
```

Signature:

```text
hex(HMAC-SHA256(upload_token, canonical_string))
```

Server verification:

1. Parse `device_id`, `timestamp`, and `signature`.
2. Reject if timestamp is outside a small window, such as 300 seconds.
3. Look up the active device by `device_id`.
4. Compute the same canonical string from the actual request body.
5. Compare signatures with constant-time comparison.
6. Reject unknown, disabled, or unbound devices.

Optional replay hardening:

- Store accepted `record_uid` values with a unique constraint.
- Add a request nonce later if non-idempotent endpoints are introduced.

## Response Signatures

The client verifies an HMAC on responses it acts on (config download and heartbeat
commands). The scheme is:

```text
X-PlayLog-Response-Signature: <hex>
<hex> = hex(HMAC-SHA256(upload_token, <raw response body bytes>))
```

The signature covers the response body exactly as sent, byte for byte; no JSON
re-serialisation is performed on either side. The client behaviour is:

- Destructive commands (`reboot`, `shutdown`) are **refused** when the header is missing
  or does not match — fail closed.
- Overwriting the cached `limits.json` from `GET /switch/devices/config` still happens when
  the header is missing, but the client logs a loud warning that the update is unverified;
  a present-but-mismatched signature is rejected outright.

## Identity And Accounts

Use OIDC for account login.

Core entities:

```text
users
  id
  oidc_issuer
  oidc_subject
  email
  display_name
  created_at

switch_devices
  id
  owner_user_id
  device_id
  alias
  upload_token_hash
  token_last4
  enabled
  created_at
  updated_at

games
  id
  title_id
  name
  first_seen_at
  last_seen_at

device_games
  device_id
  game_id
  custom_name
  first_seen_at
  last_seen_at

play_sessions
  id
  record_uid
  switch_device_id
  game_id
  delta_seconds
  started_at
  ended_at
  received_at

game_limits
  id
  switch_device_id
  game_id
  daily_limit_seconds
  weekly_limit_seconds        -- §11, seconds, 0 = unlimited
  weekday_limit_seconds       -- §11, JSONB array of 7 seconds, index 0 = Monday
  enabled
  updated_at

account_total_limits
  id
  switch_account_id           -- §16, UNIQUE: at most one total limit per account
  daily_limit_seconds         -- §16, seconds, 0 = no play (when enabled)
  weekday_limit_seconds       -- §16, JSONB: [] or exactly 7 seconds, index 0 = Monday
  enabled                     -- §16, false = unrestricted and not dispatched
  updated_at

device_heartbeats
  id
  switch_device_id
  client_name
  client_version
  uptime_seconds
  foreground_title_id
  firmware_version
  firmware_display_version
  atmosphere_detected
  atmosphere_version
  battery_percent
  battery_charging
  sd_total_bytes
  sd_free_bytes
  nand_user_total_bytes
  nand_user_free_bytes
  internet_connection_status
  wifi_strength
  payload
  received_at

device_commands
  id
  switch_device_id
  action
  status
  payload
  result
  created_at
  dispatched_at
  completed_at
```

If player profiles are needed later:

```text
players
  id
  switch_device_id
  alias
  external_profile_id

play_sessions.player_id nullable
game_limits.player_id nullable
```

The client now reports an optional `account_id` (32 hex chars, the Switch `AccountUid`)
and `account_name` per game entry. Entries without `account_id` remain device-level, and
the server must keep treating them exactly as before. Per-account game limits live in
`account_limits[account_id][title_id]` on the device config response; the effective limit
is `account_limits[account][title]` → `limits[title]`.

The client cannot always determine which account is currently playing (the Switch play
statistics for an account are only flushed when the title exits), so a client that reports
device-level entries is still fully conforming.

### Upload source tag (REFACTOR_SPEC §8)

The foreground `.nro` and the background sysmodule both report the same real playtime with
different bookkeeping, so every `games[]` entry carries an optional

```json
"source": "nro_pdm"   // foreground NRO, numbers from PDM system statistics
"source": "sysmodule" // background sysmodule, numbers from its own foreground accounting
```

The value is at most 32 characters; a missing/`""` field means "legacy, unknown source". For
every `(game, local day)` the server must aggregate each source separately and then take the
**maximum**, never the sum. That removes the double count and degrades to the single
available source when the other one is absent; records with `source = ""` behave exactly as
before. The rule applies to `day-timeline`, `weekly-summary`, `game-share`,
`limit-progress`, `month-heatmap`, `series`, the per-account variants, and the historical
over-limit reconstruction. Cumulative totals (`device_game.client_total_seconds`) are
unaffected.

### Heartbeat focus diagnostics (REFACTOR_SPEC §9)

The sysmodule cannot see "the application is focused" from `pm:*` alone (`pm:dmnt` /
`pm:shell` only report whether the process exists), so it derives a best-effort focus state
from the PDM applet focus log and reports it in the heartbeat:

```json
"focus": {
  "state": "foreground|background|suspect|unknown",
  "signals": { "app_pid": 123, "pdm_ready": true, "pdm_focus_event": 2,
               "pdm_focus_ours": true, "pdm_ours_event": 2,
               "evidence_age_s": 12, "apm_perf": 1 },
  "source": "pm_shell|apm|heuristic|none"
}
```

The server accepts the optional object, stores it verbatim in `device_heartbeats.payload`
(no new column) and otherwise ignores it. `suspect` and `unknown` are *not* failures to
report on: the client keeps its normal accounting and limit enforcement in those states, and
only a confirmed `background` pauses them. The object is small (it is sent every few
minutes) and only contains signals that were actually read; keys may be absent.

### Limit enforcement mode (REFACTOR_SPEC §10)

`switch_device.enforce_mode` (`VARCHAR(20)`, default `block_launch`) tells the sysmodule how to
enforce an exhausted daily quota:

```json
"enforce_mode": "block_launch"   // do not interrupt the running session; kill the next launch
"enforce_mode": "kill"           // terminate as soon as the quota is reached (pre-§10 behaviour)
```

It is returned by `GET /switch/devices/config` next to `limits` / `account_limits` (old clients
ignore the extra field), and is writable through `PATCH /api/me/devices/{device_id}/settings`
(session auth + ownership) and the admin-token equivalents `PATCH /api/devices/{device_id}` /
`PATCH /api/admin/devices/{device_id}`. Only those two values are accepted; anything else is
rejected with HTTP 422 by the request schema. Existing rows and new devices get the
`block_launch` default from the migration/model default, so no data backfill is needed.

### Weekly total + per-weekday limits (REFACTOR_SPEC §11 / §15)

Besides the existing per-game daily cap, a game can carry a **weekly total cap** and a
**different cap for each weekday**. The config response grows two additive keys (all values in
**seconds**), so old clients keep working off `limits` / `account_limits` alone:

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
                                                            "weekday_seconds": [] } } },
  "enforce_mode": "block_launch"
}
```

- `weekday_seconds` index **0 = Monday … 6 = Sunday** (matches Python `date.weekday()`).
  §15 makes the two encodings unambiguous, and the client mirrors them exactly:
  - **`[]`** (or field missing) = **per-weekday not configured** → the day falls back to
    `daily_seconds`; the server must **not** dispatch `[0]*7` for this case any more;
  - **exactly 7 values** = **per-weekday configured** → the day's value applies verbatim,
    **including `0` (= no play that day)**.
  `[0]*7` therefore means "blocked every day", not "unset" — an old server sending 7 zeros for
  "unconfigured" is read as "blocked every day", which §15.1 accepts.
- `weekly_seconds` is the weekly total cap; `0` = unlimited. The week starts **Monday 00:00 in
  the app timezone (`PLAYLOG_TIMEZONE` / `APP_TZ`)** and is a half-open
  `[Monday 00:00, next Monday 00:00)` window, so Sunday 23:59 still belongs to the ending week.
- Effective daily cap: `effective_daily = weekday_seconds[today] if has_weekday_override(raw)
  else daily_seconds`, where `has_weekday_override(raw)` is true iff `raw` is a list/tuple of
  exactly 7 elements.
- **Block when either cap is reached** (`effective_daily == 0`, or `today_used >= effective_daily`,
  **or** `week_used >= weekly_seconds` when `weekly_seconds > 0`) — an **OR / ceiling** rule, not
  an AND. The weekly cap is an extra ceiling and is never subtracted from the daily allowance.
- `limits` / `account_limits` still carry **today's effective daily limit** (the weekday override
  already applied), so a client that only understands the old keys behaves correctly and, for
  rows without weekday overrides, byte-identically to before.
- Storage: `game_limit`, `account_game_limit` and `game_limit_history` each gain
  `weekly_limit_seconds INT DEFAULT 0` and `weekday_limit_seconds JSONB DEFAULT '[]'`
  (migration `13_…`), with defaults so pre-existing rows stay valid.
- Upserts (`PUT …/limits/{title_id}`) accept `weekly_minutes` (0…10080) and `weekday_minutes`
  (`[]` or exactly 7 values, each 0…1440), or the second-based aliases `weekly_limit_seconds`
  (0…604800) / `weekday_limit_seconds` (7 × 0…86400). Anything else is rejected with HTTP 422 by
  Pydantic.
- `reports/limit-progress` rows gain `effective_daily_seconds`, `weekly_used_seconds`,
  `weekly_limit_seconds`, `weekday_limit_seconds` and `limit_source`
  (`daily` | `weekday` | `weekly` | `none`, i.e. which cap won); `status` is `over` whenever
  `limit_source != "none"`. The weekly usage is computed from the **uncapped real usage**: each
  `(game, local day)` is deduplicated across sources with the §8 max rule and then summed over
  Monday → today, which is exactly what the client reports (the server never truncates a delta
  on ingest).

The client side of §11 lives in `../play-log-tool/README.md` and
`../play-log-tool/sysmodule/README.md`: the sysmodule keeps a separate uncapped weekly ledger
(`limiter_week.csv`, keyed by account + title + Monday-of-week) because its daily ledger only
keeps the **current day**, so it cannot be summed into a week total (REFACTOR_SPEC §14.2.3
keeps the uncapped ledger as the single source for weekly caps).

### Account-wide "all games combined" limit (REFACTOR_SPEC §16)

Per-title caps alone let a child finish one game's quota and switch to another. §16 adds a
**per-account cap on the sum of playtime across every title**, also selectable as one value
every day or as a per-weekday rule. The config response grows two additive keys:

```json
{
  "account_total_limit_rules": { "1f2a…": { "daily_seconds": 3600, "weekday_seconds": [] } },
  "account_used_today_total":  { "1f2a…": 2699 }
}
```

- `account_total_limit_rules` is keyed by `account_id` and only contains `enabled=true` rows.
  `weekday_seconds` obeys the **same §15 rule** as `limit_rules`: `[]` / missing / any length
  other than 7 = per-weekday not configured (fall back to `daily_seconds`); exactly 7 values =
  configured, index 0 = Monday … 6 = Sunday, and that day's `0` means **no play that day**.
  `[0,0,0,0,0,3600,3600]` = no play Mon–Fri, 60 minutes per weekend day.
- `account_used_today_total` is the account's today total across all titles, using the same
  dashboard basis: `(account, game, local day)` deduplicated across sources with the §8 max
  rule, then summed across games. The client adopts it as the **floor** of its local sum
  (`total = max(local_sum, server_floor)`, raise-only — the §14.3 principle). Missing / empty
  keys simply leave the local sum in charge.
- Storage: a new `account_total_limit` table, **at most one row per account**, with
  `daily_limit_seconds INT`, `weekday_limit_seconds JSONB DEFAULT '[]'`, `enabled BOOL`;
  `enabled=false` rows are not dispatched at all.
- The client-side enforcement (OR-ed with the per-title rule) is documented in
  `sysmodule/README.md`; the server contract is frozen in `REFACTOR_SPEC.md` §16.

### Daily ledger counts real time (REFACTOR_SPEC §14)

§14 fixes a production bug where the sysmodule's per-(account, title, local day) daily ledger
was only incremented in the "a limit rule exists" branch, and was then clamped at the daily
cap. The client now always accumulates the real elapsed time into the daily ledger (with or
without a rule) and always queues the real elapsed time for upload, even after the cap is
reached. Consequences for the server contract:

- `POST /switch/statistics/daily` deltas from `source = "sysmodule"` now keep growing past the
  daily cap instead of stopping at it, so a dashboard "today" number must never be assumed to
  be frozen at the limit; it is the real day total.
- The §8 per-source **max** dedup rule is unchanged and is what keeps the two sources from
  double counting; because the sysmodule delta is no longer clamped, the max rule is now
  comparing real totals on both sides.
- §14.3 invariant: the client's local daily ledger should be ≈ the server's accumulated
  "today" for the same `(account, game, local day)`. Any `local < server-accumulated` case
  means over-limit enforcement is too lenient.
- The client logs `§14 session entry title=… used_today=… limit=… over=…` on every title
  entry (`limiter.log`) so support can compare the local ledger against the server value.

## Device-facing error responses (REFACTOR_SPEC §12, screen rule §13)

Every **device-facing** endpoint (`/switch/*`, `/minecraft/*`) must answer an error with a
self-explaining body:

```json
{ "detail": "<human-readable Chinese reason, including what to do next>",
  "code": "<machine code>",
  "hint": "<one-sentence pure-ASCII explanation, <=110 chars>" }
```

- The HTTP status code keeps its normal meaning (401 / 404 / 422 / 5xx) — the body is additive.
- `detail` must be actionable ("设备未注册或已停用：请在后台用该 device_id 绑定设备并填入新 token"),
  never a bare English noun like "Unauthorized".
- `code` is an optional machine-readable classifier (absent = unclassified).
- `hint` is ASCII-only and comes from the server's per-`code` `ERROR_HINTS` table. It exists because
  **libnx's console is a bitmap font with no UTF-8 support** (`ConsoleFont{asciiOffset,numChars,...}`):
  `printf`-ing Chinese is guaranteed mojibake. The `.nro` screen therefore prints `HTTP <status> <code>`
  plus `hint` (wrapped), and the Chinese `detail` only ever goes to the log file. When `hint` is absent
  the client falls back to the HTTP status text.
- `/api/*` and `/auth/*` keep their existing body shape (the web frontend relies on `detail`).

Required device codes:

| code | HTTP | detail |
|---|---|---|
| `device.missing_signature` | 401 | 缺少签名头：请检查客户端版本与配置 |
| `device.bad_timestamp` | 401 | 时间戳格式非法：主机时间可能未就绪 |
| `device.skew_too_large` | 401 | 主机时间与服务端相差过大（严格模式已开启） |
| `device.unknown_or_disabled` | 401 | 设备未注册或已停用：请在后台用该 device_id 绑定设备并填入新 token |
| `device.bad_signature` | 401 | 签名不匹配：设备 token 可能已重置，请在后台重置 token 后重新填入 |
| `server.unknown_or_disabled` | 401 | same text as `device.unknown_or_disabled` (Minecraft collector credentials) |
| `server.bad_signature` | 401 | same text as `device.bad_signature` (Minecraft collector credentials) |
| `request.invalid` | 422 | a flattened, single-sentence rendering of the validation errors |

### How the clients consume it

Both Switch binaries read `detail` out of the error body (`readJsonStringField` /
`read_json_string_field`) and fall back to the HTTP status text when it is missing. The `.nro`
additionally reads `code` and `hint` through the same helper (`extractServerFailureCode` /
`extractServerFailureHint`):

- the foreground `.nro` **never prints `detail`**: on non-2xx it shows
  `HTTP <status> <code>:` followed by the ASCII `hint`, word-wrapped to the 76-column hint width
  (at most 3 lines, ellipsis on overflow). Missing `hint` → HTTP status text; missing `code` → `-`;
- the sysmodule has no screen (and no `printf` at all), so it writes `detail` into `limiter.log`
  and into the shared per-day API log, and puts `code` into both log lines;
- the on-console log viewer ASCII-sanitizes every drawn line (`>= 0x80` → `?`), so the file stays
  UTF-8 while the screen stays readable.

The per-day log lives at `sdmc:/PlayLog/api-YYYYMMDD.log` (one file per local day, appended by
**both** binaries, 240 bytes per line max, 7-day retention by file name) with the format:

```text
YYYY-MM-DD HH:MM:SS [tag] METHOD /path -> HTTP <status> <code|-> <detail|OK>
```

`code` precedes the (possibly Chinese) `detail` so the sanitized screen line still shows the
failure class. `tag` is one of `config` | `push` | `heartbeat` | `write` (failed SD write, target
path in the `/path` field) | `net` (curl-level failure, `HTTP 0`). The foreground `.nro` exposes
the newest day through an on-console viewer (`L` in the main menu). See
`../play-log-tool/README.md` and `../play-log-tool/sysmodule/README.md`.

## APIs

Public upload:

```text
POST /switch/statistics/daily
POST /switch/devices/heartbeat
```

Heartbeat responses may include one pending command. Supported terminal actions are
`reboot` and `shutdown`; screenshot capture is intentionally not part of the first version.

Body:

```json
{
  "games": [
    {
      "record_uid": "uuid",
      "title_id": "0100D71004694000",
      "name": "Game Name",
      "source": "sysmodule",
      "delta_seconds": 900,
      "started_at": "2026-07-18T19:00:00",
      "ended_at": "2026-07-18T19:15:00"
    }
  ]
}
```

Authenticated web/API:

```text
GET    /api/me
GET    /api/devices
POST   /api/devices
PATCH  /api/devices/:id
POST   /api/devices/:id/rotate-token
GET    /api/devices/:id/games
GET    /api/devices/:id/playtime?range=today|week|month|all
GET    /api/devices/:id/charts/summary
GET    /api/devices/:id/limits
PUT    /api/devices/:id/limits/:game_id
DELETE /api/devices/:id/limits/:game_id
```

Manual device registration flow:

1. User opens PlayLogTool and reads `device_id`.
2. User logs in to the web app with OIDC.
3. User creates a device with that `device_id` and alias.
4. Server generates an upload token.
5. User enters token in PlayLogTool.
6. Uploads are accepted after HMAC verification.

## Dashboard

Pages:

- Devices: registered devices, alias, enabled status, rotate token.
- Games: title ID, name, total playtime, last played.
- Today: total playtime today, per-game breakdown.
- Week: daily stacked bar by game.
- Month: calendar or daily trend.
- All time: top games and cumulative hours.
- Limits: per-game daily limits, enabled/disabled.

Main metrics:

```text
today_total_seconds
week_total_seconds
month_total_seconds
all_time_total_seconds
top_games_by_range
daily_playtime_by_game
```

## Notes

- Store `upload_token_hash`, not plaintext, if you can still verify by retaining an encrypted copy or a keyed hash strategy. For straightforward HMAC verification, the server needs access to the secret. A practical approach is envelope encryption: encrypt the token at rest with a server-side KMS/app secret, and expose only `token_last4`.
- Always enforce unique `record_uid` to make client retries idempotent.
- The client should use HTTPS only in production.
