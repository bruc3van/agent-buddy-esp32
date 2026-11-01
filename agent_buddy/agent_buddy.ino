// Agent Buddy：在 JC4827W543 上显示 Claude Code / Codex / DeepSeek Harness 的工作状态（Clawd、Codex、小鲸鱼动画）和额度
// 数据来源：电脑（macOS / Windows）上的 bridge 守护进程通过 USB 串口发送 JSON 行
//   {"t":"state","state":"working","label":"运行命令","detail":"npm test","tool":"Bash","sessions":1,"now":1760000000}
//   {"t":"usage","h5":23.5,"h5r":1760003600,"d7":41.2,"d7r":1760400000,"now":1760000000}
//   {"t":"ping","now":1760000000}
//   {"t":"gamma","g":1.6}   —— 屏幕颜色校正强度（>1 加深，1 关闭），保存在 NVS
//   {"t":"cap","ms":30000}   —— 录屏：接下来 ms 毫秒内每帧把画面 RLE 压缩后发回（ms=0 停止）
//   {"t":"hello"}            —— 回一行 HELLO {"fw":"v4.0"}，buddy.py 用来识别固件版本
// 回传帧格式：A5 5A 'F' 'R' | uint32 millis | uint32 长度 | 若干 [数量 u8, 颜色 RGB565 小端 u16]
#include <U8g2lib.h>  // 必须在 Arduino_GFX 之前包含，才会启用 U8g2 中文字体
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <TAMC_GT911.h>
#include "QspiFast.h"
#include "fast_canvas.h"  // 快速刷屏：双缓冲异步 DMA + 发送时校色
#include <Preferences.h>
#include <WiFi.h>
#include "magpie_data.h"
#include "magpie_wag.h"
#include "brand_icons.h"
#include "hw_state.h"
#include "net_link.h"  // Wi-Fi 链路：输出经 Host 送到当前链路
#include "codex_pet.h"
#include "deepseek_pet.h"

// ---------- 硬件 ----------
#define GFX_BL 1
Arduino_DataBus *bus = new QspiFast(45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */);
Arduino_GFX *lcd = new Arduino_NV3041A(bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, false /* IPS：此屏不需要反色 */);
Arduino_Canvas *gfx = new FastCanvas(480, 272, lcd);  // 整帧画在 PSRAM 里再一次刷屏，避免闪烁
bool panelInv = true;      // 屏幕校准：反色（见 fixPanel）
uint8_t panelRot = 2;      // 屏幕校准：扫描方向

TAMC_GT911 tp(8 /* SDA */, 4 /* SCL */, 3 /* INT */, 38 /* RST */, 480, 272);

// ---------- 颜色 ----------
#define C_BG      0x10A2  // 深灰背景
#define C_PANEL   0x18E3
#define C_CLAUDE  0xDBAA  // Claude 橙 #D97757
#define C_CLAUDE_D 0xA2A7 // 暗一点的橙，用于腿
#define C_GREY    0x7BEF
#define C_DIM     0x4208
#define C_WHITE   0xFFFF
#define C_BLACK   0x0000
#define C_GREEN   0x4E8A
#define C_YELLOW  0xF5C5
#define C_RED     0xE8E4
#define C_BLUE    0x5D7F
#define C_SHINE   0xF5B3  // 流光高亮 #F5B49A
#define C_SHINE_M 0xECAF  // 流光过渡 #E8977A

// 以 U 像素为一格的像素小怪：8 格宽的身体、两侧 2 格宽的手、底下 4 条腿（共 12 格宽，同官方 Clawd）
int U = 14;  // 角色像素格大小，首页缩略图会临时改小

struct Pose {
  int dx = 0, dy = 0;      // 整体偏移（像素）
  int armL = 0, armR = 0;  // 手臂抬起格数（0 正常，1~2 抬高）
  int legLift[4] = {0, 0, 0, 0};  // 每条腿抬起像素
  int eye = 0;             // 0 睁眼 1 闭眼 2 开心^^ 3 叉叉 4 睡觉线
  int lookX = 0, lookY = 0;  // 眼睛朝向偏移（像素）
  int squash = 0;          // >0 身体变宽变矮（像素）
  uint16_t color = C_CLAUDE;
};


// ---------- 屏幕 ----------
enum Screen { SCR_HOME, SCR_BUDDY, SCR_MAGPIE, SCR_SET, SCR_KEYB, SCR_RADAR };  // SCR_SET：设置页（含 Wi-Fi）
Screen screen = SCR_HOME;

// ---------- 状态 ----------
enum Mode { M_OFFLINE, M_IDLE, M_SLEEP, M_THINKING, M_WORKING, M_WAITING, M_DONE, M_ERROR, M_COMPACT };

Mode mode = M_OFFLINE;
String label = "未连接";
String statusDetail = "请在电脑上启动 bridge（USB 或 Wi-Fi）";
String tool = "";
int sessions = 0;
uint32_t modeSince = 0;     // 进入当前模式的 millis
uint32_t lastMsgAt = 0;     // 最近一次收到串口消息的 millis

float h5Used = -1, d7Used = -1;  // -1 表示还没有数据
uint32_t h5Reset = 0, d7Reset = 0;
const int BS_X = 44, BS_W = 110;     // Buddy 页标题栏的 Claude Code / Codex / DeepSeek 标签：起点、每个宽度（ui_codex.ino 画）
const int QC_Y1 = 110, QC_Y2 = 188;  // Buddy 页两张额度卡的 y（标题栏到 42，状态卡 46~100，卡片之间留 10px）
uint32_t hostNow = 0;            // 最近一次收到的电脑时间（epoch 秒）
uint32_t hostNowAt = 0;          // 收到它时的 millis
int32_t tzOff = 8 * 3600;        // 电脑的本地时区偏移（秒），由 bridge 随消息下发

uint32_t pokeUntil = 0;
uint32_t turnStart = 0;
uint32_t capUntil = 0;           // 录屏截止 millis，0 表示不录
uint8_t *rleBuf = nullptr;       // 录屏压缩缓冲（PSRAM）

// ---------- 屏幕颜色校正 ----------
// 这块面板实际显示偏亮偏灰，刷屏前按 out = in^g 把颜色压暗，让实物接近设计色；录屏取的是校正前的画面
Preferences prefs;
float panelGamma = 1.6;
uint8_t lut5[32], lut6[64];

void buildLut() {
  for (int i = 0; i < 64; i++) {
    if (i < 32) {
      int v = lround(pow(i / 31.0, panelGamma) * 31);
      lut5[i] = (i > 0 && v < 1) ? 1 : v;  // 非零颜色至少保留 1 级，避免暗部糊成纯黑
    }
    int v = lround(pow(i / 63.0, panelGamma) * 63);
    lut6[i] = (i > 0 && v < 1) ? 1 : v;
  }
}

