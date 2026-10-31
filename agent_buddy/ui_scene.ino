// 首页 · 桌面场景：Clawd 站在书桌上，旁边是显示器（显示 Agent 在干什么），喜鹊站在显示器上（magpie），
// Codex 站在显示器和台灯之间，DeepSeek 的小鲸鱼住在墙上的鱼缸里（和左边的窗户对称）。
// 窗外随真实时间变成白天 / 黄昏 / 夜晚，夜里台灯自动亮。底部三块小组件：额度 / 当前状态 / magpie 今天。
// 离开电脑后切到待机时钟（drawStandbyClock）。

const int DESK_Y = 200;                     // 桌面
const int MON_X = 180, MON_Y = 92, MON_W = 144, MON_H = 90;  // 显示器外框
const int SC_X = MON_X + 6, SC_Y = MON_Y + 6, SC_W = MON_W - 12, SC_H = MON_H - 12;  // 屏幕
const int DOCK_Y = 226, DOCK_H = 42;
int lampOverride = -1;  // 点台灯手动开关：-1 跟随时间
uint32_t sceneFlapAt = 0, lastTripN = 0;

#define S_WOOD    0x6A45  // 桌面 #6B4A2E
#define S_WOOD_HI 0x8B28  // 桌面高光
#define S_LAMP    0xFF13  // 暖光 #FFE29A
#define S_LEAF    0x3D88  // 叶子
#define S_POT     0xB367  // 花盆

// 本地时间（分钟，0~1439），不知道时间时返回 -1
int localMinute() {
  uint32_t e = epochNow();
  if (!e) return -1;
  int64_t lt = (int64_t)e + tzOff;
  return (int)((lt % 86400) / 60);
}

bool isNight() {
  int m = localMinute();
  return m >= 0 && (m >= 18 * 60 + 30 || m < 6 * 60 + 30);
}

// 像素喜鹊放大 2 倍
void drawBirdPx2(const uint8_t *frame, int size, int x, int y, uint16_t fg) {
  int bpr = (size + 3) / 4;
  for (int yy = 0; yy < size; yy++) {
    const uint8_t *row = frame + yy * bpr;
    for (int xx = 0; xx < size; xx++)
      if ((row[xx >> 2] >> (6 - 2 * (xx & 3))) & 3) gfx->fillRect(x + xx * 2, y + yy * 2, 2, 2, fg);
  }
}

// ---------- 窗户：天空随时间变化 ----------
// 只画落在 [lo, hi) 横向范围里的部分，飘出窗框的云被裁掉
void fillClipX(int rx, int ry, int rw, int rh, int lo, int hi, uint16_t c) {
  int x0 = max(rx, lo), x1 = min(rx + rw, hi);
  if (x1 > x0) gfx->fillRect(x0, ry, x1 - x0, rh, c);
}

// 广州塔（小蛮腰）：上窄、腰细、下宽的扭转网格，顶上一根天线。夜里网格亮彩灯，颜色从下往上流动
void drawCantonTower(int cx, int gy, int phase, uint32_t now, uint16_t body, uint16_t hi) {
  static const uint16_t NEON[5] = {0xF81F, 0x841F, 0x07FF, 0x07E8, 0xFFE0};
  const int H = 50, WAIST = 20;  // 塔身高度，细腰在第几行（从上往下）
  int top = gy - H;
  for (int r = 0; r < H; r++) {
    int tw = r < WAIST ? 8 - 4 * r / WAIST : 4 + 9 * (r - WAIST) / (H - WAIST);
    int lx = cx - tw / 2;
    gfx->fillRect(lx, top + r, tw, 1, body);
    uint16_t lc = phase == 0 ? NEON[(r / 5 + 50 - (now / 400) % 50) % 5] : hi;
    for (int c = 0; c < tw; c++)  // 斜柱 + 每 6 行一道环
      if ((c + r) % 3 == 0 || r % 6 == 0) gfx->drawPixel(lx + c, top + r, lc);
  }
  gfx->fillRect(cx - 5, top, 10, 2, phase == 0 ? NEON[(now / 400) % 5] : hi);  // 顶部观景平台
  uint16_t mast = phase == 0 ? 0x6B4D : body;  // 天线：夜里塔身颜色和夜空几乎一样，换成灰白才看得见塔尖
  gfx->fillRect(cx - 1, top - 6, 2, 6, mast);
  gfx->fillRect(cx, top - 14, 1, 8, mast);
  if (phase == 0 && (now / 700) % 2) gfx->drawPixel(cx, top - 14, 0xF800);  // 航空障碍灯
}

