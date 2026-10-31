// 像素风小样（目前只做 magpie「用量」页）：整数倍放大的像素字体、切角粗边框、实色错位阴影、方块柱状图
// 风格开关：串口 {"t":"ui","style":1}（bridge: POST /ui?style=1），0 切回原样；保存在 NVS


// 切角面板：2px 边框，四角各切掉 2px，右下 3px 实色阴影
// 只画看得见的部分（原来阴影、三层边框、填充会把整块区域重复写 5 遍，大面板很慢）：
// 阴影只画右边和下边露出来的细条，边框只画一圈，内部填一次。画出来和原来逐像素一致。
void pxPanel(int x, int y, int w, int h, uint16_t fill, uint16_t edge, bool shadow) {
  if (shadow) {
    gfx->fillRect(x + w, y + 3, 3, h, P_SHADOW);      // 右边
    gfx->fillRect(x + 3, y + h, w - 3, 3, P_SHADOW);  // 下边
    gfx->fillRect(x + w - 2, y + h - 2, 2, 2, P_SHADOW);  // 右下切角里露出的一点
  }
  if (fill == edge) {  // 实心：三块叠出切角
    gfx->fillRect(x + 2, y, w - 4, h, fill);
    gfx->fillRect(x, y + 2, 2, h - 4, fill);
    gfx->fillRect(x + w - 2, y + 2, 2, h - 4, fill);
    gfx->fillRect(x + 1, y + 1, 1, h - 2, fill);
    gfx->fillRect(x + w - 2, y + 1, 1, h - 2, fill);
    return;
  }
  gfx->fillRect(x + 2, y, w - 4, 2, edge);                // 上边两行（第二行左右各多 1 像素）
  gfx->fillRect(x + 1, y + 1, 1, 1, edge);
  gfx->fillRect(x + w - 2, y + 1, 1, 1, edge);
  gfx->fillRect(x + 2, y + h - 2, w - 4, 2, edge);        // 下边两行
  gfx->fillRect(x + 1, y + h - 2, 1, 1, edge);
  gfx->fillRect(x + w - 2, y + h - 2, 1, 1, edge);
  gfx->fillRect(x, y + 2, 2, h - 4, edge);                // 左右各两列
  gfx->fillRect(x + w - 2, y + 2, 2, h - 4, edge);
  gfx->fillRect(x + 2, y + 2, w - 4, h - 4, fill);        // 内部
}

// 1bit 喜鹊：只取不透明度高的像素，干净的像素剪影
void drawBirdPx(const uint8_t *frame, int size, int x, int y, uint16_t fg) {
  int bpr = (size + 3) / 4;
  for (int yy = 0; yy < size; yy++) {
    const uint8_t *row = frame + yy * bpr;
    for (int xx = 0; xx < size; xx++) {
      int a = (row[xx >> 2] >> (6 - 2 * (xx & 3))) & 3;
      if (a >= 1) gfx->drawPixel(x + xx, y + yy, fg);
    }
  }
}

// 像素字：cubic11（11px 像素中文），scale 倍整数放大
void pxText(const String &s, int x, int y, uint16_t c, int scale) {
  if (txtDraw(0, s, x, y, c, scale)) return;  // 文字缓存（ui_textcache.ino）
  gfx->setFont(u8g2_font_cubic11_h_cjk);
  gfx->setTextSize(scale);
  printAt(s, x, y, c);
  gfx->setTextSize(1);
}

int pxW(const String &s, int scale) {
  int cw = txtWidth(0, s, scale);
  if (cw >= 0) return cw;
  gfx->setFont(u8g2_font_cubic11_h_cjk);
  gfx->setTextSize(scale);
  int w = textW(s);
  gfx->setTextSize(1);
  return w;
}

// 标题栏：左上 home 按钮（与 Agent Buddy 页一致）+ 喜鹊图标（打开 / 切换标签时点头扇翅），右边四个标签
const int TAB_X = 82, TAB_W = 98;

// 像素「≈」：两道小波浪，按放大倍数画（字体里没有这个字符）
int pxApprox(int x, int y, int sc, uint16_t c) {
  const uint8_t wave[2][5] = {{0, 1, 0, 0, 1}, {1, 0, 1, 1, 0}};
  for (int t = 0; t < 2; t++)
    for (int r = 0; r < 2; r++)
      for (int q = 0; q < 5; q++)
        if (wave[r][q]) gfx->fillRect(x + q * sc, y - 8 * sc + t * 3 * sc + r * sc, sc, sc, c);
  return 6 * sc;
}

void drawMagHeaderPx(uint32_t now) {
  gfx->fillRect(0, 0, 480, M_TOP, P_BG);
  pxHomeBtn(6, 6);
  drawBirdPx(wag28[wagFrame(now, magWagAt)], WAG28_SIZE, 44, 6, P_TEXT);
  const char *names[4] = {"Agent", "额度", "用量", "路由"};
  for (int i = 0; i < 4; i++) {
    int x = TAB_X + i * TAB_W;
    String s = names[i];
    if (i == 0 && maCount) s += " " + String(maCount);
    if (i == magTab) pxPanel(x + 2, 6, TAB_W - 4, 28, P_ACCENT, P_ACCENT, false);
    int w = pxW(s, 1);
    pxText(s, x + TAB_W / 2 - w / 2, 25, i == magTab ? P_BG : P_DIM, 1);
  }
  for (int x = 0; x < 480; x += 4) gfx->fillRect(x, M_TOP - 2, 2, 2, P_GRID);  // 标题栏下的像素虚线
}