void applyLut() {
  if (panelGamma == 1.0f) return;
  uint16_t *fb = gfx->getFramebuffer();
  uint16_t prevIn = 0, prevOut = 0;
  for (int i = 0; i < 480 * 272; i++) {
    uint16_t c = fb[i];
    if (c != prevIn) {  // 画面大块同色，缓存上一个结果省掉大部分计算
      prevIn = c;
      prevOut = (lut5[c >> 11] << 11) | (lut6[(c >> 5) & 0x3F] << 5) | lut5[c & 0x1F];
    }
    fb[i] = prevOut;
  }
}          // 本轮开始干活的 millis，用于"已用时"          // 点击角色后的开心动画截止时间

Mode parseMode(const char *s) {
  if (!strcmp(s, "idle")) return M_IDLE;
  if (!strcmp(s, "sleep")) return M_SLEEP;
  if (!strcmp(s, "thinking")) return M_THINKING;
  if (!strcmp(s, "working")) return M_WORKING;
  if (!strcmp(s, "waiting")) return M_WAITING;
  if (!strcmp(s, "done")) return M_DONE;
  if (!strcmp(s, "error")) return M_ERROR;
  if (!strcmp(s, "compacting")) return M_COMPACT;
  return M_IDLE;
}

// 当前时间：优先用电脑（bridge）发来的；bridge 没开时用 Wi-Fi 网络对时（连上 Wi-Fi 后自动开始）
uint32_t epochNow() {
  if (hostNow) return hostNow + (millis() - hostNowAt) / 1000;
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;
}

// ---------- 串口协议 ----------
String lineBuf;

void handleLine(const String &line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  lastMsgAt = millis();
  if (doc["tz"].is<int>() && (int32_t)doc["tz"] != tzOff) {  // 时区：记下来，bridge 没开时网络对时也要用
    tzOff = doc["tz"];
    prefs.putInt("tz", tzOff);
  }
  if (doc["now"].is<uint32_t>()) {
    hostNow = doc["now"];
    hostNowAt = millis();
  }
  const char *t = doc["t"] | "";
  if (!strcmp(t, "state")) {
    Mode m = parseMode(doc["state"] | "idle");
    bool wasActive = mode == M_THINKING || mode == M_WORKING || mode == M_WAITING || mode == M_COMPACT;
    bool isActive = m == M_THINKING || m == M_WORKING || m == M_WAITING || m == M_COMPACT;
    if (isActive && !wasActive) turnStart = millis();
    if (m != mode) modeSince = millis();
    mode = m;
    label = String(doc["label"] | "");
    statusDetail = String(doc["detail"] | "");
    tool = String(doc["tool"] | "");
    sessions = doc["sessions"] | 0;
    doneSeqSet(0, doc["dn"] | -1L);
  } else if (!strcmp(t, "cx")) {  // Codex 的状态
    Mode m = parseMode(doc["state"] | "idle");
    bool busy = m == M_THINKING || m == M_WORKING || m == M_WAITING || m == M_COMPACT;
    bool wasBusy = cxMode == M_THINKING || cxMode == M_WORKING || cxMode == M_WAITING || cxMode == M_COMPACT;
    if (busy && !wasBusy) cxTurnStart = millis();
    if (m != cxMode) cxModeSince = millis();
    cxMode = m;
    cxLabel = String(doc["label"] | "");
    cxDetail = String(doc["detail"] | "");
    cxSessions = doc["sessions"] | 0;
    doneSeqSet(1, doc["dn"] | -1L);
  } else if (!strcmp(t, "ds")) {  // DeepSeek Harness 的状态
    Mode m = parseMode(doc["state"] | "idle");
    bool busy = m == M_THINKING || m == M_WORKING || m == M_WAITING || m == M_COMPACT;
    bool wasBusy = dsMode == M_THINKING || dsMode == M_WORKING || dsMode == M_WAITING || dsMode == M_COMPACT;
    if (busy && !wasBusy) dsTurnStart = millis();
    if (m != dsMode) dsModeSince = millis();
    dsMode = m;
    dsLabel = String(doc["label"] | "");
    dsDetail = String(doc["detail"] | "");
    dsTool = String(doc["tool"] | "");
    dsSessions = doc["sessions"] | 0;
    doneSeqSet(2, doc["dn"] | -1L);
  } else if (!strcmp(t, "dsusage")) {  // DeepSeek 账户余额（magpie 或 API Key）
    dsBal = String(doc["bal"] | "");
    dsBalOk = (doc["ok"] | 1) == 1;
    dsGrant = String(doc["g"] | "");
    dsTopup = String(doc["tp"] | "");
    dsPerDay = doc["pd"] | 0.0f;
    JsonArray tr = doc["tr"].as<JsonArray>();
    dsTrendN = 0;
    for (JsonVariant v : tr)
      if (dsTrendN < 24) dsTrend[dsTrendN++] = v.as<float>();
  } else if (!strcmp(t, "dstoday")) {  // magpie 里 DeepSeek Harness 今天的用量
    dsCalls = doc["n"] | -1;
    dsTokIn = String(doc["tin"] | "");
    dsTokOut = String(doc["tout"] | "");
    dsHit = String(doc["hit"] | "");
    dsHour = doc["hn"] | -1;
    JsonArray bars = doc["bars"].as<JsonArray>();
    dsBarN = 0;
    for (JsonVariant v : bars)
      if (dsBarN < 24) dsBars[dsBarN++] = v.as<int>();
  } else if (!strcmp(t, "cusage")) {
    cxH5Used = doc["h5"] | -1.0f;
    cxD7Used = doc["d7"] | -1.0f;
    cxH5Reset = doc["h5r"] | 0;
    cxD7Reset = doc["d7r"] | 0;
    cxAcct = String(doc["acct"] | "");
    cxAcctAuto = (doc["auto"] | 1) == 1;
    cxAcctN = doc["n"] | 0;
  } else if (!strcmp(t, "gamma")) {
    float g = doc["g"] | 1.0f;
    panelGamma = constrain(g, 0.5f, 3.0f);
    buildLut();
    prefs.putFloat("gamma", panelGamma);
  } else if (!strcmp(t, "cap")) {
    uint32_t ms = doc["ms"] | 0;
    capUntil = ms ? millis() + ms : 0;
  } else if (!strcmp(t, "hello")) {  // buddy.py 识别板子上是不是本固件、什么版本（决定要不要烧录）
    Host.printf("HELLO {\"fw\":\"%s\"%s}\n", FW_VERSION, netHelloFields().c_str());
  } else if (!strcmp(t, "pair")) {  // Wi-Fi 配对：只认串口（插着 USB 是可信通道），网络上来的一律忽略
    if (!lineFromNet) netPair(doc);
  } else if (!strcmp(t, "net")) {  // 电脑的最新地址（也只认串口）
    if (!lineFromNet) netSetDial(doc);
  } else if (!strcmp(t, "ui")) {  // 切换页面：{"t":"ui","screen":2,"tab":1,"period":0}
    if (doc["agent"].is<int>()) buddyAgent = constrain((int)doc["agent"], 0, 2);  // Buddy 页：0 Claude / 1 Codex / 2 DeepSeek
    if (doc["set"].is<int>()) setSec = constrain((int)doc["set"], 0, 5);  // 设置页分区
    if (doc["screen"].is<int>()) setScreen((Screen)constrain((int)doc["screen"], 0, 5));
    if (doc["tab"].is<int>()) magTab = constrain((int)doc["tab"], 0, 3);
    if (doc["period"].is<int>()) muPeriod = constrain((int)doc["period"], 0, 3);
    if (doc["inv"].is<int>() || doc["rot"].is<int>()) {  // 屏幕校准：反色 / 扫描方向
      if (doc["inv"].is<int>()) panelInv = (int)doc["inv"] == 1;
      if (doc["rot"].is<int>()) panelRot = constrain((int)doc["rot"], 0, 3);
      prefs.putBool("pinv", panelInv);
      prefs.putUChar("prot", panelRot);
      fixPanel();
    }
    if (doc["rcol"].is<int>()) routeS = (int)doc["rcol"] ? ROUTE_FOLD : 0;  // 调试：路由舞台收起 / 展开
    if (doc["perf"].is<int>()) perfOn = (int)doc["perf"] == 1;  // 调试：性能计时
    if (doc["scan"].is<int>()) hwScan();
    if (doc["radar"].is<int>()) radarLog = (int)doc["radar"] == 1;  // 雷达调试：串口报目标
    if (doc["rmode"].is<int>()) radarMultiTarget();
    if (doc["seatcal"].is<int>()) { if ((int)doc["seatcal"]) seatCalibStart(); else seatCalibClear(); }  // 标定 / 清除座位  // 重新设成多目标模式并查询
    if (doc["mic"].is<int>()) micBar = (int)doc["mic"] == 1;  // 麦克风调试：串口报音量
    if (doc["vol"].is<int>() && !doc["beep"].is<int>()) spkSetVolume(doc["vol"]);  // 喇叭音量 0~100
    settingsFromJson(doc);
    if (doc["react"].is<int>() || doc["standby"].is<int>() || doc["rdemo"].is<int>()) {  // 调试：强制显示 15 秒
      dbgReact = doc["react"] | 0;
      dbgStandby = (doc["standby"] | 0) == 1;
      dbgRadarDemo = (doc["rdemo"] | 0) == 1;
      dbgUntil = millis() + 15000;
    }  // 声音开关：voice / sfx / mute / dsec（bridge 的 /audio）
    if (doc["beep"].is<int>()) {  // 喇叭测试；引脚可临时覆盖
      if (doc["bclk"].is<int>() || doc["ws"].is<int>() || doc["dout"].is<int>())
        spkSetPins(doc["bclk"] | -1, doc["ws"] | -1, doc["dout"] | -1);
      spkBeep((int)doc["beep"], doc["vol"] | -1);
    }  // 扫描板上接的传感器 / 模组

  } else if (!strcmp(t, "sfx")) {  // 8-bit 音效
    sfxPlay(doc["n"] | "");
  } else if (!strcmp(t, "au")) {  // bridge 发来的语音片段（关了语音播报、静音、有人说话时丢弃）
    if (voiceAllowed()) spkPushB64(doc["d"] | "");
  } else if (!strcmp(t, "act")) {  // Mac 键鼠空闲秒数 + 麦克风是否被占用
    actKeySec = doc["k"] | -1.0f;
    actInpSec = doc["i"] | -1.0f;
    actMacMic = (doc["m"] | 0) == 1;
    actAt = millis();
  } else if (!strcmp(t, "key")) {  // Mac 上新按下的键数（只有次数）
    int n = min((int)(doc["n"] | 0), 8);
    uint32_t now = millis();
    for (int i = 0; i < n; i++) macKeys[macKeyN++ % 32] = now;
    if (n > 0) macLastKeyAt = now;
  } else if (!strcmp(t, "back")) {  // 离开后回到电脑前
    backAt = millis();
    backAway = doc["a"] | 0;
    backDone = doc["d"] | 0;
    backCx = doc["c"] | 0;
    backDs = doc["s"] | 0;
    backWait = doc["w"] | 0;
  } else if (magpieHandle(t, doc)) {
    // magpie 面板数据，见 ui_magpie.ino
  } else if (!strcmp(t, "usage")) {
    if (doc["h5"].is<float>()) { h5Used = doc["h5"]; h5Reset = doc["h5r"] | 0; }
    if (doc["d7"].is<float>()) { d7Used = doc["d7"]; d7Reset = doc["d7r"] | 0; }
  }
}

