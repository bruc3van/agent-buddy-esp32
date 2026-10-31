// Wi-Fi：首页右上角的信号图标、设置页的 Wi-Fi 分区（扫描 / 连接）、像素风密码键盘
// 扫描用异步方式（不阻塞界面）；连接成功后把网络名和密码存进 NVS，开机自动重连

struct WNet {
  String ssid;
  int rssi;
  bool lock;
};
WNet wnets[24];
int wnN = 0;
bool wScanning = false;
String wSavedSsid, wSavedPass;
String wPendSsid, wPendPass;
bool wConnecting = false;
uint32_t wConnStart = 0;
String wMsg = "";

// 键盘
String kbSsid, kbText;
bool kbShift = false, kbSym = false, kbShow = false;
int kbFlashKey = -1;
uint32_t kbFlashAt = 0;

int rssiBars(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

// 像素 Wi-Fi 图标（12×8，点 + 三道弧）；每格数字表示属于第几道弧（0 是中心点）
const char *WIFI_ICON[8] = {
  "..33333333..",
  ".3........3.",
  "3..222222..3",
  "..2......2..",
  ".2..1111..2.",
  "...1....1...",
  ".....00.....",
  ".....00.....",
};

int arcsOf(int bars) { return bars >= 4 ? 3 : bars >= 3 ? 2 : bars >= 1 ? 1 : 0; }

// (x, y) 为左上角，sc 为放大倍数；arcs 为点亮的弧数（0~3），arcs<0 表示未连接（灰色 + 斜杠）
void pxWifiIcon(int x, int y, int arcs, uint16_t on, int sc = 1);
void pxWifiIcon(int x, int y, int arcs, uint16_t on, int sc) {
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 12; c++) {
      char ch = WIFI_ICON[r][c];
      if (ch == '.') continue;
      int lv = ch - '0';
      uint16_t col = arcs < 0 ? on : (lv <= arcs ? on : P_EDGE);  // 未连接：弧线用 on 色，再叠一道红斜杠
      gfx->fillRect(x + c * sc, y + r * sc, sc, sc, col);
    }
  if (arcs < 0)
    for (int k = 0; k < 10; k++) gfx->fillRect(x + (1 + k) * sc, y + (k * 8 / 10) * sc, sc, sc, P_RED);  // 斜杠
}

String signalWord(int rssi) {
  int b = rssiBars(rssi);
  return b >= 4 ? "极好" : b == 3 ? "良好" : b == 2 ? "一般" : "较弱";
}

uint16_t signalColor(int rssi) {
  int b = rssiBars(rssi);
  return b >= 3 ? P_GREEN : b == 2 ? C_YELLOW : P_RED;
}

void pxLock(int x, int y, uint16_t c) {  // 8×9 像素小锁
  gfx->drawRect(x + 2, y, 4, 4, c);
  gfx->fillRect(x, y + 4, 8, 5, c);
}

// 按像素宽度截断（unifont），超出加 "…"
String fitU(const String &s, int maxW) {
  if (pxUW(s) <= maxW) return s;
  String t = s;
  while (t.length() && pxUW(t + "…") > maxW) {
    int k = t.length() - 1;
    while (k > 0 && (t[k] & 0xC0) == 0x80) k--;
    t.remove(k);
  }
  return t + "…";
}

// ---------- 连接管理 ----------
void wifiInit() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wSavedSsid = prefs.getString("wssid", "");
  wSavedPass = prefs.getString("wpass", "");
  if (wSavedSsid.length()) WiFi.begin(wSavedSsid.c_str(), wSavedPass.length() ? wSavedPass.c_str() : nullptr);
}

bool wScanPaused = false;  // 为了扫描停掉了对已保存网络的后台重连，扫完再接上

void wifiStartScan() {
  if (wScanning) return;
  // 没连上时板子在后台反复重连已保存的网络（例如换了地方，那个网络不在了），这期间扫描会被拒绝：先停掉重连
  if (!wifiUp() && !wConnecting) {
    WiFi.disconnect();
    wScanPaused = wSavedSsid.length() > 0;
  }
  WiFi.scanDelete();
  if (WiFi.scanNetworks(true, false) == WIFI_SCAN_FAILED) {
    if (wScanPaused) WiFi.begin(wSavedSsid.c_str(), wSavedPass.length() ? wSavedPass.c_str() : nullptr);
    wScanPaused = false;
    return;
  }
  wScanning = true;
}

