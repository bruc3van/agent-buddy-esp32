// magpie 面板数据：bridge 从 magpie 本地网关取来，整理成可直接显示的文字后经串口下发
#pragma once
#include <Arduino.h>

// ---------- 额度（mq）----------
struct MqWindow {
  String label;  // "5h" / "7d"
  int left;      // 剩余百分比
};
struct MqCard {
  String user;   // 账号（邮箱）
  String reset;  // "02:19 · 7d 10月19日 18:59"
  MqWindow w[3];
  int nw = 0;
};
struct MqGroup {
  String name;   // "Claude Code"
  String icon;   // "claude-color"
  String plan;   // "pro"
  MqCard cards[6];
  int nc = 0;
};
MqGroup mqGroups[8];
int mqCount = 0;

// ---------- 用量（mu），4 个周期：今天 / 7 天 / 30 天 / 全部 ----------
struct MuPeriod {
  bool has = false;
  String tok, tsub;    // "12M" / "输入 106 · 输出 30.4K · 缓存 12M"
  String calls, csub;  // "52" / "没有失败"
  String cost, hit;    // "≈$3.59" / "99%"
  String gcalls, gcost;  // 只算经过网关的请求数 / 费用：首页用，和路由页同口径
  String ymax;         // 纵轴最大值 "20M"
  uint16_t bars[60];   // 0~1000，按 ymax 归一化
  int nb = 0;
  String xl[5];        // 横轴标签
  int nxl = 0;
};
MuPeriod muPeriods[4];
int muPeriod = 0;

// ---------- 路由（mr）：与 magpie 面板同口径，只看网关请求 ----------
struct MrRow {
  String agent, icon, model, kind, tm, served, info;
  bool err = false;
};
MrRow mrRows[10];
int mrCount = 0;
String mrHead = "";          // "今天 36 次调用 · 1.0M token"

// 连线图：左边 Agent、右边服务方，最多各 3 个
struct MsNode {
  String name, sub, icon;
  uint16_t color = 0;        // Agent 的颜色（magpie routing.js 的 HUES）
  uint8_t st = 1;            // 服务方状态：1 成功 2 失败
};
MsNode msAgs[3], msDst[3];
int msAgN = 0, msDstN = 0;

// 一次请求的飞行：光点 Agent → magpie → 服务方 → 等回答 → 原路返回（绿 / 红）
struct Trip {
  bool on = false;
  int8_t a = 0, d = 0;
  bool ok = true;
  uint32_t t0 = 0;
};
Trip trips[4];
const int TRIP_MS = 420 + 460 + 900 + 260 + 420 + 420;  // 一次飞行最长多久（tripPose 各段之和）
// 飞行中某一刻：光点位置、颜色、是否在服务方处等回答、点亮哪条线
struct TripPose { bool vis; int x, y; uint16_t c; bool waiting; int litA, litD; };

// ---------- Agent（ma）----------
struct MaAgent {
  String name, icon, model;
  bool hidden = false;
};
MaAgent maAgents[24];
int maCount = 0;
int maVisible = 0;

uint32_t magWagAt = 0;       // magpie 标题栏喜鹊上次开始点头扇翅的时间
float routeCol = 0, routeColTarget = 0;  // 路由舞台收起程度：0 展开，1 收起成细条
float routeS = 0;            // 路由页的滚动量：0~54 先收起舞台，超出部分滚动列表（跟手）
const int ROUTE_FOLD = 54;   // 舞台从展开到收起的高度差
const bool pixelStyle = true;  // 只保留像素风（经典风格已删除）
bool mrListDirty = true;    // 路由列表内容变了，需要重画缓存图（见 ui_pixel.ino routeListBlit）
bool perfOn = false;                       // 性能计时：开启后每秒经串口报一次各段耗时
uint32_t perfAcc[4] = {0, 0, 0, 0};       // 0 整帧 1 列表 2 舞台 3 刷屏（微秒累计）
uint32_t perfN = 0, perfAt = 0;     // 像素风小样开关（目前只影响 magpie 用量页）
int magTab = 1;              // 当前标签：0 Agent 1 额度 2 用量 3 路由
bool magpieOnline = false;   // magpie 网关是否可达（bridge 报告）

// ---------- 像素风配色（ui_pixel.ino）----------
#define P_BG      0x0862  // #0E0E12 底
#define P_PANEL   0x18C4  // #1A1A22 面板
#define P_EDGE    0x39C9  // #3A3A48 边框
#define P_SHADOW  0x0000  // 阴影
#define P_TEXT    0xEF7E  // #E8E8F0
#define P_DIM     0x8C54  // #8A8AA0
#define P_ACCENT  0x7B7F  // #7C6CFF magpie 紫
#define P_GREEN   0x3EF0  // #3DDC84
#define P_RED     0xFA8A  // #FF5050
#define P_GRID    0x2945  // #2A2A2E