void drawWindow(uint32_t now) {
  const int x = 24, y = 46, w = 96, h = 74;
  int m = localMinute();
  uint16_t sky;
  int phase;  // 0 夜 1 清晨 2 白天 3 黄昏
  if (m < 0) { sky = 0x10A6; phase = 0; }
  else if (m < 5 * 60 + 30 || m >= 19 * 60 + 30) { sky = 0x0866; phase = 0; }
  else if (m < 7 * 60) { sky = 0xC3D0; phase = 1; }
  else if (m < 17 * 60 + 30) { sky = 0x5D3D; phase = 2; }
  else { sky = 0xD326; phase = 3; }
  pxPanel(x - 4, y - 4, w + 8, h + 8, P_EDGE, P_EDGE, true);
  gfx->fillRect(x, y, w, h, sky);
  if (phase == 0) {
    for (int i = 0; i < 9; i++) {  // 星星一闪一闪
      int sx = x + 6 + (i * 37) % (w - 12), sy = y + 5 + (i * 23) % (h - 30);
      if (((now / 600) + i) % 4) gfx->fillRect(sx, sy, 2, 2, 0xC618);
    }
    static const char *const MOON[10] = {"..1111", ".11111", "1111..", "111...", "111...",
                                         "111...", "111...", "1111..", ".11111", "..1111"};
    for (int r = 0; r < 10; r++)  // 弯月，让开右边的广州塔
      for (int c = 0; c < 6; c++)
        if (MOON[r][c] == '1') gfx->drawPixel(x + 56 + c, y + 9 + r, 0xF7BB);
  } else {
    int sx = phase == 1 ? x + 16 : (phase == 3 ? x + w - 34 : x + w / 2 - 6);
    int sy = phase == 2 ? y + 10 : y + h - 30;
    gfx->fillRect(sx, sy, 16, 16, phase == 2 ? 0xFEA0 : 0xFC40);  // 太阳
    gfx->fillRect(sx - 2, sy + 2, 20, 12, phase == 2 ? 0xFEA0 : 0xFC40);
    gfx->fillRect(sx + 2, sy - 2, 12, 20, phase == 2 ? 0xFEA0 : 0xFC40);
    if (phase == 2) {  // 慢慢飘的云，从窗框左边进、右边出
      int cx = x + (int)((now / 120) % (w + 40)) - 30;
      fillClipX(cx, y + 34, 26, 6, x, x + w, 0xFFFF);
      fillClipX(cx + 6, y + 30, 14, 4, x, x + w, 0xFFFF);
    }
  }
  // 远处的广州：珠江新城西塔、东塔，江对岸的广州塔，脚下是珠江
  uint16_t bld = phase == 0 ? 0x18E5 : (phase == 2 ? 0x4B12 : 0x6187);
  uint16_t hi = phase == 0 ? 0x3A0B : (phase == 2 ? 0x8D59 : 0x9A6A);
  uint16_t lit = 0xFE8C;
  const int gy = y + h - 5;  // 江岸
  const int8_t lx[6] = {0, 7, 46, 54, 63, 88}, lw[6] = {7, 6, 8, 9, 7, 8}, lh[6] = {12, 18, 14, 21, 11, 15};
  for (int i = 0; i < 6; i++) {  // 矮楼
    int bx = x + lx[i], by = gy - lh[i];
    gfx->fillRect(bx, by, lw[i], lh[i], bld);
    if (phase == 0)  // 夜里亮几盏灯
      for (int k = 0; k < lh[i] / 5; k++)
        if ((i * 5 + k * 3) % 3 == 0) gfx->fillRect(bx + 2 + (k % 2) * 2, by + 3 + k * 4, 2, 2, lit);
  }
  {  // 西塔：玻璃幕墙菱形网格，顶部略收
    const int bx = x + 14, bw = 11, bh = 47;
    for (int r = 0; r < bh; r++) {
      int in = r < 10 ? (10 - r) / 4 : 0;
      gfx->fillRect(bx + in, gy - bh + r, bw - 2 * in, 1, bld);
      for (int c = in; c < bw - in; c++)
        if ((c + r) % 6 == 0 || (c - r + 60) % 6 == 0) gfx->drawPixel(bx + c, gy - bh + r, phase == 0 ? 0x4ABF : hi);
    }
  }
  {  // 东塔：竖向肋条，往上分段收窄，顶上一圈镂空的皇冠
    const int cx = x + 37, bh = 55;
    for (int r = 0; r < bh; r++) {
      int tw = r < 7 ? 7 : (r < 18 ? 9 : (r < 32 ? 11 : 13));
      int bx = cx - tw / 2;
      if (r < 7) {  // 皇冠：竖条间隔镂空
        for (int c = 0; c < tw; c += 2) gfx->drawPixel(bx + c, gy - bh + r, phase == 0 ? 0xFFFF : hi);
        continue;
      }
      gfx->fillRect(bx, gy - bh + r, tw, 1, bld);
      for (int c = 1; c < tw; c += 3) gfx->drawPixel(bx + c, gy - bh + r, hi);
      if (phase == 0 && r % 4 == 1 && (r * 7) % 3) gfx->drawPixel(bx + 2 + (r % 3) * 3, gy - bh + r, lit);
    }
  }
  drawCantonTower(x + 79, gy, phase, now, bld, hi);
  // 珠江：水面 + 慢慢移动的波光
  uint16_t water = phase == 0 ? 0x0844 : (phase == 2 ? 0x3C17 : 0x8A49);
  gfx->fillRect(x, gy, w, y + h - gy, water);
  for (int i = 0; i < 7; i++) {
    int wx = x + (i * 29 + (int)(now / 250)) % (w - 4), wy = gy + 1 + (i % 3);
    gfx->fillRect(wx, wy, 3, 1, phase == 0 ? (i % 2 ? lit : 0xF81F) : hi);
  }
  gfx->fillRect(x + w / 2 - 1, y, 3, h, P_EDGE);  // 窗框十字
  gfx->fillRect(x, y + h / 2 - 1, w, 3, P_EDGE);
}

// ---------- 台灯 ----------
bool lampOn() { return lampOverride >= 0 ? lampOverride == 1 : isNight(); }

// 摇臂台灯（右边）：圆底座 → 下臂斜向上到肘关节 → 上臂向左上到灯头，灯罩朝左下照向桌面
// 灯罩开口两端 (RIM_AX, RIM_AY)-(RIM_BX, RIM_BY)，光从开口往下张开。整盏灯压在鱼缸下面，不挡鱼缸
const int RIM_AX = 414, RIM_AY = 150, RIM_BX = 430, RIM_BY = 166;

// 粗像素线：沿线每一步画 w×w 的方块
void pxLine(int x0, int y0, int x1, int y1, int w, uint16_t c) {
  int n = max(abs(x1 - x0), abs(y1 - y0));
  for (int i = 0; i <= n; i++) gfx->fillRect(x0 + (x1 - x0) * i / max(n, 1) - w / 2, y0 + (y1 - y0) * i / max(n, 1) - w / 2, w, w, c);
}

// 光：空气里是稀疏的暖色点，桌面上一块光斑。画在 Codex 之前，被它挡住
void lampLight() {
  for (int y = RIM_AY + 2; y < DESK_Y; y += 2) {
    int l = RIM_AX - (y - RIM_AY) * 3 / 5;                          // 左边界：往左下张开
    int r = y < RIM_BY ? RIM_AX + (y - RIM_AY) : RIM_BX + (y - RIM_BY) / 8;  // 右边界：先沿开口，再几乎竖直往下
    for (int x = l + ((y / 2) % 2) * 2; x < r; x += 4)
      if ((x + y) % 8 < 4) gfx->drawPixel(x, y, 0x5A46);
  }
  int l = RIM_AX - (DESK_Y - RIM_AY) * 3 / 5, r = RIM_BX + (DESK_Y - RIM_BY) / 8;
  gfx->fillRect(l, DESK_Y, r - l, 2, 0xD52C);  // 光斑
  gfx->fillRect(l + 10, DESK_Y + 2, r - l - 20, 2, 0xA407);
}