void wifiConnect(const String &ssid, const String &pass) {
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
  wPendSsid = ssid;
  wPendPass = pass;
  wConnecting = true;
  wConnStart = millis();
  wMsg = "正在连接 " + ssid + "…";
}

void wifiForget() {
  WiFi.disconnect(false, true);
  prefs.remove("wssid");
  prefs.remove("wpass");
  wSavedSsid = wSavedPass = "";
  wMsg = "已断开并忘记";
}

void wifiTick() {
  static bool ntpStarted = false;  // 连上 Wi-Fi 后开始网络对时（之后系统自己定时校准）
  if (!ntpStarted && WiFi.status() == WL_CONNECTED) {
    ntpStarted = true;
    configTime(0, 0, "ntp.aliyun.com", "time.apple.com", "pool.ntp.org");
  }
  if (wScanning) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      wnN = 0;
      for (int i = 0; i < n && wnN < 24; i++) {
        String s = WiFi.SSID(i);
        if (!s.length()) continue;  // 隐藏网络
        bool dup = false;
        for (int k = 0; k < wnN; k++)
          if (wnets[k].ssid == s) {  // 同名多个 AP：保留信号最强的
            dup = true;
            if (WiFi.RSSI(i) > wnets[k].rssi) wnets[k].rssi = WiFi.RSSI(i);
          }
        if (dup) continue;
        wnets[wnN++] = {s, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN};
      }
      for (int i = 0; i < wnN; i++)  // 按信号强弱排序
        for (int j = i + 1; j < wnN; j++)
          if (wnets[j].rssi > wnets[i].rssi) { WNet t = wnets[i]; wnets[i] = wnets[j]; wnets[j] = t; }
      WiFi.scanDelete();
      wScanning = false;
    } else if (n == WIFI_SCAN_FAILED) {
      wScanning = false;
    }
    if (!wScanning && wScanPaused) {  // 扫完了：已保存的网络在附近就接着连
      wScanPaused = false;
      for (int i = 0; i < wnN; i++)
        if (wnets[i].ssid == wSavedSsid && !wifiUp() && !wConnecting) {
          WiFi.begin(wSavedSsid.c_str(), wSavedPass.length() ? wSavedPass.c_str() : nullptr);
          break;
        }
    }
  }
  if (wConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wConnecting = false;
      wSavedSsid = wPendSsid;
      wSavedPass = wPendPass;
      prefs.putString("wssid", wSavedSsid);
      prefs.putString("wpass", wSavedPass);
      wMsg = "";
    } else if (millis() - wConnStart > 15000) {
      wConnecting = false;
      WiFi.disconnect();
      wMsg = "连接失败：密码错误或信号太弱";
    }
  }
}

bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

// 首页右上角用
// 首页右上角：放大 2 倍（24×16），(x, y) 为左上角
void drawWifiBadge(int x, int y) {
  if (wifiUp()) pxWifiIcon(x, y, arcsOf(rssiBars(WiFi.RSSI())), P_TEXT, 2);
  else if (wConnecting) pxWifiIcon(x, y, (millis() / 250) % 4, P_TEXT, 2);  // 连接中：弧线依次点亮
  else pxWifiIcon(x, y, -1, P_DIM, 2);
}

// ---------- 设置页的 Wi-Fi 分区 ----------
// 小标签（同额度页的「剩余」）
int pxChip(const String &t, int x, int y, uint16_t bg, uint16_t fg) {
  int w = pxW(t, 1) + 12;
  pxPanel(x, y, w, 18, bg, bg, false);
  pxText(t, x + 6, y + 13, fg, 1);
  return w;
}

const int W_ROW = 36;