void pollSerial() {
  if (Serial.available()) serialRxAt = millis();
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      handleLine(lineBuf);
      lineBuf = "";
    } else if (c != '\r' && lineBuf.length() < 8192) {
      lineBuf += c;
    }
  }
}

// ---------- Clawd 角色绘制 ----------
int CX = 128;   // 角色中心 x
int BASE = 230; // 脚底 y（Buddy 页；头顶的气泡要让开标题栏）

void drawClawd(const Pose &p) {
  // 比例照 Claude 桌面端的官方 Clawd：8 格宽 6 格高的身体，两侧 2 格宽的手，1 格见方的眼睛，4 条 1 格宽的腿
  int w = 8 * U + p.squash * 2;
  int h = 6 * U - p.squash;
  int x0 = CX - w / 2 + p.dx;
  int y0 = BASE - 2 * U - h + p.dy;

  // 影子
  gfx->fillEllipse(CX + p.dx / 2, BASE + 4 * U / 14, (70 - min(abs(p.dy), 30)) * U / 14, max(2, 6 * U / 14), C_DIM);

  // 腿：和身体同色，第 0、2、5、7 格
  int legX[4] = {x0, x0 + 2 * U, x0 + w - 3 * U, x0 + w - U};
  for (int i = 0; i < 4; i++) gfx->fillRect(legX[i], y0 + h, U, 2 * U - p.legLift[i], p.color);
  // 身体
  gfx->fillRect(x0, y0, w, h, p.color);
  // 手臂：2 格宽、2 格高
  gfx->fillRect(x0 - 2 * U, y0 + (2 - p.armL) * U, 2 * U, 2 * U, p.color);
  gfx->fillRect(x0 + w, y0 + (2 - p.armR) * U, 2 * U, 2 * U, p.color);

  // 眼睛：第 1 行，左右各离边 1 格
  int ex[2] = {x0 + U + p.lookX, x0 + w - 2 * U + p.lookX};
  int ey = y0 + U + p.lookY;
  int t = max(2, U / 4);  // 线条粗细
  for (int i = 0; i < 2; i++) {
    int x = ex[i], y = ey;
    switch (p.eye) {
      case 0: gfx->fillRect(x, y, U, U, C_BLACK); break;
      case 1: gfx->fillRect(x - 2, y + U / 2 - t / 2, U + 4, t, C_BLACK); break;  // 闭眼 / 眯眼：一条横线
      case 2:  // ^ ^
        for (int k = 0; k < t; k++) {
          gfx->drawLine(x - 2, y + U + k, x + U / 2, y + k, C_BLACK);
          gfx->drawLine(x + U / 2, y + k, x + U + 2, y + U + k, C_BLACK);
        }
        break;
      case 3:  // x x
        for (int k = -1; k <= 1; k++) {
          gfx->drawLine(x - 2 + k, y - 2, x + U + 2 + k, y + U + 2, C_BLACK);
          gfx->drawLine(x + U + 2 + k, y - 2, x - 2 + k, y + U + 2, C_BLACK);
        }
        break;
      case 4: gfx->fillRect(x - 3, y + U - t, U + 6, t, C_BLACK); break;  // 睡觉：低垂的一条线
      case 5: gfx->fillRect(x - 3, y - 3, U + 6, U + 6, C_BLACK); break;  // 瞪大眼
    }
  }
}

