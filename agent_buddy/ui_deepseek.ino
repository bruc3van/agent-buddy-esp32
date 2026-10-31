// DeepSeek 小鲸鱼：绘制、各状态的姿势与表情、Buddy 页的 DeepSeek 视图；以及三个 Agent 共用的取值小工具
// 贴图见 deepseek_pet.h；眼睛、胸鳍、尾鳍、喷水由程序画

// ---------- 三个 Agent 共用：a = 0 Claude / 1 Codex / 2 DeepSeek ----------
int agMode(int a) { return a == 2 ? dsMode : (a ? cxMode : (int)mode); }
String agLabel(int a) { return a == 2 ? (dsMode == M_THINKING ? String(DS_THINKING) : dsLabel) : (a ? cxLabel : label); }
String agDetail(int a) { return a == 2 ? dsDetail : (a ? cxDetail : statusDetail); }
int agSessions(int a) { return a == 2 ? dsSessions : (a ? cxSessions : sessions); }
uint32_t agTurnStart(int a) { return a == 2 ? dsTurnStart : (a ? cxTurnStart : turnStart); }
uint32_t agPokeUntil(int a) { return a == 2 ? dsPokeUntil : (a ? cxPokeUntil : pokeUntil); }
uint16_t agColor(int a) { return a == 2 ? C_DS : (a ? C_CODEX : C_CLAUDE); }
uint16_t agHi(int a) { return a == 2 ? C_DS_HI : (a ? C_CODEX_HI : C_CLAUDE); }  // 等你确认时闪的颜色
const char *agName(int a) { return a == 2 ? "DeepSeek" : (a ? "Codex" : "Claude"); }
const char *agTitle(int a) { return a == 2 ? "DeepSeek" : (a ? "Codex" : "Claude Code"); }
Icon8 agIcon8(int a) { return a == 2 ? IC_DS8 : (a ? IC_CODEX8 : IC_CLAWD8); }

// 今天完成了几个任务（跨天了还没收到新的完成就是 0）
int doneTodayOf(int a) {
  uint32_t e = epochNow();
  int day = e ? (int)(((int64_t)e + tzOff) / 86400) : -1;
  return day == doneDay ? doneToday[a] : 0;
}

// 时长，同 dsh 桌面版的紧凑写法：4秒 / 1分20秒 / 1小时5分
String dsDuration(uint32_t sec) {
  if (sec < 60) return String(sec) + "秒";
  if (sec < 3600) return String(sec / 60) + "分" + String(sec % 60) + "秒";
  return String(sec / 3600) + "小时" + String(sec % 3600 / 60) + "分";
}

// 甩尾巴的当前帧（IC_DSTAIL8）：正 → 左 → 正 → 右，每帧 160ms
int dsTailFrame(uint32_t now) {
  static const uint8_t SEQ[4] = {0, 1, 0, 2};
  return SEQ[(now / 160) % 4];
}

// ---------- 小鲸鱼 ----------
uint16_t dsColor(char ch, bool grey) {
  switch (ch) {
    case 'K': return grey ? 0x2945 : 0x10EA;  // #141E50
    case 'D': return grey ? 0x6B4D : 0x2A39;  // #2F45C8
    case 'L': return grey ? 0xC618 : 0xDF1F;  // #DCE3FF
    default: return grey ? 0x8410 : C_DS;     // B
  }
}

// 喷水：(x, y) 是头顶的喷水孔，level 1 小水花 / 2 大喷泉。
// 像喷泉一样：喷口一小截水柱，水珠一颗接一颗往上喷，到顶后向两边散开、沿抛物线洒下来
void drawSpout(int x, int y, int s, int level, uint32_t now) {
  const int n = level == 2 ? 16 : 8;                      // 同时在空中的水珠
  const float H = (level == 2 ? 9 : 5) * s;              // 喷多高
  const float R = (level == 2 ? 7 : 4) * s;              // 往两边洒多远
  const uint32_t T = level == 2 ? 1000 : 750;             // 一颗水珠从喷出到落下
  const int d = max(2, s * 2 / 3);
  int stem = (int)(H * 0.35f);
  gfx->fillRect(x - d / 2, y - stem, d, stem, C_DS_WATER);  // 喷口的水柱
  for (int i = 0; i < n; i++) {
    float t = ((now + i * T / n) % T) / (float)T;
    float dir = ((i * 5) % n) / (float)(n - 1) * 2 - 1;  // -1 ~ 1：往左还是往右，每颗不一样
    float px = x + dir * R * t;
    float py = y - H * 4 * t * (1 - t) + t * t * 3 * s;  // 先上后下，落得比喷口低一点
    gfx->fillRect((int)px - d / 2, (int)py - d / 2, d, d, i % 3 == 0 ? 0xFFFF : C_DS_WATER);
  }
}

