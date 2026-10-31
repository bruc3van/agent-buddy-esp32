// 硬件模块（麦克风等）的共享状态：主程序画 Clawd 时要读，所以放在头文件里（Arduino 会把函数原型提到最前面）
#pragma once
#include <Arduino.h>

// ---------- 麦克风（hw_mic.ino）----------
volatile bool micOn = false, micBar = false;
volatile float micDb = -90, micPeakDb = -90, micBase = -60;  // 当前块 RMS、峰值（回落）、底噪（dBFS）
volatile uint32_t micStartleAt = 0, micLastSoundMs = 0, micLastOnsetMs = 0, micOnsetN = 0;
volatile uint32_t micOnsets[8] = {0};  // 最近 8 个节拍点的时间

// ---------- 设置（ui_settings.ino，存 NVS；声音相关的会经串口 "CFG {...}" 同步给 bridge）----------
#define FW_VERSION "v4.1.1"
bool cfgSfx = true;       // 提示音
bool cfgVoice = false;    // 语音播报（默认关：只响提示音 + 屏幕文字）
bool spkMute = false;     // 静音（音量保留）
int cfgDoneSec = 20;      // 「任务完成」播报门槛（秒），0 = 不播报
bool cfgMicReact = true;  // Clawd 对声音做反应
int cfgBright = 100;      // 屏幕亮度 10~100
int setSec = 0;           // 设置页当前分区：0 声音 / 1 Wi-Fi / 2 显示 / 3 雷达 / 4 关于 / 5 电脑
bool clockPinned = false, clockDismissed = false;  // 待机时钟：手动打开 / 这次离开里手动关掉了（ui_scene.ino）
int txtClipL = 0, txtClipR = 480, txtClipT = 0, txtClipB = 272;  // 文字缓存绘制的裁剪范围（跑马灯、翻页钟用，ui_textcache.ino）
int homeDateX = 0;        // 首页状态栏日期时钟的起点 x，0 表示还没画过
int homeIconX = 0;        // 首页状态栏图标的起点 x（齿轮），0 表示还没画过
const int HOME_PILL_W = 122;  // 首页右上角设置按钮宽度
bool cfgMeetMute = true;   // 有人说话 / 开会时不播报语音
bool cfgGreet = true;      // 离开回来时打招呼
bool cfgHomeScene = true;  // 首页：桌面场景（false = 两张卡片）
bool cfgClock = true;      // 离开电脑时首页显示待机时钟

// ---------- 你在电脑前做什么（hw_act.ino）----------
enum React { R_NONE, R_STARTLE, R_DANCE, R_TYPE, R_SHH, R_WAVE, R_SLEEP };  // Clawd 对你的反应
volatile uint32_t micClicks[16] = {0};  // 最近 16 次键盘敲击声（短促、前后都安静）
volatile uint32_t micClickN = 0;
volatile uint8_t micLoudBits[24] = {0};  // 最近 192 块（约 3 秒）每块是否有声，环形位图
volatile uint32_t micBlockN = 0;
float actKeySec = -1, actInpSec = -1;  // Mac 距上次按键 / 任意键鼠输入的秒数（bridge 每秒发 {"t":"act"}）
uint32_t actAt = 0;                    // 收到的 millis
bool actMacMic = false;                // Mac 的麦克风正被别的程序用（开会 / 语音）
uint32_t talkSince = 0, talkUntil = 0;  // 板子听到有人在说话
uint32_t backAt = 0;                    // 「欢迎回来」开始的 millis
int backAway = 0, backDone = 0, backWait = 0;
uint32_t lastTouchAt = 0;
uint32_t macKeys[32] = {0};  // Mac 上最近 32 次按键的时间（bridge 实时发来）
uint32_t macKeyN = 0, macLastKeyAt = 0;
uint32_t dbgUntil = 0;  // 调试：强制显示某个反应 / 待机时钟（{"t":"ui","react":3} / {"t":"ui","standby":1}）
int dbgReact = 0;
bool dbgStandby = false;
bool dbgRadarDemo = false;  // 调试：雷达演示数据（{"t":"ui","rdemo":1}，15 秒）

// ---------- Codex（bridge 发 {"t":"cx"} / {"t":"cusage"}，字段同 Claude 的 state / usage）----------
int cxMode = 0;  // Mode 枚举值（M_OFFLINE = 0）
String cxLabel = "未连接", cxDetail = "";
int cxSessions = 0;
uint32_t cxModeSince = 0, cxTurnStart = 0, cxPokeUntil = 0;
float cxH5Used = -1, cxD7Used = -1;
uint32_t cxH5Reset = 0, cxD7Reset = 0;  // Codex 额度的重置时刻（epoch 秒，magpie 给的；0 = 不知道）
String cxAcct = "";      // Codex 额度用的是哪个账号（magpie 打码设置照样生效）
bool cxAcctAuto = true;  // 自动选（5 小时剩余最多的）
int cxAcctN = 0;         // 一共几个 Codex 账号
int backCx = 0;     // 离开期间 Codex 完成的任务数
int buddyAgent = 0;  // Buddy 页显示谁：0 Claude / 1 Codex / 2 DeepSeek