// ---------- Claude 星芒（Claude Code 转圈符号 · ✢ ✳ ✶ ✻ ✽ 的图形版）----------
struct SparkFrame { uint8_t rays; float len, width; uint8_t style; };  // style 0 圆点 1 水滴 2 细线 3 尖角
const SparkFrame SPARK[6] = {
  {0, 0.00, 0.20, 0},  // ·
  {4, 0.80, 0.30, 1},  // ✢
  {8, 0.90, 0.10, 2},  // ✳
  {6, 0.95, 0.34, 3},  // ✶
  {8, 0.92, 0.24, 1},  // ✻
  {8, 1.00, 0.32, 1},  // ✽
};

int sparkFrame(uint32_t now) {
  int f = (now / 110) % 10;      // 0..5..1 往返
  return f < 6 ? f : 10 - f;
}

void drawSpark(int cx, int cy, int r, int frame, float rot, uint16_t color) {
  const SparkFrame &F = SPARK[frame];
  if (F.style == 0) {
    gfx->fillCircle(cx, cy, max(2, (int)(r * F.width)), color);
    return;
  }
  float L = r * F.len, W = r * F.width;
  for (int i = 0; i < F.rays; i++) {
    float a = rot + i * 2 * PI / F.rays;
    float ux = cos(a), uy = sin(a), px = -uy, py = ux;
    int tx = cx + ux * L, ty = cy + uy * L;
    if (F.style == 2) {
      for (int k = -1; k <= 1; k++) gfx->drawLine(cx + px * k, cy + py * k, tx + px * k, ty + py * k, color);
      gfx->fillCircle(tx, ty, max(2, (int)(W * 0.8)), color);
    } else if (F.style == 3) {
      gfx->fillTriangle(cx + px * W / 2, cy + py * W / 2, cx - px * W / 2, cy - py * W / 2, tx, ty, color);
    } else {
      float m = L - W / 2;  // 水滴：外端圆头，内端收尖
      int mx = cx + ux * m, my = cy + uy * m;
      gfx->fillCircle(mx, my, W / 2, color);
      gfx->fillTriangle(cx, cy, mx + px * W / 2, my + py * W / 2, mx - px * W / 2, my - py * W / 2, color);
    }
  }
  gfx->fillCircle(cx, cy, max(2, (int)(r * 0.12)), color);
}

// 逐字打印 UTF-8 文本，一道高光从左往右扫过（Claude Code 的 shimmer 效果）
void printShimmer(const String &text, uint32_t now, uint16_t base) { printShimmerC(text, now, base, C_SHINE, C_SHINE_M); }

// 同上，高光颜色自己定（hi 最亮的一个字，mid 它两边的字）
void printShimmerC(const String &text, uint32_t now, uint16_t base, uint16_t hi, uint16_t mid) {
  int n = 0;
  for (unsigned i = 0; i < text.length(); i++) if ((text[i] & 0xC0) != 0x80) n++;
  int hp = (int)((now / 110) % (n + 8)) - 4;
  int idx = 0;
  for (unsigned i = 0; i < text.length();) {
    unsigned j = i + 1;
    while (j < text.length() && (text[j] & 0xC0) == 0x80) j++;
    int d = abs(idx - hp);
    gfx->setTextColor(d == 0 ? hi : (d == 1 ? mid : base));
    gfx->print(text.substring(i, j));
    i = j;
    idx++;
  }
}

const char *THINK_VERBS[] = {"思考中", "琢磨中", "酝酿中", "推敲中", "构思中", "斟酌中"};

// ---------- 道具 / 气泡 ----------
void bubble(int x, int y, int w, int h, uint16_t border) {
  if (pixelStyle) {
    bubblePx(x, y, w, h, border);
    return;
  }
  gfx->fillRoundRect(x, y, w, h, 8, C_WHITE);
  gfx->drawRoundRect(x, y, w, h, 8, border);
  gfx->fillTriangle(x + 14, y + h - 1, x + 28, y + h - 1, x + 8, y + h + 12, C_WHITE);
}

void iconTerminal(int x, int y, uint32_t t) {
  gfx->fillRoundRect(x, y, 56, 40, 4, C_BLACK);
  gfx->setFont(u8g2_font_unifont_h_chinese4);
  gfx->setTextColor(C_GREEN);
  gfx->setCursor(x + 6, y + 26);
  gfx->print(">");
  if ((t / 400) % 2) gfx->fillRect(x + 18, y + 24, 9, 3, C_GREEN);
}

void iconDoc(int x, int y, uint32_t t) {
  gfx->fillRect(x + 6, y, 40, 46, C_WHITE);
  gfx->drawRect(x + 6, y, 40, 46, C_GREY);
  int lines = (t / 250) % 5 + 1;
  for (int i = 0; i < lines; i++) gfx->fillRect(x + 11, y + 6 + i * 8, 30 - (i % 2) * 10, 3, C_BLUE);
}

void iconSearch(int x, int y, uint32_t t) {
  int ox = (int)(6 * sin(t / 300.0));
  gfx->fillRect(x + 4, y + 2, 40, 44, C_WHITE);
  for (int i = 0; i < 5; i++) gfx->fillRect(x + 9, y + 8 + i * 8, 28, 3, C_GREY);
  for (int k = 0; k < 3; k++) gfx->drawCircle(x + 22 + ox, y + 20, 11 - k, C_BLACK);
  for (int k = 0; k < 4; k++) gfx->drawLine(x + 29 + ox + k, y + 29, x + 40 + ox + k, y + 42, C_BLACK);
}

void iconGlobe(int x, int y, uint32_t t) {
  int cx = x + 26, cy = y + 22, r = 20;
  gfx->fillCircle(cx, cy, r, C_BLUE);
  int ew = (int)(r * fabs(cos(t / 500.0)));
  gfx->drawEllipse(cx, cy, max(ew, 1), r, C_WHITE);
  gfx->drawFastHLine(cx - r, cy, 2 * r, C_WHITE);
  gfx->drawFastVLine(cx, cy - r, 2 * r, C_WHITE);
}

void iconAgents(int x, int y, uint32_t t) {
  for (int i = 0; i < 3; i++) {
    int bob = ((t / 200 + i) % 2) * 3;
    gfx->fillRect(x + i * 20, y + 18 - bob, 16, 12, C_CLAUDE);
    gfx->fillRect(x + i * 20 + 3, y + 21 - bob, 3, 4, C_BLACK);
    gfx->fillRect(x + i * 20 + 10, y + 21 - bob, 3, 4, C_BLACK);
    gfx->fillRect(x + i * 20 + 2, y + 30 - bob, 3, 4, C_CLAUDE_D);
    gfx->fillRect(x + i * 20 + 11, y + 30 - bob, 3, 4, C_CLAUDE_D);
  }
}