void drawUsagePx(uint32_t now) {
  gfx->fillScreen(P_BG);
  // 周期选择：和设置页同一种分段选择器（每格 58 宽，点按区域见 magpieTap）
  static const char *const pn[4] = {"今天", "7 天", "30 天", "全部"};
  pxSegDraw(8, 47, 58, 4, pn, muPeriod, 28);
  pxText("网关 + 会话日志", 470 - pxW("网关 + 会话日志", 1), 65, P_DIM, 1);

  const MuPeriod &P = muPeriods[muPeriod];
  if (!P.has) {
    String s = magpieOnline ? "读取中…" : "未连接";
    pxText(s, 240 - pxW(s, 2) / 2, 170, P_DIM, 2);
    return;
  }

  // 四个指标：各自一块切角面板
  const char *lab[4] = {"Token", "请求", "费用", "缓存命中率"};
  String val[4] = {P.tok, P.calls, "$" + P.cost.substring(P.cost.startsWith("$") ? 1 : 0), P.hit};
  String sub[4] = {P.tsub, P.csub, "生效价格", "占提示词"};
  for (int i = 0; i < 4; i++) {
    int x = 8 + i * 117;
    pxPanel(x, 82, 108, 104, P_PANEL, P_EDGE, true);
    pxText(lab[i], x + 10, 100, P_DIM, 1);
    const int sc = 2;  // 卡片数值统一 22px（同 Buddy 页、雷达页）
    uint16_t vc = i == 2 ? P_GREEN : (i == 0 ? P_ACCENT : P_TEXT);
    int vx = x + 10, vy = 130;
    if (i == 2) vx += pxApprox(vx, vy, sc, vc);
    pxText(val[i], vx, vy, vc, sc);
    // 说明按 " · " 分行
    String rest = sub[i];
    int ly = 152;
    while (rest.length() && ly <= 180) {
      int k = rest.indexOf(" · ");
      String seg = k < 0 ? rest : rest.substring(0, k);
      rest = k < 0 ? String("") : rest.substring(k + strlen(" · "));
      pxText(seg, x + 10, ly, i == 1 && P.csub != "没有失败" ? P_RED : P_DIM, 1);
      ly += 14;
    }
  }

  // 方块柱状图
  pxPanel(8, 196, 464, 72, P_PANEL, P_EDGE, true);
  const int px0 = 52, px1 = 462, py0 = 204, py1 = 252;
  pxText(P.ymax, px0 - 6 - pxW(P.ymax, 1), py0 + 8, P_DIM, 1);
  pxText("0", px0 - 6 - pxW("0", 1), py1 + 2, P_DIM, 1);
  for (int x = px0; x < px1; x += 4) {
    gfx->fillRect(x, py0, 2, 1, P_GRID);
    gfx->fillRect(x, (py0 + py1) / 2, 2, 1, P_GRID);
  }
  gfx->fillRect(px0, py1 + 1, px1 - px0, 2, P_EDGE);
  if (P.nb) {
    float step = (float)(px1 - px0) / P.nb;
    int bw = max(4, ((int)(step * 0.6)) / 2 * 2);
    for (int i = 0; i < P.nb; i++) {
      int h = (py1 - py0) * P.bars[i] / 1000;
      if (P.bars[i] > 0 && h < 3) h = 3;
      int bx = px0 + (int)(i * step + (step - bw) / 2);
      for (int yy = py1 - 3; yy >= py1 - h; yy -= 4) gfx->fillRect(bx, yy, bw, 3, P_ACCENT);  // 一格一格往上叠
    }
    for (int k = 0; k < P.nxl; k++) {
      int idx = P.nxl > 1 ? k * (P.nb - 1) / (P.nxl - 1) : 0;
      int cx = px0 + (int)(idx * step + step / 2);
      int w = pxW(P.xl[k], 1);
      pxText(P.xl[k], constrain(cx - w / 2, px0, px1 - w), 266, P_DIM, 1);
    }
  }
}

// =====================================================================
// 以下把像素风铺到所有页面
// =====================================================================

// unifont（16px 像素字体）1 倍：正文
void pxU(const String &s, int x, int y, uint16_t c) {
  gfx->setFont(u8g2_font_unifont_h_chinese4);
  gfx->setTextSize(1);
  // unifont 里没有 "·" 和 "…"：分段打印，这两个符号手画成像素点
  int from = 0;
  while (true) {
    int a = s.indexOf("·", from), b = s.indexOf("…", from);
    int k = a < 0 ? b : (b < 0 ? a : min(a, b));
    String part = s.substring(from, k < 0 ? s.length() : k);
    if (!txtDraw(1, part, x, y, c, 1)) printAt(part, x, y, c);
    int pw = txtWidth(1, part, 1);
    x += pw >= 0 ? pw : textW(part);
    if (k < 0) break;
    if (k == a) {
      gfx->fillRect(x + 3, y - 6, 2, 2, c);
      x += 8;
      from = k + strlen("·");
    } else {
      for (int d = 0; d < 3; d++) gfx->fillRect(x + 1 + d * 4, y - 2, 2, 2, c);
      x += 13;
      from = k + strlen("…");
    }
  }
}

int pxUW(const String &s) {
  int cw = txtWidth(1, s, 1);
  if (cw >= 0) return cw;
  gfx->setFont(u8g2_font_unifont_h_chinese4);
  gfx->setTextSize(1);
  return textW(s);
}

void pxDots(int x0, int x1, int y, uint16_t c) {  // 像素虚线
  for (int x = x0; x < x1; x += 4) gfx->fillRect(x, y, 2, 2, c);
}

void pxHomeBtn(int x, int y) {
  pxPanel(x, y, 30, 28, P_PANEL, P_EDGE, false);
  int cx = x + 15;
  for (int k = 0; k < 5; k++) gfx->fillRect(cx - 1 - k * 2, y + 6 + k * 2, 2 + k * 4, 2, P_DIM);  // 台阶状屋顶
  gfx->fillRect(cx - 6, y + 16, 12, 6, P_DIM);
  gfx->fillRect(cx - 2, y + 18, 4, 4, P_PANEL);
}