// 标题栏右侧的「扫描」按钮
void drawWifiHeaderBtn() {
  String sb = wScanning ? "扫描中" : "扫描";
  int bw = 72, bx = SET_PX + SET_PW - bw;
  pxPanel(bx, 7, bw, 28, wScanning ? P_PANEL : P_ACCENT, wScanning ? P_EDGE : P_ACCENT, false);
  pxText(sb, bx + bw / 2 - pxW(sb, 1) / 2, 26, wScanning ? P_DIM : P_BG, 1);
  setHit(bx, 0, bw, SET_TOP, H_WSCAN);
}

// 从 y 开始画（已减去滚动量），返回内容高度
int drawWifiPane(int y, uint32_t now) {
  const int x0 = SET_PX, w = SET_PW, y0 = y;
  String cur = wifiUp() ? WiFi.SSID() : String("");

  // 当前连接
  pxPanel(x0, y, w, 52, P_PANEL, P_EDGE, true);
  if (wifiUp()) {
    int rs = WiFi.RSSI();
    pxWifiIcon(x0 + 12, y + 10, arcsOf(rssiBars(rs)), P_GREEN, 3);  // 放大 3 倍的图标
    pxU(fitU(WiFi.SSID(), w - 140), x0 + 58, y + 22, P_TEXT);
    pxText(signalWord(rs) + " · " + rs + " dBm · " + WiFi.localIP().toString(), x0 + 58, y + 42, P_DIM, 1);
    pxPanel(x0 + w - 70, y + 12, 58, 28, P_PANEL, P_EDGE, false);
    pxText("断开", x0 + w - 41 - pxW("断开", 1) / 2, y + 31, P_RED, 1);
    setHit(x0 + w - 76, y + 4, 76, 44, H_WFORGET);
  } else {
    pxWifiIcon(x0 + 12, y + 10, wConnecting ? (now / 250) % 4 : -1, P_DIM, 3);
    bool fail = wMsg.startsWith("连接失败");
    pxU(wConnecting ? String("正在连接") : (fail ? String("连接失败") : String("未连接")), x0 + 58, y + 22, fail ? P_RED : P_TEXT);
    String sub = wConnecting ? fitU(wPendSsid, w - 80) + "…" : (fail ? String("密码错误或信号太弱，再试一次") : String("从下方选择一个网络"));
    pxText(sub, x0 + 58, y + 42, P_DIM, 1);
  }
  y += 60;

  // 附近的网络
  String head = "附近的网络";
  pxText(head, x0 + 4, y + 13, P_DIM, 1);
  if (wnN) {
    String n = String(wnN);
    int hx = x0 + 4 + pxW(head, 1) + 6;
    pxPanel(hx, y + 1, pxW(n, 1) + 10, 16, P_EDGE, P_EDGE, false);
    pxText(n, hx + 5, y + 13, P_TEXT, 1);
  }
  y += 20;
  if (!wnN) {
    String s = wScanning ? "正在扫描附近的网络…" : "暂无网络 · 点右上角「扫描」";
    pxText(s, x0 + w / 2 - pxW(s, 1) / 2, y + 40, P_DIM, 1);
    return y + 60 - y0;
  }
  pxPanel(x0, y, w, wnN * W_ROW + 6, P_PANEL, P_EDGE, true);
  for (int i = 0; i < wnN; i++) {
    const WNet &N = wnets[i];
    int ry = y + 3 + i * W_ROW;
    if (ry > 272 || ry + W_ROW < SET_TOP) continue;
    if (i) pxDots(x0 + 8, x0 + w - 8, ry, P_GRID);
    bool isCur = N.ssid == cur;
    if (isCur) gfx->fillRect(x0 + 2, ry + 4, 3, W_ROW - 8, P_GREEN);  // 当前连接：左侧绿条
    pxWifiIcon(x0 + 12, ry + 14, arcsOf(rssiBars(N.rssi)), isCur ? P_GREEN : P_TEXT, 1);
    // 右侧：信号评级 + dBm + 锁
    String r = String(N.rssi);
    int rw = pxW(r, 1);
    pxText(r, x0 + w - 10 - rw, ry + 23, P_DIM, 1);
    String sw = signalWord(N.rssi);
    int sx = x0 + w - 10 - rw - 6 - pxW(sw, 1);
    pxText(sw, sx, ry + 23, signalColor(N.rssi), 1);
    int lockX = sx - 14;
    if (N.lock) pxLock(lockX, ry + 13, P_DIM);
    // 左侧：网络名 + 标签
    String tag = isCur ? String("已连接") : (N.ssid == wSavedSsid ? String("已保存") : String(""));
    int tagW = tag.length() ? pxW(tag, 1) + 20 : 0;
    String name = fitU(N.ssid, lockX - (x0 + 32) - tagW - 4);
    pxU(name, x0 + 32, ry + 24, isCur ? P_GREEN : P_TEXT);
    if (tagW) pxChip(tag, x0 + 32 + pxUW(name) + 8, ry + 9, isCur ? 0x1A47 : P_EDGE, isCur ? P_GREEN : P_TEXT);
    setHit(x0, ry, w, W_ROW, H_WNET + i);
  }
  return y + wnN * W_ROW + 12 - y0;
}