void iconGear(int x, int y, uint32_t t) {
  int cx = x + 26, cy = y + 22;
  float a0 = t / 400.0;
  for (int i = 0; i < 8; i++) {
    float a = a0 + i * PI / 4;
    gfx->fillCircle(cx + 17 * cos(a), cy + 17 * sin(a), 5, C_GREY);
  }
  gfx->fillCircle(cx, cy, 15, C_GREY);
  gfx->fillCircle(cx, cy, 6, C_PANEL);
}

bool toolIsTerminal(const String &tool) { return tool == "Bash" || tool == "BashOutput" || tool == "KillShell"; }

void drawToolIcon(int x, int y, uint32_t t, const String &tool) {
  if (toolIsTerminal(tool)) iconTerminal(x, y, t);
  else if (tool == "Edit" || tool == "Write" || tool == "MultiEdit" || tool == "NotebookEdit") iconDoc(x, y, t);
  else if (tool == "Read" || tool == "Grep" || tool == "Glob" || tool == "LS") iconSearch(x, y, t);
  else if (tool == "WebFetch" || tool == "WebSearch" || tool.startsWith("mcp__Claude_Browser") || tool.startsWith("mcp__claude-in-chrome")) iconGlobe(x, y, t);
  else if (tool == "Agent" || tool == "Task") iconAgents(x, y, t);
  else iconGear(x, y, t);
}

// ---------- 场景 ----------
void drawCharacterScene(uint32_t now) {
  uint32_t t = now - modeSince;
  Pose p;
  Mode m = mode;
  if (pokeUntil > now) m = M_DONE;

  // 对你的反应（hw_act.ino）：只在空闲 / 睡觉时生效
  int react = clawdReact(m, now);
  if (react == R_SLEEP) {
    m = M_SLEEP;  // 你离开了 / 安静太久：打瞌睡
    react = R_NONE;
  } else if (react) {
    m = M_IDLE;  // 先按空闲姿势画，再叠加动作
  }

  // 每隔一段时间眨眼
  bool blink = (now % 3700) < 140;

  switch (m) {
    case M_OFFLINE:
      p.color = C_GREY;
      p.eye = 1;
      p.dy = (int)(2 * sin(now / 900.0));
      break;

    case M_IDLE: {
      // 同官方 LookingAroundEyesOnly：身子不动，只有眼神在变——看左、看右、往上瞟，偶尔眨眼
      static const int8_t LX[8] = {0, 0, -1, -1, 0, 1, 1, 0}, LY[8] = {0, 0, 0, 0, -1, 0, 0, 0};
      uint32_t ph = (now / 900) % 8;
      p.lookX = LX[ph] * U / 2;
      p.lookY = LY[ph] * U / 2;
      p.eye = blink ? 1 : 0;
      break;
    }

    case M_SLEEP: {
      p.dy = (int)(4 * sin(now / 1200.0));
      p.eye = 4;
      p.squash = 3 + (int)(2 * sin(now / 1200.0));
      gfx->setFont(u8g2_font_unifont_h_chinese4);
      gfx->setTextColor(C_GREY);
      for (int i = 0; i < 3; i++) {
        uint32_t zt = (now + i * 900) % 2700;
        int zx = CX + 40 + i * 14 + (int)(zt / 90);
        int zy = 126 - (int)(zt / 50) - i * 6;  // 飘得慢一点，不飘进标题栏
        gfx->setTextSize(1 + (i == 2));
        gfx->setCursor(zx, zy);
        gfx->print("z");
      }
      gfx->setTextSize(1);
      break;
    }

    case M_THINKING: {
      // 同官方 Thinking：眯起眼往上想，头顶一个蓝色对话泡，里面的点一个一个冒出来
      p.lookY = -U / 2;
      p.lookX = U / 4;
      p.armR = 1;
      p.eye = 1;
      p.dy = ((now / 700) % 2) ? -2 : 0;
      const uint16_t C_THINK = 0x6CD9;  // 官方蓝 #699ACB
      int bx = CX + 34, by = 50 + (((now / 700) % 2) ? -2 : 0), bw = 70, bh = 40;
      gfx->fillRect(bx + 4, by, bw - 8, bh, C_THINK);  // 像素圆角
      gfx->fillRect(bx, by + 4, bw, bh - 8, C_THINK);
      gfx->fillRect(bx + 10, by + bh, 10, 6, C_THINK);  // 小尾巴
      gfx->fillRect(bx + 6, by + bh + 6, 6, 4, C_THINK);
      int nd = (now / 450) % 4;
      for (int i = 0; i < 3; i++)
        if (i < nd) gfx->fillRect(bx + 15 + i * 16, by + bh / 2 - 4, 8, 8, C_BG);
      break;
    }

    case M_WORKING: {
      // 同官方 Laptop：站定，低头盯着右前方，两只手交替敲；每敲一下身子轻轻一顿
      int ph = (now / 140) % 4;
      p.dy = (ph % 2) ? 1 : 0;
      p.armL = ph == 0 ? 1 : 0;
      p.armR = ph == 2 ? 1 : 0;
      p.lookX = U / 2;
      p.lookY = U / 2;
      p.eye = blink ? 1 : 0;
      bubble(CX + 30, 46, 64, toolIsTerminal(tool) ? 49 : 55, C_GREY);  // 贴着工具图标（终端 56×40，其它最大 56×46）留 4px 白边
      drawToolIcon(CX + 34, 51, now, tool);
      break;
    }

    case M_WAITING: {
      // 举手挥舞，头顶感叹号闪烁，屏幕边框闪橙色
      int ph = (now / 250) % 2;
      p.armL = ph ? 2 : 1;
      p.armR = ph ? 1 : 2;
      p.dy = ph ? -4 : 0;
      p.eye = 0;
      bubble(CX + 36, 44, 54, 62, C_CLAUDE);
      gfx->fillRoundRect(CX + 57, 52, 12, 32, 4, (now / 400) % 2 ? C_CLAUDE : C_RED);
      gfx->fillCircle(CX + 63, 94, 6, (now / 400) % 2 ? C_CLAUDE : C_RED);
      if ((now / 500) % 2) {
        for (int k = 0; k < 4; k++) gfx->drawRect(k, k, 480 - 2 * k, 272 - 2 * k, C_CLAUDE);
      }
      break;
    }

    case M_DONE: {
      // 跳起来 + ^^ + 星星
      uint32_t jt = t % 900;
      float s = jt / 900.0;
      p.dy = -(int)(60 * 4 * s * (1 - s));
      p.armL = p.armR = 2;
      p.eye = 2;
      if (p.dy > -8) p.squash = 4;
      for (int i = 0; i < 6; i++) {
        if (((now / 150) + i) % 3 == 0) continue;
        int sx = CX - 110 + (i * 47) % 220;
        int sy = 54 + (i * 31) % 70;
        uint16_t c = (i % 2) ? C_YELLOW : C_WHITE;
        gfx->fillRect(sx - 1, sy - 7, 3, 15, c);
        gfx->fillRect(sx - 7, sy - 1, 15, 3, c);
      }
      break;
    }

    case M_ERROR: {
      p.dx = ((now / 60) % 2) ? -4 : 4;
      p.eye = 3;
      // 汗滴
      int dy = (now / 40) % 30;
      gfx->fillCircle(CX + 80, 96 + dy, 7, C_BLUE);
      gfx->fillTriangle(CX + 73, 94 + dy, CX + 87, 94 + dy, CX + 80, 80 + dy, C_BLUE);
      break;
    }

    case M_COMPACT: {
      // 一挤一挤地压缩
      p.squash = 6 + (int)(6 * sin(now / 250.0));
      p.eye = 1;
      int n = (now / 200) % 4;
      for (int i = 0; i < n; i++) gfx->fillRect(CX + 60, 86 - i * 14, 40, 10, C_GREY);
      break;
    }
  }
  reactPre(p, react, now);
  drawClawd(p);
  reactPost(react, now);
}

