// 雷达页：LD2450 的扇形视野（±60°），最多 3 个人的位置点和移动轨迹；右边列出每个人的距离、方向、速度，
// 以及「在不在座位」「有人靠近」的判断。量程跟着最远的人自动缩放（1.5～6 米）。

const uint16_t RD_COL[3] = {P_GREEN, C_YELLOW, 0xFC1F};
float rdRange = 2000;  // 当前量程（毫米），平滑变化
struct RdTrail { int16_t x[16], y[16]; uint8_t n = 0, head = 0; };
RdTrail rdTrail[3];
uint32_t rdTrailAt = 0;


// ---------- 像素图标 ----------
// 雷达 16×16：两道同心圆，右上开一个缺口，扫描线从圆心穿出缺口（不会看成钟表或禁止符号），左下一个光点
// R 圆环 / S 扫描线 / B 光点
const char *const RADAR16[16] = {
  "......RRRR......",
  "....RRRRRR......",
  "..RRR........S..",
  "..RR.......SSS..",
  ".RR..RRRR.SS....",
  ".R..RR...SS.....",
  "RR..R...SS....RR",
  "RR..R..SS..R..RR",
  "RR..R..SS..R..RR",
  "RR..R......R..RR",
  ".R.BBR....RR..R.",
  ".RRBBRRRRRR..RR.",
  "..RR........RR..",
  "..RRR......RRR..",
  "....RRRRRRRR....",
  "......RRRR......",
};
// 有人靠近 16×16：左下是你（实心），右上一个人从你背后探出来（淡色），中间留一道缝
const char *const IC_BEHIND16[16] = {
  "................",
  ".........222....",
  "........22222...",
  "........22222...",
  "...111..22222...",
  "..11111..222....",
  "..11111.........",
  "..11111.2222222.",
  "...111.222222222",
  "........22222222",
  ".1111111.2222222",
  "111111111.222222",
  "111111111.222222",
  "111111111.222222",
  "111111111.222222",
  "111111111.222222",
};
// 小人、椅子，都是 8×8
const char *const IC_PERSON[8] = {"...11...", "..1111..", "..1111..", "...11...", ".111111.", "11111111", "11111111", "11111111"};
const char *const IC_SEAT[8] = {".111111.", ".1....1.", ".1....1.", "11111111", "1.1111.1", "11111111", ".1....1.", ".1....1."};  // 正面的扶手椅

void drawIcon8(const char *const *g, int x, int y, int sc, uint16_t c) {
  for (int r = 0; r < 8; r++)
    for (int k = 0; k < 8; k++)
      if (g[r][k] == '1') gfx->fillRect(x + k * sc, y + r * sc, sc, sc, c);
}

// sc=1 画 16px；bg 是底色，内圈按它混成暗色
void drawRadarIcon(int x, int y, int sc, uint16_t ring, uint16_t sweep, uint16_t blip, uint16_t bg) {
  uint16_t inner = mix565(bg, ring, 110);
  for (int r = 0; r < 16; r++)
    for (int c = 0; c < 16; c++) {
      char ch = RADAR16[r][c];
      if (ch == '.') continue;
      uint16_t col = ch == 'R' ? ring : ch == 'r' ? inner : ch == 'S' ? sweep : blip;
      gfx->fillRect(x + c * sc, y + r * sc, sc, sc, col);
    }
}
// 有人靠近：front 是你，back 是靠近的那个人
void drawBehindIcon(int x, int y, int sc, uint16_t front, uint16_t back) {
  for (int r = 0; r < 16; r++)
    for (int c = 0; c < 16; c++) {
      char ch = IC_BEHIND16[r][c];
      if (ch != '.') gfx->fillRect(x + c * sc, y + r * sc, sc, sc, ch == '1' ? front : back);
    }
}
// 首页状态栏用：没数据灰色；有数据时光点闪烁，你在座位时绿色、有人靠近时黄色
void drawRadarBadge(int x, int y) {
  if (!radarFresh()) { drawRadarIcon(x, y, 1, P_DIM, P_DIM, P_DIM, P_BG); return; }
  uint16_t c = privacyOn ? C_YELLOW : (radarSeatPresent() ? P_GREEN : P_TEXT);
  drawRadarIcon(x, y, 1, P_TEXT, P_TEXT, (millis() / 500) % 2 ? c : mix565(P_BG, c, 90), P_BG);
}

