# PlayLogTool 开发与排障指南（血泪经验）

本文件记录这个项目（Switch 端 PlayLogTool + sysmodule）在真机上踩过的坑、根因和验证方法。
**改任何客户端相关代码前先读完“黑屏排查”和“已确认根因”，避免重复踩坑。**

---

## 0. 真机环境（重要前提）

- 机型：Mariko（续航/OLED）**hwfly / SX Core 类硬破（modchip）**。
- 系统：**真实系统 sysMMC**（无 emuMMC），固件 **20.5.0**，Atmosphere **1.12.1**，hekate **6.5.4**。
- 引导链：`boot.dat` / `boot.ini`（hwfly）→ `payload.bin`（hekate）→ `autoboot=2` 直接进真实系统。
- **Switch 的 RTC 长期偏差约 639 天（设备时间可能是 2025-02-xx），且不要试图修设备时间**：
  改时间/联网对时在这台机器上可能导致异常重启。客户端**绝不能依赖设备时间**做日期/星期判断。
- 设备文件服务（只读）：`http://<switch-lan-ip>:1234/`（SD 卡根目录，可读 `PlayLog/`、`atmosphere/crash_reports/` 等）。
- 本机发布服务：`http://<dev-lan-ip>:8000/`（`python3 -m http.server` 于 `/tmp/playlog-deploy`），给 Switch/其他电脑下载 NRO/字体。
- 文件服务不支持 PUT/上传（返回 501），部署需手动拷贝或换工具。

---

## 1. 黑屏 / 崩溃排查黄金流程（先证据，后改动）

1. **先看崩溃报告**：`SD:/atmosphere/crash_reports/` 最新 `<timestamp>_010000000000100d.log`。
   - 注意 `Process Name: hbloader` 是正常的（NRO 在 hbloader 里运行）。
   - 看 `Result`、`Type`、`PC`、`LR`、`ReturnAddress[...]`、`Fault Address`。
2. **用 addr2line 对本地 ELF 定位**（NRO 的 offset 是相对模块基址）：
   ```bash
   /opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line -f -C -e PlayLogTool.elf 0x98e8 0x16a40 0xc4
   ```
   之前多次崩溃都指向 `__libnx_init_cwd → fsdev_chdir`（`fs_dev.c:1228`，Fault 0x118）。
3. **看 `SD:/PlayLog/boot.log`**（NRO 的早期引导日志）：
   ```
   ctor101            ← 库构造函数之前
   ctor200            ← 库构造函数之后
   main enter         ← main 开始
   appInit done mount=0x0  ← 自定义 __appInit 完成、fsdevMountSdmc 成功
   ```
   - 只有 `ctor101/ctor200` 没有 `main enter` → 卡在库构造函数或 main 之前；
   - 连 `ctor101` 都没有 → 卡在 libnx 初始化（`__appInit`）；
   - `appInit done mount=0x...` 非 0 → SD 挂载失败。
4. **看 `SD:/PlayLog/ui.log`**（framebuffer/freetype）：
   ```
   uiInit start
   framebuffer ok
   font loaded: sdmc:/PlayLog/PlayLogFont.otf
   ```
5. **无日志 + 无崩溃 + 黑屏**：说明卡在 `main` 之前的初始化（常见于 `__nx_fs_num_sessions` 或库构造函数）。
6. **不要盲改**。先加日志或拿崩溃栈，确认卡点后再改。

---

## 2. 已确认的根因（按历史出现顺序）

### 2.1 最终黑屏根因：`__nx_fs_num_sessions = 16`（必看）

- 曾经在 `source/main.c` 里写了：
  ```c
  u32 __nx_fs_num_sessions = 16;
  ```
  以为“多给 fs session 更保险”。
- 实际结果：libnx 4.12 的 NRO 在固件 20.5.0 上申请 16 个 fs session 会让 `fsInitialize`
  **卡死在 main 之前**：黑屏、无 boot.log、无崩溃报告。
- **修复：删除这个全局定义，使用 libnx 默认值（3）。**
- 这是“所有 NRO 都黑屏”的真正最后一环；ConsoleTest（不设置该值）能跑就是最直接的对照。

### 2.2 libnx 4.12 的 `__libnx_init_cwd` 崩溃

