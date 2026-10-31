<div align="center">

# Agent Buddy

**放在桌上的 AI 编程伙伴：把 Claude Code、Codex、DeepSeek Harness 的工作状态和额度，显示在一块 4.3 寸 ESP32-S3 像素屏上。**

中文 | [English](README.en.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
![ESP32-S3](https://img.shields.io/badge/ESP32--S3-JC4827W543-orange)
![macOS | Windows](https://img.shields.io/badge/macOS%20%7C%20Windows-supported-blue)

<img src="docs/images/hero.gif" alt="Agent Buddy 桌面首页" width="720">

</div>

## 它能做什么

- **一眼看到 Agent 在干嘛**：Claude 的 Clawd 和 Codex 的云朵小机器人在像素书桌前，DeepSeek 的小鲸鱼住在墙上的鱼缸里。它们在思考、写代码、运行命令、等你确认、完成或出错时，各有一套动画，等你确认时屏幕边框会闪。
- **额度随时看**：Claude Code 和 Codex 的 5 小时 / 7 天剩余额度、重置时间；DeepSeek 按量计费，显示账户余额、余额走势和今天的用量（来自 magpie）。
- **[magpie](https://github.com/yetone/magpie) 面板**：还原 magpie 菜单栏面板的 Agent / 额度 / 用量 / 路由四页，新请求进来时喜鹊会扇翅膀、路由页播放光点飞行。
- **声音提醒**：8-bit 提示音，可选中文语音播报（「Claude 需要你确认：git push」「任务完成，用时 3 分钟」）。接上麦克风后，Clawd 会跟着音乐摇摆、听到你说话就安静下来。
- **知道你在不在**：接上毫米波雷达后，离开座位自动切到待机时钟，回来时打招呼，身后有人驻足时提醒你。
- **不插线也能用**：板子连上 Wi-Fi 后，拔掉 USB 自动改走 Wi-Fi（双向认证 + AES-256-GCM 加密）。支持多台电脑，用屏幕验证码配对。
- **装好就能用**：macOS / Windows 一条命令完成安装、烧录和 hooks 配置，不用装 Arduino。

## 屏幕截图

以下都是从板子上直接截的图（480×272，放大 2 倍）。

| | |
|:---:|:---:|
| <img src="docs/images/screen-home.png" width="420"><br>桌面首页：显示器上是 Claude 的当前状态，底部是额度、状态和 magpie 今日用量 | <img src="docs/images/screen-claude.png" width="420"><br>Claude Code：正在运行命令，右边是 5 小时 / 7 天额度 |
| <img src="docs/images/screen-codex.png" width="420"><br>Codex：待命中的云朵小机器人 | <img src="docs/images/screen-magpie-agent.png" width="420"><br>magpie · Agent：各 Agent 的模型和推理强度 |
| <img src="docs/images/screen-magpie-usage.png" width="420"><br>magpie · 用量：Token、请求数、费用、缓存命中率 | <img src="docs/images/screen-radar.png" width="420"><br>雷达：你在座位上、身后的盲区、有没有人靠近 |
| <img src="docs/images/screen-settings-pc.png" width="420"><br>设置 · 电脑：Wi-Fi 配对的电脑和控制权 | <img src="docs/images/screen-settings-sound.png" width="420"><br>设置 · 声音：音量、提示音、语音播报 |

## 实拍

| | | |
|:---:|:---:|:---:|
| <img src="docs/images/photo-screen.jpg" width="280"><br>点亮后的 magpie Agent 页（早期浅色界面） | <img src="docs/images/photo-back.jpg" width="280"><br>板子背面：ESP32-S3 模组、外置天线、2030 喇叭接在 Speak 口 | <img src="docs/images/photo-radar.jpg" width="280"><br>HLK-LD2450 毫米波雷达和接线 |

## 3D 打印外壳

像素风小电视造型，全卡扣，不用螺丝、不用胶。同一套 STL，换耗材颜色就是两套配色：

**经典黑灰**：前框、支架黑色，机身、后盖深灰

<img src="case/preview/theme-classic.png" alt="经典黑灰外壳三视图">

**米白 × Claude 橙**：前框、后盖、支架米白，机身 Claude 橙

<img src="case/preview/theme-cream.png" alt="米白 × Claude 橙外壳三视图">

## 选购清单

下面是作者实际买的型号，价格是 2026 年 10 月在拼多多下单时的到手价（基本都包邮），仅供参考。搜索时用「搜索关键词」那一列，选规格时注意「要选的规格」。

**主件（必需）**

| 部件 | 搜索关键词 | 要选的规格 | 参考价 | 用途 |
|---|---|---|---:|---|
| Guition 4.3 寸 ESP32-S3 触摸屏 | `JC4827W543` | 电容触摸版（`JC4827W543C`），ESP32-S3 N4R8 | ¥99 | 主板 + 屏幕：480×272 NV3041A QSPI 屏，GT911 电容触摸，板载 I2S 功放、锂电池充电 |
| USB-C 数据线 | — | 能传数据的线（有的线只能充电） | 自备 | 第一次配置、烧录固件；之后可以改走 Wi-Fi |

**配件（可选，按需买）**

| 部件 | 搜索关键词 | 要选的规格 | 参考价 | 接到 | 用途 |
|---|---|---|---:|---|---|
| 毫米波雷达 | `HLK-LD2450` | 【测试套件 A】模块 + 线材 + 天线 | ¥28.2 | **P1** | 判断在不在座位、身后有没有人 |
| 麦克风 | `INMP441 模块` | 已焊接排针 | ¥15.8 | **P3**（电源取 **P4**） | 听音乐节拍、判断你在不在说话 |
| 喇叭 | `2030 腔体喇叭` | 2030 椭圆，8Ω 0.5–2W，**2P 1.25mm 端子**，线长 160mm | ¥5.4 | **Speak** | 提示音、语音播报 |
| 锂电池 | `603450 聚合物锂电池` | 3.7V 1200mAh，插头选 **1.25mm 2P** | ¥13.86 | **BAT** | 拔掉 USB 后靠电池运行；插着 USB 时顺便充电 |
| 转接线 | `MX1.25 转杜邦线 4P` | 20cm，一份 5 条 | ¥10.56 | P1 / P3 / P4 | 板子的 1.25mm 扩展口转成杜邦头，接麦克风和雷达 |
| 杜邦线 | `杜邦线 20cm` | 公对公，10 条一排 | ¥2.47 | — | 雷达套件线（母头）和转接线（母头）之间对接 |

配件合计约 **¥76**，加上主板全套约 **¥175**。外壳可以自己 3D 打印（[两套配色](#3d-打印外壳)），PLA 合计约 160 g，STL 在 [`case/stl/`](case/stl)，配色和打印方法见[打印说明](case/打印说明.md)。

注意：
- 锂电池**插之前先量好极性**，不同店家的红黑线顺序可能和板子的 BAT 口相反。插上后按一下板子上的 **SW1** 开机。
- 雷达的 TX / RX 接反了也没关系，固件会自动换过来试。
- 可选配件都不接也能用：没喇叭就不出声，没麦克风、没雷达就看电脑键鼠的空闲时间。具体引脚见[使用手册 · 配件接线](docs/使用手册.md#配件接线可选)。

## 快速开始

1. 用**能传数据的 USB-C 线**把板子连到电脑。
2. 下载本项目，然后运行一键配置：
   - **macOS**：
     ```bash
     ./setup.sh
     ```
   - **Windows**：双击根目录的 `setup.cmd`。
3. 按提示回答几个问题（都有默认答案）。完成后屏幕会弹出一条「欢迎回来」测试通知。
4. 重启已经打开的 Claude Code / Codex / dsh，让 hooks 生效。

setup 会依次完成：建 Python 环境 → 注册 Claude Code / Codex / DeepSeek Harness 的 hooks → 检测板子 → 需要时烧录 `firmware/` 里的预编译固件 → 开机自启 → 启动并自检。可以重复运行，已经配好的步骤会跳过。改 `~/.claude/settings.json`、`~/.codex/config.toml`、`~/.dsh/cordis.patch.yml` 之前都会先备份。

### 拔掉 USB，改用 Wi-Fi

在板子的 设置 → Wi-Fi 里连上网络。插着 USB 启动 bridge 时会自动配对，之后拔掉线就会自动改走 Wi-Fi，插回去再切回 USB。没有 USB 的电脑（比如第二台电脑）这样配对：

1. 在板子上打开 设置 → 电脑 → 添加电脑。
2. 在电脑上运行：
   ```bash
   bridge/buddy pair --wifi
   ```
3. 核对板子和终端上的 6 位验证码一致，在板子上点「允许」。

## 工作原理

```
Claude Code ─hooks / statusline─┐
Codex ──────hooks───────────────┤
DeepSeek Harness ─hooks─────────┼─▶ bridge（电脑上，127.0.0.1:47823）══ USB 或 Wi-Fi ══▶ ESP32-S3 屏幕
magpie 本地网关 ◀──只读────────┘     汇总各会话状态、额度、用量                           动画 / 额度 / 面板
```

- **bridge**（`bridge/agent_bridge.py`）跑在你的电脑上：接收 Claude Code / Codex / DeepSeek Harness 的 hooks 事件和 statusline 额度，读取 magpie 本地网关的只读接口，汇总后发给板子。它只监听本机，不读取任何登录凭证；唯一的例外是你主动给了 DeepSeek API Key 时，用它查一下账户余额。
- **固件**（`agent_buddy/`）负责画面、触摸、声音、雷达和 Wi-Fi 链路。
- **链路**：插着 USB 时走 USB；拔掉后走 Wi-Fi（TCP 47824，HMAC 双向认证后每帧 AES-256-GCM 加密）。macOS 上由板子去连电脑，Windows 上由电脑去连板子，两种情况都不用改防火墙。设计见 [无线通讯方案](docs/无线通讯方案.md)。

## 常用命令

所有操作都通过 `bridge/buddy`（Windows 是 `bridge\buddy`）：

| 命令 | 作用 |
|---|---|
| `buddy status` | 逐项检查：进程、连接方式（USB / Wi-Fi）、固件版本、hooks、magpie |
| `buddy start` / `stop` / `restart` | 启动 / 停止 / 重启 bridge |
| `buddy logs -f` | 看日志 |
| `buddy flash` | 烧录预编译固件（`--build` 先编译当前代码） |
| `buddy pair --wifi` | 不插 USB，用屏幕验证码配对 |
| `buddy record 30` / `snap` | 录屏 30 秒 / 截图 |
| `buddy uninstall` | 停止、关闭开机自启、移除 hooks |

## 文档

- [使用手册](docs/使用手册.md)：每个功能的详细说明，包括状态动画、声音、雷达判断规则、待机时钟、电池、调试接口和自己编译固件。
- [无线通讯方案](docs/无线通讯方案.md)：Wi-Fi 链路的协议、配对、安全和多台电脑的规则。
- [外壳打印说明](case/打印说明.md)、[组装清单](case/组装清单.md)

## 目录

```
agent_buddy/   固件（Arduino，ESP32-S3）
bridge/        电脑端：bridge 守护进程、buddy 管理工具、hooks
firmware/      预编译固件，buddy flash 用
case/          3D 打印外壳：参数化模型和 STL
docs/          使用手册、设计文档、README 用的图片
tools/         生成喜鹊动画帧和品牌图标的脚本
promo/         宣传片的 Blender 场景和合成脚本
touch_test/    屏幕 + 触摸最小测试程序
setup.sh / setup.cmd   一键配置入口
```

## 致谢

- [magpie](https://github.com/yetone/magpie)：面板的数据来源和界面原型。
- Clawd 和 Codex 的形象参照 Anthropic 和 OpenAI 的官方角色，小鲸鱼参照 DeepSeek 的 logo，品牌图标归各自所有者。
- 固件用到 [Arduino_GFX](https://github.com/moononournation/Arduino_GFX)、[U8g2](https://github.com/olikraus/u8g2)、[TAMC_GT911](https://github.com/TAMCTec/gt911-arduino)、[ArduinoJson](https://github.com/bblanchon/ArduinoJson)。

## 协议

代码以 [MIT 协议](LICENSE) 开源。Claude、Codex、DeepSeek、magpie 等名称、标志和角色形象归各自所有者，不在本协议授权范围内。