// cx：中心 x；base：肚皮底 y；s：每个贴图像素画多大。
// 游动：每一列按一道往尾巴传的正弦波上下错开，越靠尾巴错得越多（头不动），像鲸鱼在水里摆尾；flip 时头朝右
void drawWhale(int cx, int base, int s, const DsPose &p, uint32_t now) {
  int x0 = cx - 16 * s + p.dx, y0 = base - 19 * s + p.dy;
  int off[32];
  for (int c = 0; c < 32; c++) {
    float k = c > 10 ? powf((c - 10) / 21.0f, 1.6f) : 0;
    off[c] = (int)lroundf(p.wave * s * k * sinf(p.wph - c * 0.22f) + p.tilt * (c - 16) * s);  // tilt > 0：尾巴往下、头往上
  }
  auto X = [&](int c) { return x0 + (p.flip ? 31 - c : c) * s; };
  auto px = [&](int c, int r, uint16_t col) { gfx->fillRect(X(c), y0 + r * s + off[c], s, s, col); };
  for (int r = 0; r < 19; r++) {
    const char *row = r < 4 ? DS_FLUKE[p.fluke][r] : DS_WHALE[r];
    for (int c = 0; c < 32; c++)
      if (row[c] != '.') px(c, r, dsColor(row[c], p.grey));
  }
  int fr = p.fin ? 11 : 14;  // 胸鳍
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (DS_FIN[p.fin][r][c] != '.') px(11 + c, fr + r, dsColor(DS_FIN[p.fin][r][c], p.grey));
  // 眼睛（第 10~11 行、第 8~9 列）
  uint16_t ink = dsColor('K', p.grey);
  int g = max(1, s / 2);  // 眼睛里的高光：画在朝前的那一角
  auto glint = [&](int c, int r) { gfx->fillRect(X(c) + (p.flip ? s - g : 0), y0 + r * s, g, g, 0xFFFF); };
  switch (p.eye) {
    case DE_BLINK: for (int c = 7; c < 10; c++) px(c, 11, ink); break;
    case DE_SLEEP: px(7, 11, ink); px(8, 12, ink); px(9, 11, ink); break;  // ︶
    case DE_HAPPY: px(7, 11, ink); px(8, 10, ink); px(9, 11, ink); break;  // ^
    case DE_X: px(7, 10, ink); px(9, 10, ink); px(8, 11, ink); px(7, 12, ink); px(9, 12, ink); break;
    case DE_BIG:
      for (int c = 7; c < 10; c++)
        for (int r = 9; r < 12; r++) px(c, r, ink);
      glint(7, 9);
      break;
    default: {  // 睁眼；DE_UP 往上瞟一格
      int r = p.eye == DE_UP ? 9 : 10;
      for (int c = 8; c < 10; c++) { px(c, r, ink); px(c, r + 1, ink); }
      glint(8, r);
    }
  }
  if (p.blush) { px(11, 12, 0xFCD6); px(12, 12, 0xFCD6); }
  if (p.spout) drawSpout(X(12) + s / 2, y0 + 4 * s, s, p.spout, now);
}

// 各状态的姿势（鱼缸、待机时钟和 Buddy 页共用）。wave / wph：游动的摆幅（贴图像素）和相位
DsPose dsPose(int m, uint32_t now, int s) {
  DsPose p;
  bool blink = (now % 3700) < 140;
  int bob = (int)(s * 0.6f * sinf(now / 900.0f));
  switch (m) {
    case M_OFFLINE: p.grey = true; p.eye = DE_BLINK; break;
    case M_SLEEP: p.eye = DE_SLEEP; p.dy = (int)(s * 0.5f * sinf(now / 1400.0f)); p.wave = 0.6f; p.wph = now / 600.0f; break;
    case M_THINKING:  // 同官方：一下下用力甩尾巴，眼睛往上看
      p.eye = DE_UP;
      p.dy = bob;
      p.wave = 3.5f;
      p.wph = now / 110.0f;
      p.fluke = dsTailFrame(now) ? 1 : 0;
      break;
    case M_WORKING:  // 游得飞快，时不时喷一下水
      p.eye = blink ? DE_BLINK : DE_OPEN;
      p.wave = 3.0f;
      p.wph = now / 80.0f;
      p.fluke = (now / 150) % 2;
      if ((now / 400) % 4 == 0) p.spout = 1;
      break;
    case M_WAITING:  // 停下来瞪大眼，举起胸鳍挥，尾巴轻轻摆着等你拿主意
      p.eye = DE_BIG;
      p.fin = (now / 250) % 2;
      p.dy = p.fin ? -s : 0;
      p.wave = 1.2f;
      p.wph = now / 300.0f;
      break;
    case M_DONE: {  // 往上一窜 + 大喷泉
      float k = (now % 900) / 900.0f;
      p.dy = -(int)(s * 8 * 4 * k * (1 - k));
      p.eye = DE_HAPPY;
      p.blush = true;
      p.fin = 1;
      p.spout = 2;
      p.wave = 3.0f;
      p.wph = now / 90.0f;
      break;
    }
    case M_ERROR: p.dx = ((now / 60) % 2) ? -s : s; p.eye = DE_X; break;
    case M_COMPACT: p.eye = DE_BLINK; p.dy = bob; p.wave = 1.5f; p.wph = now / 200.0f; break;
    default:  // 待命：慢悠悠地游，每 9 秒喷一小口水
      p.eye = blink ? DE_BLINK : DE_OPEN;
      p.dy = bob;
      p.wave = 2.2f;
      p.wph = now / 200.0f;
      if (now % 9000 < 500) p.spout = 1;
  }
  return p;
}