- 现象：崩溃报告 `Data Abort`，PC/LR 指向
  `__libnx_init_cwd → fsdev_chdir`（`fs_dev.c:1228`），`Fault Address 0x118`（`deviceData` 为空）。
- 原因：libnx 4.12 默认 `__appInit` 会 `fsdevMountSdmc()` 后调用 `__libnx_init_cwd()`
  （chdir 到 NRO 目录），在 hbloader/该固件组合下会崩。
- **修复：在 `source/main.c` 自定义 `__appInit`，按顺序初始化但跳过 `__libnx_init_cwd()`：**
  `smInitialize → setsys/hosversion → fsInitialize → fsdevMountSdmc → appletInitialize → hidInitialize → timeInitialize → __libnx_init_time`
  （PlayLogTool 全部使用绝对路径 `sdmc:/...`，不需要 cwd）。
- 参考实现就在 `source/main.c` 的 `__appInit()`；不要删掉它，也不要再加 `__libnx_init_cwd()`。

### 2.3 不要用 SDL2 / SDL2_ttf / mesa / EGL / GLES 做 NRO UI

- 历史尝试：SDL2 + SDL2_ttf（+ freetype/harfbuzz + mesa/EGL/GLESv2）静态链接。
- 结果：从 Atmosphere 1.7 → 1.12.1、libnx 4.9 / 4.12 全部失败：
  - 有的卡在 libnx 初始化；
  - 有的卡在 SDL/mesa 的全局构造函数（`main` 之前），无 boot.log、无崩溃；
  - 黑屏、按 Home 白屏。
- **结论：这台设备/这套旧 CFW 上不要再用 SDL2/mesa 路线。**
- **当前方案：libnx framebuffer + freetype 自绘。**
  - 头文件：`<switch/display/framebuffer.h>`、`<switch/display/native_window.h>`（含在 `<switch.h>` 里）；
  - 初始化：
    ```c
    Framebuffer fb;
    Result rc = framebufferCreate(&fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&fb);   // 线性 shadow buffer，才能按 y*stride + x*4 直接写像素
    ```
  - 每帧 `framebufferBegin(&fb,&stride)` 拿线性 buffer → 绘制 → `framebufferEnd(&fb)`。
  - 文本用 freetype（见 §4），NRO 体积从 8.8MB 降到 ~2.3MB。

### 2.4 字体

- SD 上字体实际文件是：`SD:/PlayLog/PlayLogFont.otf`（Noto Sans SC，约 8.3MB）。
- 客户端字体查找顺序必须兼容：`font.otf`、`font.ttf`、`PlayLogFont.otf`、`switch/PlayLogFont.otf`。
- freetype 渲染注意：UTF-8 解码、字形 bitmap 缓存（否则每帧重渲染很卡）、帧率限制（`svcSleepThread(16ms)`）。

### 2.5 时间绝不能依赖 Switch

- 设备 RTC 偏差约 639 天，且不能改。
- sysmodule 侧已实现“服务端时间驱动日/周账本”（`server_local_date` / `server_weekday`，§19）。
- 客户端/服务端判断“今天/星期几”必须使用服务端下发的日期，不要用 `time()/localtime()` 的结果做最终决策。

---

## 3. 构建（NRO）

- 工具链：`/opt/devkitpro`（devkitA64 / libnx 4.12 / portlibs）。
- Makefile 关键点：
  ```make
  ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec
  CFLAGS += -I$(PORTLIBS)/include/freetype2
  LIBS := -lfreetype -lharfbuzz -lpng -lbz2 -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lm -lnx
  ```
  - **不要**链接 `-lSDL2 -lSDL2_ttf -lEGL -lglapi -ldrm_nouveau -lGLESv2`。
  - freetype 2.13 依赖 harfbuzz（静态库引用 `hb_*`），所以 `-lharfbuzz` 不能省。
- 编译：
  ```bash
  export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM DEVKITA64=/opt/devkitpro/devkitA64
  export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH
  make clean && make -j4
  ```
- 产物：`PlayLogTool.nro`；发布时复制到 `SD:/switch/PlayLogTool.nro`。
- 注意 hbmenu 重复下载会出现 `PlayLogTool (18).nro` 之类文件名；部署前清理，避免打开旧文件。
- 校验部署是否成功：从设备 HTTP 下载 `switch/PlayLogTool.nro` 计算 md5，与本地一致。

