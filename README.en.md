# PlayLogTool

[简体中文](README.md) | **English**

Playtime tracking and parental-control tooling for the Nintendo Switch. It consists of a foreground homebrew (NRO) and a background sysmodule, working with the `play-log-tool-server` backend to track playtime and enforce account/game limits.

> Technical specification: [docs/CLIENT_SPEC.md](docs/CLIENT_SPEC.md). Troubleshooting notes: [AGENTS.md](AGENTS.md).

---

## Overview

- **PlayLogTool.nro (foreground)**: shows playtime and limits per game/account, syncs configuration from the server. Read-only while online; when offline, local editing depends on a server-controlled flag.
- **PlayLogLimiter (sysmodule)**: runs in the background, tracks and uploads playtime, and enforces limits pushed by the server.
- **Server integration**:
  - uploads daily playtime (per account, per game);
  - downloads limit rules (daily / weekly / per-weekday), the authoritative server time, and the offline-edit policy;
  - supports three limit scopes: **game limit (device-wide)**, **account + game**, and **account total (all games)**.
- UI supports **Chinese / English** (persisted on the SD card). The log viewer, push/sync flow, and API error messages are localized.

## Project layout

```
source/           foreground NRO (UI, config sync, uploads)
sysmodule/        background sysmodule (tracking, enforcement, heartbeat)
limits_common.h   shared constants and limit decision logic
fonts/            CJK font (Noto Sans SC)
docs/             design documents
AGENTS.md         development / troubleshooting notes
Makefile          foreground NRO build
```

## Development environment

### 1. Toolchain

- [devkitPro](https://devkitpro.org/) (devkitA64 + libnx)
- portlibs: `switch-freetype`, `switch-harfbuzz`, `switch-libpng`, `switch-bzip2`
- libs: `curl`, `mbedtls`

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH
```

> ⚠️ The UI uses **libnx framebuffer + freetype**. Do **not** link SDL2 / mesa / EGL (black screens on older CFW; see AGENTS.md).

### 2. Build

```bash
# foreground NRO
make clean && make
# output: PlayLogTool.nro

# background sysmodule
cd sysmodule
make package
# output: sysmodule/package/atmosphere/contents/4200000000504C54/exefs.nsp
```

### 3. Deploy to the Switch (SD card)

| File | Target |
|---|---|
| `PlayLogTool.nro` | `SD:/switch/PlayLogTool.nro` |
| `sysmodule/package/atmosphere/contents/4200000000504C54/exefs.nsp` | same path (overwrite) |
| `fonts/PlayLogFont.otf` | `SD:/PlayLog/PlayLogFont.otf` |

- Reboot the console after replacing the sysmodule;
- In the NRO, open **Settings** and fill in the **server URL** and **upload token**.

### 4. Backend (optional)

Companion repo: `play-log-tool-server` (FastAPI + PostgreSQL). Devices pull configuration via the signed `GET /switch/devices/config` endpoint and upload playtime via `POST /switch/statistics/daily`. Without a backend, the NRO can work offline (local editing is subject to the server policy).

## Usage

### Buttons

| Button | Action |
|---|---|
| Up / Down | Select game (stick supported) |
| Left / Right | Switch account |
| ZL / ZR | Page the game list (±10) |
| A | Set limit (offline and only if allowed by the server) |
| Y | Clear limit (same conditions) |
| R | Build report and push |
| L | API log viewer |
| - | Cloud sync (pull configuration) |
| X | Settings: server URL / upload token / intervals / language |
| + | Upload-token shortcut |
| B | Back / exit |

### Limit scopes

| Scope | Meaning |
|---|---|
| Game limit (device-wide) | One rule for the game applied to **each account independently** (not a shared total across accounts) |
| Account + game | Limit for one account on one game (takes precedence over the device-wide rule) |
| Account total | Total across all games for one account (OR with per-game limits) |

- Daily, weekly and per-weekday (Mon–Sun) rules are supported;
- Enforcement is done by the sysmodule; the NRO only displays limits (online) or edits local rules (offline).

### Online / Offline

- On launch or after pressing `-`, the client pulls configuration from the server: success = **Online (read-only)**;
- Failure = **Offline**:
  - server `allow_offline_edit=true` → editing allowed (A/Y appear);
  - `false` (default) → read-only even offline.
- The UI language is stored in `SD:/PlayLog/config.json` (`ui_lang`) and applies immediately.

### Logs and uploads

- `L` shows the API log (`SD:/PlayLog/api-YYYYMMDD.log`, UTF-8);
- `R` builds a report and pushes it; the sysmodule also uploads periodically;
- sysmodule log: `SD:/PlayLog/limiter.log`.

## Notes

- **Do not rely on the Switch system clock**: the RTC can be far off; date/weekday logic is driven by the server time.
- **Never use the online system update in CFW** (may burn fuses and break the setup).
- Do not use SDL2/mesa for the foreground UI, and do not set `__nx_fs_num_sessions=16` (causes startup hangs).
- See [AGENTS.md](AGENTS.md) for black-screen/ABI/font/deployment troubleshooting.

## Star

If this project is useful to you, please consider giving it a **Star** ⭐

[![GitHub stars](https://img.shields.io/github/stars/elijahxb/switch-playlogtool?style=social)](https://github.com/elijahxb/switch-playlogtool)

[![Star History Chart](https://api.star-history.com/svg?repos=elijahxb/switch-playlogtool&type=Date)](https://star-history.com/#elijahxb/switch-playlogtool&Date)