// 鱼缸给的动作（dsTank 设置，画完清掉）：dsFx 0 不在鱼缸 / 1 在鱼缸里 / 2 在鱼缸里游（要划水）
int dsFx = 0, dsFxSpout = 0;
float dsFxTilt = 0;

// 带特效的完整绘制：被点、欢迎回来、睡觉的 z。flip：头朝右
void drawDeepSeekAt(int cx, int base, int s, int m, uint32_t now, bool flip) {
  DsPose p = dsPose(m, now, s);
  if (dsPokeUntil > now) {  // 被点：往上一窜喷水
    float k = ((now - (dsPokeUntil - 1800)) % 900) / 900.0f;
    p = DsPose();
    p.dy = -(int)(s * 6 * 4 * k * (1 - k));
    p.eye = DE_HAPPY;
    p.blush = true;
    p.spout = 2;
    p.wave = 3.0f;
    p.wph = now / 90.0f;
  } else if ((m == M_IDLE || m == M_SLEEP) && backAt && now - backAt < 8000) {  // 欢迎回来：挥胸鳍
    p.eye = DE_HAPPY;
    p.fin = (now / 220) % 2;
  } else if (m == M_IDLE && userAway()) {
    p.eye = DE_SLEEP;
    p.spout = 0;
    p.wave = 0.6f;
  }
  if (dsFx) {  // 在鱼缸里：上下位置由鱼缸控制（浮上水面、跳出水面），每划一下水身子一顶一沉、胸鳍跟着划
    p.dy = dsFx == 2 ? (int)(0.8f * s * sinf(p.wph + 1.57f)) : (m == M_WAITING ? p.dy : 0);
    if (dsFx == 2 && p.wave >= 2) p.fin = sinf(p.wph) > 0;
    p.tilt = dsFxTilt;
    p.spout = max(p.spout, dsFxSpout);
  }
  p.flip = flip;
  drawWhale(cx, base, s, p, now);
  int hx = cx + (flip ? 12 : -12) * s + p.dx, y0 = base - 19 * s + p.dy;  // 头顶
  if (m == M_SLEEP || p.eye == DE_SLEEP)
    for (int i = 0; i < 3; i++) {
      uint32_t zt = (now + i * 900) % 2700;
      int zx = hx + (flip ? 1 : -1) * (i * 2 * s + (int)(zt / 150));
      pxText("z", zx, max(56, y0 + 4 * s - (int)(zt / 150) - i * 4), P_DIM, 1 + (i == 2 && s >= 3));
    }
}

// ---------- 鱼缸（首页墙上的、Buddy 页左边的共用）----------
// (x, y, w, h)：缸内；wy：水面；s：鲸鱼放大倍数；base：平时游的深度（鲸鱼肚皮的 y）。
// 小鲸鱼在缸里左右来回游，每划一下水往前一冲；隔一阵子浮上水面喷水，或者冲出水面再扎回去；完成任务时连着跳
int dsTankWhaleX = 0, dsTankWhaleBase = 0;  // 这一帧鲸鱼的位置、朝向（Buddy 页上的特效要跟着它）
bool dsTankFlip = false;

// 水花：鲸鱼进出水面时，(x, wy) 两边溅起一圈水滴，sp 0→1 是溅起到落下
void dsSplash(int x, int wy, int s, float sp) {
  if (sp <= 0 || sp >= 1) return;
  int d = max(2, s);
  for (int i = -4; i <= 4; i++) {
    if (!i) continue;
    float hgt = sinf(sp * PI) * (5 - abs(i)) * s * 1.6f;
    int dx = i * (int)(d * (2 + sp * 3));
    gfx->fillRect(x + dx, wy - (int)hgt - d, d, d, abs(i) % 2 ? C_DS_WATER : 0xFFFF);
  }
}

// ---------- 缸底的小东西：贝壳、螃蟹 ----------
// 字母：s 贝壳 / S 贝壳的纹路 / R 蟹壳 / r 蟹壳暗面 / C 钳子 / L 腿 / e 眼睛；u：每格画多大
const char *const SHELL[5] = {"..sss..", ".sSsSs.", "sSsSsSs", ".sssss.", "..sSs.."};
const char *const CRAB[2][6] = {
  {"e.......e", "C.e...e.C", "CC.RRR.CC", ".RRRRRRR.", ".rRRRRRr.", "L.L...L.L"},   // 钳子张开、腿外撇
  {"e.......e", ".Ce...eC.", ".CCRRRCC.", ".RRRRRRR.", ".rRRRRRr.", ".L.L.L.L."},   // 钳子合上、腿收拢
};