void drawLamp() {
  bool on = lampOn();
  const uint16_t M = 0x52AA, MH = 0x8C71, SH = 0xB5B6, SD = 0x7BCF;  // 金属 / 高光 / 灯罩 / 灯罩暗面
  // 底座：两层圆台
  gfx->fillRect(440, DESK_Y - 5, 34, 5, M);
  gfx->fillRect(446, DESK_Y - 9, 22, 4, M);
  gfx->fillRect(448, DESK_Y - 9, 18, 1, MH);
  gfx->fillRect(442, DESK_Y - 5, 30, 1, MH);
  // 下臂：底座 → 肘（右上），上臂：肘 → 灯头（左上）；旁边一根细弹簧线
  const int EX = 468, EY = 156, HX = 440, HY = RIM_AY - 14;  // 肘关节、灯头左上角
  pxLine(457, DESK_Y - 10, EX, EY, 3, M);
  pxLine(460, DESK_Y - 12, EX + 3, EY + 2, 1, MH);
  pxLine(EX, EY, HX + 2, HY + 6, 3, M);
  pxLine(EX, EY - 3, HX + 3, HY + 3, 1, MH);
  gfx->fillRect(EX - 3, EY - 3, 7, 7, MH);  // 肘关节
  gfx->fillRect(EX - 1, EY - 1, 3, 3, M);
  gfx->fillRect(455, DESK_Y - 13, 6, 5, MH);  // 底座转轴
  // 灯罩：一个朝左下的锥形（后窄前宽）
  gfx->fillTriangle(HX, HY, HX + 8, HY + 8, RIM_AX, RIM_AY, SH);
  gfx->fillTriangle(HX + 8, HY + 8, RIM_BX, RIM_BY, RIM_AX, RIM_AY, SD);
  gfx->fillRect(HX, HY, 8, 8, SH);  // 灯头
  pxLine(RIM_AX, RIM_AY, RIM_BX, RIM_BY, 2, on ? S_LAMP : P_DIM);  // 开口边沿
  if (on) gfx->fillRect((RIM_AX + RIM_BX) / 2 - 3, (RIM_AY + RIM_BY) / 2 - 1, 6, 4, 0xFFFF);  // 灯泡
}

// ---------- 桌上的小东西 ----------
void drawDeskProps(uint32_t now) {
  // 盆栽（龟背竹）：像素贴图放大 2 倍，叶子随风轻轻晃
  static const char *const PLANT[24] = {
    "......LL........",
    ".....LLLL.......",
    "....LLDLLL..LL..",
    "....LLDLLL.LLLL.",
    "..LL.LDLL.LLDLLL",
    ".LLLL.DL.LLDLLL.",
    "LLDLLL.D.LDLL...",
    "LLLDLLLD.DLL....",
    ".LLLDLLDD.L.....",
    "..LLLLDDD.......",
    "....LL.D..LLL...",
    "......D..LLDLLL.",
    "......DDLLDLLLL.",
    ".......DDLLL.L..",
    ".......D........",
    "..RRRRRRRRRRRR..",
    "..RooooooooooR..",
    "...PPPPPPPPPPd..",
    "...PPPPPPPPPPd..",
    "...PPPPPPPPPPd..",
    "....PPPPPPPPd...",
    "....PPPPPPPPd...",
    "....PPPPPPPPd...",
    ".....PPPPPPd....",
  };
  const int px0 = 8, py0 = DESK_Y - 48;
  int sway = (int)(sinf(now / 1800.0f) * 1.5f);
  for (int r = 0; r < 24; r++)
    for (int c = 0; c < 16; c++) {
      char ch = PLANT[r][c];
      if (ch == '.') continue;
      uint16_t col = ch == 'L' ? 0x4E8A : ch == 'D' ? 0x2C66 : ch == 'R' ? 0xCC0A : ch == 'o' ? 0x4204 : ch == 'd' ? 0x8A45 : S_POT;
      int dx = r < 10 ? sway : (r < 15 ? sway / 2 : 0);  // 越往上晃得越多
      gfx->fillRect(px0 + c * 2 + dx, py0 + r * 2, 2, 2, col);
    }
  // 马克杯 + 热气
  int mx = 154;
  gfx->fillRect(mx, DESK_Y - 16, 16, 16, 0xEF7D);
  gfx->fillRect(mx + 16, DESK_Y - 12, 4, 8, 0xEF7D);
  gfx->fillRect(mx + 2, DESK_Y - 14, 12, 3, C_CLAUDE);
  for (int i = 0; i < 2; i++) {
    uint32_t st = (now + i * 900) % 1800;
    int sy = DESK_Y - 20 - (int)(st / 60);
    if (st < 1500) gfx->fillRect(mx + 4 + i * 6 + (int)(2 * sinf(st / 200.0f)), sy, 2, 4, P_DIM);
  }
}

// ---------- 显示器：Agent 在干什么 ----------
// 屏幕上的一块：显示一个 Agent 的状态。(x, w) 为这一块的范围，窄的时候内容跟着缩小
// 显示器上现在显示的是什么：点显示器就进对应的页面（MON_CLAUDE / MON_CODEX / MON_DS 和 Agent 序号一致）
enum { MON_CLAUDE, MON_CODEX, MON_DS, MON_SPLIT, MON_RADAR };
int monShow = MON_CLAUDE, monSplit[2] = {0, 1};  // 分屏时左右两半各是哪个 Agent