String rdDist(int mm) { return String(mm / 1000.0f, mm < 1000 ? 2 : 1) + " 米"; }
String rdAngle(const RadarTarget &t) {
  int a = (int)(atan2f(t.x, max(1, (int)t.y)) * 180 / PI);
  if (abs(a) < 5) return "正前方";
  return String(a > 0 ? "偏右 " : "偏左 ") + abs(a) + "°";
}

// ---------- 整页 ----------
// 左：雷达屏（CRT 磷光绿），4px 方格拼出扇形视野、距离环、方向线，扫描线后面拖一道余辉；
// 右：三张卡片——几个人、你在哪、有没有人靠近，每张都配像素图标。
#define RS_BG    0x00A1  // 雷达屏底 #06140E
#define RS_FAN   0x0943  // 雷达屏 #0B2A1A
#define RS_GLOW1 0x1BC9  // 余辉（近）#1E7A4A
#define RS_GLOW2 0x1266  // 余辉（中）
#define RS_GLOW3 0x09C4  // 余辉（远）
#define RS_LINE  0x12E7  // 距离环、十字线 #165C3A
#define RS_SHADE 0x0061  // 盲区斜纹（比雷达屏底还暗）
// 120° 扇形雷达屏（LD2450 只看得到前方左右各 60°）：雷达在屏底中间，正前方朝上。
// 扫描线在扇形里来回摆（正弦：到两边慢下来再折返），每个方格按「扫描线离开它多久了」来定余辉亮度，
// 所以折返时两边的余辉也是连续的，不会突然跳到另一边。
const int RS_OX = 153, RS_OY = 258, RS_R = 166;  // 雷达位置（屏底中间）、视野半径
const uint32_t SWEEP_MS = 4000;                  // 一个来回 4 秒
ScopeTab scopeBig, scopeMini;

// 方格查表：开机后第一次用时算一次（位置、角度、距离、扫描线经过它的两个相位），之后每帧只做整数比较
void scopeBuild(ScopeTab &T, int ox, int oy, int r, int cell) {
  if (T.c && T.ox == ox && T.oy == oy && T.r == r && T.cell == cell) return;
  int cap = (2 * r / cell + 2) * (r / cell + 2);
  if (T.c) free(T.c);
  if (T.base) free(T.base);
  T.c = (ScopeCell *)ps_malloc(cap * sizeof(ScopeCell));
  T.base = (uint16_t *)ps_malloc(cap * sizeof(uint16_t));
  T.n = 0; T.ox = ox; T.oy = oy; T.r = r; T.cell = cell; T.baseRange = -1;
  if (!T.c || !T.base) return;
  for (int cy = oy - r; cy < oy; cy += cell)
    for (int cx = ox - r; cx < ox + r; cx += cell) {
      float dx = cx + cell / 2.0f - ox, dy = oy - (cy + cell / 2.0f);
      float d = sqrtf(dx * dx + dy * dy);
      if (d > r || dy <= 0 || cx < 0 || cy < 0 || cx + cell > 480 || cy + cell > 272) continue;
      float a = atan2f(dx, dy) * 180 / PI;
      if (fabsf(a) > 60) continue;
      // 扫描线角度 θ = 60·sin(φ)；经过 a 的相位：φ1 = asin(a/60)（往右扫时），φ2 = π − φ1（往左扫时）
      float f1 = asinf(constrain(a / 60.0f, -1.0f, 1.0f));
      float f2 = PI - f1;
      if (f1 < 0) f1 += 2 * PI;
      T.c[T.n++] = {(int16_t)cx, (int16_t)cy, (int16_t)a, (uint8_t)min(255, (int)d),
                    (uint16_t)(f1 / (2 * PI) * 65536), (uint16_t)(f2 / (2 * PI) * 65536)};
    }
}