// 秒数 → "2小时13分" / "3天4小时"
// 统一的时长格式：<1 分钟「N 秒」，<1 小时「N 分 N 秒」，<1 天「N 小时 N 分」，再往上「N 天 N 小时」（数字与中文之间空一格）
String fmtDuration(int32_t s) {
  if (s <= 0) return "即将";
  int d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
  if (d > 0) return String(d) + " 天 " + String(h) + " 小时";
  if (h > 0) return String(h) + " 小时 " + String(m) + " 分";
  if (m > 0) return String(m) + " 分 " + String(sec) + " 秒";
  return String(sec) + " 秒";
}

void drawQuota(int y, const char *name, float used, uint32_t resetAt) {
  const int x = 262, w = 206;
  gfx->setFont(u8g2_font_unifont_h_chinese4);
  gfx->setTextSize(1);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(x, y + 14);
  gfx->print(name);

  if (used < 0) {
    gfx->setTextColor(C_GREY);
    gfx->setCursor(x + 110, y + 14);
    gfx->print("暂无数据");
    gfx->fillRoundRect(x, y + 22, w, 14, 7, C_DIM);
    return;
  }

  float left = constrain(100.0 - used, 0, 100);
  uint16_t c = left > 50 ? C_GREEN : (left > 20 ? C_YELLOW : C_RED);

  String pct = "剩 " + String((int)(left + 0.5)) + "%";
  gfx->setTextColor(c);
  gfx->setCursor(x + 150, y + 14);
  gfx->print(pct);

  gfx->fillRoundRect(x, y + 22, w, 14, 7, C_DIM);
  int fw = (int)(w * left / 100.0);
  if (fw > 0) gfx->fillRoundRect(x, y + 22, max(fw, 14), 14, 7, c);

  gfx->setFont(u8g2_font_cubic11_h_cjk);
  gfx->setTextColor(C_GREY);
  gfx->setCursor(x, y + 52);
  uint32_t nowE = epochNow();
  if (resetAt && nowE) {
    gfx->print(fmtDuration((int32_t)(resetAt - nowE)) + "后重置");
  }
}

// ---------- 录屏 ----------
void sendFrame() {
  const int n = 480 * 272;
  if (!rleBuf) rleBuf = (uint8_t *)ps_malloc(n * 3 + 16);
  if (!rleBuf) return;
  uint16_t *fb = gfx->getFramebuffer();
  uint8_t *o = rleBuf + 12;
  for (int i = 0; i < n;) {
    uint16_t c = fb[i];
    int j = i + 1;
    while (j < n && fb[j] == c && j - i < 255) j++;
    *o++ = j - i;
    *o++ = c & 0xFF;
    *o++ = c >> 8;
    i = j;
  }
  uint32_t len = o - rleBuf - 12, ms = millis();
  rleBuf[0] = 0xA5; rleBuf[1] = 0x5A; rleBuf[2] = 'F'; rleBuf[3] = 'R';
  memcpy(rleBuf + 4, &ms, 4);
  memcpy(rleBuf + 8, &len, 4);
  if (!hostNetActive()) Serial.write(rleBuf, len + 12);  // 录屏只走 USB
}

// ---------- 背光 ----------
int blLevel = -1;
void setBacklight(int level) {
  if (level == blLevel) return;
  blLevel = level;
  ledcWrite(GFX_BL, level);
}

// ---------- 触摸：区分点按和拖动 ----------
void setScreen(Screen s) {
  if (s == SCR_MAGPIE && screen != SCR_MAGPIE) magWagAt = millis();  // 打开面板时喜鹊点头扇翅，同 magpie
  if (s == SCR_SET && screen != SCR_KEYB) settingsOpen();  // 打开 Wi-Fi 分区时扫一次
  screen = s;
}

// 记住开机页和 magpie 标签：停留 5 秒以上、而且和存过的不一样才写闪存（闪存写入次数有限）
void persistTick(uint32_t now) {
  static int savedScreen = -1, savedTab = -1;
  static int lastScreen = -1, lastTab = -1;
  static uint32_t since = 0;
  int sc = screen <= SCR_MAGPIE ? (int)screen : lastScreen;  // 设置 / 键盘 / 雷达页不作为开机页
  if (sc != lastScreen || magTab != lastTab) { lastScreen = sc; lastTab = magTab; since = now; return; }
  if (now - since < 5000) return;
  if (savedScreen < 0) { savedScreen = prefs.getUChar("screen", 0); savedTab = prefs.getUChar("mtab", 1); }
  if (sc >= 0 && sc != savedScreen) { prefs.putUChar("screen", sc); savedScreen = sc; }
  if (magTab != savedTab) { prefs.putUChar("mtab", magTab); savedTab = magTab; }
}

uint32_t frameT0 = 0;
bool tDown = false, tDrag = false;
int tx0, ty0, tLastY;
uint32_t tLastT = 0;
float tVel = 0;  // 手指速度（px/ms），松手时用来做惯性滑动
int tLastX = 0;
uint32_t pageDotsAt = 0;  // 刚滑动换页：底部显示页码小圆点
bool tSlider = false;  // 按下时落在设置页的滑块上：之后的移动都交给滑块

