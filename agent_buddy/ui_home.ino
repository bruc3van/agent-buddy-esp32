// 首页：Agent Buddy / magpie 二选一；以及两个界面共用的小部件（返回按钮、喜鹊、Agent 图标、文字工具）

// ---------- 通用小工具 ----------
uint16_t mix565(uint16_t a, uint16_t b, uint8_t t) {  // t=0 → a，t=255 → b
  int r = ((a >> 11) * (255 - t) + (b >> 11) * t) / 255;
  int g = (((a >> 5) & 0x3F) * (255 - t) + ((b >> 5) & 0x3F) * t) / 255;
  int bl = ((a & 0x1F) * (255 - t) + (b & 0x1F) * t) / 255;
  return (r << 11) | (g << 5) | bl;
}

int textW(const String &s) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

void printAt(const String &s, int x, int y, uint16_t color) {
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

void printCenter(const String &s, int cx, int y, uint16_t color) {
  printAt(s, cx - textW(s) / 2, y, color);
}

void printRight(const String &s, int rx, int y, uint16_t color) {
  printAt(s, rx - textW(s), y, color);
}

// ---------- magpie 喜鹊（2bit 灰度帧，见 magpie_wag.h）----------
void drawBird(const uint8_t *frame, int size, int x, int y, uint16_t bg, uint16_t fg) {
  uint16_t lv[4] = {bg, mix565(bg, fg, 90), mix565(bg, fg, 175), fg};
  int bpr = (size + 3) / 4;
  for (int yy = 0; yy < size; yy++) {
    const uint8_t *row = frame + yy * bpr;
    for (int xx = 0; xx < size; xx++) {
      int a = (row[xx >> 2] >> (6 - 2 * (xx & 3))) & 3;
      if (a) gfx->drawPixel(x + xx, y + yy, lv[a]);
    }
  }
}

// magpie logo 的 wag 动画：从 start 起 0.9 秒播完 23 帧（点头 + 扇翅 + 翘尾），之后停在第 0 帧
int wagFrame(uint32_t now, uint32_t start) {
  uint32_t t = now - start;
  return t < 900 ? t * (WAG28_FRAMES - 1) / 900 : 0;
}

// ---------- Agent / 订阅图标 ----------
// 先按图标名找 magpie 的品牌图标（brand_icons.h），再按 Agent 显示名找，都没有才画字母圆
const BrandIcon *brandFind(const String &icon, const String &name) {
  if (icon.length())
    for (const BrandIcon &b : BRAND_ICONS)
      if (icon == b.icon) return &b;
  for (const BrandAlias &a : BRAND_ALIAS)
    if (name == a.name)
      for (const BrandIcon &b : BRAND_ICONS)
        if (!strcmp(a.icon, b.icon)) return &b;
  return nullptr;
}

// logo 主题色；黑白 logo 用当前主题的前景色
uint16_t brandColor(const String &icon, const String &name, uint16_t fallback) {
  const BrandIcon *b = brandFind(icon, name);
  if (!b) return fallback;
  return b->mono ? (pixelStyle ? 0xEF7E : 0x18E3) : b->color;
}

void drawAgentIcon(const String &icon, const String &name, int x, int y, int s) {
  const BrandIcon *b = brandFind(icon, name);
  if (b) {
    int ox = x + (s - BRAND_ICON_SIZE) / 2, oy = y + (s - BRAND_ICON_SIZE) / 2;
    for (int i = 0; i < BRAND_ICON_SIZE * BRAND_ICON_SIZE; i++) {
      if (!((b->mask[i >> 3] >> (7 - (i & 7))) & 1)) continue;
      uint16_t c = b->px[i];
      if (b->mono && !pixelStyle) c = 0x18E3;  // 浅色主题下黑白 logo 用深色
      gfx->drawPixel(ox + i % BRAND_ICON_SIZE, oy + i / BRAND_ICON_SIZE, c);
    }
    return;
  }
  int cx = x + s / 2, cy = y + s / 2;
  if (icon.indexOf("claude") >= 0 || icon.indexOf("anthropic") >= 0 || name.startsWith("Claude")) {
    drawSpark(cx, cy, s / 2, 5, 0, C_CLAUDE);
  } else if (icon.indexOf("codex") >= 0 || icon.indexOf("openai") >= 0 || name.startsWith("Codex")) {
    gfx->fillRoundRect(x, y, s, s, s / 4, 0x5B7E);
    int k = s / 5;
    gfx->drawLine(cx - 2 * k, cy - k, cx - k / 2, cy, C_WHITE);
    gfx->drawLine(cx - k / 2, cy, cx - 2 * k, cy + k, C_WHITE);
    gfx->drawFastHLine(cx, cy + k, k + 2, C_WHITE);
  } else if (icon.indexOf("xai") >= 0 || icon.indexOf("grok") >= 0 || name.startsWith("Grok")) {
    uint16_t ink = pixelStyle ? 0xEF7E : 0x3186;  // 深色底上要用浅色
    for (int k = 0; k < 2; k++) {
      gfx->drawLine(x + 2 + k, y + s - 2, x + s - 3 + k, y + 1, ink);
      gfx->drawLine(x + 2 + k, y + 3, x + s / 2 - 1 + k, y + s / 2, ink);
    }
  } else {
    gfx->fillCircle(cx, cy, s / 2, 0x9CF3);
    gfx->setFont(u8g2_font_cubic11_h_cjk);
    String c = name.length() ? name.substring(0, 1) : String("?");
    printCenter(c, cx, cy + 4, C_WHITE);
  }
}

// ---------- 首页 ----------
uint32_t homeFlapAt = 0;

uint32_t standbyLastMs = 0;  // 最近一次画待机时钟的时间：点一下只是唤醒，不触发按钮

void drawHome(uint32_t now) {
  if (standbyOn(now)) {
    standbyLastMs = now;
    drawStandbyClock(now);  // 离开电脑：待机时钟（ui_scene.ino）
    return;
  }
  drawHomePx(now);
}

void homeTap(int x, int y) {
  uint32_t now = millis();
  if (standbyOn(now)) {  // 待机时钟页：点一下回首页
    clockDismiss();
    return;
  }
  if (standbyLastMs && now - standbyLastMs < 1500) return;
  if (y < 40 && homeDateX && x >= homeDateX - 8) {  // 点日期时钟：切到待机时钟页
    clockPinned = true;
    return;
  }
  if (y < 40 && (batHas || batUsb) && homeIconX && x >= homeIconX - 34 && x < homeIconX + 4) {  // 电池 → 关于
    setSec = 4;
    setScreen(SCR_SET);
    return;
  }
  bool inPill = homeIconX ? (x >= homeIconX - 8 && x < homeIconX + HOME_PILL_W + 8) : x > 280;
  if (y < 40 && inPill) {  // 右上角设置按钮（放宽 8px 好点）
    if (homeIconX && x >= homeIconX + 90) {  // 雷达 → 雷达页
      setScreen(SCR_RADAR);
      return;
    }
    if (homeIconX && x >= homeIconX + 32 && x < homeIconX + 56) setSec = 0;  // 麦克风 → 声音
    else if (homeIconX && x >= homeIconX + 56) setSec = 1;                   // Wi-Fi
    setScreen(SCR_SET);
    return;
  }
  if (cfgHomeScene) {
    sceneTap(x, y);
    return;
  }
  if (y < 38 || y > 266) return;
  if (x >= 14 && x <= 234) setScreen(SCR_BUDDY);
  else if (x >= 246 && x <= 466) setScreen(SCR_MAGPIE);
}