// 底色：距离环（按量程每 0.5 / 1 米一圈）、外弧、视野两条边、0° 和 ±30° 方向线、座位弧（1.5 米、±28°），
// 以及你身后的盲区（被你身体挡住的阴影锥，见 inShadow()）：隔一格涂暗，像斜纹
void scopeBase(ScopeTab &T, float rangeMm, bool seatArc, bool shadow) {
  int key = (int)(rangeMm / 25);
  uint16_t seatC = seatArc ? (radarSeatPresent() ? P_GREEN : RS_GLOW1) : 0;
  int refAng = (int)lroundf(atan2f(presRefX, max(1, presRefY)) * 180 / PI);
  int d0 = (int)sqrtf((float)presRefX * presRefX + (float)presRefY * presRefY);
  int shKey = shadow ? (refAng + 100) * 1000 + d0 / 50 + 1 : 0;
  if (key == T.baseRange && seatC == T.baseSeat && shKey == T.baseShadow) return;
  T.baseRange = key;
  T.baseSeat = seatC;
  T.baseShadow = shKey;
  int step = rangeMm <= 2500 ? 500 : 1000;
  float ringPx = step * T.r / rangeMm, seatPx = SEAT_Y * T.r / rangeMm, tol = T.cell * 0.55f;
  float shPx = (d0 + 150) * T.r / rangeMm, shHalf = shadowHalf(presRefX, presRefY) * 180 / PI;
  for (int i = 0; i < T.n; i++) {
    const ScopeCell &c = T.c[i];
    uint16_t col = RS_FAN;
    if (shadow && c.dist > shPx && fabsf(c.ang - refAng) < shHalf && ((c.x + c.y) / T.cell) % 2) col = RS_SHADE;
    float m = fmodf(c.dist, ringPx);
    if ((m < tol || ringPx - m < tol) && c.dist > ringPx / 2) col = RS_LINE;
    if (c.dist >= T.r - T.cell) col = RS_LINE;
    int a = abs(c.ang);
    if (a >= 58 || (a % 30 < 1 || a % 30 > 29)) col = RS_LINE;
    if (seatArc && fabsf(c.dist - seatPx) < tol && a < 28) col = seatC;
    T.base[i] = col;
  }
}

// 当前相位（0~65535）与扫描线角度
uint16_t sweepPhase(uint32_t now) { return (uint16_t)((now % SWEEP_MS) * 65536 / SWEEP_MS); }

// 扫描线离开某个方格多久了（相位差，0~65535）：两个经过时刻里取最近的一次
inline uint16_t sinceBeam(uint16_t ph, uint16_t p1, uint16_t p2) {
  uint16_t e1 = ph - p1, e2 = ph - p2;
  return e1 < e2 ? e1 : e2;
}

// 每帧：底色 + 余辉，直接写进画布内存
void scopeDraw(ScopeTab &T, float rangeMm, uint32_t now, bool seatArc, bool shadow) {
  if (!T.c || !T.base) return;
  scopeBase(T, rangeMm, seatArc, shadow);
  uint16_t ph = sweepPhase(now);
  int cs = T.cell - 1;  // 留 1px 缝：方格感
  uint16_t *fb = gfx->getFramebuffer();
  for (int i = 0; i < T.n; i++) {
    const ScopeCell &c = T.c[i];
    uint16_t col = T.base[i];
    uint16_t e = sinceBeam(ph, c.p1, c.p2);  // 65536 = 4 秒
    if (col == RS_SHADE) { if (e < 3200) col = RS_GLOW3; }       // 盲区：扫描线扫过也只微微亮一下
    else if (e < 700) col = P_GREEN;                                   // 扫描线本身（约 0.04 秒）
    else if (e < 3200) col = col == RS_FAN ? RS_GLOW1 : P_GREEN;  // 0.2 秒内
    else if (e < 7000) { if (col == RS_FAN) col = RS_GLOW2; }     // 0.43 秒内
    else if (e < 12000 && col == RS_FAN) col = RS_GLOW3;          // 0.73 秒内
    uint16_t *row = fb + c.y * 480 + c.x;
    for (int yy = 0; yy < cs; yy++, row += 480)
      for (int xx = 0; xx < cs; xx++) row[xx] = col;
  }
}

void rsToScreen(int xmm, int ymm, int &sx, int &sy) {
  float k = RS_R / rdRange;
  sx = RS_OX + (int)(xmm * k);
  sy = RS_OY - (int)(ymm * k);
}

// 光点亮度：扫描线刚扫过时最亮，之后慢慢变暗（但不会完全消失）
int blipLevel(int xmm, int ymm, uint32_t now) {
  float a = constrain(atan2f(xmm, max(1, ymm)) * 180 / PI, -60.0f, 60.0f);
  float f1 = asinf(a / 60.0f), f2 = PI - f1;
  if (f1 < 0) f1 += 2 * PI;
  uint16_t e = sinceBeam(sweepPhase(now), (uint16_t)(f1 / (2 * PI) * 65536), (uint16_t)(f2 / (2 * PI) * 65536));
  return e < 6000 ? 3 : (e < 20000 ? 2 : 1);
}