// 分段选择器（统一样式）：深色轨道 + 选中格紫底。h 为高度
void pxSegDraw(int x, int y, int cw, int n, const char *const *labels, int sel, int h) {
  pxPanel(x, y, cw * n, h, P_BG, P_EDGE, false);
  for (int i = 0; i < n; i++) {
    int cx = x + i * cw;
    if (i == sel) pxPanel(cx + 2, y + 2, cw - 4, h - 4, P_ACCENT, P_ACCENT, false);
    String s = labels[i];
    pxText(s, cx + cw / 2 - pxW(s, 1) / 2, y + h / 2 + 5, i == sel ? P_BG : P_TEXT, 1);
  }
}

// 分段方块进度条
void pxSegBar(int x, int y, int w, int h, float frac, uint16_t on, uint16_t off) {
  const int seg = 8, gap = 2;
  int n = (w + gap) / (seg + gap);
  int lit = (int)(n * frac + 0.5);
  if (frac > 0 && lit == 0) lit = 1;
  for (int i = 0; i < n; i++) gfx->fillRect(x + i * (seg + gap), y, seg, h, i < lit ? on : off);
}

// 方块圆环：20 个小方块围一圈，从 12 点钟方向顺时针按比例点亮
void pxRing(int cx, int cy, int left) {
  bool low = left < 20;
  const int n = 20, r = 19;
  int lit = (n * left + 50) / 100;
  if (left > 0 && lit == 0) lit = 1;
  for (int i = 0; i < n; i++) {
    float a = -PI / 2 + i * 2 * PI / n;
    int x = cx + lround(r * cos(a)) - 2, y = cy + lround(r * sin(a)) - 2;
    gfx->fillRect(x, y, 5, 5, i < lit ? (low ? 0xFC08 : P_TEXT) : P_EDGE);
  }
  String s = String(left) + "%";
  pxText(s, cx - pxW(s, 1) / 2, cy + 4, low ? 0xFC08 : P_TEXT, 1);
}

// 像素对话气泡（替代圆角气泡）：白底，四角两级台阶圆角，左下一条斜着指向角色的小尾巴
void bubblePx(int x, int y, int w, int h, uint16_t border) {
  uint16_t edge = border == C_CLAUDE ? C_CLAUDE : 0xFFFF;
  if (edge == 0xFFFF) {
    gfx->fillRect(x + 4, y, w - 8, h, 0xFFFF);
    gfx->fillRect(x + 2, y + 1, w - 4, h - 2, 0xFFFF);
    gfx->fillRect(x + 1, y + 2, w - 2, h - 4, 0xFFFF);
    gfx->fillRect(x, y + 4, w, h - 8, 0xFFFF);
  } else {
    pxPanel(x, y, w, h, 0xFFFF, edge, false);
  }
  for (int k = 0; k < 3; k++) gfx->fillRect(x + 12 - k * 3, y + h + k * 3, 9 - k * 3, 3, 0xFFFF);  // 尾巴：9 / 6 / 3 宽，往左下收尖
}

// ---------- 首页 ----------
#define P_DOT     0x18E4  // 背景点阵
#define C_ACCENT_CLAUDE C_CLAUDE

// 像素 X logo（粗的 \ 是双线描边，细的 / 穿过去），约 12×12
void pxXLogo(int x, int y, uint16_t c) {
  gfx->drawLine(x, y, x + 8, y + 11, c);
  gfx->drawLine(x + 3, y, x + 11, y + 11, c);
  gfx->drawFastHLine(x, y, 4, c);
  gfx->drawFastHLine(x + 8, y + 11, 4, c);
  gfx->drawLine(x + 11, y, x + 6, y + 5, c);
  gfx->drawLine(x + 5, y + 6, x, y + 11, c);
}

// 本地日期：days 为 1970-01-01 以来的天数（Howard Hinnant 的 civil_from_days）
void civilDate(int32_t days, int &mo, int &d) {
  days += 719468;
  int32_t era = days / 146097, doe = days - era * 146097;
  int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  mo = mp < 10 ? mp + 3 : mp - 9;
}

void drawStatusLine(const String &s, int cx, int y, uint16_t dot, uint16_t c) {
  int w = pxW(s, 1) + 10;
  int x = cx - w / 2;
  gfx->fillRect(x, y - 7, 6, 6, dot);
  pxText(s, x + 10, y, c, 1);
}

void homeCard(int x, int y, int w, int h, uint16_t accent) {
  pxPanel(x, y, w, h, P_PANEL, P_EDGE, true);
  gfx->fillRect(x + 2, y + 2, w - 4, 4, accent);  // 顶部品牌色细边
}

void drawHomePx(uint32_t now) {
  if (cfgHomeScene) {
    drawHomeScene(now);  // 桌面场景（ui_scene.ino）
    return;
  }
  gfx->fillScreen(P_BG);
  for (int y = 6; y < 272; y += 12)  // 淡淡的点阵底纹
    for (int x = 6; x < 480; x += 12) gfx->drawPixel(x, y, P_DOT);
  drawHomeTopBar(now);
  drawHomeCards(now);
}