void monitorPane(int x, int w, int a, int md, const String &lb, uint32_t now) {
  uint16_t acc = agColor(a), hi = a ? agHi(a) : C_YELLOW;
  bool narrow = w < 100;
  int cx = x + w / 2, cy = SC_Y + SC_H / 2 - 8;
  String line;
  uint16_t lc = P_TEXT;
  switch (md) {
    case M_OFFLINE:
      for (int k = 0; k < 3; k++) gfx->fillRect(x + 10 + k * 8, SC_Y + 8 + k * 6, 20, 2, 0x18C3);  // 反光
      line = "未连接";
      lc = P_DIM;
      break;
    case M_THINKING:
      if (a == 1) {  // Codex：三个点
        for (int i = 0; i < 3; i++) gfx->fillRect(cx - 16 + i * 14, cy - 2 - ((now / 300) % 3 == i ? 4 : 0), 6, 6, C_CODEX);
      } else if (a == 2) {  // DeepSeek：同官方，鲸鱼尾巴一下下甩
        drawIcon8(IC_DSTAIL8[dsTailFrame(now)], cx - (narrow ? 8 : 12), cy - (narrow ? 10 : 14), narrow ? 2 : 3, C_DS);
      } else {
        drawSpark(cx, cy, narrow ? 12 : 16, sparkFrame(now), now / 1600.0f, C_CLAUDE);
      }
      line = a == 2 ? DS_THINKING : "思考中";
      lc = acc;
      break;
    case M_WORKING: {  // 往上滚的代码行
      int off = (now / 60) % 12;
      for (int k = 0; k < 6; k++) {
        int y = SC_Y + 6 + k * 10 - off;
        if (y < SC_Y + 2 || y > SC_Y + SC_H - 26) continue;
        int seed = (int)((now / 720 + k + a * 3) * 2654435761u >> 24);
        int ind = (seed % 3) * (narrow ? 5 : 8), len = (narrow ? 16 : 30) + seed % (narrow ? 30 : 60);
        uint16_t c = (seed % 4 == 0) ? acc : (seed % 4 == 1 ? P_ACCENT : (seed % 4 == 2 ? P_GREEN : P_DIM));
        gfx->fillRect(x + 6 + ind, y, min(len, w - 12 - ind), 4, c);
      }
      line = lb.length() ? lb : String("干活中");
      lc = acc;
      break;
    }
    case M_WAITING: {
      bool f = (now / 400) % 2;
      for (int k = 0; k < 2; k++) gfx->drawRect(x + k, SC_Y + k, w - 2 * k, SC_H - 2 * k, f ? acc : hi);
      pxText("?", cx - pxW("?", narrow ? 2 : 3) / 2, cy + (narrow ? 10 : 14), f ? acc : hi, narrow ? 2 : 3);
      line = "等你确认";
      lc = acc;
      break;
    }
    case M_DONE: {  // 像素对勾
      for (int k = 0; k < 4; k++) gfx->fillRect(cx - 18 + k * 4, cy + k * 4 - 2, 5, 5, P_GREEN);
      for (int k = 0; k < 7; k++) gfx->fillRect(cx - 2 + k * 4, cy + 10 - k * 4, 5, 5, P_GREEN);
      line = "完成啦";
      lc = P_GREEN;
      break;
    }
    case M_ERROR:
      for (int k = 0; k < 7; k++) {
        gfx->fillRect(cx - 12 + k * 4, cy - 12 + k * 4, 4, 4, P_RED);
        gfx->fillRect(cx + 12 - k * 4, cy - 12 + k * 4, 4, 4, P_RED);
      }
      line = "出错了";
      lc = P_RED;
      break;
    case M_COMPACT: {
      int n = (now / 200) % 4;
      for (int i = 0; i < 4; i++) gfx->fillRect(cx - 24, cy - 14 + i * 8, 48 - (i < n ? 24 : 0), 4, P_DIM);
      line = "压缩上下文";
      break;
    }
    default: {  // 待命 / 睡觉：有雷达时显示迷你雷达屏，否则是命令行光标
      if (radarFresh() && !narrow) {
        monShow = MON_RADAR;
        drawMiniScope(x, SC_Y, w, SC_H, now);
        int n = radarCount();
        line = n ? String(n) + " 人" : String("没人");
        pxText("雷达", x + 5, SC_Y + 13, 0x1BC9, 1);  // 左上角：扇形外的空白处
        pxText(line, x + w - 5 - pxW(line, 1), SC_Y + 13, P_GREEN, 1);
        return;
      }
      pxText(">", x + 8, SC_Y + 22, P_GREEN, 1);
      if ((now / 500) % 2) gfx->fillRect(x + 18, SC_Y + 13, 7, 10, P_TEXT);
      line = md == M_SLEEP ? String("休息中") : String("待命");
      lc = P_DIM;
    }
  }
  if (md != M_OFFLINE) line = String(agName(a)) + (narrow ? "" : " · " + line);
  if (narrow && md != M_OFFLINE) lc = acc;
  pxMarquee(line, x + 5, SC_Y + SC_H - 7, lc, 1, w - 10);
}

// 三个 Agent 按「忙不忙」排序（等确认 > 干活 > 出错 > 完成 > 待命 > 未连接），一样忙时 Claude → Codex → DeepSeek
int agPri(int m) { return m == M_WAITING ? 5 : (m == M_WORKING || m == M_THINKING || m == M_COMPACT) ? 4 : m == M_ERROR ? 3 : m == M_DONE ? 2 : m == M_OFFLINE ? 0 : 1; }
void agOrder(int ord[3]) {
  for (int i = 0; i < 3; i++) ord[i] = i;
  for (int i = 1; i < 3; i++)
    for (int j = i; j > 0 && agPri(agMode(ord[j])) > agPri(agMode(ord[j - 1])); j--) {
      int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t;
    }
}

void drawMonitor(uint32_t now) {
  pxPanel(MON_X, MON_Y, MON_W, MON_H, P_EDGE, P_EDGE, true);
  gfx->fillRect(MON_X + MON_W / 2 - 8, MON_Y + MON_H, 16, DESK_Y - 6 - MON_Y - MON_H, P_EDGE);  // 支架
  gfx->fillRect(MON_X + MON_W / 2 - 24, DESK_Y - 6, 48, 6, P_EDGE);
  // 最忙的两个都在干活（含等确认）：左右分屏；否则显示最忙的那个
  int ord[3];
  agOrder(ord);
  bool both = agPri(agMode(ord[0])) >= 4 && agPri(agMode(ord[1])) >= 4;
  int a0 = ord[0], md = agMode(a0);
  gfx->fillRect(SC_X, SC_Y, SC_W, SC_H, md == M_OFFLINE ? 0x0000 : 0x0841);
  monShow = both ? MON_SPLIT : a0;  // 迷你雷达在 monitorPane 里改成 MON_RADAR
  if (both) {
    int hw = SC_W / 2;
    monSplit[0] = min(ord[0], ord[1]);  // 左右按固定顺序放，不随谁更忙跳来跳去
    monSplit[1] = max(ord[0], ord[1]);
    for (int k = 0; k < 2; k++) monitorPane(SC_X + k * (hw + 1), hw - 1, monSplit[k], agMode(monSplit[k]), agLabel(monSplit[k]), now);
    gfx->fillRect(SC_X + hw - 1, SC_Y, 2, SC_H, P_EDGE);  // 分屏线
  } else {
    monitorPane(SC_X, SC_W, a0, md, agLabel(a0), now);
  }
  uint16_t acc = agColor(a0);
  // 键盘
  int kx = MON_X + 14, kw = MON_W - 28;
  gfx->fillRect(kx, DESK_Y - 5, kw, 5, P_EDGE);
  bool typing = md == M_WORKING || userTyping();
  for (int c = 0; c < kw / 6; c++) {
    bool lit = typing && ((now / 90) * 7 + c * 3) % 9 == 0;
    gfx->fillRect(kx + 2 + c * 6, DESK_Y - 4, 4, 2, lit ? acc : P_DIM);
  }
  // 鼠标 + 鼠标垫（键盘右边）：干活时滚轮亮起，鼠标偶尔挪一下
  int mx = kx + kw + 9 + ((typing && (now / 700) % 3 == 0) ? 1 : 0);
  gfx->fillRect(kx + kw + 4, DESK_Y - 2, 22, 2, 0x2124);  // 垫子
  gfx->fillRect(mx + 3, DESK_Y - 8, 6, 1, P_EDGE);          // 圆圆的背
  gfx->fillRect(mx + 1, DESK_Y - 7, 10, 1, P_EDGE);
  gfx->fillRect(mx, DESK_Y - 6, 12, 4, P_EDGE);
  gfx->fillRect(mx + 2, DESK_Y - 7, 3, 1, 0x5ACF);           // 高光
  gfx->fillRect(mx + 5, DESK_Y - 7, 2, 2, typing ? acc : P_DIM);  // 滚轮
}

