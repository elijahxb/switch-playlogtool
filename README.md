# PlayLogTool

[English](README.en.md) | **简体中文**

Nintendo Switch 的游玩时长统计与家长控制工具。包含前台 homebrew（NRO）与后台系统模块（sysmodule），配合服务端 `play-log-tool-server` 实现账号/游戏维度的限额与时长上报。

> 详细技术规范见 [docs/CLIENT_SPEC.md](docs/CLIENT_SPEC.md)，排障与踩坑经验见 [AGENTS.md](AGENTS.md)。

---

## 项目介绍

- **PlayLogTool.nro（前台）**：查看游戏/账号时长与限额，登录后从服务端拉取配置；在线时只读展示，离线时可按服务端开关决定是否允许本地修改限额。
- **PlayLogLimiter（后台 sysmodule）**：常驻后台统计游玩时长并上报，按服务端下发的规则执行限额。
- **与服务端联动**：
  - 上报每日游玩时长（按账号、游戏）；
  - 拉取限额规则（日/周/按星期）、服务端权威时间、在线/离线编辑策略；
  - 限额支持三种维度：**游戏限额（设备级）**、**账号 + 游戏**、**账号合计（全部游戏）**。
- 界面支持**中文 / English** 切换（保存到 SD 卡配置）；日志查看器、推送/同步流程、接口错误均为中文/英文提示。

## 目录结构

```
source/           前台 NRO（UI、配置同步、时长上报）
sysmodule/        后台系统模块（统计、限额执行、心跳）
limits_common.h   前后台共享的常量与限额判定
fonts/            中文字体（Noto Sans SC）
docs/             设计文档
AGENTS.md         开发/排障经验（黑屏、构建、部署等）
Makefile          前台 NRO 构建
```

## 环境搭建（开发）

### 1. 工具链

- [devkitPro](https://devkitpro.org/)（devkitA64 + libnx）
- portlibs：`switch-freetype`、`switch-harfbuzz`、`switch-libpng`、`switch-bzip2`
- 依赖库：`curl`、`mbedtls`（libnx/portlibs 已提供）

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH
```

> ⚠️ 本项目前台 UI 使用 **libnx framebuffer + freetype**，**不要**再引入 SDL2 / mesa / EGL（在较旧 CFW 上会黑屏，详见 AGENTS.md）。

### 2. 编译

```bash
# 前台 NRO
make clean && make
# 产物: PlayLogTool.nro

# 后台 sysmodule
cd sysmodule
make package
# 产物: sysmodule/package/atmosphere/contents/4200000000504C54/exefs.nsp
```

### 3. 部署到 Switch（SD 卡）

| 文件 | 目标路径 |
|---|---|
| `PlayLogTool.nro` | `SD:/switch/PlayLogTool.nro` |
| `sysmodule/package/atmosphere/contents/4200000000504C54/exefs.nsp` | 同路径覆盖 |
| `fonts/PlayLogFont.otf` | `SD:/PlayLog/PlayLogFont.otf` |

- sysmodule 覆盖后需要**重启主机**才会加载；
- 首次使用请在 NRO 的「设置」中填写**服务器地址**与**上传 Token**（由服务端生成）。

### 4. 服务端（可选）

配套服务端仓库：`play-log-tool-server`（FastAPI + PostgreSQL）。设备通过签名接口 `GET /switch/devices/config` 拉取配置、`POST /switch/statistics/daily` 上报时长。没有服务端时，NRO 可以离线工作（是否允许离线修改由本地/服务端策略决定）。

## 使用方法

### 按键

| 按键 | 功能 |
|---|---|
| ↑↓ | 选择游戏（支持摇杆） |
| ←→ | 切换账号 |
| ZL / ZR | 游戏列表翻页（±10） |
| A | 设置限额（仅离线且服务端允许时） |
| Y | 清除限额（同上） |
| R | 生成报告并推送 |
| L | 接口日志查看器 |
| - | 云同步（拉取服务端配置） |
| X | 设置：服务器地址 / 上传 Token / 同步上传间隔 / 界面语言 |
| + | 上传 Token 快捷入口 |
| B | 返回 / 退出 |

### 限额维度

| 维度 | 含义 |
|---|---|
| 游戏限额（设备级） | 所有账号对该游戏的通用规则，**按每个账号各自计算**，不是所有账号共享总时长 |
| 账号 + 游戏 | 某账号在某游戏上的限额（优先级高于设备级） |
| 账号合计 | 某账号所有游戏加起来的合计上限（与单游戏限额是 OR，任一到点即拦） |

- 支持每日、周总时长、按星期（周一…周日）规则；
- 真正执行限额的是后台 sysmodule；NRO 只负责展示（在线）或离线编辑本地规则。

### 在线 / 离线

- 启动或按 `-` 时从服务端同步配置：成功 = **在线（只读）**；
- 同步失败 = **离线**：
  - 服务端 `allow_offline_edit=true` → 可编辑（A/Y 出现）；
  - `false`（默认）→ 离线也只读。
- 界面语言保存在 `SD:/PlayLog/config.json` 的 `ui_lang`，切换后立即生效。

### 日志与上报

- `L` 查看接口日志（`SD:/PlayLog/api-YYYYMMDD.log`，UTF-8）；
- `R` 生成报告并推送到服务端；后台 sysmodule 也会按配置定时上报；
- sysmodule 运行日志：`SD:/PlayLog/limiter.log`。

## 注意事项

- **不要依赖 Switch 的系统时间**：设备 RTC 可能严重跑偏，客户端/服务端按服务端时间驱动日/周/星期判断。
- **不要在 CFW 中在线更新系统**（可能烧熔断、破坏破解环境）。
- 前台 UI 不要使用 SDL2/mesa；不要设置 `__nx_fs_num_sessions=16`（会导致启动卡死）。
- 更多排障细节（黑屏、ABI、字体、部署）见 [AGENTS.md](AGENTS.md)。

## 相关项目与 Star

- 客户端（本项目）：[elijahxb/switch-playlogtool](https://github.com/elijahxb/switch-playlogtool)
- 服务端：[elijahxb/switch-playlogtool-server](https://github.com/elijahxb/switch-playlogtool-server)

如果这个项目对你有帮助，欢迎给客户端或服务端点个 **Star** ⭐

客户端：

[![GitHub stars](https://img.shields.io/github/stars/elijahxb/switch-playlogtool?style=social)](https://github.com/elijahxb/switch-playlogtool)

[![Star History Chart](https://api.star-history.com/svg?repos=elijahxb/switch-playlogtool&type=Date)](https://star-history.com/#elijahxb/switch-playlogtool&Date)

服务端：

[![GitHub stars](https://img.shields.io/github/stars/elijahxb/switch-playlogtool-server?style=social)](https://github.com/elijahxb/switch-playlogtool-server)

[![Star History Chart](https://api.star-history.com/svg?repos=elijahxb/switch-playlogtool-server&type=Date)](https://star-history.com/#elijahxb/switch-playlogtool-server&Date)