void wifiHit(int id) {
  if (id == H_WSCAN) wifiStartScan();
  else if (id == H_WFORGET) wifiForget();  // 「断开」：断开并忘记，不再自动重连
  if (id < H_WNET || id >= H_WNET + wnN) return;
  const WNet &N = wnets[id - H_WNET];
  if (wifiUp() && N.ssid == WiFi.SSID()) return;
  if (!N.lock) {
    wifiConnect(N.ssid, "");
  } else if (N.ssid == wSavedSsid && wSavedPass.length()) {
    wifiConnect(N.ssid, wSavedPass);  // 已保存过密码：直接连
  } else {
    kbSsid = N.ssid;
    kbText = "";
    kbShift = kbSym = kbShow = false;
    setScreen(SCR_KEYB);
  }
}

// ---------- 密码键盘 ----------
// 5 行，每行 10 个单位宽；特殊键用单位宽度表示
const char *KB_L[3] = {"qwertyuiop", "asdfghjkl.", "zxcvbnm"};
const char *KB_S[3] = {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",/?"};
const int KB_Y = 96, KB_H = 32, KB_GAP = 3, KB_KW = 46;

// 第 r 行第 i 个键的位置与内容；返回该行键数。label 为显示文字，code 为输入字符（0 表示特殊键）
int kbRow(int r, int i, int &x, int &w, String &label, char &code) {
  int y0 = 0;
  (void)y0;
  code = 0;
  if (r == 0) {
    x = 8 + i * (KB_KW + 1);
    w = KB_KW - 2;
    code = "1234567890"[i];
    label = String(code);
    return 10;
  }
  if (r == 1 || r == 2) {
    const char *row = kbSym ? KB_S[r - 1] : KB_L[r - 1];
    x = 8 + i * (KB_KW + 1);
    w = KB_KW - 2;
    code = row[i];
    if (!kbSym && kbShift && code >= 'a' && code <= 'z') code -= 32;
    label = String(code);
    return 10;
  }
  if (r == 3) {  // ⇧ + 7 键 + ⌫
    const char *row = kbSym ? KB_S[2] : KB_L[2];
    if (i == 0) { x = 8; w = KB_KW + KB_KW / 2 - 2; label = kbSym ? "" : (kbShift ? "大写" : "小写"); return 9; }
    if (i == 8) { x = 8 + KB_KW + KB_KW / 2 + 7 * (KB_KW + 1); w = 472 - x; label = "退格"; return 9; }
    x = 8 + KB_KW + KB_KW / 2 + (i - 1) * (KB_KW + 1);
    w = KB_KW - 2;
    code = row[i - 1];
    if (!kbSym && kbShift && code >= 'a' && code <= 'z') code -= 32;
    label = String(code);
    return 9;
  }
  // r == 4：符号 / 空格 / 显示 / 连接
  const int xs[4] = {8, 8 + 2 * (KB_KW + 1), 8 + 6 * (KB_KW + 1), 8 + 8 * (KB_KW + 1)};
  const int ws[4] = {2 * (KB_KW + 1) - 3, 4 * (KB_KW + 1) - 3, 2 * (KB_KW + 1) - 3, 472 - (8 + 8 * (KB_KW + 1))};
  const char *lb[4] = {kbSym ? "ABC" : "#+=", "空格", kbShow ? "隐藏" : "显示", "连接"};
  x = xs[i];
  w = ws[i];
  label = lb[i];
  if (i == 1) code = ' ';
  return 4;
}