void drawGrid(const char *const *rows, int nr, int x, int y, int u, uint16_t s1, uint16_t s2) {
  for (int r = 0; r < nr; r++)
    for (int c = 0; rows[r][c]; c++) {
      uint16_t col;
      switch (rows[r][c]) {
        case 's': col = s1; break;
        case 'S': col = s2; break;
        case 'R': col = 0xE8E4; break;  // 蟹壳 #E91E24
        case 'r': col = 0xA800; break;
        case 'C': col = 0xF9C7; break;  // 钳子亮一点
        case 'L': col = 0xA800; break;
        case 'e': col = 0x0000; break;
        default: continue;
      }
      gfx->fillRect(x + c * u, y + r * u, u, u, col);
    }
}

// 两个贝壳躺在石子上；一只螃蟹横着来回爬，钳子一张一合
void dsTankFloor(int x, int y, int w, int h, int gs, int s, uint32_t now) {
  int u = max(1, s), floorY = y + h - gs * 2;  // 和鲸鱼同样的格子大小；石子顶面
  drawGrid(SHELL, 5, x + w * 36 / 100, floorY - 5 * u + u, u, 0xFDB8, 0xE30F);  // 粉色扇贝
  drawGrid(SHELL, 5, x + w * 70 / 100, floorY - 5 * u + u, u, 0xEF5D, 0xB5B6);  // 白色扇贝
  uint32_t T = 18000, t = now % T;
  float k = t < T / 2 ? (float)t / (T / 2) : 2.0f - (float)t / (T / 2);  // 0 → 1 → 0，横着走
  int cx = x + w * 18 / 100 + (int)(k * w * 50 / 100);
  int f = (now / 260) % 2;
  drawGrid(CRAB[f], 6, cx, floorY - 6 * u + u - (f ? 0 : u / 2), u, 0, 0);
}

// 鱼缸的玻璃：左右两面缸壁、缸底，上面敞口；两侧缸壁顶上往外翻一点当缸沿，右下实色阴影（同 pxPanel）
void dsTankFrame(int x, int y, int w, int h) {
  const int ox = x - 4, ow = w + 8, top = y - 2, bot = y + h + 4;
  gfx->fillRect(ox + ow, top + 3, 3, bot - top - 3, P_SHADOW);  // 阴影：右边、下边
  gfx->fillRect(ox + 3, bot, ow - 3, 3, P_SHADOW);
  gfx->fillRect(ox, top, 4, bot - top - 2, P_EDGE);              // 左右缸壁
  gfx->fillRect(x + w, top, 4, bot - top - 2, P_EDGE);
  gfx->fillRect(ox + 2, y + h, ow - 4, 4, P_EDGE);               // 缸底，两角切掉
  gfx->fillRect(ox - 2, top, 6, 2, P_EDGE);                      // 缸沿
  gfx->fillRect(x + w, top, 6, 2, P_EDGE);
}

// 浮在水面的大气泡（代替头顶的对话气泡，鲸鱼在水里不会说话）。
// 立体感：几层往左上偏的同心椭圆，外圈深、越往里越亮；水面以下换一套更蓝更暗的颜色，像泡在水里。
// 再加左上一块高光、右下一道反光。rx / ry 不同时是被压扁的样子（落回水面那一下）
void dsAirBubble(int cx, int cy, int rx, int ry, int wy) {
  static const uint16_t AIR[4] = {0x5D5C, 0xAEDE, 0xDF7F, 0xF7DF};  // #5FA8E0 → #F2FAFF
  static const uint16_t SUB[4] = {0x3C9A, 0x75DD, 0x9E7E, 0xBEFE};  // 水下：#3A92D2 → #B8DCF6
  static const float SC[4] = {1.0f, 0.9f, 0.74f, 0.52f}, OFF[4] = {0, 0.06f, 0.14f, 0.22f};
  for (int yy = cy - ry; yy <= cy + ry; yy++) {
    const uint16_t *pal = yy > wy ? SUB : AIR;
    for (int i = 0; i < 4; i++) {
      float lrx = rx * SC[i], lry = ry * SC[i], lcx = cx - rx * OFF[i], lcy = cy - ry * OFF[i];
      float d = (yy - lcy) / lry;
      if (d < -1 || d > 1) continue;
      int half = (int)(lrx * sqrtf(1 - d * d));
      gfx->drawFastHLine((int)lcx - half, yy, half * 2 + 1, pal[i]);
    }
  }
  int hx = cx - rx * 11 / 20, hy = cy - ry * 11 / 20;  // 高光：台阶状一块
  gfx->fillRect(hx + 2, hy - 3, 9, 3, 0xFFFF);
  gfx->fillRect(hx - 2, hy, 7, 4, 0xFFFF);
  gfx->fillRect(hx - 4, hy + 4, 3, 7, 0xFFFF);
  for (int a = 20; a <= 70; a += 5) {  // 右下的反光：贴着边缘的一小段弧
    float t = a * PI / 180;
    gfx->fillRect(cx + (int)((rx - 4) * cosf(t)), cy + (int)((ry - 4) * sinf(t)), 2, 2, 0xDF7F);
  }
}