// 顶部状态栏：X 账号 + 电池 / 齿轮 / 麦克风 / Wi-Fi / 雷达 + 日期时钟（点日期时钟切到待机时钟页）
void drawHomeTopBar(uint32_t now) {
  // 左：X 账号；右：设置按钮（齿轮 / 麦克风 / Wi-Fi 收在一个按钮里）+ 日期时间。文字统一 16px、同一颜色
  pxXLogo(14, 12, P_TEXT);
  pxU("@bruc3van", 34, 24, P_TEXT);
  int rx = 470;
  uint32_t e = epochNow();
  if (e) {
    int64_t lt = (int64_t)e + tzOff;
    int32_t days = lt / 86400, sec = lt % 86400;
    int mo, d;
    civilDate(days, mo, d);
    const char *wk[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    char hh[3], mm[3];
    snprintf(hh, 3, "%02d", (int)(sec / 3600));
    snprintf(mm, 3, "%02d", (int)(sec % 3600 / 60));
    String t = String(hh) + ((now / 500) % 2 ? ":" : " ") + mm;  // 冒号每秒闪一下
    String full = String(mo) + "/" + d + " " + wk[(days + 4) % 7] + " " + hh + ":" + mm;
    rx -= pxUW(full);
    homeDateX = rx;
    pxU(String(mo) + "/" + d + " " + wk[(days + 4) % 7] + " " + t, rx, 24, P_TEXT);
    rx -= 12;
  }
  // 状态图标：不加底框，和左边的账号、右边的时间同在一条基线上；点齿轮进设置，点麦克风 → 声音，点 Wi-Fi → Wi-Fi，点雷达 → 雷达页
  int bx = rx - HOME_PILL_W;
  homeIconX = bx;
  drawGearBadge(bx + 10, 10);
  drawMicBadge(bx + 36, 10);
  drawWifiBadge(bx + 60, 10);
  drawRadarBadge(bx + 94, 10);
  drawBatBadge(bx - 26, 12);  // 靠电池：电量；插着 USB：插头
}

// 两张卡片的首页（设置 → 显示 → 首页：卡片）
void drawHomeCards(uint32_t now) {
  const int CY = 38, CH = 226, CW = 222;
  // ---- 左：Agent Buddy ----
  int lx = 12, lcx = lx + CW / 2;
  homeCard(lx, CY, CW, CH, C_CLAUDE);
  Pose p;
  p.dy = ((now / 350) % 2) ? -2 : 0;
  p.eye = (now % 3700) < 140 ? 1 : 0;
  if (mode == M_OFFLINE) { p.color = C_GREY; p.eye = 1; }
  if (mode == M_WORKING) {
    int ph = (now / 120) % 4;
    p.legLift[0] = p.legLift[2] = (ph < 2) ? 3 : 0;
    p.legLift[1] = p.legLift[3] = (ph < 2) ? 0 : 3;
    p.armL = (ph < 2) ? 1 : 0;
    p.armR = (ph < 2) ? 0 : 1;
  }
  int oU = U, oCX = CX, oBASE = BASE;
  U = 8; CX = lcx; BASE = CY + 128;
  drawClawd(p);
  U = oU; CX = oCX; BASE = oBASE;
  if (mode == M_THINKING || mode == M_WORKING) drawSpark(lcx + 54, CY + 44, 12, sparkFrame(now), 0, C_CLAUDE);
  pxText("Agent Buddy", lcx - pxW("Agent Buddy", 2) / 2, CY + 162, P_TEXT, 2);
  bool busy = mode == M_THINKING || mode == M_WORKING || mode == M_COMPACT || mode == M_WAITING;
  uint16_t sc = mode == M_OFFLINE ? P_DIM : (busy ? C_CLAUDE : P_GREEN);
  drawStatusLine(mode == M_OFFLINE ? String("未连接") : label, lcx, CY + 184, sc, mode == M_OFFLINE ? P_DIM : P_TEXT);
  pxDots(lx + 12, lx + CW - 12, CY + 194, P_GRID);
  // 迷你数据：5 小时额度
  pxText("5h", lx + 14, CY + 214, P_DIM, 1);
  if (h5Used >= 0) {
    float left = constrain(100.0 - h5Used, 0, 100);
    uint16_t c = left > 50 ? P_GREEN : (left > 20 ? C_YELLOW : P_RED);
    String pct = String((int)(left + 0.5)) + "%";
    int pw = pxW(pct, 1);
    pxSegBar(lx + 34, CY + 205, CW - 34 - 22 - pw, 9, left / 100.0, c, P_EDGE);
    pxText(pct, lx + CW - 14 - pw, CY + 214, c, 1);
  } else {
    pxText("暂无数据", lx + CW - 14 - pxW("暂无数据", 1), CY + 214, P_DIM, 1);
  }

  // ---- 右：magpie ----
  int rx = 246, rcx = rx + CW / 2;
  homeCard(rx, CY, CW, CH, P_ACCENT);
  if (now - homeFlapAt > 4000) homeFlapAt = now;
  drawBirdPx(wag100[wagFrame(now, homeFlapAt)], WAG100_SIZE, rcx - WAG100_SIZE / 2, CY + 26, P_TEXT);
  pxText("magpie", rcx - pxW("magpie", 2) / 2, CY + 162, P_TEXT, 2);
  String s2 = mrHead;  // 状态 + 关键数字："今天 41 次 · 272K"
  s2.replace("次调用", "次");
  s2.replace(" token", "");
  if (s2.endsWith(" · 0")) s2.remove(s2.length() - strlen(" · 0"));
  if (!s2.length()) s2 = magpieOnline ? "读取中…" : "未连接";
  drawStatusLine(s2, rcx, CY + 184, magpieOnline ? P_GREEN : P_DIM, magpieOnline ? P_TEXT : P_DIM);
  pxDots(rx + 12, rx + CW - 12, CY + 194, P_GRID);
  // 迷你数据：今天每小时的用量
  const MuPeriod &T = muPeriods[0];
  pxText("今天", rx + 14, CY + 214, P_DIM, 1);
  int bx0 = rx + 46, bx1 = rx + CW - 14, base = CY + 216;
  if (T.has && T.nb) {
    float step = (float)(bx1 - bx0) / T.nb;
    int bw = max(2, (int)step - 2);
    for (int k = 0; k < T.nb; k++) {
      int h = 14 * T.bars[k] / 1000;
      int x = bx0 + (int)(k * step);
      if (T.bars[k] > 0) gfx->fillRect(x, base - max(h, 2), bw, max(h, 2), P_ACCENT);
      else gfx->fillRect(x, base - 1, bw, 1, P_EDGE);
    }
  } else {
    pxText(magpieOnline ? "读取中…" : "未连接", rx + CW - 14 - pxW(magpieOnline ? "读取中…" : "未连接", 1), CY + 214, P_DIM, 1);
  }
}

// ---------- Agent Buddy 右侧面板 ----------
void drawQuotaPx(int y, const char *name, float used, uint32_t resetAt) {
  const int x = 264, w = 200;
  pxU(name, x, y + 14, P_TEXT);
  if (used < 0) {
    pxText("暂无数据", x + w - pxW("暂无数据", 1), y + 14, P_DIM, 1);
    pxSegBar(x, y + 22, w, 12, 0, P_EDGE, P_EDGE);
    return;
  }
  float left = constrain(100.0 - used, 0, 100);
  uint16_t c = left > 50 ? P_GREEN : (left > 20 ? C_YELLOW : P_RED);
  String pct = String((int)(left + 0.5)) + "%";
  pxText(pct, x + w - pxW(pct, 2), y + 16, c, 2);
  pxSegBar(x, y + 24, w, 12, left / 100.0, c, P_EDGE);
  uint32_t nowE = epochNow();
  if (resetAt && nowE) {
    int32_t l = (int32_t)(resetAt - nowE);
    pxText(l > 0 ? fmtDuration(l) + "后重置" : String("即将重置"), x, y + 52, P_DIM, 1);
  }
}

// Buddy 页右侧：三张卡片（状态 / 5 小时额度 / 7 天额度），三个 Agent 共用。a：0 Claude / 1 Codex / 2 DeepSeek（额度卡换成余额，见 ui_deepseek.ino）
// 按像素宽度截断（cubic11），超出加 "…"
String fitPx(const String &s, int maxW) {
  if (pxW(s, 1) <= maxW) return s;
  String t = s;
  while (t.length() && pxW(t + "…", 1) > maxW) {
    int k = t.length() - 1;
    while (k > 0 && (t[k] & 0xC0) == 0x80) k--;
    t.remove(k);
  }
  return t + "…";
}

// 重置时刻：今天 → 00:16，明天 → 明天 00:16，更远 → 11月1日 14:12（同 bridge 的 when()）
String resetWhen(uint32_t at) {
  int64_t la = (int64_t)at + tzOff, ln = (int64_t)epochNow() + tzOff;
  int dd = (int)(la / 86400 - ln / 86400);
  char hm[8];
  snprintf(hm, sizeof hm, "%02d:%02d", (int)(la % 86400 / 3600), (int)(la % 3600 / 60));
  if (dd == 0) return hm;
  if (dd == 1) return String("明天 ") + hm;
  int mo, d;
  civilDate((int32_t)(la / 86400), mo, d);
  return String(mo) + "月" + String(d) + "日 " + hm;
}

void quotaCard(int y, const char *name, float used, uint32_t resetAt, const String &foot = "");
void quotaCard(int y, const char *name, float used, uint32_t resetAt, const String &foot) {
  const int x = 252, w = 222;
  pxPanel(x, y, w, 68, P_PANEL, P_EDGE, true);
  pxText(name, x + 12, y + 22, P_DIM, 1);
  if (used < 0) {
    pxText("暂无数据", x + w - 12 - pxW("暂无数据", 1), y + 22, P_DIM, 1);
    pxSegBar(x + 12, y + 32, w - 24, 10, 0, P_EDGE, P_EDGE);
    return;
  }
  float left = constrain(100.0f - used, 0.0f, 100.0f);
  uint16_t c = left > 50 ? P_GREEN : (left > 20 ? C_YELLOW : P_RED);
  String pct = String((int)(left + 0.5f)) + "%";
  pxText(pct, x + w - 12 - pxW(pct, 2), y + 26, c, 2);
  pxSegBar(x + 12, y + 34, w - 24, 10, left / 100.0f, c, P_EDGE);
  // 底行：左边重置的时刻，右边倒计时（Codex 多账号时换成账号名）
  uint32_t nowE = epochNow();
  const int fy = y + 60;
  if (resetAt && nowE && resetAt + 600 > nowE) {
    int32_t l = (int32_t)(resetAt - nowE);
    String lt = l > 0 ? resetWhen(resetAt) + " 重置" : String("正在重置");
    pxText(lt, x + 12, fy, P_DIM, 1);
    // 倒计时只取最大的单位（6 天后 / 4 小时后 / 12 分钟后），一行放得下；账号名放不下就截断加 …
    String rt = foot.length() ? foot : (l <= 0 ? String("") : l >= 86400 ? String(l / 86400) + " 天后" : l >= 3600 ? String(l / 3600) + " 小时后" : String(max(1, (int)(l / 60))) + " 分钟后");
    int rw = x + w - 12 - (x + 12 + pxW(lt, 1) + 10);
    rt = fitPx(rt, rw);
    pxText(rt, x + w - 12 - pxW(rt, 1), fy, P_DIM, 1);
  } else if (resetAt && nowE) {  // 过了重置时间 10 分钟还没收到新数据：上面的百分比是旧的
    pxText("数据已过期，等待更新", x + 12, fy, P_DIM, 1);
  } else if (foot.length()) {
    pxMarquee(foot, x + 12, fy, P_DIM, 1, w - 24);
  }
}

void drawAgentPanel(int a, uint32_t now) {
  int m = agMode(a);
  bool poke = agPokeUntil(a) > now;
  bool active = m == M_THINKING || m == M_WORKING || m == M_COMPACT;
  uint16_t acc = agColor(a);
  drawBuddySwitch();
  // 状态卡：矮一点，三张卡之间留 10px，右侧不挤；顶上让开标题栏
  const int x = 252, y = 46, w = 222, h = 54;
  pxPanel(x, y, w, h, P_PANEL, m == M_WAITING ? acc : P_EDGE, true);
  pxPanel(x + 10, y + 10, 34, 34, P_BG, P_BG, false);
  if (!a && active && !poke) drawSpark(x + 27, y + 27, 12, sparkFrame(now), now / 1600.0f, C_CLAUDE);
  else drawIcon8(agIcon8(a), x + 15, y + 15, 3, m == M_OFFLINE ? P_DIM : acc);
  String big, small;
  if (m == M_OFFLINE) {
    big = "未连接";
    small = "在电脑上启动 bridge";
  } else {
    const char *think = a == 2 ? DS_THINKING : (a ? CX_THINK[(now / 3000) % 4] : THINK_VERBS[(now / 3000) % 6]);
    big = poke ? String(a == 2 ? "噗～" : (a ? "嗨！" : "嘿！")) : (m == M_THINKING ? String(think) : agLabel(a));
    uint32_t ts = agTurnStart(a);
    small = active && ts ? "已用时 " + fmtDuration((millis() - ts) / 1000) : agDetail(a);
  }
  const int tx = x + 54, tw = w - 54 - 10;
  int sc = pxW(big, 2) <= tw ? 2 : 1;
  uint16_t bc = m == M_OFFLINE ? P_DIM : (m == M_WAITING ? agHi(a) : (active ? acc : P_TEXT));
  if (!a && active && !poke && sc == 2) {  // Claude 思考 / 干活：流光字
    gfx->setFont(u8g2_font_cubic11_h_cjk);
    gfx->setTextSize(2);
    gfx->setCursor(tx, y + 27);
    printShimmer(big, now, C_CLAUDE);
    gfx->setTextSize(1);
  } else {
    pxMarquee(big, tx, y + (sc == 2 ? 27 : 23), bc, sc, tw);
  }
  pxMarquee(small, tx, y + 46, P_DIM, 1, tw);
  if (a == 2) return;  // DeepSeek 的两张卡由 drawDeepSeekPanel 画
  // 额度卡
  String suf = cxAcctN > 1 ? (cxAcctAuto ? String(" · 自动") : String(" · 手选")) : String("");
  String acct = a && cxAcct.length() ? cxAcct + suf : String("");
  quotaCard(QC_Y1, "5 小时额度剩余", a ? cxH5Used : h5Used, a ? cxH5Reset : h5Reset, acct);
  quotaCard(QC_Y2, "7 天额度剩余", a ? cxD7Used : d7Used, a ? cxD7Reset : d7Reset);
}

void drawPanelPx(uint32_t now) { drawAgentPanel(0, now); }

// ---------- magpie：Agent ----------
void drawAgentsPx() {
  int y = M_TOP + 4 - magScroll[0];
  int h = maCount * 32 + 8;
  magContentH[0] = h + 10;
  magViewH[0] = 272 - M_TOP;
  if (!maCount) {
    String s = magpieOnline ? "读取中…" : "未连接";
    pxText(s, 240 - pxW(s, 1) / 2, 150, P_DIM, 1);
    return;
  }
  pxPanel(8, y, 464, h, P_PANEL, P_EDGE, true);
  for (int i = 0; i < maCount; i++) {
    const MaAgent &A = maAgents[i];
    int ry = y + 4 + i * 32;
    if (ry > 272 || ry + 32 < M_TOP) continue;
    if (i) pxDots(44, 460, ry, P_GRID);
    drawAgentIcon(A.icon, A.name, 18, ry + 8, 16);
    pxU(A.name, 44, ry + 22, A.hidden ? P_DIM : P_TEXT);
    String m = A.hidden ? String("已隐藏") : A.model;
    pxText(m, 460 - pxW(m, 1), ry + 21, P_DIM, 1);
  }
}

// ---------- magpie：额度 ----------
void pxRefresh(int x, int y) {  // 像素 ↻：开口圆圈 + 右上角箭头（8×8，(x, y) 为中心）
  static const char *ICON[8] = {
    "..XXXX.X",
    ".X....XX",
    "X....XXX",
    "X.......",
    "X.......",
    "X......X",
    ".X....X.",
    "..XXXX..",
  };
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if (ICON[r][c] == 'X') gfx->drawPixel(x - 4 + c, y - 4 + r, P_DIM);
}

void drawQuotaTabPx() {
  int y = M_TOP + 4 - magScroll[1], y0 = y;
  magViewH[1] = 272 - M_TOP;
  if (!mqCount) {
    String s = magpieOnline ? "读取中…" : "未连接";
    pxText(s, 240 - pxW(s, 1) / 2, 150, P_DIM, 1);
    magContentH[1] = 0;
    return;
  }
  for (int g = 0; g < mqCount; g++) {
    const MqGroup &G = mqGroups[g];
    drawAgentIcon(G.icon, G.name, 12, y + 6, 16);
    String nm = G.name;
    nm.toUpperCase();
    pxText(nm, 36, y + 20, P_DIM, 1);
    int x = 36 + pxW(nm, 1) + 8;
    if (G.nc > 1) {
      pxPanel(x, y + 7, 18, 16, P_EDGE, P_EDGE, false);
      pxText(String(G.nc), x + 9 - pxW(String(G.nc), 1) / 2, y + 19, P_TEXT, 1);
    }
    int cw = pxW("剩余", 1) + 12;
    pxPanel(470 - cw, y + 6, cw, 18, P_EDGE, P_EDGE, false);
    pxText("剩余", 476 - cw, y + 19, P_TEXT, 1);
    if (G.plan.length()) pxText(G.plan, 462 - cw - pxW(G.plan, 1), y + 19, P_DIM, 1);
    y += 30;
    for (int c = 0; c < G.nc; c++) {
      const MqCard &C = G.cards[c];
      if (y < 272 && y + 76 > M_TOP) {
        pxPanel(8, y, 464, 72, P_PANEL, P_EDGE, true);
        pxU(C.user, 22, y + 26, P_TEXT);
        pxRefresh(26, y + 47);
        pxText(C.reset, 36, y + 52, P_DIM, 1);
        for (int i = 0; i < C.nw; i++) {
          int cx = 436 - (C.nw - 1 - i) * 58;
          pxRing(cx, y + 30, C.w[i].left);
          pxText(C.w[i].label, cx - pxW(C.w[i].label, 1) / 2, y + 66, P_DIM, 1);
        }
      }
      y += 80;
    }
    y += 4;
  }
  magContentH[1] = y - y0 + 8;
}

// ---------- magpie：路由 ----------
// 像素风连线：直角折线（像电路走线），光点是方块数据包
void pxPath(bool left, int i, int pts[4][2]) {
  if (left) {
    int x1 = agX1[i] + 2, y1 = agY[i], x2 = HUB_X - HUB_R - 2, mx = (x1 + x2) / 2 & ~1;
    int p[4][2] = {{x1, y1}, {mx, y1}, {mx, ST_CY}, {x2, ST_CY}};
    memcpy(pts, p, sizeof(p));
  } else {
    int x1 = HUB_X + HUB_R + 2, x2 = dsX0[i] - 2, y2 = dsY[i], mx = (x1 + x2) / 2 & ~1;
    int p[4][2] = {{x1, ST_CY}, {mx, ST_CY}, {mx, y2}, {x2, y2}};
    memcpy(pts, p, sizeof(p));
  }
}

void pxPathPos(bool left, int i, float t, int &x, int &y) {
  int p[4][2];
  pxPath(left, i, p);
  float len[3], total = 0;
  for (int k = 0; k < 3; k++) total += len[k] = abs(p[k + 1][0] - p[k][0]) + abs(p[k + 1][1] - p[k][1]);
  float d = t * total;
  for (int k = 0; k < 3; k++) {
    if (d <= len[k] || k == 2) {
      float f = len[k] ? min(1.0f, d / len[k]) : 0;
      x = p[k][0] + (p[k + 1][0] - p[k][0]) * f;
      y = p[k][1] + (p[k + 1][1] - p[k][1]) * f;
      return;
    }
    d -= len[k];
  }
}

void pxWire(bool left, int i, uint16_t c) {
  int p[4][2];
  pxPath(left, i, p);
  for (int k = 0; k < 3; k++) {
    int x0 = min(p[k][0], p[k + 1][0]), x1 = max(p[k][0], p[k + 1][0]);
    int y0 = min(p[k][1], p[k + 1][1]), y1 = max(p[k][1], p[k + 1][1]);
    if (y0 == y1) for (int x = x0; x <= x1; x += 4) gfx->fillRect(x, y0 - 1, 2, 2, c);
    else for (int y = y0; y <= y1; y += 4) gfx->fillRect(x0 - 1, y, 2, 2, c);
  }
}

void drawStagePx(uint32_t now) {
  pxPanel(8, ST_Y, 464, ST_H, P_PANEL, P_EDGE, true);
  // 收起时两边各只画第一个节点，其余节点重叠在它的位置上（飞行动画照常）
  bool cp = stageCompact();
  int nA = cp ? min(msAgN, 1) : msAgN, nD = cp ? min(msDstN, 1) : msDstN;
  for (int i = 0; i < msAgN; i++) { agY[i] = nodeY(i, msAgN); agX1[i] = HUB_X - HUB_R - 40; }
  for (int i = 0; i < msDstN; i++) { dsY[i] = nodeY(i, msDstN); dsX0[i] = HUB_X + HUB_R + 40; }

  TripPose poses[4];
  bool litA[3] = {false, false, false}, litD[3] = {false, false, false}, waitD[3] = {false, false, false};
  uint16_t litC[3] = {0, 0, 0};
  bool busy = false;
  for (int k = 0; k < 4; k++) {
    poses[k] = trips[k].on && trips[k].a < msAgN && trips[k].d < msDstN ? tripPose(trips[k], now) : TripPose{false};
    if (trips[k].on && !poses[k].vis && (int32_t)(now - trips[k].t0) > 0) trips[k].on = false;
    if (!poses[k].vis) continue;
    busy = true;
    int la = cp ? 0 : poses[k].litA, ld = cp ? 0 : poses[k].litD;
    if (poses[k].litA >= 0) litA[la] = true;
    if (poses[k].litD >= 0) { litD[ld] = true; litC[ld] = msAgs[trips[k].a].color; }
    if (poses[k].waiting) waitD[cp ? 0 : trips[k].d] = true;
  }
  for (int i = 0; i < nA; i++) pxWire(true, i, litA[i] ? msAgs[i].color : P_EDGE);
  for (int i = 0; i < nD; i++) pxWire(false, i, litD[i] ? litC[i] : P_EDGE);

  for (int i = 0; i < nA; i++) {
    const MsNode &N = msAgs[i];
    int w = pxUW(N.name) + 34, x = agX1[i] - w;
    pxPanel(x, agY[i] - 11, w, 22, P_BG, N.color, false);
    drawAgentIcon(N.icon, N.name, x + 7, agY[i] - 7, 14);
    pxU(N.name, x + 26, agY[i] + 6, P_TEXT);
  }
  for (int i = 0; i < nD; i++) {
    const MsNode &N = msDst[i];
    int x = dsX0[i];
    int nw = pxUW(N.name), sw = N.sub.length() ? pxW(N.sub, 1) + 6 : 0;
    int w = min(22 + nw + sw + 8, (cp ? 444 : 466) - x);  // 收起时给右侧的 ▾ 留位置
    pxPanel(x, dsY[i] - 11, w, 22, P_BG, P_EDGE, false);
    uint16_t dc = N.st == 2 ? P_RED : P_GREEN;
    if (waitD[i]) dc = (now / 250) % 2 ? P_DIM : P_EDGE;
    gfx->fillRect(x + 8, dsY[i] - 3, 6, 6, dc);
    pxU(N.name, x + 20, dsY[i] + 6, P_TEXT);
    if (sw) pxText(N.sub, x + 24 + nw, dsY[i] + 5, P_DIM, 1);
  }

  // 中枢：浅色方块 + 黑色像素喜鹊；忙时方形框往外扩
  if (busy) {
    int k = (now / 150) % 4;
    if (k < 3) {
      int r = HUB_R + 2 + k * 3;
      gfx->drawRect(HUB_X - r, ST_CY - r, 2 * r, 2 * r, k == 0 ? P_DIM : P_EDGE);
    }
  }
  pxPanel(HUB_X - HUB_R, ST_CY - HUB_R, 2 * HUB_R, 2 * HUB_R, P_TEXT, P_TEXT, false);
  drawBirdPx(wag28[0], WAG28_SIZE, HUB_X - 14, ST_CY - 15, P_BG);

  if (cp) for (int k = 0; k < 4; k++) gfx->fillRect(452 + k, ST_CY - 2 + k, 8 - 2 * k, 1, P_DIM);  // ▾ 可展开

  // 数据包：6px 方块 + 两个越来越小的拖尾
  for (int k = 0; k < 4; k++) {
    const TripPose &P = poses[k];
    if (!P.vis) continue;
    if (P.waiting && (now / 250) % 2) continue;
    gfx->fillRect(P.x - 3, P.y - 3, 6, 6, P.c);
  }
  if (!msAgN) {
    String s = magpieOnline ? "今天暂无经过 magpie 的请求" : "未连接";
    pxText(s, HUB_X - pxW(s, 1) / 2, ST_Y + ST_H - 8, P_DIM, 1);
  }
}

// ---------- 路由列表缓存：内容变了才重画成一张长图，滑动时只按行复制可见部分（文字渲染很慢，复制很快）----------
Arduino_Canvas *listCv = nullptr;
const int LISTCV_H = 10 * 50 + 16;

void drawRouteRowsPx(int y) {
  if (mrCount) pxPanel(8, y, 464, mrCount * 50 + 6, P_PANEL, P_EDGE, true);
  for (int i = 0; i < mrCount; i++) {
    const MrRow &R = mrRows[i];
    int ry = y + 3 + i * 50;
    if (i) pxDots(16, 464, ry, P_GRID);
    drawAgentIcon(R.icon, R.agent, 16, ry + 8, 14);
    pxU(R.agent, 36, ry + 21, P_TEXT);
    int x = 36 + pxUW(R.agent) + 8;
    int mw = pxW(R.model, 1) + 10;
    pxPanel(x, ry + 7, mw, 18, P_BG, P_BG, false);
    pxText(R.model, x + 5, ry + 20, P_TEXT, 1);
    x += mw + 6;
    if (R.kind.length()) {
      int kw = pxW(R.kind, 1) + 10;
      pxPanel(x, ry + 7, kw, 18, P_PANEL, P_EDGE, false);
      pxText(R.kind, x + 5, ry + 20, P_DIM, 1);
    }
    pxText(R.tm, 464 - pxW(R.tm, 1), ry + 20, P_DIM, 1);
    pxText("→", 20, ry + 41, R.err ? P_RED : P_EDGE, 1);
    pxText(R.served, 36, ry + 41, R.err ? P_RED : P_DIM, 1);
    pxText(R.info, 464 - pxW(R.info, 1), ry + 41, P_DIM, 1);
  }
}

void routeListBlit(int listTop) {
  if (!listCv) {
    listCv = new FastCanvas(480, LISTCV_H, nullptr);
    if (!listCv->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      delete listCv;
      listCv = nullptr;
    }
    if (listCv) listCv->setUTF8Print(true);  // 和主画布一样按 UTF-8 解码中文
  }
  if (!listCv) {  // 内存不够时退回直接画
    drawRouteRowsPx(listTop - magScroll[3]);
    return;
  }
  if (mrListDirty) {
    Arduino_Canvas *screenCv = gfx;
    gfx = listCv;  // 借用全部绘图函数，画到缓存图上
    gfx->fillScreen(P_BG);
    drawRouteRowsPx(0);
    gfx = screenCv;
    mrListDirty = false;
  }
  int h = mrCount ? mrCount * 50 + 6 + 3 : 0;  // 含右下阴影
  uint16_t *dst = gfx->getFramebuffer(), *src = listCv->getFramebuffer();
  for (int y = listTop; y < 272; y++) {
    int sy = y - listTop + magScroll[3];
    if (sy >= 0 && sy < h) memcpy(dst + y * 480, src + sy * 480, 480 * 2);
  }
}

void drawRouteTabPx(uint32_t now) {
  updateStageGeom();
  const int LIST_TOP = ST_Y + ST_H + 6;  // 舞台收起后列表跟着上移（166 → 112）
  magViewH[3] = 272 - LIST_TOP;
  magContentH[3] = mrCount * 50 + 10;
  gfx->fillScreen(P_BG);
  uint32_t t0 = micros();
  routeListBlit(LIST_TOP);
  // 没有请求时，空状态只在上方舞台里说一次，列表区不再重复

  if (perfOn) perfAcc[1] += micros() - t0;
  t0 = micros();
  gfx->fillRect(0, M_TOP, 480, LIST_TOP - M_TOP, P_BG);
  if ((now / 600) % 2 || !magpieOnline) gfx->fillRect(12, 50, 8, 8, magpieOnline ? P_GREEN : P_DIM);
  gfx->setFont(u8g2_font_unifont_h_chinese4);
  gfx->setTextSize(1);
  printDots(mrHead.length() ? mrHead : String("未连接"), 28, 61, P_TEXT);  // unifont 没有 "·"，手画
  drawStagePx(now);
  if (perfOn) perfAcc[2] += micros() - t0;
}

void drawMagpiePx(uint32_t now) {
  if (magTab == 2) {
    drawUsagePx(now);
  } else {
    gfx->fillScreen(P_BG);
    if (magTab == 0) drawAgentsPx();
    else if (magTab == 1) drawQuotaTabPx();
    else drawRouteTabPx(now);
  }
  drawMagHeaderPx(now);
}