---

## 4. UI / 输入 / 在线离线（当前实现）

- 输入：libnx `PadState`，使用 `HidNpadButton_AnyUp/AnyDown/AnyLeft/AnyRight`（同时覆盖十字键和左摇杆）。
  - 只写 `HidNpadButton_Up/Down` 时摇杆方向不触发，曾导致“切不了”。
- 在线/离线判断：
  - 启动时调用 `syncLimitsFromServer()`：成功 = 在线，失败 = 离线；
  - 在线：**只读**（A/Y 隐藏）；
  - 离线：是否可编辑由服务端 `allow_offline_edit`（`limits.json` 字段，默认 `false`）决定。
- 服务端开关：**每设备** `switch_device.allow_offline_edit`（与 `enforce_mode` 同维度）。
  - 网页「设备参数」可开关；默认 `false`。
  - NRO 解析 `limits.json` 的 `"allow_offline_edit":true`；缺失按 `false`。
- 主界面布局：
  - 左栏：游戏表格（游戏/累计，不再显示“今日限额”）+ 系统信息框（设备编码/Token/服务器/间隔/执行模式/离线编辑/当前账号）；
  - 右栏：三张周一…周日表：游戏限额（所有账号各自计算）、账号+游戏（当前账号·这个游戏）、账号合计（当前账号·所有游戏）；
  - 底部单行按键：`↑↓ 游戏  ←→ 账号  A 设置  Y 清除  ZL/ZR 翻页  R 推送  L 日志  - 同步  X 设置  B 退出`
    （在线/离线只读时隐藏 A/Y）。
- 账号列表：优先从 `limits.json` 的 `accounts` 填充（主机 `accountListAllUsers` 可能拿不全）；
  `parseAccountsFromLimits()` 会把昵称/`is_active` 同步过来，默认选第一个 active。
- 设置入口：`X` 打开设置页（服务器地址 / Token / 间隔）；`+` 是 Token 快捷。

---

## 5. 限额维度与语义（不要混淆）

| 维度 | 数据表 | 语义 |
|---|---|---|
| 游戏限额（设备级） | `game_limit`（device+game） | 该游戏对所有账号的通用规则，**按每个账号各自记账/判定**，不是所有账号共享总时长 |
| 账号 + 游戏 | `account_game_limit`（account+game） | 某账号在某游戏上的限额 |
| 账号合计 | `account_total_limit`（account） | 该账号所有游戏加起来的合计上限 |

- 优先级：账号+游戏 > 设备级（sysmodule `find_effective_rule`）。
- 合计限额与单游戏限额是 **OR**（任一到点即拦）。
- sysmodule 负责真正执行；NRO 在线只读展示。
- 服务端下发的完整规则：
  - `limits` / `limit_rules`（设备级，含 `daily_seconds/weekly_seconds/weekday_seconds`）；
  - `account_limits` / `account_limit_rules`（账号+游戏）；
  - `account_total_limit_rules`（账号合计）；
  - `account_total_limits`（今日生效值）。
- `weekday_seconds` 长度恰好 7 = 已配置（0 = 当天封禁）；`[]`/缺失 = 未配置，回退每日值。
- 客户端之前只解析设备级/合计，**漏了 `account_limit_rules`**（账号+游戏），导致“未设置”。
  已新增 `AccountGameLimitRecord` + `parseAccountGameRules()`。

---

## 6. 服务端配合（本仓库之外，但联调必知）

- `/switch/devices/config` 下发：`limits`、`limit_rules`、`account_limits`、`account_limit_rules`、
  `account_total_limit_rules`、`account_total_limits`、`used_today`、`account_used_today(_total)`、
  `enforce_mode`、`allow_offline_edit`、`server_local_date`、`server_weekday`、`clock_skew_seconds`。
- 客户端**不提供在线写限额**（在线只读）；离线本地写 `limits.json`。
- 服务端新增设备维度字段时：
  - `models.py` 加列 → aerich 迁移（或手动 `ALTER TABLE switch_device ...`）；
  - `schemas.py`（`DeviceConfigResponse` / `DeviceUpdate` / `DeviceSettingsUpdate` / `DeviceOut`）；
  - `main.py`：`get_device_config` 下发、`device_to_out`、设备更新接口；
  - 前端 `types.ts` + `DevicesView.vue` 设备参数开关。