// 首页显示器上的迷你雷达（同样的 120° 扇形），Claude 和 Codex 都待命时显示
void drawMiniScope(int x, int y, int w, int h, uint32_t now) {
  gfx->fillRect(x, y, w, h, RS_BG);
  int r = min(h - 6, (int)(w / 2 / 0.87f) - 2), ox = x + w / 2, oy = y + h - 3;
  scopeBuild(scopeMini, ox, oy, r, 3);
  scopeDraw(scopeMini, rdRange, now, false, false);
  gfx->fillRect(ox - 4, oy - 2, 8, 3, C_BLUE);
  int me = radarNearest();
  float k = r / rdRange;
  for (int i = 0; i < 3; i++) {
    if (!radarT[i].on) continue;
    int bx = ox + (int)(radarT[i].x * k), by = oy - (int)(radarT[i].y * k);
    by = min(by, oy - 4);
    uint16_t c = (i == me && inSeat(radarT[i])) ? P_GREEN : C_YELLOW;
    int lv = blipLevel(radarT[i].x, radarT[i].y, now);
    if (lv == 3) gfx->fillRect(bx - 3, by - 3, 6, 6, RS_GLOW1);
    gfx->fillRect(bx - 2, by - 2, 4, 4, lv >= 2 ? c : RS_GLOW1);
  }
}

void drawScope(uint32_t now) {
  scopeBuild(scopeBig, RS_OX, RS_OY, RS_R, 4);
  bool shadow = radarFresh() && youPresent();
  scopeDraw(scopeBig, rdRange, now, !seatCal, shadow);
  int step = rdRange <= 2500 ? 500 : 1000;
  float k = RS_R / rdRange;
  for (int dmm = step; dmm < (int)rdRange; dmm += step) {  // 距离刻度：写在右边视野边线外侧
    int r = (int)(dmm * k);
    String t = ((dmm % 1000) ? String(dmm / 1000.0f, 1) : String(dmm / 1000)) + "m";
    int lx = RS_OX + (int)(r * 0.866f) + 6, ly = RS_OY - r / 2 + 4;
    if (lx < 296 - pxW(t, 1)) pxText(t, lx, ly, RS_GLOW1, 1);
  }
  gfx->fillRect(RS_OX - 12, RS_OY - 2, 24, 6, C_BLUE);  // 雷达
  gfx->fillRect(RS_OX - 5, RS_OY - 6, 10, 4, C_BLUE);
  // 扇形外的四个角。左上是这张图现在画到多远（跟着最远的人自动缩放），右上是雷达本身能看多远（LD2450 手册：6 米、左右各 60°），
  // 两个别混在一起；右下说明盲区斜纹
  pxText("画面", 10, 62, RS_GLOW1, 1);
  pxText(String(rdRange / 1000.0f, 1) + " 米", 10, 78, P_GREEN, 1);
  pxText("雷达能看", 296 - pxW("雷达能看", 1), 62, RS_GLOW1, 1);
  pxText("6 米 ±60°", 296 - pxW("6 米 ±60°", 1), 78, P_GREEN, 1);
  pxText(seatCal ? String("座位已标定") : String("座位 1.5 米"), 10, 262, radarSeatPresent() ? P_GREEN : RS_GLOW1, 1);
  if (shadow) pxText("斜纹: 被你挡住", 296 - pxW("斜纹: 被你挡住", 1), 262, RS_GLOW1, 1);
}