void drawKeyboardPx(uint32_t now) {
  gfx->fillScreen(P_BG);
  pxPanel(6, 6, 30, 28, P_PANEL, P_EDGE, false);  // ‹ 返回 Wi-Fi 页
  for (int k = 0; k < 2; k++) {
    gfx->drawLine(17 + k, 20, 23 + k, 14, P_DIM);
    gfx->drawLine(17 + k, 20, 23 + k, 26, P_DIM);
  }
  pxLock(46, 13, P_DIM);
  pxU(fitU(kbSsid, 300), 62, 26, P_TEXT);
  pxText("输入密码", 472 - pxW("输入密码", 1), 25, P_DIM, 1);
  for (int x = 0; x < 480; x += 4) gfx->fillRect(x, M_TOP - 2, 2, 2, P_GRID);

  // 输入框
  pxPanel(8, 48, 464, 40, P_PANEL, P_ACCENT, false);
  String shown = "";
  for (unsigned i = 0; i < kbText.length(); i++) shown += kbShow ? String(kbText[i]) : String("*");
  while (shown.length() && pxUW(shown) > 430) shown.remove(0, 1);  // 太长只显示末尾
  if (!kbText.length()) pxU("输入密码", 20, 74, P_DIM);
  else pxU(shown, 20, 74, P_TEXT);
  if ((now / 500) % 2) gfx->fillRect(22 + (kbText.length() ? pxUW(shown) : 0), 58, 2, 20, P_ACCENT);  // 光标

  for (int r = 0; r < 5; r++) {
    int n = r == 3 ? 9 : (r == 4 ? 4 : 10);
    for (int i = 0; i < n; i++) {
      int x, w;
      String label;
      char code;
      kbRow(r, i, x, w, label, code);
      int y = KB_Y + r * (KB_H + KB_GAP);
      bool flash = kbFlashKey == r * 16 + i && now - kbFlashAt < 120;
      bool special = r == 4 || (r == 3 && (i == 0 || i == 8));
      bool accent = r == 4 && i == 3;
      uint16_t bg = flash ? P_TEXT : (accent ? P_ACCENT : (special ? P_EDGE : P_PANEL));
      pxPanel(x, y, w, KB_H, bg, flash ? P_TEXT : (accent ? P_ACCENT : P_EDGE), false);
      if (label.length()) {
        uint16_t fc = flash || accent ? P_BG : P_TEXT;
        if (code && code != ' ') pxU(label, x + w / 2 - pxUW(label) / 2, y + 22, fc);
        else pxText(label, x + w / 2 - pxW(label, 1) / 2, y + 21, fc, 1);
      }
    }
  }
}

void keyboardTap(int x, int y, uint32_t now) {
  if (y < 40) {
    if (x < 44) setScreen(SCR_SET);
    return;
  }
  int r = (y - KB_Y) / (KB_H + KB_GAP);
  if (y < KB_Y || r > 4) return;
  int n = r == 3 ? 9 : (r == 4 ? 4 : 10);
  for (int i = 0; i < n; i++) {
    int kx, kw;
    String label;
    char code;
    kbRow(r, i, kx, kw, label, code);
    if (x < kx || x >= kx + kw + 3) continue;
    kbFlashKey = r * 16 + i;
    kbFlashAt = now;
    if (code) {
      if (kbText.length() < 63) kbText += code;
      if (kbShift && r != 0) kbShift = false;  // 大写只作用一次
    } else if (r == 3 && i == 0) {
      if (!kbSym) kbShift = !kbShift;
    } else if (r == 3 && i == 8) {
      if (kbText.length()) kbText.remove(kbText.length() - 1);
    } else if (r == 4 && i == 0) {
      kbSym = !kbSym;
    } else if (r == 4 && i == 2) {
      kbShow = !kbShow;
    } else if (r == 4 && i == 3) {
      wifiConnect(kbSsid, kbText);
      setScreen(SCR_SET);
    }
    return;
  }
}