// 干活 / 等你确认时：气泡浮在水面右边，一下下弹起来再落回水面（落水时压扁一点、荡开涟漪）。
// 画在鲸鱼前面：鲸鱼跳到右边时从它后面过，图标始终看得清
void dsStateBubble(int x, int w, int wy, uint32_t now) {
  if (dsPokeUntil > now || (dsMode != M_WORKING && dsMode != M_WAITING)) return;
  const int r = 36, cx = x + w - r - 10;
  const uint32_t P = 1500;
  float q = (now % P) / (float)P;
  float hop = q < 0.6f ? sinf(q / 0.6f * PI) : 0;               // 前 60% 在空中，后面落在水面上
  float sq = q >= 0.6f && q < 0.8f ? sinf((q - 0.6f) / 0.2f * PI) : 0;  // 落水时压扁
  int rx = r + (int)(3 * sq), ry = r - (int)(3 * sq);
  int cy = wy - r * 4 / 9 - (int)(7 * hop) + (int)(2 * sq);
  dsAirBubble(cx, cy, rx, ry, wy);
  int iy = cy - 4;  // 图标往上一点，大部分在水面以上
  if (dsMode == M_WORKING && toolIsTerminal(dsTool)) {  // 终端：原图标 56×40 的方框角会伸出圆泡，画小一号
    gfx->fillRoundRect(cx - 22, iy - 15, 44, 30, 4, C_BLACK);
    pxText(">", cx - 16, iy + 7, C_GREEN, 2);
    if ((now / 400) % 2) gfx->fillRect(cx - 4, iy + 4, 8, 3, C_GREEN);
  } else if (dsMode == M_WORKING) drawToolIcon(cx - 26, iy - 22, now, dsTool);
  else pxText("?", cx - pxW("?", 4) / 2, iy + 20, (now / 400) % 2 ? C_DS : C_DS_HI, 4);
  if (cy + ry > wy) {  // 水线横穿气泡
    int half = (int)(rx * sqrtf(max(0.0f, 1 - powf((wy - cy) / (float)ry, 2))));
    gfx->drawFastHLine(cx - half, wy, half * 2 + 1, C_DS_WATER);
  }
  if (q >= 0.6f) {  // 涟漪：从气泡两边往外荡开、变短
    float e = (q - 0.6f) / 0.4f;
    int gap = rx + 2 + (int)(e * 14), len = 10 - (int)(e * 8);
    uint16_t c = e < 0.5f ? 0xFFFF : C_DS_WATER;
    gfx->drawFastHLine(cx - gap - len, wy + 1, len, c);
    if (cx + gap + len < x + w) gfx->drawFastHLine(cx + gap, wy + 1, len, c);
  }
}

// 海水的水面：一长一短两道正弦波叠在一起，往两个方向慢慢滚，起伏约 ±1.3s 像素
int dsWaveY(int px, int wy, int s, uint32_t now) {
  return wy + (int)lroundf(s * (0.9f * sinf(px * 0.075f + now / 520.0f) + 0.45f * sinf(px * 0.19f - now / 340.0f)));
}