void drawBlips(uint32_t now) {
  int me = radarNearest();
  for (int i = 0; i < 3; i++) {
    if (!radarT[i].on) continue;
    bool you = i == me && inSeat(radarT[i]);
    uint16_t col = you ? P_GREEN : (i == 2 ? 0xFC1F : C_YELLOW);
    RdTrail &T = rdTrail[i];
    for (int k = 1; k < T.n; k += 2) {  // 轨迹：越久越暗的小方格
      int idx = (T.head - 1 - k + 32) % 16, x, y;
      rsToScreen(T.x[idx], T.y[idx], x, y);
      gfx->fillRect(x - 2, y - 2, 4, 4, k < 6 ? RS_GLOW1 : RS_GLOW3);
    }
    int x, y;
    rsToScreen(radarT[i].x, radarT[i].y, x, y);
    x = constrain(x, 10, 296);
    y = constrain(y, 50, RS_OY - 8);
    int lv = blipLevel(radarT[i].x, radarT[i].y, now);  // 扫描线扫过时最亮
    if (lv == 3) gfx->fillRect(x - 9, y - 9, 18, 18, RS_GLOW1);
    if (lv >= 2) gfx->fillRect(x - 7, y - 7, 14, 14, RS_GLOW2);
    gfx->fillRect(x - 5, y - 5, 10, 10, lv >= 2 ? col : RS_GLOW1);
    if (lv == 3) gfx->fillRect(x - 3, y - 3, 6, 6, P_TEXT);
    String lab = you ? String("你") : "#" + String(i + 1);
    lab += " " + String(radarT[i].y / 1000.0f, radarT[i].y < 1000 ? 2 : 1) + "m";
    int lw = pxW(lab, 1) + 10, lx = constrain(x + 10, 4, 300 - lw), ly = max(y - 22, 46);
    pxPanel(lx, ly, lw, 16, RS_BG, col, false);
    pxText(lab, lx + 5, ly + 12, col, 1);
  }
  if (ghostOn && radarFresh()) {  // 盲区里记着的人：闪烁的空心框 + 问号
    int x, y;
    rsToScreen(ghostX, ghostY, x, y);
    x = constrain(x, 10, 296);
    y = constrain(y, 50, RS_OY - 8);
    uint16_t c = (now / 400) % 2 ? C_YELLOW : RS_GLOW1;
    gfx->fillRect(x - 7, y - 7, 14, 2, c);
    gfx->fillRect(x - 7, y + 5, 14, 2, c);
    gfx->fillRect(x - 7, y - 7, 2, 14, c);
    gfx->fillRect(x + 5, y - 7, 2, 14, c);
    String lab = "? 被挡住";
    int lw = pxW(lab, 1) + 10, lx = constrain(x + 10, 4, 300 - lw), ly = max(y - 22, 46);
    pxPanel(lx, ly, lw, 16, RS_BG, C_YELLOW, false);
    pxText(lab, lx + 5, ly + 12, C_YELLOW, 1);
  }
  if (!radarFresh()) {
    String t = "没有雷达数据 · 检查 P1 接线";
    pxPanel(RS_OX - pxW(t, 1) / 2 - 12, RS_OY - 13, pxW(t, 1) + 24, 26, RS_BG, P_DIM, false);
    pxText(t, RS_OX - pxW(t, 1) / 2, RS_OY + 5, P_DIM, 1);
  }
}

// 右边的卡片：图标 + 大字 + 小字
void rdCard(int y, int h, const char *const *icon, uint16_t ic, const String &big, uint16_t bc, const String &small, bool hi) {
  const int x = 306, w = 168;
  pxPanel(x, y, w, h, hi ? 0x3A40 : P_PANEL, hi ? C_YELLOW : P_EDGE, true);
  pxPanel(x + 8, y + (h - 32) / 2, 32, 32, P_BG, P_BG, false);
  if (icon) drawIcon8(icon, x + 12, y + (h - 32) / 2 + 4, 3, ic);
  else drawBehindIcon(x + 8, y + (h - 32) / 2, 2, ic, mix565(P_BG, ic, hi ? 110 : 40));  // 没人靠近时只剩一个淡淡的影子
  int sc = pxW(big, 2) <= w - 56 ? 2 : 1;
  int by = small.length() ? y + h / 2 + (sc == 2 ? 2 : -2) : y + h / 2 + (sc == 2 ? 11 : 5);  // 没有小字时大字居中
  pxText(fitU(big, w - 56), x + 48, by, bc, sc);
  if (small.length()) pxText(fitU(small, w - 56), x + 48, y + h / 2 + 20, P_DIM, 1);
}

// 靠近的那个人在哪：「离你 1.2 米」；被你挡住、只是记着的：「在你身后」
String behindWhere() {
  if (behindIdx >= 0 && radarT[behindIdx].on)
    return "离你 " + String(sqrtf(dist2(radarT[behindIdx].x, radarT[behindIdx].y, presRefX, presRefY)) / 1000.0f, 1) + " 米";
  return ghostOn ? String("在你身后") : String("");
}

// 右边三张卡片：内容变了才重画到缓存图上，平时每帧只拷贝这一块（像素字放大画很慢）
Arduino_Canvas *cardCv = nullptr;
String cardSig;

