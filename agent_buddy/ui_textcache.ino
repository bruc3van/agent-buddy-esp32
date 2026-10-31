// 文字缓存：像素字放大画很慢（每个字形像素一次 fillRect），而且同样的文字每帧都在重画。
// 第一次画时在一块小画布上按 1 倍渲染成掩码存起来（最多 160 段，按最近使用淘汰），
// 之后按掩码直接往当前画布的内存里填颜色，宽度也一并缓存。
// font：0 = cubic11（pxText），1 = unifont（pxU）

const int TXT_N = 160;
TxtEnt txtCache[TXT_N];
uint32_t txtTick = 0;
Arduino_Canvas *txtScratch = nullptr;

uint32_t txtHash(uint8_t font, const String &s) {
  uint32_t h = 2166136261u ^ font;
  for (unsigned i = 0; i < s.length(); i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
  return h;
}

TxtEnt *txtGet(uint8_t font, const String &s) {
  uint32_t h = txtHash(font, s);
  int victim = 0;
  for (int i = 0; i < TXT_N; i++) {
    TxtEnt &e = txtCache[i];
    if (e.hash == h && e.font == font && e.mask && e.s == s) {
      e.used = ++txtTick;
      return &e;
    }
    if (e.used < txtCache[victim].used) victim = i;
  }
  if (!txtScratch) {
    txtScratch = new FastCanvas(480, 48, nullptr);
    if (!txtScratch->begin(GFX_SKIP_OUTPUT_BEGIN)) { delete txtScratch; txtScratch = nullptr; return nullptr; }
    txtScratch->setUTF8Print(true);
  }
  Arduino_Canvas *c = txtScratch;
  c->setFont(font ? u8g2_font_unifont_h_chinese4 : u8g2_font_cubic11_h_cjk);
  c->setTextSize(1);
  c->fillScreen(0);
  c->setTextColor(0xFFFF);
  const int CX = 4, CY = 34;
  int16_t bx, by;
  uint16_t bw, bh;
  c->getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  if (CX + bx + bw > 480) return nullptr;  // 比临时画布宽：掩码会被截断，交给调用方直接画
  c->setCursor(CX, CY);
  c->print(s);
  // 实际墨迹范围
  uint16_t *fb = c->getFramebuffer();
  int x0 = 480, y0 = 48, x1 = -1, y1 = -1;
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 480; x++)
      if (fb[y * 480 + x]) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
  TxtEnt &e = txtCache[victim];
  if (e.mask) free(e.mask);
  e.mask = nullptr;
  e.hash = h; e.font = font; e.s = s; e.adv = bw; e.used = ++txtTick;
  if (x1 < 0) { e.w = e.h = 0; e.mask = (uint8_t *)ps_malloc(1); return &e; }  // 空白
  e.w = x1 - x0 + 1; e.h = y1 - y0 + 1; e.ox = x0 - CX; e.oy = y0 - CY;
  e.mask = (uint8_t *)ps_malloc(e.w * e.h);
  if (!e.mask) return nullptr;
  for (int y = 0; y < e.h; y++)
    for (int x = 0; x < e.w; x++) e.mask[y * e.w + x] = fb[(y0 + y) * 480 + x0 + x] ? 1 : 0;
  return &e;
}

// 在 (x, 基线 y) 按 scale 倍画文字；返回 false 表示缓存不可用（调用方退回原来的画法）
bool txtDraw(uint8_t font, const String &s, int x, int y, uint16_t color, int scale) {
  TxtEnt *e = txtGet(font, s);
  if (!e || !e->mask) return false;
  uint16_t *fb = gfx->getFramebuffer();
  int W = gfx->width(), H = gfx->height();
  for (int r = 0; r < e->h; r++) {
    int py = y + (e->oy + r) * scale;
    if (py + scale <= 0 || py >= H) continue;
    const uint8_t *m = e->mask + r * e->w;
    for (int k = 0; k < e->w; k++) {
      if (!m[k]) continue;
      int px = x + (e->ox + k) * scale;
      if (px + scale <= txtClipL || px >= txtClipR) continue;
      for (int dy = 0; dy < scale; dy++) {
        int yy = py + dy;
        if (yy < 0 || yy >= H || yy < txtClipT || yy >= txtClipB) continue;
        uint16_t *row = fb + yy * W;
        for (int dx = 0; dx < scale; dx++) {
          int xx = px + dx;
          if (xx >= 0 && xx < W && xx >= txtClipL && xx < txtClipR) row[xx] = color;
        }
      }
    }
  }
  return true;
}

// 跑马灯：放得下就照常画；放不下就在 maxW 宽的范围里循环滚动（开头停 1.5 秒再滚，每秒 25 像素），不再截成「…」
void pxMarquee(const String &s, int x, int y, uint16_t c, int scale, int maxW) {
  int tw = pxW(s, scale);
  if (tw <= maxW) { pxText(s, x, y, c, scale); return; }
  const int PAUSE = 1500, SPEED = 25;
  int period = tw + 24 * scale;
  uint32_t t = millis() % (PAUSE + (uint32_t)period * 1000 / SPEED);
  int off = t < PAUSE ? 0 : (int)((t - PAUSE) * SPEED / 1000);
  txtClipL = max(0, x);
  txtClipR = min(480, x + maxW);
  pxText(s, x - off, y, c, scale);
  if (period - off < maxW) pxText(s, x - off + period, y, c, scale);
  txtClipL = 0;
  txtClipR = 480;
}

int txtWidth(uint8_t font, const String &s, int scale) {
  TxtEnt *e = txtGet(font, s);
  return e ? e->adv * scale : -1;
}