// ---------- 场景里的 Clawd ----------
void drawSceneClawd(uint32_t now) {
  Pose p;
  Mode m = mode;
  if (pokeUntil > now) m = M_DONE;
  int react = clawdReact(m, now);
  if (react == R_SLEEP) { m = M_SLEEP; react = R_NONE; }
  else if (react) m = M_IDLE;
  bool blink = (now % 3700) < 140;
  int oU = U, oCX = CX, oBASE = BASE;
  U = 5; CX = 100; BASE = DESK_Y;  // 和喜鹊、鱼缸里的小鲸鱼差不多大
  int top = BASE - 8 * U;
  switch (m) {
    case M_OFFLINE: p.color = C_GREY; p.eye = 1; break;
    case M_SLEEP:
      p.eye = 4;
      p.squash = 2 + (int)(1.5f * sinf(now / 1200.0f));
      for (int i = 0; i < 3; i++) {
        uint32_t zt = (now + i * 900) % 2700;
        pxText("z", CX + 4 * U + i * 7 + (int)(zt / 150), top - (int)(zt / 60) - i * 4, P_DIM, 1 + (i == 2));
      }
      break;
    case M_THINKING:  // 同官方：眯眼往上想（思考的星芒在显示器上）
      p.dy = ((now / 700) % 2) ? -1 : 0;
      p.lookX = U / 4; p.lookY = -U / 2; p.armR = 1;
      p.eye = 1;
      break;
    case M_WORKING: {  // 同官方 Laptop：站定盯着显示器，两手交替敲
      int ph = (now / 140) % 4;
      p.dy = (ph % 2) ? 1 : 0;
      p.armL = ph == 0 ? 1 : 0;
      p.armR = ph == 2 ? 1 : 0;
      p.lookX = U / 2; p.lookY = U / 2;
      p.eye = blink ? 1 : 0;
      break;
    }
    case M_WAITING: {  // 举手挥舞，头顶感叹号
      int ph = (now / 250) % 2;
      p.armL = ph ? 2 : 1; p.armR = ph ? 1 : 2;
      p.dy = ph ? -3 : 0;
      uint16_t c = (now / 400) % 2 ? C_CLAUDE : C_RED;
      gfx->fillRect(CX + 4 * U, top - 22, 4, 12, c);
      gfx->fillRect(CX + 4 * U, top - 7, 4, 4, c);
      break;
    }
    case M_DONE: {
      float s = (now % 900) / 900.0f;
      p.dy = -(int)(20 * 4 * s * (1 - s));
      p.armL = p.armR = 2;
      p.eye = 2;
      for (int i = 0; i < 4; i++) {
        if (((now / 150) + i) % 3 == 0) continue;
        int sx = CX - 42 + i * 28, sy = top - 20 + (i * 13) % 20;
        gfx->fillRect(sx - 1, sy - 5, 3, 11, i % 2 ? C_YELLOW : C_WHITE);
        gfx->fillRect(sx - 5, sy - 1, 11, 3, i % 2 ? C_YELLOW : C_WHITE);
      }
      break;
    }
    case M_ERROR:
      p.dx = ((now / 60) % 2) ? -2 : 2;
      p.eye = 3;
      break;
    case M_COMPACT:
      p.squash = 3 + (int)(3 * sinf(now / 250.0f));
      p.eye = 1;
      break;
    default: {  // 待命：同官方，身子不动，只有眼神左右上下转
      static const int8_t LX[8] = {0, 0, 1, 1, 0, -1, -1, 0}, LY[8] = {0, 0, 0, 0, -1, 0, 0, 0};
      uint32_t ph = (now / 900) % 8;
      p.lookX = LX[ph] * U / 2; p.lookY = LY[ph] * U / 2;
      p.eye = blink ? 1 : 0;
    }
  }
  reactPre(p, react, now);
  drawClawd(p);
  reactPost(react, now);
  U = oU; CX = oCX; BASE = oBASE;
}

// 「谁完成了几个任务」：只有一方时「期间 Claude 完成了 3 个任务」，多方都有时「期间完成 5 个任务（Claude 3 · Codex 2）」
String doneSummary(int cl, int cx, int ds, const char *when) {
  int n[3] = {cl, cx, ds}, who = 0, total = 0;
  String parts;
  for (int a = 0; a < 3; a++) {
    if (!n[a]) continue;
    who++;
    total += n[a];
    parts += String(parts.length() ? " · " : "") + agName(a) + " " + n[a];
  }
  if (!who) return "";
  if (who == 1)
    for (int a = 0; a < 3; a++)
      if (n[a]) return String(when) + " " + agName(a) + " 完成了 " + n[a] + " 个任务";
  return String(when) + "完成 " + total + " 个任务（" + parts + "）";
}

// ---------- 欢迎回来的气泡 ----------
void drawGreeting(uint32_t now) {
  if (!backAt || now - backAt > 12000) return;
  String l1 = "欢迎回来，你离开了 " + (backAway < 3600 ? String(backAway / 60) + " 分钟" : fmtDuration(backAway));
  String l2 = doneSummary(backDone, backCx, backDs, "期间");
  if (!l2.length()) l2 = "期间没有新任务";
  if (backWait) l2 += "，" + String(backWait) + " 个等你确认";
  int w = max(pxW(l1, 1), pxW(l2, 1)) + 24;
  // 气泡底边离 Clawd 头顶（约 y=160）留出一条尾巴的长度，尾巴尖落在头顶正上方（Clawd 中线 x=100）
  int x = 14, y = 94;
  pxPanel(x, y, w, 50, 0xFFFF, 0xFFFF, false);
  for (int k = 0; k < 4; k++) gfx->fillRect(104 - k * 2, y + 50 + k * 3, 10 - k * 2, 3, 0xFFFF);  // 尾巴：往左下收尖，尖在 x≈100
  pxText(l1, x + 12, y + 21, C_BLACK, 1);
  pxText(l2, x + 12, y + 40, backWait ? 0xC300 : 0x4208, 1);
}