void dsTank(int x, int y, int w, int h, int wy, int s, int base, uint32_t now) {
  // 上面敞口：水面以上不画，露出后面的墙 / 页面（鲸鱼跳出水面、喷水都不会撞顶）
  const int A = s * 135 / 100 + 1, wc = max(2, s - 1);  // 浪的最大起伏；水面按 wc 像素一列画，保持像素感
  for (int r = wy + A; r < y + h; r++) gfx->fillRect(x, r, w, 1, mix565(0x1A75, 0x0828, (r - wy) * 255 / (y + h - wy)));  // 越深越暗
  for (int c = 0; c < w; c += wc) {  // 浪：每列从这一列的水面画到 wy + A，浪尖勾一道亮边，最高处带白浪花
    int cw = min(wc, w - c), sy = dsWaveY(x + c, wy, s, now);
    gfx->fillRect(x + c, sy, cw, wy + A - sy, 0x1A75);
    gfx->fillRect(x + c, sy, cw, 1, sy <= wy - A + 2 ? 0xFFFF : (sy < wy ? C_DS_WATER : 0x3C9A));
  }
  int gs = max(2, s);  // 石子大小跟着缸的大小走
  for (int c = 0; c < w; c += gs) {
    int gh = gs * 2 + ((c / gs) % 3 == 1 ? gs / 2 : 0);
    gfx->fillRect(x + c, y + h - gh, gs, gh, (c / gs) % 3 ? 0x6B4D : 0x8C51);
  }
  // 水草：几丛，跟着水晃，越往上晃得越多
  int n = w > 150 ? 4 : 3;
  for (int k = 0; k < n; k++) {
    int sx = k < n / 2 ? 6 + k * gs * 3 : w - 10 - (n - 1 - k) * gs * 4, sh = (y + h - wy) * (k % 2 ? 2 : 3) / 7;
    for (int r = 0; r < sh; r += gs) {
      int sw = (int)(r / (6.0f * gs) * sinf(now / 800.0f + r * 0.08f / gs + k * 1.7f));
      gfx->fillRect(x + sx + sw, y + h - gs * 2 - r, gs, gs, (r / gs) % 3 ? 0x2C66 : 0x4E8A);
    }
  }
  dsTankFloor(x, y, w, h, gs, s, now);
  // 泡泡：从石子里往上冒
  for (int i = 0; i < 4; i++) {
    uint32_t bt = (now + i * 977) % 3600;
    int by = y + h - gs * 3 - (int)(bt * (y + h - gs * 3 - wy - 2) / 3600);
    gfx->drawCircle(x + w * (2 + i * 2) / 11 + (int)(2 * sinf(bt / 260.0f)), by, 1 + (i % 2) * (gs / 2), C_DS_WATER);
  }

  int m = dsMode;
  bool poke = dsPokeUntil > now;
  bool still = !poke && (m == M_WAITING || m == M_ERROR || m == M_OFFLINE);  // 停在中间
  bool rest = !poke && (m == M_SLEEP || (m == M_IDLE && userAway()));      // 趴在缸底慢慢漂
  DsPose ref = dsPose(m, now, s);  // 只用它的划水节奏
  // 横向：来回游（三角波），每划一下水往前一冲；睡觉时漂得很慢
  int range = max(0, (w - 32 * s) / 2 - 2 * s);
  uint32_t T = (rest ? 40000 : 9000) + (uint32_t)range * 120, t = now % T;
  float u = t < T / 2 ? (float)t / (T / 2) : 2.0f - (float)t / (T / 2);  // 0 → 1 → 0
  bool flip = t < T / 2;  // 往右游时头朝右
  int surge = rest ? 0 : (int)(0.8f * s * sinf(ref.wph)) * (flip ? 1 : -1);
  int wx = still ? x + w / 2 : x + w / 2 + (int)((u - 0.5f) * 2 * range) + surge;
  // 纵向：平时在 base 深度；浮上水面喷水（背露出水面），或者冲出水面
  int surfB = wy + 14 * s;                          // 浮在水面：喷水孔刚好露出来
  int jumpH = max(0, surfB - (y + 2 + 19 * s));     // 跳起来最高到缸口
  int wb = rest ? base + s : base;
  float tilt = 0;
  int spout = 0;
  float splash = -1;                                 // 水花进度（0~1），-1 不溅
  int splashX = wx;
  auto breach = [&](float q) {  // 一次跳跃：q 0→1，起跳抬头、落水低头，进出水面都溅水花
    wb = surfB - (int)(jumpH * 4 * q * (1 - q));
    tilt = 0.32f * (1 - 2 * q);
    if (q < 0.25f) splash = q / 0.25f;
    else if (q > 0.75f) splash = (q - 0.75f) / 0.25f;
  };
  if (poke) {
    breach(((now - (dsPokeUntil - 1800)) % 1800) / 1800.0f);
  } else if (m == M_DONE) {  // 完成：一下接一下地跳
    breach(((now - dsModeSince) % 1600) / 1600.0f);
    spout = 2;
  } else if (!still && !rest && m != M_THINKING) {  // 思考时同官方，只在原来的深度用力甩尾巴
    uint32_t P = m == M_WORKING ? 9000 : 13000, k = now / P, tt = now % P;
    if (tt >= P - 5000) {
      uint32_t e = tt - (P - 5000);
      if (e < 1200) {  // 往上游，抬头
        float q = easeIO(e / 1200.0f);
        wb = base + (int)((surfB - base) * q);
        tilt = 0.12f * sinf(q * PI);
      } else if (e < 3000) {  // 到水面：两次里喷一次水，第三次冲出水面
        float q = (e - 1200) / 1800.0f;
        if (k % 3 == 2) breach(q);
        else {
          wb = surfB;
          spout = q < 0.3f ? 1 : (q < 0.8f ? 2 : 1);
        }
      } else {  // 扎回去，低头
        float q = easeIO((e - 3000) / 2000.0f);
        wb = surfB + (int)((base - surfB) * q);
        tilt = -0.14f * sinf(q * PI);
      }
    }
  }
  dsTankWhaleX = wx;
  dsTankWhaleBase = wb;
  dsTankFlip = flip && !still;
  dsFx = !still && !rest ? 2 : 1;
  dsFxTilt = tilt;
  dsFxSpout = spout;
  drawDeepSeekAt(wx, wb, s, m, now, dsTankFlip);
  dsFx = 0;
  dsFxTilt = 0;
  dsFxSpout = 0;
  // 尾巴后面拖一串泡泡（游的时候）
  if (!still && !rest) {
    int dir = dsTankFlip ? -1 : 1, tx = wx + dir * 14 * s;
    for (int i = 0; i < 4; i++) {
      float a = ((now + i * 350) % 1400) / 1400.0f;
      int bx = tx + dir * (int)(a * 6 * s), by = wb - 14 * s - (int)(a * 8 * s);
      if (by > wy + 2) gfx->drawCircle(bx, by, 1 + (i % 2) * max(1, s / 2), C_DS_WATER);
    }
  }
  dsSplash(splashX, wy, s, splash);
  if (s >= 3) dsStateBubble(x, w, dsWaveY(x + w - 46, wy, s, now), now);  // 只在 Buddy 页的大缸里（首页墙上的小缸放不下）；跟着所在处的浪起伏
  // 水面的波光（跳出水面时正好压在鲸鱼身上，像一道水线）、玻璃上的反光
  for (int i = 0; i < w / 16; i++) {  // 波光贴着浪面走
    int gx = x + (i * 17 + (int)(now / 200)) % (w - 6);
    gfx->fillRect(gx, dsWaveY(gx + 3, wy, s, now) + 2, 6, 1, C_DS_WATER);
  }
  gfx->fillRect(x + 4, wy + 6, 1, (y + h - wy) / 4, 0x5ACB);
  gfx->fillRect(x + 7, wy + 6, 1, (y + h - wy) / 8, 0x5ACB);
}