void onTap(int x, int y, uint32_t now) {
  if (lastWakeAt && now - lastWakeAt < 1500) return;  // 熄屏时的第一下只用来唤醒，不当成点击
  if (toastTap(x, y)) return;  // 点提示条：跳到对应页面
  if (screen == SCR_HOME) {
    homeTap(x, y);
  } else if (screen == SCR_BUDDY) {
    if (x < 50 && y < 46) setScreen(SCR_HOME);  // home 按钮：触控区比画出来的大一圈
    else if (y < 42 && x >= BS_X && x < BS_X + 3 * BS_W) buddyAgent = (x - BS_X) / BS_W;  // 标题栏：Claude / Codex / DeepSeek 切换
    else if (buddyAgent == 1 && x >= 252 && y >= QC_Y1 && cxAcctN > 1) Host.println("CXACC");  // 点 Codex 额度卡：换账号（自动 → 账号 1 → 账号 2 → 自动）
    else if (buddyAgent == 2 && x < 250 && y > 60 && dsPokeUntil < now) {  // 点小鲸鱼：跳出水面喷一下水，咕噜一声
      dsPokeUntil = now + 1800;
      sfxPlay("dpoke");
    } else if (buddyAgent == 1 && x < 250 && y > 60 && cxPokeUntil < now) {
      cxPokeUntil = now + 1800;
      sfxPlay("cpoke");
    } else if (buddyAgent == 0 && x < 250 && y > 60 && pokeUntil < now) {  // 点角色：开心跳一下，啵一声
      pokeUntil = now + 1800;
      sfxPlay("poke");
    }
  } else if (screen == SCR_SET) {
    settingsTap(x, y);
  } else if (screen == SCR_KEYB) {
    keyboardTap(x, y, now);
  } else if (screen == SCR_RADAR) {
    radarTap(x, y);
  } else {
    magpieTap(x, y);
  }
}

// ---------- 左右滑动换页：首页 → Claude → Codex → DeepSeek → magpie → 雷达 ----------
const int PAGE_N = 6;
int pageIndex() {
  switch (screen) {
    case SCR_HOME: return 0;
    case SCR_BUDDY: return 1 + buddyAgent;
    case SCR_MAGPIE: return 4;
    case SCR_RADAR: return 5;
    default: return -1;  // 设置页、键盘不参与
  }
}

bool pageSwipe(int dir, uint32_t now) {
  int i = pageIndex();
  if (i < 0) return false;
  int j = constrain(i + dir, 0, PAGE_N - 1);
  pageDotsAt = now;
  if (j == i) return true;
  if (j == 0) setScreen(SCR_HOME);
  else if (j <= 3) { buddyAgent = j - 1; setScreen(SCR_BUDDY); }
  else setScreen(j == 4 ? SCR_MAGPIE : SCR_RADAR);
  return true;
}

// 页码小圆点：换页后显示 1.5 秒（当前页是一条长方块）
void drawPageDots(uint32_t now) {
  int i = pageIndex();
  if (i < 0 || !pageDotsAt || now - pageDotsAt > 1500) return;
  int w = (PAGE_N - 1) * 10 + 8 + 16, x = 240 - w / 2, y = 226;
  pxPanel(x, y, w, 14, P_PANEL, P_EDGE, false);
  int cx = x + 8;
  for (int k = 0; k < PAGE_N; k++) {
    int dw = k == i ? 16 : 6;
    gfx->fillRect(cx, y + 5, dw, 4, k == i ? P_TEXT : P_DIM);
    cx += dw + 4;
  }
}

void pollTouch(uint32_t now) {
  tp.read();
  bool down = tp.isTouched && tp.touches > 0;
  int x = down ? tp.points[0].x : 0, y = down ? tp.points[0].y : 0;
  if (down && !tDown) {
    tDown = true;
    tDrag = false;
    tx0 = x; ty0 = y; tLastY = y; tLastX = x;
    tLastT = now;
    tVel = 0;
    tSlider = screen == SCR_SET && settingsDown(x, y);
    lastTouchAt = now;
    wakeUp(now);
  } else if (down && tSlider) {
    settingsSlide(x);
  } else if (down) {
    if (!tDrag && (abs(x - tx0) > 10 || abs(y - ty0) > 10)) tDrag = true;
    if (tDrag && screen == SCR_MAGPIE) magpieDrag(y - tLastY);
    if (tDrag && screen == SCR_SET) settingsDrag(y - tLastY);
    if (now > tLastT) tVel = 0.6f * tVel + 0.4f * (float)(y - tLastY) / (now - tLastT);
    tLastY = y;
    tLastX = x;
    tLastT = now;
  } else if (tDown) {
    tDown = false;
    int dx = tLastX - tx0, dy = tLastY - ty0;
    if (tSlider) settingsUp();
    else if (!tDrag) onTap(tx0, ty0, now);
    else if (abs(dx) > 70 && abs(dx) > abs(dy) * 3 / 2 && pageSwipe(dx < 0 ? 1 : -1, now)) {}  // 左右滑：换页
    else if (screen == SCR_MAGPIE) magpieFling(now - tLastT < 80 ? tVel : 0);  // 松手：惯性 + 吸附
  }
}

// ---------- 主流程 ----------
void setup() {
  batBootGuard();  // 电池快没电就直接深睡，不开屏幕和喇叭
  Serial.setTxBufferSize(16384);  // 录屏时一次要发几十 KB
  Serial.setRxBufferSize(16384);  // magpie 消息一条 1~2KB，画一帧期间要能存得下
  Serial.begin(115200);
  Serial.setTimeout(0);
  Serial.setTxTimeoutMs(200);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  delay(150);
  {
    Preferences p;  // setup 里 prefs 还没打开，先单独读校准值
    p.begin("buddy", true);
    panelInv = p.getBool("pinv", true);
    panelRot = p.getUChar("prot", 2);
    p.end();
  }
  fixPanel();
  gfx->setUTF8Print(true);
  flushInit();
  ledcAttach(GFX_BL, 5000, 8);
  setBacklight(255);

  Wire.setBufferSize(256);  // GT911 配置块 185 字节，超过默认 128 字节缓冲
  tp.begin();
  tp.setRotation(ROTATION_NORMAL);
  Wire.setClock(400000);

  prefs.begin("buddy", false);
  panelGamma = prefs.getFloat("gamma", 1.6);
  buildLut();
  screen = (Screen)min((int)prefs.getUChar("screen", SCR_HOME), (int)SCR_MAGPIE);
  magTab = min((int)prefs.getUChar("mtab", 1), 3);
  settingsLoad();
  tzOff = prefs.getInt("tz", 8 * 3600);
  wifiInit();  // 有保存的网络就自动连接
  netInit();   // Wi-Fi 链路：配对过就去连电脑上的 bridge
  audioInit();  // 喇叭：常驻音频任务
  micStart();   // 麦克风：常开
  radarInit();  // 毫米波雷达（P1），给 Clawd 的声音反应用
  batInit();    // 锂电池电压（GPIO5）

  modeSince = millis();
}

// 这块屏没有硬件复位脚，冷启动后屏幕芯片处于上电默认状态（不再是出厂固件留下的配置）：
// 反色与扫描方向需要按实物校准，保存在 NVS（{"t":"ui","inv":1,"rot":2}）

void fixPanelNow() {
  lcd->invertDisplay(panelInv);
  lcd->setRotation(panelRot);
}

// ---------- 双画布 + 后台刷屏 ----------
// 两块整帧画布轮流用：主循环（核心 1）画这一帧的同时，后台任务（核心 0）把上一帧发给屏幕，
// CPU 不再干等那 21ms。屏幕命令（反色 / 扫描方向）也交给刷屏任务去发，免得和发送中的数据抢总线。
Arduino_Canvas *cvs[2] = {nullptr, nullptr};
int cvDraw = 0;
volatile int flushIdx = 0;
volatile bool fixReq = false;
SemaphoreHandle_t flushGo = nullptr, flushIdle = nullptr;
TaskHandle_t flushTask = nullptr;