- 部署：
  1. 前端 `cd frontend && pnpm install --frozen-lockfile && pnpm build`（`.npmrc` 已用 npmmirror）；
  2. 源码传到目标服务器（本机无 rsync 时用 `tar czf - ... | ssh ... tar xzf -`）；
  3. `docker build -t <your-registry>/play-log-tool-server:latest .`；
  4. `docker push ...`；
  5. `cd ~/Documents && docker compose pull switch && docker compose up -d switch`；
  6. 容器启动会执行 `python -m aerich upgrade`；无新迁移文件时需确认 DB 已手动加列。
- 账号昵称：服务端 `accounts[].nickname` 来自设备账号（alias/昵称），NRO 会用它覆盖本地昵称。

---

## 7. 其他高频坑

- **代理**：本机 7890 代理可能导致 `git push` / 公网 SSH 卡住（`Connection closed`/超时）。推送前确认代理状态；Git 服务器 偶发失败可重试。
- **文件服务只读**：`<switch-lan-ip>:1234` 是 Apache 风格目录，不支持 PUT/上传；部署靠手动拷贝/其他工具。
- **DBI / AtmoXL**：升级 Atmosphere 1.12.1 后，旧 ABI 的 NRO 会报
  `This application does not support the current ABI`；AtmoXL 1.7.3（2023）基本不适配，优先用 DBI。
  DBI 官方只有俄语，中文/英文需 DBIPatcher 的 patched NRO + `translation.bin`（同版本成对使用）；
  DBI 需要 `SD:/switch/prod.keys`（Lockpick_RCM 提取，默认写到 `atmosphere/automatic_backups/`，
  需复制到 `switch/`）。
- **硬破真实系统**：不要在线点“系统更新”（烧熔断、可能进不去 CFW）；升级 Atmosphere 只替换 SD 文件。
- **日志文件名**：`limiter.log` 是 sysmodule 写的；`boot.log`/`ui.log` 是 NRO 写的（仅调试期）。
- **NRO 崩溃报告**：`hbloader` 进程 + `PlayLogTool + offset`；用 addr2line 定位，不要只看 Result。

---

## 8. 验证清单（每次改动后）

1. `PlayLogTool.nro` 的 md5 与本地构建一致（从设备 HTTP 下载对比）。
2. 无新增崩溃报告；`boot.log` 有 `main enter` 与 `appInit done mount=0x0`。
3. `ui.log` 有 `framebuffer ok` 和字体加载成功。
4. 在线：副标题「在线（只读）」，A/Y 不出现，三张表显示服务端规则。
5. 离线（改服务器地址为不可达或断网，按 `-`）：
   - `allow_offline_edit=true` → 「离线（可编辑）」，A/Y 出现；
   - `false` → 「离线（只读 · 服务端禁止编辑）」。
6. `←/→` 能切换账号（`boot.log` 的 `menu accounts=2` 确认账号已加载）。
7. 左栏不再显示“今日限额”；右栏三张表标题为“所有账号，各自独立计算 / 当前账号·这个游戏 / 当前账号·所有游戏”。

---

## 9. 关键文件

- `source/main.c`：NRO 主逻辑（`__appInit`、配置同步、账号解析、限额展示、按键/在线离线）。
- `source/ui.c` / `source/ui.h`：framebuffer + freetype 文本/面板/表格渲染（含字形缓存、限帧）。
- `Makefile`：NRO 构建（不链 SDL/mesa；链 freetype/harfbuzz 等）。
- `fonts/PlayLogFont.otf`：中文字体（复制到 `SD:/PlayLog/PlayLogFont.otf`）。
- `sysmodule/`：后台执行限额（libnx 编译，`__appInit` 自定义、`AppletType_None`）。
- SD 目录：`SD:/PlayLog/{boot.log,ui.log,limits.json,config.json,limiter.log,PlayLogFont.otf,cacert.pem}`。
- 发布目录（本机）：`/tmp/playlog-deploy/switch/PlayLogTool.nro`（HTTP 下载）。