// ---------- Buddy 页 · DeepSeek ----------
// 左边是一个大鱼缸，小鲸鱼在里面游；右边三张卡：状态（思考时同官方「深度求索中 · 用时 N 秒」+ 甩尾巴）/ 账户余额和走势 / 今天的请求和每小时分布
void drawDeepSeekScene(uint32_t now) {
  // 缸内；外面一圈 4px 框，上下边和右边的卡片对齐（46 ~ QC_Y2 + 68）。水面上方留得多，鲸鱼能整个跳出水面
  const int x = 12, y = 50, w = 228, h = QC_Y2 + 68 - 4 - y, wy = y + 60, s = 3;
  int m = dsMode;
  bool poke = dsPokeUntil > now;
  dsTankFrame(x, y, w, h);
  dsTank(x, y, w, h, wy, s, y + h - 16, now);
  if (!poke) switch (m) {
    case M_WAITING: {  // 问号气泡（dsStateBubble）+ 屏幕边框闪 DeepSeek 蓝
      if ((now / 500) % 2)
        for (int k = 0; k < 4; k++) gfx->drawRect(k, k, 480 - 2 * k, 272 - 2 * k, C_DS);
      break;
    }
    case M_DONE:  // 星星
      for (int i = 0; i < 6; i++) {
        int sx = x + 14 + (i * 47) % (w - 28), sy = y + 12 + (i * 31) % 40;
        if (((now / 150) + i) % 3 == 0 || abs(sx - dsTankWhaleX) < 18 * s) continue;  // 不压在鲸鱼身上
        uint16_t c = (i % 2) ? C_YELLOW : C_WHITE;
        gfx->fillRect(sx - 1, sy - 7, 3, 15, c);
        gfx->fillRect(sx - 7, sy - 1, 15, 3, c);
      }
      break;
    case M_ERROR: {  // 汗滴：挂在头顶旁边
      int dy = (now / 40) % 20, hx = dsTankWhaleX - 13 * s, hy = dsTankWhaleBase - 19 * s;
      gfx->fillCircle(hx, hy + dy, 5, C_BLUE);
      gfx->fillTriangle(hx - 5, hy - 2 + dy, hx + 5, hy - 2 + dy, hx, hy - 12 + dy, C_BLUE);
      break;
    }
    case M_COMPACT: {  // 一条条压扁
      int k = (now / 200) % 4;
      for (int i = 0; i < k; i++) gfx->fillRect(x + w - 50, y + 44 - i * 14, 40, 10, C_GREY);
      break;
    }
  }
}

// ---------- 右边三张卡 ----------
const int DSP_X = 252, DSP_W = 222;

// 状态卡：图标格里思考 / 干活时是甩尾巴，文字同官方「深度求索中」，下面一行「用时 N 秒 ···」
void dsStatusCard(uint32_t now) {
  int m = dsMode, x = DSP_X, y = 46, w = DSP_W, h = 54;
  bool poke = dsPokeUntil > now, active = m == M_THINKING || m == M_WORKING || m == M_COMPACT;
  pxPanel(x, y, w, h, P_PANEL, m == M_WAITING ? C_DS : P_EDGE, true);
  pxPanel(x + 10, y + 10, 34, 34, P_BG, P_BG, false);
  if (active && !poke) drawIcon8(IC_DSTAIL8[dsTailFrame(now)], x + 15, y + 15, 3, C_DS_HI);
  else drawIcon8(IC_DS8, x + 15, y + 15, 3, m == M_OFFLINE ? P_DIM : C_DS);
  String big, small;
  if (m == M_OFFLINE) {
    big = "未连接";
    small = "在电脑上启动 bridge";
  } else if (poke) {
    big = "噗～";
    small = dsDetail;
  } else {
    // 文字照 dsh 桌面版：思考时「深度求索中，用时 4秒 ···」，完成时「已完成，用时 1分20秒」
    big = m == M_THINKING ? String(DS_THINKING) : dsLabel;
    if (active && dsTurnStart) {
      String took = "用时 " + dsDuration((millis() - dsTurnStart) / 1000);
      small = m == M_WORKING && dsDetail.length() ? dsDetail + " · " + took : took + " ···";
    } else if (m == M_DONE && dsTurnStart && dsModeSince > dsTurnStart) {
      small = "用时 " + dsDuration((dsModeSince - dsTurnStart) / 1000);
    } else {
      small = dsDetail;
    }
  }
  const int tx = x + 54, tw = w - 54 - 10;
  int sc = pxW(big, 2) <= tw ? 2 : 1;
  uint16_t bc = m == M_OFFLINE ? P_DIM : (m == M_WAITING ? C_DS_HI : (active ? C_DS_HI : P_TEXT));
  if (active && !poke && sc == 2) {  // 思考 / 干活：蓝色流光字
    gfx->setFont(u8g2_font_cubic11_h_cjk);
    gfx->setTextSize(2);
    gfx->setCursor(tx, y + 27);
    printShimmerC(big, now, C_DS_HI, 0xFFFF, 0xCE7F);
    gfx->setTextSize(1);
  } else if (sc == 1 && pxUW(big) <= tw) {  // 两倍放不下（如「正在读取文件」）：用 16px 的中号字，别一下缩成最小
    pxU(big, tx, y + 26, bc);
  } else {
    pxMarquee(big, tx, y + (sc == 2 ? 27 : 23), bc, sc, tw);
  }
  pxMarquee(small, tx, y + 46, P_DIM, 1, tw);
}