// ---------- DeepSeek Harness（bridge 发 {"t":"ds"} / {"t":"dsusage"}，状态字段同 Claude 的 state）----------
int dsMode = 0;  // Mode 枚举值（M_OFFLINE = 0）
String dsLabel = "未连接", dsDetail = "", dsTool = "";
int dsSessions = 0;
uint32_t dsModeSince = 0, dsTurnStart = 0, dsPokeUntil = 0;
String dsBal = "";   // 账户余额（如 "¥12.34"）；空 = magpie 没开、bridge 也没设 API Key
bool dsBalOk = true;  // 余额够用（DeepSeek 接口的 is_available）
String dsGrant = "", dsTopup = "";  // 余额里赠送 / 充值的部分（magpie 给的）
float dsPerDay = 0;                 // 最近每天大约花多少（magpie 按余额走势拟合）
float dsTrend[24];                  // 最近的余额走势（旧 → 新）
int dsTrendN = 0;
int dsCalls = -1;                   // 今天的请求数（magpie，agent=dsh）；-1 = 没有数据
String dsTokIn = "", dsTokOut = "", dsHit = "";  // 今天输入 / 输出 token、缓存命中率
uint16_t dsBars[24];                // 今天每小时的请求数（0~1000）
int dsBarN = 0, dsHour = -1;        // 小时数、现在是第几个小时
int backDs = 0;      // 离开期间 DeepSeek 完成的任务数

// ---------- 毫米波雷达 HLK-LD2450（hw_radar.ino，接 P1：UART0，256000 波特率）----------
struct RadarTarget { int16_t x, y, v; bool on; };  // x 左右、y 前方距离（毫米），v 速度（厘米/秒）
RadarTarget radarT[3];
uint32_t radarFrameAt = 0, radarFrames = 0;  // 最近一次收到完整数据帧的时间、累计帧数
bool radarLog = false;                       // 调试：每秒经串口报 "RADAR ..."
bool cfgRadarSeat = true;  // 用雷达判断你在不在座位（存 NVS）
bool cfgPrivacy = true;    // 有人靠近时提醒（存 NVS）
bool privacyOn = false;    // 有人靠近（正在提醒）
uint32_t seatSeenAt = 0;   // 最近一次在座位区看到人的时间

// 雷达屏方格查表（ui_radar.ino）：每个方格的位置、角度（0 = 正上方，顺时针）、到圆心的距离
struct ScopeCell {
  int16_t x, y;
  int16_t ang;      // -60 ~ 60（0 = 正前方，右为正）
  uint8_t dist;
  uint16_t p1, p2;  // 扫描线经过这个角度时的两个相位（0~65535 = 一个来回）：一次往右、一次往左
};
struct ScopeTab {
  ScopeCell *c = nullptr;
  uint16_t *base = nullptr;  // 每个方格的底色（距离环、十字线、座位弧、盲区），量程、座位状态或盲区变了才重算
  int n = 0, ox = 0, oy = 0, r = 0, cell = 0;
  int baseRange = -1;
  uint16_t baseSeat = 0;
  int baseShadow = 0;  // 画盲区时的方向和距离（0 = 不画）
};

// ---------- 节能（hw_act.ino 的 powerTick）----------
int cfgSleepMin = 15;      // 离开多少分钟后熄屏，0 = 从不（存 NVS）
bool cfgMicPause = true;   // 离开时暂停麦克风（存 NVS）
bool screenOff = false;    // 屏幕已熄灭（背光关、不再画）
bool clockDim = false;     // 离开久了、开着待机时钟：背光调到约 10%（白天不熄屏，留着远远看时间）
uint32_t awayAt = 0, lastWakeAt = 0;

// 文字缓存（ui_textcache.ino）：一段文字第一次画时渲染成掩码存起来，之后直接按掩码往画布里填颜色
struct SbTime { bool ok; String hh, mm, date; int hour, min, days; };  // 待机时钟用的本地时间（ui_scene.ino）

struct TxtEnt {
  uint32_t hash = 0, used = 0;
  uint8_t font = 0;
  uint16_t w = 0, h = 0;   // 掩码尺寸（放大前）
  int16_t ox = 0, oy = 0;  // 掩码左上角相对光标（基线）的偏移（放大前）
  int16_t adv = 0;         // 文字宽度（同 getTextBounds，放大前）
  uint8_t *mask = nullptr;
  String s;
};

// 座位标定（设置 → 雷达 → 标定座位）：你正常坐好时的位置（毫米），以它为中心判断在不在座位、有没有人靠近
bool seatCal = false;
int seatCalX = 0, seatCalY = 0;
uint32_t calibUntil = 0;  // 正在标定（5 秒）
long calibSumX = 0, calibSumY = 0;
int calibN = 0;
String calibMsg = "";
bool cfgNightDim = true;   // 夜里（23:00～7:00）自动调暗（存 NVS）
int doneToday[3] = {0, 0, 0}; // 今天完成的任务数：Claude / Codex / DeepSeek（待机时钟显示）
int doneDay = -1;          // 计数对应的日期（本地日序号）

// 麦克风频段（hw_mic.ino）：每块 16ms 算 6 个频点的能量（dB），用来区分人声和其他声音
const float MIC_FREQ[6] = {250, 500, 1000, 2000, 3000, 5000};
volatile float micBand[6] = {-90, -90, -90, -90, -90, -90};
volatile uint8_t micVoiceBits[24] = {0};  // 最近 192 块里，哪些块「像人声」（能量集中在 500～3000Hz）