// ---------- 底部组件 ----------
// 一行额度：左边品牌色小方块 + 名字，中间方块条，右边剩余百分比（5 小时窗口）
void dockQuotaLine(const char *name, uint16_t tint, float used, int x, int y, int w) {
  gfx->fillRect(x, y - 8, 6, 6, tint);
  pxText(name, x + 10, y, P_DIM, 1);
  if (used < 0) {
    pxText("—", x + w - pxW("—", 1), y, P_DIM, 1);
    return;
  }
  float left = constrain(100.0f - used, 0.0f, 100.0f);
  uint16_t c = left > 50 ? P_GREEN : (left > 20 ? C_YELLOW : P_RED);
  String pct = String((int)(left + 0.5f)) + "%";
  int pw = pxW(pct, 1);
  int bx = x + 10 + pxW(name, 1) + 6;
  pxSegBar(bx, y - 8, x + w - pw - 6 - bx, 8, left / 100.0f, c, P_EDGE);
  pxText(pct, x + w - pw, y, c, 1);
}

// 一行余额：DeepSeek 没有额度窗口，右边直接写余额
void dockBalLine(int x, int y, int w) {
  gfx->fillRect(x, y - 8, 6, 6, C_DS);
  pxText("DeepSeek", x + 10, y, P_DIM, 1);
  pxText(dsBal, x + w - pxW(dsBal, 1), y, dsBalOk ? P_GREEN : P_RED, 1);
}

int dockQuotaA[2] = {0, 1}, dockRowA[2] = {0, 1};  // 底部两块组件每一行是哪个 Agent（点哪行进哪个）

void drawDock(uint32_t now) {
  const int w = 150;
  // 额度：有数据的排前面（Claude / Codex 的 5 小时额度，DeepSeek 的余额），只放得下两行
  int n = 0;
  bool has[3] = {h5Used >= 0, cxH5Used >= 0, dsBal.length() > 0};
  for (int a = 0; a < 3 && n < 2; a++)
    if (has[a]) dockQuotaA[n++] = a;
  for (int a = 0; a < 2 && n < 2; a++)
    if (!has[a] && (n == 0 || dockQuotaA[0] != a)) dockQuotaA[n++] = a;
  if (dockQuotaA[0] > dockQuotaA[1]) { int t = dockQuotaA[0]; dockQuotaA[0] = dockQuotaA[1]; dockQuotaA[1] = t; }  // 按固定顺序放
  int x = 8;
  pxPanel(x, DOCK_Y, w, DOCK_H, P_PANEL, P_EDGE, true);
  for (int k = 0; k < 2; k++) {
    int a = dockQuotaA[k], y = DOCK_Y + 18 + k * 17;
    if (a == 2) dockBalLine(x + 10, y, w - 20);
    else dockQuotaLine(agName(a), agColor(a), a ? cxH5Used : h5Used, x + 10, y, w - 20);
  }
  // 当前状态：两行，放最忙的两个 Agent
  int ord[3];
  agOrder(ord);
  dockRowA[0] = min(ord[0], ord[1]);  // 两行按固定顺序放
  dockRowA[1] = max(ord[0], ord[1]);
  x = 165;
  pxPanel(x, DOCK_Y, w, DOCK_H, P_PANEL, P_EDGE, true);
  for (int k = 0; k < 2; k++) {
    int a = dockRowA[k], m = agMode(a), y = DOCK_Y + 18 + k * 17;
    bool busy = m == M_THINKING || m == M_WORKING || m == M_COMPACT || m == M_WAITING;
    gfx->fillRect(x + 10, y - 8, 6, 6, agColor(a));
    String t = m == M_OFFLINE ? String("未连接") : agLabel(a);
    uint32_t ts = agTurnStart(a);
    if (busy && ts) t += a == 2 ? "，用时 " + dsDuration((now - ts) / 1000) : " " + fmtDuration((now - ts) / 1000);  // DeepSeek 照 dsh 的写法
    pxMarquee(t, x + 20, y, m == M_WAITING ? agHi(a) : (busy ? P_TEXT : P_DIM), 1, w - 30);
  }
  // magpie 今天
  x = 322;
  pxPanel(x, DOCK_Y, w, DOCK_H, P_PANEL, P_EDGE, true);
  drawBirdPx(wag28[0], WAG28_SIZE, x + 6, DOCK_Y + 7, magpieOnline ? P_TEXT : P_DIM);
  const MuPeriod &T = muPeriods[0];
  String s1 = !magpieOnline ? String("未连接") : (T.has ? T.gcalls + " 次请求" : String("读取中…"));  // 网关请求，同路由页
  pxMarquee(s1, x + 40, DOCK_Y + 18, magpieOnline ? P_TEXT : P_DIM, 1, w - 46);
  if (T.has) pxMarquee("今天花费 " + T.gcost, x + 40, DOCK_Y + 35, P_DIM, 1, w - 46);
}

// ---------- 鱼缸：DeepSeek 的小鲸鱼住在墙上的鱼缸里，位置大小和左边的窗户左右对称 ----------
const int TK_W = 96, TK_H = 74, TK_X = 480 - 24 - TK_W, TK_Y = 46;  // 缸内（外面一圈 4px 框，同窗户）

void drawAquarium(uint32_t now) {
  dsTankFrame(TK_X, TK_Y, TK_W, TK_H);  // 上面敞口
  dsTank(TK_X, TK_Y, TK_W, TK_H, TK_Y + 20, 2, TK_Y + 54, now);  // 画法和 Buddy 页的大鱼缸一样（ui_deepseek.ino）
}

