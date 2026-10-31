// Codex 小机器人：绘制、各状态的姿势与表情、Buddy 页的 Codex 视图
// 贴图见 codex_pet.h；脸（头上的屏幕）由程序画，所以能有表情

uint16_t cxColor(char ch, bool grey) {
  switch (ch) {
    case 'K': case 'k': return grey ? 0x18C3 : 0x08A6;
    case 'N': return grey ? 0x2945 : 0x216D;
    case 'L': return grey ? 0xA514 : 0x751F;
    case 'D': case 'e': return grey ? 0x6B4D : 0x433B;
    case 'C': return grey ? 0xC618 : 0xAFFF;
    default: return grey ? 0x8410 : 0x541E;  // B b
  }
}

// 在贴图坐标 (c, r) 画一个 s×s 的点
#define CXPX(c, r, col) gfx->fillRect(x0 + (c) * s, y0 + (r) * s, s, s, col)

void drawCodexFace(int x0, int y0, int s, int face, uint32_t now, bool grey) {
  uint16_t cy = grey ? 0x8410 : 0xAFFF;
  switch (face) {
    case CF_PROMPT:
    case CF_TYPING: {  // >_ ：光标闪烁；干活时闪得快、往右挪，留下「打出来的字」
      for (int k = 0; k < 4; k++) {
        CXPX(12 + k, 13 + k, cy); CXPX(13 + k, 13 + k, cy);
        CXPX(12 + k, 19 - k, cy); CXPX(13 + k, 19 - k, cy);
      }
      bool on = face == CF_TYPING ? (now / 150) % 2 : (now / 530) % 2;
      int c0 = face == CF_TYPING ? 19 + (int)((now / 300) % 3) : 19;
      if (on)
        for (int c = c0; c < c0 + 5 && c < 26; c++) { CXPX(c, 18, cy); CXPX(c, 19, cy); }
      for (int c = 19; c < c0; c++) CXPX(c, 16, 0x433B);
      break;
    }
    case CF_SLEEP: {  // ︶ ︶
      const int arc[5][2] = {{0, 0}, {1, 1}, {2, 1}, {3, 1}, {4, 0}};
      for (auto &a : arc) { CXPX(11 + a[0], 16 + a[1], cy); CXPX(19 + a[0], 16 + a[1], cy); }
      break;
    }
    case CF_DOTS: {  // 思考：三个点依次跳起
      int k = (now / 300) % 4;
      for (int i = 0; i < 3; i++) {
        int r = 16 - (i == k ? 2 : 0);
        uint16_t c = i == k ? cy : 0x433B;
        for (int d = 0; d < 4; d++) CXPX(11 + i * 5 + d % 2, r + d / 2, c);
      }
      break;
    }
    case CF_HAPPY: {  // ^ ^
      const int hp[5][2] = {{0, 2}, {1, 1}, {2, 0}, {3, 1}, {4, 2}};
      for (auto &h : hp)
        for (int t = 0; t < 2; t++) { CXPX(11 + h[0], 14 + h[1] + t, cy); CXPX(19 + h[0], 14 + h[1] + t, cy); }
      break;
    }
    case CF_X:  // × ×
      for (int k = 0; k < 5; k++) {
        CXPX(11 + k, 13 + k, cy); CXPX(15 - k, 13 + k, cy);
        CXPX(19 + k, 13 + k, cy); CXPX(23 - k, 13 + k, cy);
      }
      break;
    case CF_EYES:  // | | ：睁大眼看着你
      for (int r = 14; r < 19; r++) { CXPX(13, r, cy); CXPX(14, r, cy); CXPX(20, r, cy); CXPX(21, r, cy); }
      break;
    case CF_LOOKUP:  // ' ' ：眼睛往左上瞟
      for (int r = 13; r < 16; r++) { CXPX(12, r, cy); CXPX(13, r, cy); CXPX(19, r, cy); CXPX(20, r, cy); }
      break;
    case CF_WINCE:  // > < ：疼得挤眼
      for (int k = 0; k < 4; k++) {
        CXPX(12 + k, 13 + k, cy); CXPX(12 + k, 19 - k, cy);
        CXPX(22 - k, 13 + k, cy); CXPX(22 - k, 19 - k, cy);
      }
      break;
    case CF_SQUINT: {  // ≥ ≤ ：皱成一团
      static const char *const L[5] = {"##...", "..###", "##...", ".....", "#####"};
      for (int r = 0; r < 5; r++)
        for (int k = 0; k < 5; k++)
          if (L[r][k] == '#') { CXPX(11 + k, 14 + r, cy); CXPX(23 - k, 14 + r, cy); }
      break;
    }
    case CF_ASK: {  // 等你确认：> 还在，光标的位置变成一个小问号（亮蓝 / 青色交替）
      for (int k = 0; k < 4; k++) {
        CXPX(12 + k, 13 + k, cy); CXPX(13 + k, 13 + k, cy);
        CXPX(12 + k, 19 - k, cy); CXPX(13 + k, 19 - k, cy);
      }
      uint16_t c = (now / 450) % 2 ? cy : C_CODEX_HI;
      static const char *const Q[7] = {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."};
      for (int r = 0; r < 7; r++)
        for (int k = 0; k < 5; k++)
          if (Q[r][k] == '#') CXPX(19 + k, 13 + r, c);
      break;
    }
    case CF_SQUEEZE:  // 整理上下文：三条横线一挤一挤
      for (int i = 0; i < 3; i++) {
        int w = 12 - (int)((now / 200 + i) % 4) * 2;
        for (int c = 0; c < w; c++) CXPX(17 - w / 2 + c, 14 + i * 2, cy);
      }
      break;
    default: break;  // CF_OFF：黑屏
  }
}

// cx：中心 x；base：脚底 y；s：每个贴图像素画多大
void drawCodex(int cx, int base, int s, const CxPose &p) {
  gfx->fillEllipse(cx + p.dx / 2, base + max(2, s), 12 * s, max(2, s), C_DIM);  // 影子
  int x0 = cx - 16 * s + p.dx, y0 = base - 39 * s + p.dy;
  int hoff = p.sit ? 2 : 0;  // 坐下时头往下挪
  for (int r = 0; r < (p.sit ? 27 : 39); r++) {
    const char *row = CODEX_PET[r];
    for (int c = 0; c < 32; c++) {
      char ch = row[c];
      if (ch == '.' || ch == 'k' || ch == 'b' || ch == 'e') continue;
      gfx->fillRect(x0 + c * s, y0 + (r + hoff) * s, s, s, cxColor(ch, p.grey));
    }
  }
  if (p.sit) {
    for (int r = 0; r < 10; r++)
      for (int c = 0; c < 34; c++)
        if (CODEX_SIT[r][c] != '.') gfx->fillRect(x0 + (1 + c) * s, y0 + (28 + r) * s, s, s, cxColor(CODEX_SIT[r][c], p.grey));
    // 笔记本屏幕：往右后方斜的平行四边形，底边贴着键盘
    for (int i = 0; i < 14; i++) {
      int xl = 25 - i * 4 / 13;
      for (int c = xl; c < xl + 10; c++) {
        bool edge = i == 0 || i == 13 || c == xl || c == xl + 9;
        gfx->fillRect(x0 + c * s, y0 + (21 + i) * s, s, s, cxColor(edge ? 'K' : 'N', p.grey));
      }
    }
    const int gt[5][2] = {{3, 4}, {4, 5}, {5, 6}, {4, 7}, {3, 8}};  // 屏幕上的 >
    for (auto &g : gt) gfx->fillRect(x0 + (25 - g[1] * 4 / 13 + g[0]) * s, y0 + (21 + g[1]) * s, s, s, cxColor('C', p.grey));
    for (int r = 0; r < 4; r++)  // 手
      for (int c = 0; c < 6; c++)
        if (CODEX_HAND[r][c] != '.') gfx->fillRect(x0 + (12 + c + p.armL) * s, y0 + (31 + r) * s, s, s, cxColor(CODEX_HAND[r][c], p.grey));
    for (int c = 9; c < 35; c++) {  // 键盘底座
      gfx->fillRect(x0 + c * s, y0 + 35 * s, s, s, cxColor('K', p.grey));
      gfx->fillRect(x0 + c * s, y0 + 36 * s, s, s, cxColor(c == 9 || c == 34 ? 'K' : 'L', p.grey));
      gfx->fillRect(x0 + c * s, y0 + 37 * s, s, s, cxColor('K', p.grey));
    }
    return;
  }
  // 手臂单独一层：左右各自可以举高
  for (int r = 26; r < 35; r++) {
    const char *row = CODEX_PET[r];
    for (int c = 0; c < 32; c++) {
      char ch = row[c];
      if (ch != 'k' && ch != 'b' && ch != 'e') continue;
      int up = c < 16 ? p.armL : p.armR;
      gfx->fillRect(x0 + c * s, y0 + (r - up) * s, s, s, cxColor(ch, p.grey));
    }
  }
}

// 按官方 Codex 宠物的帧时长（hatch-pet/references/animation-rows.md）算当前第几帧
int cxFrame(uint32_t now, const uint16_t *ms, int n) {
  uint32_t total = 0;
  for (int i = 0; i < n; i++) total += ms[i];
  uint32_t t = now % total;
  for (int i = 0; i < n; i++) {
    if (t < ms[i]) return i;
    t -= ms[i];
  }
  return n - 1;
}

// 各状态的姿势（首页和 Buddy 页共用），动作和表情顺序照官方 Codex 宠物
CxPose codexPose(int m, uint32_t now, int s) {
  CxPose p;
  switch (m) {
    case M_OFFLINE: p.grey = true; p.face = CF_OFF; break;
    case M_SLEEP: p.face = CF_SLEEP; break;
    case M_THINKING: p.face = CF_DOTS; p.armL = 5; break;  // 一只手托着下巴想
    case M_WORKING: {  // 官方 running：坐着抱笔记本敲，表情 >_ ^^ ︶︶ || >_ ^^ 轮换
      static const uint16_t MS[6] = {120, 120, 120, 120, 120, 220};
      static const uint8_t F[6] = {CF_PROMPT, CF_HAPPY, CF_SLEEP, CF_EYES, CF_PROMPT, CF_HAPPY};
      int f = cxFrame(now, MS, 6);
      p.sit = true;
      p.face = F[f];
      p.armL = f % 3;
      break;
    }
    case M_WAITING: {  // 官方 waiting：手托下巴，>_ || '' >_ >_ || 轮换，等你拿主意
      static const uint16_t MS[6] = {150, 150, 150, 150, 150, 260};
      static const uint8_t F[6] = {CF_PROMPT, CF_EYES, CF_LOOKUP, CF_PROMPT, CF_PROMPT, CF_EYES};
      p.face = F[cxFrame(now / 2, MS, 6)];  // 放慢一倍：等待要持续很久，太快会晃眼
      p.armL = 5;
      break;
    }
    case M_DONE: {  // 官方 review：>_ ^^，中间轻轻一跳挥手
      static const uint16_t MS[6] = {150, 150, 150, 150, 150, 280};
      static const uint8_t F[6] = {CF_PROMPT, CF_HAPPY, CF_HAPPY, CF_PROMPT, CF_PROMPT, CF_HAPPY};
      int f = cxFrame(now, MS, 6);
      p.face = F[f];
      if (f == 2) { p.dy = -2 * s; p.armR = 6; }
      break;
    }
    case M_ERROR: {  // 官方 failed：双手抱头，>< ×× ≥≤ 轮换
      static const uint16_t MS[8] = {140, 140, 140, 140, 140, 140, 140, 240};
      static const uint8_t F[8] = {CF_PROMPT, CF_WINCE, CF_X, CF_SQUINT, CF_PROMPT, CF_WINCE, CF_X, CF_SQUINT};
      int f = cxFrame(now, MS, 8);
      p.face = F[f];
      if (f) p.armL = p.armR = 7;
      break;
    }
    case M_COMPACT: p.face = CF_SQUEEZE; p.dy = (int)(s * sinf(now / 250.0f)); break;
    default: {  // 官方 idle：不跳动，>_ 和 ︶︶ 交替眨眼；每轮之间停一会儿，免得一直眨
      static const uint16_t MS[7] = {280, 110, 110, 140, 140, 320, 3000};
      static const uint8_t F[7] = {CF_PROMPT, CF_SLEEP, CF_PROMPT, CF_PROMPT, CF_PROMPT, CF_SLEEP, CF_PROMPT};
      p.face = F[cxFrame(now, MS, 7)];
      break;
    }
  }
  return p;
}

// 带表情的完整绘制（表情需要时间做动画）
void drawCodexAt(int cx, int base, int s, int m, uint32_t now) {
  CxPose p = codexPose(m, now, s);
  if (cxPokeUntil > now) {  // 被点：原地跳起来，一只手高高举起挥一挥
    float k = ((now - (cxPokeUntil - 1800)) % 900) / 900.0f;
    p = CxPose();
    p.face = CF_PROMPT;
    p.dy = -(int)(s * 8 * 4 * k * (1 - k));
    p.armR = (now / 150) % 2 ? 7 : 5;
    p.armL = 1;
  } else if ((m == M_IDLE || m == M_SLEEP) && backAt && now - backAt < 8000) {  // 欢迎回来：挥手
    p.face = CF_HAPPY;
    p.armR = (now / 220) % 2 ? 5 : 2;
  } else if (m == M_IDLE && userAway()) {
    p.face = CF_SLEEP;
  }
  drawCodex(cx, base, s, p);
  int x0 = cx - 16 * s + p.dx, y0 = base - 39 * s + p.dy + (p.sit ? 2 * s : 0);
  drawCodexFace(x0, y0, s, p.face, now, p.grey);
  if (p.sit && (now / 250) % 2) {  // 笔记本屏幕上 > 后面的光标一闪一闪
    int ly = base - 39 * s + p.dy + 29 * s, lx = cx - 16 * s + p.dx + (25 - 8 * 4 / 13 + 6) * s;
    gfx->fillRect(lx, ly, 2 * s, s, 0xAFFF);
  }
  if (m == M_SLEEP || p.face == CF_SLEEP)
    for (int i = 0; i < 3; i++) {
      uint32_t zt = (now + i * 900) % 2700;
      pxText("z", cx + 11 * s + i * 2 * s + (int)(zt / 150), max(56, y0 - (int)(zt / 150) - i * 4), P_DIM, 1 + (i == 2 && s >= 3));  // 不飘进 Buddy 页标题栏
    }
}

// ---------- Buddy 页 · Codex ----------
// 8×8 小图标：Clawd / Codex 的 >_（这里和 ui_pixel、ui_toast 共用；ino 按文件名拼接，要定义在最先用到的文件里）
const char *const IC_CLAWD8[8] = {"........", ".111111.", ".1.11.1.", "11111111", ".111111.", ".111111.", ".11..11.", "........"};
const char *const IC_CODEX8[8] = {"........", ".1......", "..1.....", "...1....", "..1.....", ".1..111.", "........", "........"};

// 标题栏：屋子图标右边是 Claude Code / Codex / DeepSeek 切换标签，样式同 magpie 页（选中的填色块 + 深色字，
// 只是填的是各自的品牌色）；没选中的：灰图标 + 暗字。会话数大于 1 时，名字后面跟一个小数字。底下一道像素虚线
void drawBuddySwitch() {
  for (int i = 0; i < 3; i++) {
    int x = BS_X + i * BS_W;
    bool on = i == buddyAgent;
    uint16_t c = agColor(i), fg = on ? P_BG : P_DIM;
    int n = agSessions(i);
    String t = agTitle(i), ns = n > 1 ? String(n) : "";
    if (on) pxPanel(x + 2, 6, BS_W - 4, 28, c, c, false);
    int cw = 16 + 6 + pxW(t, 1) + (ns.length() ? 5 + pxW(ns, 1) : 0);  // 图标 16 + 间隔 + 名字 + 数字
    int lx = x + BS_W / 2 - cw / 2;
    drawIcon8(agIcon8(i), lx, 12, 2, fg);
    pxText(t, lx + 22, 25, fg, 1);
    if (ns.length()) pxText(ns, lx + 22 + pxW(t, 1) + 5, 25, on ? P_BG : c, 1);
  }
  for (int x = 0; x < 480; x += 4) gfx->fillRect(x, 40, 2, 2, P_GRID);
}

void drawCodexScene(uint32_t now) {
  drawCodexAt(128, 238, 4, cxMode, now);  // 比首页大、但头顶让开标题栏（被点时会跳起来）
}

const char *CX_THINK[4] = {"推理中", "构思中", "琢磨中", "盘算中"};

void drawCodexPanel(uint32_t now) { drawAgentPanel(1, now); }