// 余额卡：大字余额，下面是最近的余额走势（面积图），底行赠送 / 充值 / 每天大约花多少
void dsBalanceCard(int y) {
  const int x = DSP_X, w = DSP_W, gx = x + 12, gw = w - 24, gy = y + 31, gh = 15;
  pxPanel(x, y, w, 68, P_PANEL, P_EDGE, true);
  pxText("账户余额", x + 12, y + 22, P_DIM, 1);
  String big = dsBal.length() ? dsBal : String("—");
  pxText(big, x + w - 12 - pxW(big, 2), y + 26, !dsBal.length() ? P_DIM : (dsBalOk ? P_GREEN : P_RED), 2);
  if (dsTrendN >= 2) {
    float lo = dsTrend[0], hi = dsTrend[0];
    for (int i = 1; i < dsTrendN; i++) { lo = min(lo, dsTrend[i]); hi = max(hi, dsTrend[i]); }
    for (int c = 0; c < gw; c++) {
      float t = (float)c * (dsTrendN - 1) / (gw - 1);
      int i = min((int)t, dsTrendN - 2);
      float v = dsTrend[i] + (dsTrend[i + 1] - dsTrend[i]) * (t - i);
      int hpx = hi - lo < 0.005f ? gh / 2 : 1 + (int)((v - lo) / (hi - lo) * (gh - 2));
      gfx->fillRect(gx + c, gy + gh - hpx, 1, hpx, 0x18EC);
      gfx->fillRect(gx + c, gy + gh - hpx, 1, 2, C_DS);
    }
  } else {
    pxDots(gx, gx + gw, gy + gh - 1, P_EDGE);
  }
  String foot;
  uint16_t fc = P_DIM;
  if (!dsBal.length()) foot = "开着 magpie 或设置 API Key 后显示";
  else if (!dsBalOk) { foot = "余额不足，请充值"; fc = P_RED; }
  else {
    if (dsGrant.length()) foot += "赠送 " + dsGrant;
    if (dsTopup.length()) foot += String(foot.length() ? " · " : "") + "充值 " + dsTopup;
    if (dsPerDay > 0) foot += String(foot.length() ? " · " : "") + "每天约 ¥" + String(dsPerDay, 2);
    if (!foot.length()) foot = "按量计费";
  }
  pxMarquee(foot, x + 12, y + 60, fc, 1, w - 24);
}

// 今天卡：大字请求数，下面 24 小时的请求柱（现在这个小时亮一点），底行 token / 缓存命中 / 完成任务数
void dsTodayCard(int y) {
  const int x = DSP_X, w = DSP_W;
  pxPanel(x, y, w, 68, P_PANEL, P_EDGE, true);
  pxText("今天", x + 12, y + 22, P_DIM, 1);
  int done = doneTodayOf(2);
  String big = dsCalls >= 0 ? String(dsCalls) + " 次" : String(done) + " 个任务";
  pxText(big, x + w - 12 - pxW(big, 2), y + 26, P_TEXT, 2);
  if (dsBarN) {
    int step = (w - 24) / dsBarN, bx0 = x + 12 + (w - 24 - step * dsBarN) / 2;
    for (int k = 0; k < dsBarN; k++) {
      int bh = dsBars[k] * 14 / 1000, bx = bx0 + k * step;
      if (dsBars[k] > 0) gfx->fillRect(bx, y + 46 - max(bh, 2), step - 2, max(bh, 2), k == dsHour ? C_DS_HI : C_DS);
      else gfx->fillRect(bx, y + 45, step - 2, 1, k == dsHour ? C_DS_HI : P_EDGE);
    }
  } else {
    pxDots(x + 12, x + w - 12, y + 45, P_EDGE);
  }
  String foot = dsCalls >= 0 ? "输入 " + dsTokIn + " · 输出 " + dsTokOut + " · 缓存命中 " + dsHit + " · 完成 " + String(done) + " 个"
                             : "当前会话 " + String(dsSessions) + " 个 · 开着 magpie 看用量";
  pxMarquee(foot, x + 12, y + 60, P_DIM, 1, w - 24);
}

void drawDeepSeekPanel(uint32_t now) {
  drawBuddySwitch();
  dsStatusCard(now);
  dsBalanceCard(QC_Y1);
  dsTodayCard(QC_Y2);
}