// ---------- 整页 ----------
void drawHomeScene(uint32_t now) {
  gfx->fillScreen(P_BG);
  for (int y = 42; y < DESK_Y; y += 12)  // 墙上的点阵
    for (int x = 6; x < 480; x += 12) gfx->drawPixel(x, y, P_DOT);
  drawHomeTopBar(now);
  drawWindow(now);
  drawLamp();
  drawAquarium(now);
  // 桌子
  gfx->fillRect(0, DESK_Y, 480, 8, S_WOOD);
  gfx->fillRect(0, DESK_Y, 480, 2, S_WOOD_HI);
  gfx->fillRect(0, DESK_Y + 8, 480, DOCK_Y - DESK_Y - 8, 0x0841);
  drawDeskProps(now);
  drawMonitor(now);
  if (lampOn()) lampLight();
  drawCodexAt(372, DESK_Y, 1, cxMode, now);  // Codex 站在显示器和台灯之间（ui_codex.ino）
  // 喜鹊站在显示器上：有新的路由请求就扇一下翅膀，平时每 8 秒点头一次
  uint32_t tn = 0;
  for (Trip &T : trips)
    if (T.on && T.t0 > tn) tn = T.t0;
  if (tn != lastTripN || now - sceneFlapAt > 8000) sceneFlapAt = now;
  lastTripN = tn;
  drawBirdPx2(wag28[wagFrame(now, sceneFlapAt)], WAG28_SIZE, MON_X + MON_W - 62, MON_Y - 52, magpieOnline ? P_TEXT : P_DIM);
  drawSceneClawd(now);
  drawGreeting(now);
  drawDock(now);
}

void sceneTap(int x, int y) {
  if (y >= DOCK_Y) {
    int k = y >= DOCK_Y + DOCK_H / 2 ? 1 : 0;  // 上一行 / 下一行
    if (x < 160) buddyAgent = dockQuotaA[k];
    else if (x < 322) buddyAgent = dockRowA[k];
    setScreen(x >= 322 ? SCR_MAGPIE : SCR_BUDDY);
  } else if (x >= MON_X + MON_W - 70 && x < MON_X + MON_W && y < MON_Y + 10) {
    setScreen(SCR_MAGPIE);  // 喜鹊
  } else if (x >= TK_X - 4 && y < TK_Y + TK_H + 7) {
    buddyAgent = 2;  // 鱼缸里的小鲸鱼：DeepSeek
    setScreen(SCR_BUDDY);
  } else if (x >= 344 && x < 404 && y >= 140 && y < DESK_Y + 4) {
    buddyAgent = 1;  // Codex
    setScreen(SCR_BUDDY);
  } else if (x >= 404 && y >= RIM_AY - 16 && y < DESK_Y) {
    lampOverride = lampOn() ? 0 : 1;  // 台灯
  } else if (x >= 56 && x < 146 && y >= 140) {
    buddyAgent = 0;
    setScreen(SCR_BUDDY);  // 点 Clawd 进 Agent Buddy 页（戳它的效果留在 Buddy 页里）
  } else if (x >= MON_X && x < MON_X + MON_W) {  // 显示器：显示的是什么，就进什么
    if (monShow == MON_RADAR) {
      setScreen(SCR_RADAR);
      return;
    }
    if (monShow == MON_SPLIT) buddyAgent = monSplit[x >= SC_X + SC_W / 2 ? 1 : 0];  // 分屏：点哪半边进哪个
    else buddyAgent = monShow;
    setScreen(SCR_BUDDY);
  }
}

// ---------- 待机时钟 ----------
// 待机时钟：离开电脑时自动出现；也可以手动开关——首页点日期时钟打开，时钟页点一下关掉。
// 手动关掉的那次「离开」不再自动弹出，等你回来、下次再离开时才会
bool standbyOn(uint32_t now) {
  if (now < dbgUntil && dbgStandby) return true;
  if (clockPinned) return true;
  bool autoOn = cfgClock && userAway() && !(backAt && now - backAt < 12000);
  if (!autoOn) clockDismissed = false;
  return autoOn && !clockDismissed;
}

void clockDismiss() {
  clockPinned = false;
  clockDismissed = true;
  dbgUntil = 0;
}