void drawRadarCards() {
  int n = radarFresh() ? radarCount() : 0;
  rdCard(46, 68, IC_PERSON, n ? P_GREEN : P_DIM, radarFresh() ? String(n) + " 人" : String("未连接"), P_TEXT,
         radarFresh() ? String("在视野内") : String("检查 P1 接线"), false);
  int me = radarNearest();
  bool seat = radarSeatPresent();
  String big = seat ? String("你在座位") : String("座位空着");
  String small = seat && me >= 0 ? rdAngle(radarT[me]) + " " + String(radarT[me].y / 1000.0f, 1) + " 米" : String("");
  rdCard(122, 68, IC_SEAT, seat ? P_GREEN : P_DIM, big, seat ? P_GREEN : P_DIM, small, false);
  if (privacyOn) rdCard(198, 68, nullptr, C_YELLOW, "有人靠近", C_YELLOW, behindWhere(), true);
  else rdCard(198, 68, nullptr, cfgPrivacy ? P_GREEN : P_DIM, cfgPrivacy ? "没人靠近" : "提醒已关", cfgPrivacy ? P_TEXT : P_DIM, "", false);
}

String radarCardSig() {
  int me = radarNearest();
  bool seat = radarSeatPresent();
  String sig = String(radarFresh()) + radarCount() + seat + privacyOn + cfgPrivacy + (privacyOn ? behindWhere() : String(""));
  if (seat && me >= 0) sig += String(radarT[me].y / 10) + rdAngle(radarT[me]) + (abs(radarT[me].v) < 10);
  return sig;
}

void radarCardsBlit() {
  if (!cardCv) {
    cardCv = new FastCanvas(480, 272, nullptr);
    if (!cardCv->begin(GFX_SKIP_OUTPUT_BEGIN)) { delete cardCv; cardCv = nullptr; }
    else cardCv->setUTF8Print(true);
  }
  if (!cardCv) { drawRadarCards(); return; }  // 内存不够：直接画
  String sig = radarCardSig();
  if (sig != cardSig) {
    cardSig = sig;
    Arduino_Canvas *screenCv = gfx;
    gfx = cardCv;  // 借用全部绘图函数，画到缓存图上
    gfx->fillRect(300, 42, 180, 230, P_BG);
    drawRadarCards();
    gfx = screenCv;
  }
  uint16_t *dst = gfx->getFramebuffer(), *src = cardCv->getFramebuffer();
  for (int y = 42; y < 272; y++) memcpy(dst + y * 480 + 300, src + y * 480 + 300, 180 * 2);
}

void drawRadarPx(uint32_t now) {
  // 只清会变的地方：标题栏 + 左边雷达区（右边卡片整块拷贝覆盖）
  gfx->fillRect(0, 0, 480, 42, P_BG);
  gfx->fillRect(0, 42, 300, 230, P_BG);
  int far = 0;
  for (auto &t : radarT)
    if (t.on) far = max(far, (int)t.y);
  rdRange += (constrain(far * 1.5f, 1500.0f, 6000.0f) - rdRange) * 0.08f;
  if (now - rdTrailAt > 100) {  // 轨迹采样
    rdTrailAt = now;
    for (int i = 0; i < 3; i++) {
      RdTrail &T = rdTrail[i];
      if (!radarT[i].on) { T.n = 0; continue; }
      T.x[T.head] = radarT[i].x;
      T.y[T.head] = radarT[i].y;
      T.head = (T.head + 1) % 16;
      if (T.n < 16) T.n++;
    }
  }
  uint32_t t0 = micros();
  drawScope(now);
  uint32_t t1 = micros();
  drawBlips(now);
  if (perfOn) { perfAcc[1] += t1 - t0; perfAcc[2] += micros() - t1; }  // 性能计时：雷达屏 / 光点
  radarCardsBlit();

  // 标题栏
  pxHomeBtn(6, 6);
  drawRadarIcon(46, 12, 1, P_TEXT, P_TEXT, (now / 500) % 2 ? P_GREEN : mix565(P_BG, P_GREEN, 90), P_BG);
  pxU("雷达", 68, 27, P_TEXT);
  if (!radarFresh()) pxText("未连接", 472 - pxW("未连接", 1), 26, P_DIM, 1);  // 型号、帧率这些放在设置 → 雷达里
  for (int x = 0; x < 480; x += 4) gfx->fillRect(x, 40, 2, 2, P_GRID);
}

void radarTap(int x, int y) {
  if (x < 50 && y < 46) setScreen(SCR_HOME);
}