// 只发变化的部分：和上一次发出去的画面按 8 行一段比较，只把变了的段发给屏幕（大部分页面每帧只有一小块在动）。
// 校准屏幕（每 5 秒一次）后整屏重发一次，万一屏幕内容乱了也能自己恢复。
uint16_t *lastSent = nullptr;
bool fullNext = true;
volatile uint32_t flushRows = 0;  // 性能统计：每帧实际发了多少行

void flushLoop(void *) {
  const int BAND = 8, ROWB = 480 * 2;
  for (;;) {
    xSemaphoreTake(flushGo, portMAX_DELAY);
    if (fixReq) {
      fixReq = false;
      fixPanelNow();
      fullNext = true;
    }
    uint16_t *fb = cvs[flushIdx]->getFramebuffer();
    if (!lastSent || fullNext) {
      cvs[flushIdx]->flush();
      if (lastSent) memcpy(lastSent, fb, 480 * 272 * 2);
      fullNext = false;
      flushRows = 272;
    } else {
      uint32_t rows = 0;
      int y = 0;
      while (y < 272) {
        while (y < 272 && !memcmp(fb + y * 480, lastSent + y * 480, min(BAND, 272 - y) * ROWB)) y += BAND;
        if (y >= 272) break;
        int y1 = y;
        while (y1 < 272 && memcmp(fb + y1 * 480, lastSent + y1 * 480, min(BAND, 272 - y1) * ROWB)) y1 += BAND;
        y1 = min(y1, 272);
        lcd->draw16bitRGBBitmap(0, y, fb + y * 480, 480, y1 - y);
        memcpy(lastSent + y * 480, fb + y * 480, (y1 - y) * ROWB);
        rows += y1 - y;
        y = y1;
      }
      flushRows = rows;
    }
    xSemaphoreGive(flushIdle);
  }
}

void fixPanel() {
  if (flushTask) fixReq = true;  // 下一次刷屏前由刷屏任务处理
  else fixPanelNow();
}

void flushInit() {
  cvs[0] = gfx;
  cvs[1] = new FastCanvas(480, 272, lcd);
  if (!cvs[1]->begin(GFX_SKIP_OUTPUT_BEGIN)) {  // 内存不够：退回单画布同步刷屏
    delete cvs[1];
    cvs[1] = nullptr;
    return;
  }
  cvs[1]->setUTF8Print(true);
  lastSent = (uint16_t *)ps_malloc(480 * 272 * 2);
  flushGo = xSemaphoreCreateBinary();
  flushIdle = xSemaphoreCreateBinary();
  xSemaphoreGive(flushIdle);
  xTaskCreatePinnedToCore(flushLoop, "flush", 4096, nullptr, 1, &flushTask, 0);
}

// 画完一帧：等上一帧发完，把这一帧交给刷屏任务，换另一块画布接着画
void presentFrame() {
  if (!flushTask) {
    gfx->flush();
    return;
  }
  xSemaphoreTake(flushIdle, portMAX_DELAY);
  flushIdx = cvDraw;
  xSemaphoreGive(flushGo);
  cvDraw ^= 1;
  gfx = cvs[cvDraw];
}

void loop() {
  static uint32_t lastFrame = 0, lastFix = 0;
  pollSerial();
  pollNet();
  if (millis() - lastFix > 5000) {  // 每 5 秒重设一次，供电抖动导致屏幕芯片复位也能自己恢复
    lastFix = millis();
    fixPanel();
  }

  uint32_t now = millis();

  // 10 秒没收到任何消息 → 视为 bridge 断开
  if (lastMsgAt && now - lastMsgAt > 10000 && mode != M_OFFLINE) {
    mode = M_OFFLINE;
    modeSince = now;
    label = "未连接";
    statusDetail = "请在电脑上启动 bridge（USB 或 Wi-Fi）";
    sessions = 0;
    cxMode = M_OFFLINE;
    cxLabel = "未连接";
    dsMode = M_OFFLINE;
    dsLabel = "未连接";
  }

  pollTouch(now);
  wifiTick();

  if (now - lastFrame < 33) return;  // 约 30fps（画和刷屏并行后每帧绘制都在 30ms 以内）
  lastFrame = now;

  settingsTick(now);
  talkTick(now);
  voiceGateTick();
  radarTick();
  presenceTick(now);
  seatCalibTick(now);
  toastTick(now);
  persistTick(now);
  typingTick(now);
  powerTick(now);
  batTick(now);
  if (screenOff) return;  // 熄屏：传感器照常跑，只是不画
  int bl = cfgBright * 255 / 100;
  int lm = localMinute();
  if (cfgNightDim && lm >= 0 && (lm >= 23 * 60 || lm < 7 * 60)) bl = min(bl, 255 * 30 / 100);  // 夜里调暗到 30%
  if (clockDim) bl = min(bl, 255 * 10 / 100);  // 离开久了：时钟调暗（hw_act.ino 的 powerTick）
  setBacklight(bl);  // Agent 休息时不单独调暗，只跟着亮度、夜里调暗、离开后调暗走
  frameT0 = micros();

  switch (screen) {
    case SCR_HOME: drawHome(now); break;
    case SCR_BUDDY:
      gfx->fillScreen(P_BG);
      if (buddyAgent == 2) {  // DeepSeek（ui_deepseek.ino）
        drawDeepSeekScene(now);
        drawDeepSeekPanel(now);
      } else if (buddyAgent == 1) {  // Codex（ui_codex.ino）
        drawCodexScene(now);
        drawCodexPanel(now);
      } else {
        drawCharacterScene(now);
        drawPanelPx(now);
      }
      pxHomeBtn(6, 6);
      break;
    case SCR_MAGPIE: drawMagpie(now); break;
    case SCR_SET: drawSettings(now); break;
    case SCR_KEYB: drawKeyboardPx(now); break;
    case SCR_RADAR: drawRadarPx(now); break;
  }
  if (screen != SCR_HOME || !standbyOn(now)) drawToast(now);  // 全局提醒条（ui_toast.ino）
  drawPageDots(now);
  micLogTick();
  if (capUntil && (int32_t)(capUntil - now) > 0) sendFrame();  // 先录原始色
  uint32_t tf = micros();
  presentFrame();  // 校色在 QspiFast 发送时顺带完成；有双画布时这里只等上一帧发完
  if (perfOn) {
    perfAcc[3] += micros() - tf;
    perfAcc[0] += micros() - frameT0;
    perfN++;
    if (millis() - perfAt > 1000 && perfN) {
      Host.printf("PERF rows=%lu screen=%d tab=%d frame=%lu list=%lu stage=%lu flush=%lu fps=%lu\n", flushRows, screen, magTab,
                    perfAcc[0] / perfN, perfAcc[1] / perfN, perfAcc[2] / perfN, perfAcc[3] / perfN, perfN * 1000 / (millis() - perfAt));
      perfAcc[0] = perfAcc[1] = perfAcc[2] = perfAcc[3] = 0;
      perfN = 0;
      perfAt = millis();
    }
  }
}