// ---------- 待机时钟（翻页钟） ----------
SbTime sbTime() {
  SbTime t{false, "", "", "", 0, 0, 0};
  uint32_t e = epochNow();
  if (!e) return t;
  int64_t lt = (int64_t)e + tzOff;
  int32_t days = lt / 86400, sec = lt % 86400;
  int mo, d;
  civilDate(days, mo, d);
  const char *wk[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
  char hh[3], mm[3];
  t.hour = sec / 3600; t.min = sec % 3600 / 60; t.days = days;
  snprintf(hh, 3, "%02d", t.hour);
  snprintf(mm, 3, "%02d", t.min);
  t.ok = true; t.hh = hh; t.mm = mm;
  t.date = String(mo) + " 月 " + d + " 日  " + wk[(days + 4) % 7];
  return t;
}

void sbSleepers(int clawdX, int codexX, int base, int u, uint32_t now) {
  int oU = U, oCX = CX, oBASE = BASE;
  U = u; CX = clawdX; BASE = base;
  Pose p;
  p.eye = 4;
  p.squash = (int)(sinf(now / 1200.0f) + 1);
  p.color = mode == M_OFFLINE ? C_GREY : C_CLAUDE;
  drawClawd(p);
  U = oU; CX = oCX; BASE = oBASE;
  CxPose cp = codexPose(cxMode == M_OFFLINE ? M_OFFLINE : M_SLEEP, now, 1);
  drawCodex(codexX, base, 1, cp);
  drawCodexFace(codexX - 16, base - 39, 1, cp.face, now, cp.grey);
  DsPose dp = dsPose(dsMode == M_OFFLINE ? M_OFFLINE : M_SLEEP, now, 1);
  drawWhale(clawdX + 100, base, 1, dp, now);  // 小鲸鱼趴在同一张卡片右边
  uint32_t z1 = now % 2400, z2 = (now + 1200) % 2400;
  pxText("z", clawdX + 6 * u + z1 / 200, base - 10 * u - z1 / 100, P_DIM, 1);
  pxText("z", codexX + 18 + z2 / 200, base - 30 - z2 / 100, P_DIM, 1);
}

// magpie：一只停着的小喜鹊，每隔 6 秒摆一下尾巴
uint32_t sbFlapAt = 0;
void sbMagpie(int x, int y, uint32_t now) {
  if (now - sbFlapAt > 6000) sbFlapAt = now;
  drawBirdPx(wag28[wagFrame(now, sbFlapAt)], WAG28_SIZE, x, y, magpieOnline ? P_TEXT : P_DIM);
}
// 一行额度：品牌色方块 + 名字 + 方块条 + 剩余；等你确认时换成提醒
void sbQuota(int a, int x, int y, int w) {
  int m = a ? cxMode : mode;
  uint16_t tint = a ? C_CODEX : C_CLAUDE;
  if (m == M_WAITING) {
    gfx->fillRect(x, y - 8, 6, 6, tint);
    pxText(String(a ? "Codex" : "Claude") + " 需要你确认", x + 10, y, a ? C_CODEX_HI : C_CLAUDE, 1);
    return;
  }
  dockQuotaLine(a ? "Codex" : "Claude", tint, a ? cxH5Used : h5Used, x, y, w);
}

// 翻页卡：卡片底色、上半张亮一点、中缝、两侧合页；文字只画在 [clipT, clipB) 这一段里
const int FC_Y = 70, FC_H = 128, FC_W = 190, FC_X[2] = {40, 250};
void flipHalf(int k, const String &s, int clipT, int clipB) {
  int x = FC_X[k], mid = FC_Y + FC_H / 2;
  clipT = max(clipT, FC_Y);
  clipB = min(clipB, FC_Y + FC_H);
  if (clipB <= clipT) return;
  // 卡片底：上半 / 下半各自的颜色，只画在这一段
  int a = clipT, b = min(clipB, mid);
  if (b > a) gfx->fillRect(x + 2, a, FC_W - 4, b - a, 0x2945);
  a = max(clipT, mid); b = clipB;
  if (b > a) gfx->fillRect(x + 2, a, FC_W - 4, b - a, 0x2104);
  txtClipT = clipT;
  txtClipB = clipB;
  pxText(s, x + FC_W / 2 - pxW(s, 10) / 2, mid + 40, P_TEXT, 10);
  txtClipT = 0;
  txtClipB = 272;
}

// 一张翻页卡。换数字时照真的翻页钟翻一下（共 300ms）：
// 前半程，上半张（旧数字）像帘子一样往中缝收起，露出后面新数字的上半；后半程，新数字的下半从中缝往下盖住旧的下半
void drawFlipCard(int k, const String &cur, const String &old, uint32_t since) {
  int x = FC_X[k], mid = FC_Y + FC_H / 2, half = FC_H / 2;
  pxPanel(x, FC_Y, FC_W, FC_H, 0x2104, 0x2104, true);
  const uint32_t T = 300;
  if (!old.length() || old == cur || since >= T) {
    flipHalf(k, cur, FC_Y, FC_Y + FC_H);
  } else if (since < T / 2) {
    int h = half - half * since / (T / 2);  // 旧的上半还剩多高
    flipHalf(k, cur, FC_Y, mid - h);
    flipHalf(k, old, mid - h, mid);
    flipHalf(k, old, mid, FC_Y + FC_H);
    gfx->fillRect(x + 2, mid - h, FC_W - 4, 1, 0x4208);  // 翻动的边
  } else {
    int h = half * (since - T / 2) / (T / 2);  // 新的下半已经盖下来多高
    flipHalf(k, cur, FC_Y, mid);
    flipHalf(k, cur, mid, mid + h);
    flipHalf(k, old, mid + h, FC_Y + FC_H);
    gfx->fillRect(x + 2, mid + h, FC_W - 4, 1, 0x4208);
  }
  gfx->fillRect(x, mid - 1, FC_W, 3, P_BG);                  // 中缝
  gfx->fillRect(x - 3, mid - 6, 5, 12, P_EDGE);              // 合页
  gfx->fillRect(x + FC_W - 2, mid - 6, 5, 12, P_EDGE);
}

// 待机时钟：翻页钟。上面是日期；两张卡片分别是时和分，卡片之间两个点每秒闪一下当冒号；
// Clawd 和 DeepSeek 的小鲸鱼趴在左边卡片上，Codex 和 magpie 的小喜鹊停在右边卡片上，都在睡；
// 下面两行：Claude / Codex 的 5 小时额度，今天完成了几个任务；最下面一行是 DeepSeek 和 magpie
void drawStandbyClock(uint32_t now) {
  static String lastHH, lastMM, prevHH, prevMM;
  static uint32_t flipH = 0, flipM = 0;
  gfx->fillScreen(P_BG);
  SbTime t = sbTime();
  if (!t.ok) {
    pxText("等待对时…", 240 - pxW("等待对时…", 2) / 2, 140, P_DIM, 2);
    return;
  }
  if (t.hh != lastHH) { prevHH = lastHH; lastHH = t.hh; flipH = now; }
  if (t.mm != lastMM) { prevMM = lastMM; lastMM = t.mm; flipM = now; }
  pxText(t.date, 240 - pxW(t.date, 2) / 2, 44, P_DIM, 2);
  drawFlipCard(0, t.hh, prevHH, now - flipH);
  drawFlipCard(1, t.mm, prevMM, now - flipM);
  uint16_t dot = (now / 1000) % 2 ? P_EDGE : P_BG;  // 冒号
  gfx->fillRect(237, FC_Y + FC_H / 2 - 22, 6, 6, dot);
  gfx->fillRect(237, FC_Y + FC_H / 2 + 16, 6, 6, dot);
  sbSleepers(90, 420, FC_Y, 4, now);
  sbMagpie(362, FC_Y - 26, now);
  // 每张卡片下面：这一方的额度 + 今天完成了几个任务；最下面一行居中是 magpie
  for (int a = 0; a < 2; a++) {
    sbQuota(a, FC_X[a], 218, FC_W);
    int n = doneDay == t.days ? doneToday[a] : 0;
    String d = n ? "今天完成 " + String(n) + " 个任务" : String("今天还没有完成任务");
    pxText(d, FC_X[a] + 10, 235, n ? P_TEXT : P_DIM, 1);
  }
  // 最下面一行：DeepSeek（连着才显示）+ magpie，各带一个颜色方块，整行居中
  const MuPeriod &T = muPeriods[0];
  String mg = !magpieOnline ? String("magpie 未连接") : (T.has ? "magpie " + T.gcalls + " 次请求 · " + T.gcost : String("magpie 读取中…"));
  String ds;
  uint16_t dc = P_DIM;
  if (dsMode == M_WAITING) { ds = "DeepSeek 需要你确认"; dc = C_DS_HI; }
  else if (dsMode != M_OFFLINE || doneTodayOf(2)) ds = "DeepSeek 今天完成 " + String(doneTodayOf(2)) + " 个";
  int dw = ds.length() ? pxW(ds, 1) + 10 + 16 : 0;
  int mw = pxW(mg, 1) + 10, mx = 240 - (dw + mw) / 2;
  if (ds.length()) {
    gfx->fillRect(mx, 252 - 8, 6, 6, C_DS);
    pxText(ds, mx + 10, 252, dc, 1);
    mx += dw;
  }
  gfx->fillRect(mx, 252 - 8, 6, 6, P_ACCENT);
  pxText(mg, mx + 10, 252, P_DIM, 1);
}
