// 设置页：参考 macOS「系统设置」——左侧分区栏，右侧分组列表（可上下滑动）
// 分区：声音 / Wi-Fi / 电脑 / 显示 / 雷达 / 关于。设置存 NVS；声音相关的经串口 "CFG {...}" 同步给 bridge
// 点击用「命中表」：每帧画控件时登记位置，点按时倒序查找（后画的在上层）

const int SET_SB = 128;              // 左侧分区栏宽度
const int SET_PX = 140, SET_PW = 332;  // 右侧内容区 x 与宽度
const int SET_TOP = 42;              // 内容区起点（上面是标题栏）
const int SET_ROW = 34;              // 普通行高

// 命中 id
enum {
  H_NONE, H_HOME, H_SEC0,  // H_SEC0..+3：分区
  H_VOL = 10, H_MUTE, H_SFX, H_VOICE, H_DSEC0, H_MREACT = 20, H_MMUTE, H_GREET, H_RSEAT, H_PRIV, H_ROPEN,
  H_SLEEP0 = 26,  // 26~29：熄屏时长
  H_BRIGHT = 30, H_NIGHT = 32, H_SCENE0 = 34, H_CLOCK = 36, H_MICPAUSE, H_SEATCAL, H_SEATCLR,  // id 都要小于 H_WSCAN
  H_WSCAN = 40, H_WFORGET, H_WNET = 50,  // H_WNET + i：第 i 个网络
  H_PCADD = 100, H_PCALLOW, H_PCDENY, H_PCPREF, H_PCDEL, H_PC0 = 110,  // 电脑：H_PC0 + i 是第 i 台
};

struct SetHit { int16_t x, y, w, h; uint8_t id; };
SetHit setHits[48];
int setHitN = 0;
int setScroll[6] = {0}, setContentH[6] = {0};
int setSlider = 0;  // 正在拖的滑块 id
uint32_t cfgSentAt = 0;
bool cfgDirty = true;

const int DSEC_OPT[4] = {20, 60, 180, 0};
const char *DSEC_LABEL[4] = {"20 秒", "1 分", "3 分", "不播"};

// 内容区的控件滚到标题栏下面就不登记，免得点标题栏误触
void setHit(int x, int y, int w, int h, int id) {
  bool chrome = id < H_VOL || id == H_WSCAN;
  if (setHitN >= 48 || (!chrome && (y + h <= SET_TOP || y >= 272))) return;
  if (!chrome && y < SET_TOP) { h -= SET_TOP - y; y = SET_TOP; }
  setHits[setHitN++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (uint8_t)id};
}

int setHitAt(int x, int y) {
  for (int i = setHitN - 1; i >= 0; i--) {
    const SetHit &h = setHits[i];
    if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) return h.id;
  }
  return H_NONE;
}

// ---------- 读写 ----------
void settingsLoad() {
  doneDay = prefs.getInt("dnday", -1);  // 今天完成的任务数：存在 NVS 里，重启 / 烧录后不清零
  doneToday[0] = prefs.getInt("dn0", 0);
  doneToday[1] = prefs.getInt("dn1", 0);
  doneToday[2] = prefs.getInt("dn2", 0);
  cfgSfx = prefs.getBool("sfx", true);
  cfgVoice = prefs.getBool("voice", false);
  spkMute = prefs.getBool("mute", false);
  cfgDoneSec = prefs.getUShort("dsec", 20);
  cfgMicReact = prefs.getBool("mreact", true);
  cfgBright = constrain(prefs.getUChar("bright", 100), 10, 100);
  cfgMeetMute = prefs.getBool("mmute", true);
  cfgGreet = prefs.getBool("greet", true);
  cfgHomeScene = prefs.getBool("scene", true);
  cfgClock = prefs.getBool("clock", true);
  cfgRadarSeat = prefs.getBool("rseat", true);
  cfgPrivacy = prefs.getBool("priv", true);
  cfgSleepMin = prefs.getUChar("sleepm", 15);
  cfgMicPause = prefs.getBool("micpause", true);
  cfgNightDim = prefs.getBool("night", true);
  seatCal = prefs.getBool("seatcal", false);
  seatCalX = prefs.getShort("seatx", 0);
  seatCalY = prefs.getShort("seaty", 800);
}

void settingsSave() {
  prefs.putBool("sfx", cfgSfx);
  prefs.putBool("voice", cfgVoice);
  prefs.putBool("mute", spkMute);
  prefs.putUShort("dsec", cfgDoneSec);
  prefs.putBool("mreact", cfgMicReact);
  prefs.putUChar("bright", cfgBright);
  prefs.putBool("mmute", cfgMeetMute);
  prefs.putBool("greet", cfgGreet);
  prefs.putBool("scene", cfgHomeScene);
  prefs.putBool("clock", cfgClock);
  prefs.putBool("rseat", cfgRadarSeat);
  prefs.putBool("priv", cfgPrivacy);
  prefs.putUChar("sleepm", cfgSleepMin);
  prefs.putBool("micpause", cfgMicPause);
  prefs.putBool("night", cfgNightDim);
  cfgDirty = true;
}

// bridge 的 /audio 改开关：{"t":"ui","voice":0,"sfx":1,"mute":0,"dsec":60}
void settingsFromJson(JsonDocument &doc) {
  bool any = false;
  if (doc["voice"].is<int>()) { cfgVoice = (int)doc["voice"] == 1; any = true; }
  if (doc["sfx"].is<int>()) { cfgSfx = (int)doc["sfx"] == 1; any = true; }
  if (doc["mute"].is<int>()) { spkMute = (int)doc["mute"] == 1; any = true; }
  if (doc["dsec"].is<int>()) { cfgDoneSec = constrain((int)doc["dsec"], 0, 65535); any = true; }
  if (any) settingsSave();
}

// 变了马上报，平时每 10 秒报一次（bridge 重启后也能拿到）
void settingsTick(uint32_t now) {
  static bool lastTalk = false;
  if (userTalking() != lastTalk) {  // 有人说话 / 停了：马上告诉 bridge，免得它排队说话
    lastTalk = !lastTalk;
    cfgDirty = true;
  }
  if (!cfgDirty && now - cfgSentAt < 10000) return;
  cfgDirty = false;
  cfgSentAt = now;
  Host.printf("CFG {\"voice\":%d,\"sfx\":%d,\"mute\":%d,\"dsec\":%d,\"vol\":%d,\"greet\":%d,\"mmute\":%d,\"talk\":%d}\n",
                cfgVoice, cfgSfx, spkMute, cfgDoneSec, spkVol, cfgGreet, cfgMeetMute, userTalking());
}

void settingsOpen() {
  if (setSec == 1) wifiStartScan();
}

// ---------- 控件 ----------
// 开关：40×22，开 = 绿底、滑块在右。滑块和底槽一样切角；开时是带一点绿的浅色、下面压一道暗绿阴影，关时是灰色
void pxToggle(int x, int y, bool on) {
  uint16_t c = on ? P_GREEN : P_EDGE;
  pxPanel(x, y, 40, 22, c, c, false);
  int kx = on ? x + 21 : x + 3;
  if (on) pxPanel(kx, y + 4, 16, 16, mix565(P_GREEN, P_BG, 110), mix565(P_GREEN, P_BG, 110), false);
  pxPanel(kx, y + 3, 16, 15, on ? mix565(P_TEXT, P_GREEN, 50) : P_DIM, on ? mix565(P_TEXT, P_GREEN, 50) : P_DIM, false);
}

// 滑块：方块轨道 + 白色方块把手；frac 0~1
void pxSlider(int x, int y, int w, float frac, bool dim) {
  int kx = x + (int)((w - 10) * constrain(frac, 0.0f, 1.0f));
  gfx->fillRect(x, y + 6, w, 6, P_EDGE);
  gfx->fillRect(x, y + 6, kx - x + 5, 6, dim ? P_DIM : P_ACCENT);
  pxPanel(kx, y, 10, 18, P_TEXT, P_TEXT, false);
}

// 分段选择：等宽格子，选中格紫底
void pxSegment(int x, int y, int cw, int n, const char *const *labels, int sel, int idBase) {
  pxSegDraw(x, y, cw, n, labels, sel, 24);  // 统一样式（ui_pixel.ino）
  for (int i = 0; i < n; i++) setHit(x + i * cw, y - 5, cw, 34, idBase + i);
}

// 分组标题 / 脚注 / 分组底板 / 行
int setGroupHead(const String &s, int y) {
  pxText(s, SET_PX + 4, y + 13, P_DIM, 1);
  return y + 20;
}

int setFootnote(const String &s, int y) {
  pxText(s, SET_PX + 4, y + 16, P_DIM, 1);
  return y + 24;
}

void setGroup(int y, int rows, int extraH = 0);
void setGroup(int y, int rows, int extraH) {
  pxPanel(SET_PX, y, SET_PW, rows * SET_ROW + extraH + 4, P_PANEL, P_EDGE, true);
}

// 一行：左边标题，右边开关；返回下一行 y
int setToggleRow(const String &label, bool on, int y, int id, bool first) {
  if (!first) pxDots(SET_PX + 10, SET_PX + SET_PW - 10, y, P_GRID);
  pxText(label, SET_PX + 14, y + 22, P_TEXT, 1);
  pxToggle(SET_PX + SET_PW - 54, y + 6, on);
  setHit(SET_PX, y, SET_PW, SET_ROW, id);
  return y + SET_ROW;
}

// 滑块行：标题 + 滑块 + 数值；滑块命中区占整行
const int SLIDE_X = SET_PX + 64, SLIDE_W = SET_PW - 64 - 52;
int setSliderRow(const String &label, int val, bool dim, int y, int id, bool first) {
  if (!first) pxDots(SET_PX + 10, SET_PX + SET_PW - 10, y, P_GRID);
  pxText(label, SET_PX + 14, y + 25, P_TEXT, 1);
  pxSlider(SLIDE_X, y + 11, SLIDE_W, val / 100.0f, dim);
  String v = dim ? String("静音") : String(val);
  pxText(v, SET_PX + SET_PW - 14 - pxW(v, 1), y + 25, dim ? P_DIM : P_TEXT, 1);
  setHit(SET_PX, y, SET_PW, 40, id);
  return y + 40;
}

// ---------- 各分区 ----------
int drawSoundPane(int y) {
  int y0 = y;
  y = setGroupHead("输出", y);
  setGroup(y, 2, 40);
  int ry = y + 2;
  ry = setSliderRow("音量", spkVol, spkMute, ry, H_VOL, true);
  ry = setToggleRow("静音", spkMute, ry, H_MUTE, false);
  ry = setToggleRow("有人说话时不播报", cfgMeetMute, ry, H_MMUTE, false);
  y = ry + 10;

  y = setGroupHead("Claude 提醒", y);
  setGroup(y, 4);
  ry = y + 2;
  ry = setToggleRow("提示音", cfgSfx, ry, H_SFX, true);
  ry = setToggleRow("语音播报", cfgVoice, ry, H_VOICE, false);
  ry = setToggleRow("离开回来时打招呼", cfgGreet, ry, H_GREET, false);
  pxDots(SET_PX + 10, SET_PX + SET_PW - 10, ry, P_GRID);
  pxText("任务超过", SET_PX + 14, ry + 22, cfgVoice ? P_TEXT : P_DIM, 1);
  int sel = -1;  // bridge 可以设任意秒数：不在这几档里就都不高亮
  for (int i = 0; i < 4; i++)
    if (DSEC_OPT[i] == cfgDoneSec) sel = i;
  pxSegment(SET_PX + SET_PW - 12 - 4 * 50, ry + 5, 50, 4, DSEC_LABEL, sel, H_DSEC0);
  ry += SET_ROW;
  y = ry + 10;

  y = setGroupHead("麦克风", y + 4);
  setGroup(y, 4);
  ry = y + 2;
  ry = setToggleRow("Clawd 对你做反应", cfgMicReact, ry, H_MREACT, true);
  ry = setToggleRow("离开时暂停麦克风", cfgMicPause, ry, H_MICPAUSE, false);
  pxDots(SET_PX + 10, SET_PX + SET_PW - 10, ry, P_GRID);
  // 实时：听到的是什么 + 音量
  String cls = soundClass();
  uint16_t cc = cls == "说话" ? C_YELLOW : (cls == "音乐" ? P_ACCENT : (cls == "敲击" ? C_CLAUDE : (cls == "安静" ? P_DIM : P_GREEN)));
  pxText("听到", SET_PX + 14, ry + 22, P_TEXT, 1);
  pxText(cls, SET_PX + 56, ry + 22, cc, 1);
  float f = micOn ? constrain((micDb + 70) / 70, 0.0f, 1.0f) : 0;
  pxSegBar(SET_PX + 140, ry + 12, SET_PW - 154, 10, f, micOn ? P_GREEN : P_EDGE, P_EDGE);
  ry += SET_ROW;
  // 频谱：6 个频点，中间四个（人声区）用绿色
  pxDots(SET_PX + 10, SET_PX + SET_PW - 10, ry, P_GRID);
  static const char *const FL[6] = {"250", "500", "1k", "2k", "3k", "5k"};
  int bw = (SET_PW - 28) / 6;
  for (int k = 0; k < 6; k++) {
    int bx = SET_PX + 14 + k * bw, bh = micOn ? (int)constrain((micBand[k] + 90) / 70 * 16, 0.0f, 16.0f) : 0;
    gfx->fillRect(bx, ry + 6, bw - 22, 16, P_EDGE);
    gfx->fillRect(bx, ry + 22 - bh, bw - 22, bh, (k >= 1 && k <= 4) ? P_GREEN : P_DIM);
    pxText(FL[k], bx + bw - 20, ry + 22, P_DIM, 1);
  }
  ry += SET_ROW;
  y = ry + 10;
  return y + 8 - y0;
}

int drawDisplayPane(int y) {
  int y0 = y;
  y = setGroupHead("屏幕", y);
  setGroup(y, 1, 40);
  int ry = y + 2;
  ry = setSliderRow("亮度", cfgBright, false, ry, H_BRIGHT, true);
  ry = setToggleRow("夜里自动调暗", cfgNightDim, ry, H_NIGHT, false);
  y = ry + 10;

  y = setGroupHead("节能", y);
  setGroup(y, 1);
  ry = y + 2;
  pxText(cfgClock ? "离开后调暗" : "离开后熄屏", SET_PX + 14, ry + 22, P_TEXT, 1);  // 开着时钟：白天调暗、夜里才熄屏
  static const char *const SLEEP_L[4] = {"5 分", "15 分", "30 分", "从不"};
  static const int SLEEP_V[4] = {5, 15, 30, 0};
  int ssel = 3;
  for (int i = 0; i < 4; i++)
    if (SLEEP_V[i] == cfgSleepMin) ssel = i;
  pxSegment(SET_PX + SET_PW - 12 - 4 * 50, ry + 5, 50, 4, SLEEP_L, ssel, H_SLEEP0);
  ry += SET_ROW;
  y = ry + 10;

  y = setGroupHead("界面", y);
  setGroup(y, 2);
  ry = y + 2;
  pxText("首页", SET_PX + 14, ry + 22, P_TEXT, 1);
  static const char *const HOME[2] = {"桌面", "卡片"};
  pxSegment(SET_PX + SET_PW - 12 - 2 * 64, ry + 5, 64, 2, HOME, cfgHomeScene ? 0 : 1, H_SCENE0);
  ry += SET_ROW;
  ry = setToggleRow("离开时显示时钟", cfgClock, ry, H_CLOCK, false);
  y = ry + 10;
  return y + 8 - y0;
}

int setInfoRow(const String &k, const String &v, int y, bool first) {
  if (!first) pxDots(SET_PX + 10, SET_PX + SET_PW - 10, y, P_GRID);
  pxText(k, SET_PX + 14, y + 21, P_DIM, 1);
  pxText(v, SET_PX + SET_PW - 14 - pxW(v, 1), y + 21, P_TEXT, 1);
  return y + 30;
}

int drawAboutPane(int y) {
  int y0 = y;
  int ry;
  if (batHas || batUsb) {
    y = setGroupHead("电源", y);
    if (batHas) {
      pxPanel(SET_PX, y, SET_PW, 3 * 30 + 4, P_PANEL, P_EDGE, true);
      ry = y + 2;
      ry = setInfoRow("电量", String(batPct) + "%（大概）", ry, true);
      ry = setInfoRow("电压", String(batV, 2) + " V", ry, false);
      ry = setInfoRow("状态", batState(), ry, false);
      y = ry + 10;
    } else {  // 插着 USB：读不出有没有电池，见 hw_bat.ino
      pxPanel(SET_PX, y, SET_PW, 30 + 4, P_PANEL, P_EDGE, true);
      ry = setInfoRow("状态", batState(), y + 2, true);
      y = setFootnote("接了电池会顺便充电；拔掉 USB 后显示电量", ry);
    }
    y = setGroupHead("设备", y);
  }
  pxPanel(SET_PX, y, SET_PW, 6 * 30 + 4, P_PANEL, P_EDGE, true);
  ry = y + 2;
  ry = setInfoRow("设备", "JC4827W543 · 4.3 寸", ry, true);
  ry = setInfoRow("芯片", "ESP32-S3 · " + String(ESP.getCpuFreqMHz()) + " MHz", ry, false);
  ry = setInfoRow("固件", String(FW_VERSION), ry, false);
  ry = setInfoRow("可用内存", String(ESP.getFreeHeap() / 1024) + " KB · PSRAM " + String(ESP.getFreePsram() / 1024) + " KB", ry, false);
  ry = setInfoRow("已运行", fmtDuration(millis() / 1000), ry, false);
  bool br = mode != M_OFFLINE && lastMsgAt;
  ry = setInfoRow("bridge", br ? netLinkText() : String("未连接 · ") + netLinkText(), ry, false);
  y = ry + 10;
  pxXLogo(SET_PX + 6, y + 4, P_DIM);
  pxText("@bruc3van", SET_PX + 26, y + 16, P_DIM, 1);
  return y + 30 - y0;
}

// ---------- 电脑（Wi-Fi 配对的电脑，net_link.ino）----------
int pcSel = -1;  // 点开了哪一台

void pxButton(const String &label, int x, int y, int w, bool primary, int id) {
  pxPanel(x, y, w, 30, primary ? P_ACCENT : P_PANEL, primary ? P_ACCENT : P_EDGE, true);
  pxText(label, x + (w - pxW(label, 1)) / 2, y + 20, primary ? P_BG : P_TEXT, 1);
  setHit(x, y - 4, w, 38, id);
}

int drawPcPane(int y, uint32_t now) {
  int y0 = y, ry;
  int sas = pairSas;
  if (sas >= 0) {  // 有电脑请求配对：核对验证码
    y = setGroupHead("配对请求", y);
    pxPanel(SET_PX, y, SET_PW, 150, P_PANEL, P_EDGE, true);
    pxText(String(pairReqName) + " 请求配对", SET_PX + 14, y + 24, P_TEXT, 1);
    pxText("电脑上显示的验证码一致才点「允许」", SET_PX + 14, y + 44, P_DIM, 1);
    char code[8];
    snprintf(code, sizeof(code), "%03d %03d", sas / 1000, sas % 1000);
    pxText(code, SET_PX + (SET_PW - pxW(code, 3)) / 2, y + 92, P_TEXT, 3);
    pxButton("拒绝", SET_PX + 14, y + 108, 140, false, H_PCDENY);
    pxButton("允许", SET_PX + SET_PW - 154, y + 108, 140, true, H_PCALLOW);
    y += 160;
  }
  int n = hostN;
  if (pcSel >= n) pcSel = -1;
  y = setGroupHead("已配对的电脑", y);
  pxPanel(SET_PX, y, SET_PW, max(n, 1) * SET_ROW + 4, P_PANEL, P_EDGE, true);
  ry = y + 2;
  if (!n) {
    pxText("还没有", SET_PX + 14, ry + 22, P_DIM, 1);
    ry += SET_ROW;
  }
  for (int i = 0; i < n; i++) {
    if (i) pxDots(SET_PX + 10, SET_PX + SET_PW - 10, ry, P_GRID);
    char name[40];
    bool pref;
    pairLock();
    strlcpy(name, i < hostN ? hosts[i].name : "", sizeof(name));
    pref = i < hostN && !strcmp(prefHid, hosts[i].hid);
    pairUnlock();
    int st = netHostState(i);
    String right = st == 2 ? "控制中" : st == 1 ? (hostNetActive() ? "待命" : "待命（USB 在用）") : "未连接";
    if (pref) right = "首选 · " + right;
    pxText(name, SET_PX + 14, ry + 22, i == pcSel ? P_ACCENT : P_TEXT, 1);
    pxText(right + " >", SET_PX + SET_PW - 14 - pxW(right + " >", 1), ry + 22, st == 2 ? P_GREEN : P_DIM, 1);
    setHit(SET_PX, ry, SET_PW, SET_ROW, H_PC0 + i);
    ry += SET_ROW;
  }
  y = ry + 10;
  if (pcSel >= 0) {
    char name[40] = "";
    bool pref = false;
    pairLock();
    if (pcSel < hostN) {
      strlcpy(name, hosts[pcSel].name, sizeof(name));
      pref = !strcmp(prefHid, hosts[pcSel].hid);
    }
    pairUnlock();
    y = setGroupHead(name, y);
    setGroup(y, 2);
    ry = setToggleRow("首选电脑（同时在线时由它控制）", pref, y + 2, H_PCPREF, true);
    pxDots(SET_PX + 10, SET_PX + SET_PW - 10, ry, P_GRID);
    pxText("移除这台电脑", SET_PX + 14, ry + 22, P_RED, 1);
    setHit(SET_PX, ry, SET_PW, SET_ROW, H_PCDEL);
    y = ry + SET_ROW + 10;
  }
  y = setGroupHead("添加", y);
  setGroup(y, 1);
  ry = y + 2;
  if (pairOpen()) {
    int left = (int32_t)(pairOpenUntil - now) / 1000;
    pxText("等电脑连接 · " + String(left / 60) + ":" + (left % 60 < 10 ? "0" : "") + String(left % 60), SET_PX + 14, ry + 22, P_TEXT, 1);
    pxText("取消", SET_PX + SET_PW - 14 - pxW("取消", 1), ry + 22, P_ACCENT, 1);
    setHit(SET_PX, ry, SET_PW, SET_ROW, H_PCADD);
    y = setFootnote("在电脑上运行 buddy pair --wifi", ry + SET_ROW);
  } else {
    pxText("添加电脑（屏幕验证码）", SET_PX + 14, ry + 22, P_TEXT, 1);
    pxText(">", SET_PX + SET_PW - 22, ry + 22, P_DIM, 1);
    setHit(SET_PX, ry, SET_PW, SET_ROW, H_PCADD);
    y = setFootnote("插着 USB 启动 bridge 时会自动配对", ry + SET_ROW);
  }
  if (pairMsg[0] && now - pairMsgAt < 8000) y = setFootnote(pairMsg, y - 8);
  return y + 8 - y0;
}

// ---------- 分区栏 ----------
// 8×8 图标（放大 2 倍画在彩色方块上）
// ---------- 雷达 ----------
int drawRadarPane(int y) {
  int y0 = y;
  y = setGroupHead("检测", y);
  setGroup(y, 2);
  int ry = y + 2;
  ry = setToggleRow("用雷达判断在不在座位", cfgRadarSeat, ry, H_RSEAT, true);
  ry = setToggleRow("有人靠近时提醒", cfgPrivacy, ry, H_PRIV, false);
  y = ry + 10;

  // 座位：标定你正常坐着的位置（屏幕摆在旁边、雷达转过去时，座位就不在雷达正前方了）
  y = setGroupHead("座位", y);
  setGroup(y, 1);
  ry = y + 2;
  String st;
  uint16_t sc = P_TEXT;
  if (calibUntil) {
    st = calibMsg + " " + String((calibUntil - millis()) / 1000 + 1) + " 秒";
    sc = C_YELLOW;
  } else if (seatCal) {
    int a = (int)(atan2f(seatCalX, max(1, seatCalY)) * 180 / PI);
    st = String(sqrtf((float)seatCalX * seatCalX + (float)seatCalY * seatCalY) / 1000.0f, 2) + " 米 · " +
         (abs(a) < 5 ? String("正前方") : String(a > 0 ? "偏右 " : "偏左 ") + abs(a) + "°");
    sc = P_GREEN;
  } else {
    st = calibMsg.length() ? calibMsg : String("未标定");
    sc = P_DIM;
  }
  pxMarquee(st, SET_PX + 14, ry + 22, sc, 1, 150);
  int bx = SET_PX + SET_PW - 12 - 76;
  pxPanel(bx, ry + 5, 76, 24, calibUntil ? P_EDGE : P_ACCENT, calibUntil ? P_EDGE : P_ACCENT, false);
  pxText("标定", bx + 38 - pxW("标定", 1) / 2, ry + 22, P_BG, 1);
  setHit(bx - 4, ry, 84, SET_ROW, H_SEATCAL);
  if (seatCal && !calibUntil) {
    int cx = bx - 8 - 52;
    pxPanel(cx, ry + 5, 52, 24, P_PANEL, P_EDGE, false);
    pxText("清除", cx + 26 - pxW("清除", 1) / 2, ry + 22, P_DIM, 1);
    setHit(cx - 4, ry, 58, SET_ROW, H_SEATCLR);
  }
  ry += SET_ROW;
  y = ry + 10;

  y = setGroupHead("状态", y + 4);
  static uint32_t fAt = 0, fN = 0;
  static int fps = 0;
  if (millis() - fAt > 1000) { fps = radarFrames - fN; fN = radarFrames; fAt = millis(); }
  pxPanel(SET_PX, y, SET_PW, 5 * 30 + 4 + SET_ROW, P_PANEL, P_EDGE, true);
  ry = y + 2;
  ry = setInfoRow("雷达", radarFresh() ? "已连接 · " + String(fps) + " 帧/秒" : String("未连接（P1）"), ry, true);
  ry = setInfoRow("现在", radarFresh() ? String(radarCount()) + " 人" + (radarSeatPresent() ? " · 你在座位上" : " · 座位没人") : String("—"), ry, false);
  ry = setInfoRow("靠近", privacyOn ? (ghostOn ? String("有人 · 在你身后") : String("有人")) : String("没人"), ry, false);
  // LD2450 本身的能力（手册）：最远 6 米、左右各 60°、同时最多 3 人；你正后方被你身体挡住，看不到
  ry = setInfoRow("能看多远", "6 米 · 左右各 60° · 最多 3 人", ry, false);
  ry = setInfoRow("盲区", "你正后方（被你挡住）", ry, false);
  pxDots(SET_PX + 10, SET_PX + SET_PW - 10, ry, P_GRID);
  pxText("打开雷达页", SET_PX + 14, ry + 22, P_ACCENT, 1);
  pxText(">", SET_PX + SET_PW - 22, ry + 22, P_DIM, 1);
  setHit(SET_PX, ry, SET_PW, SET_ROW, H_ROPEN);
  ry += SET_ROW;
  return ry + 10 - y0;
}

const char *const SEC_GLYPH[6][8] = {
  {"...1..1.", "..11...1", "1111.1.1", "1111.1.1", "1111.1.1", "1111.1.1", "..11...1", "...1..1."},  // 喇叭
  {"", "", "", "", "", "", "", ""},                                                                  // Wi-Fi 用现成图标
  {"1..1..1.", ".1...1..", "..111...", "1.111.1.", "..111...", ".1...1..", "1..1..1.", "........"},  // 太阳
  {"", "", "", "", "", "", "", ""},                                                                  // 雷达用 16px 图标
  {"...11...", "...11...", "........", "..111...", "...11...", "...11...", "...11...", "..1111.."},  // i
  {"11111111", "1......1", "1......1", "1......1", "11111111", "...11...", "..1111..", "........"},  // 显示器
};
const char *SEC_NAME[6] = {"声音", "Wi-Fi", "显示", "雷达", "关于", "电脑"};
const uint16_t SEC_TINT[6] = {0xF96A, 0x0C3F, 0x5AFC, 0x362B, 0x8C72, 0xFC60};  // 同 macOS：粉红 / 蓝 / 靛蓝 / 绿 / 灰 / 橙
const int SEC_ORDER[6] = {0, 1, 5, 2, 3, 4};  // 分区栏里的顺序：「电脑」排在 Wi-Fi 后面

void drawSecIcon(int i, int x, int y) {
  pxPanel(x, y, 22, 22, SEC_TINT[i], SEC_TINT[i], false);
  if (i == 1) {
    pxWifiIcon(x + 5, y + 7, 3, P_TEXT, 1);
    return;
  }
  if (i == 3) {
    drawRadarIcon(x + 3, y + 3, 1, P_TEXT, P_TEXT, P_TEXT, SEC_TINT[i]);
    return;
  }
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if (SEC_GLYPH[i][r][c] == '1') gfx->fillRect(x + 3 + c * 2, y + 3 + r * 2, 2, 2, P_TEXT);
}

void drawSidebar() {
  gfx->fillRect(0, 0, SET_SB, 272, P_PANEL);
  gfx->fillRect(SET_SB, 0, 2, 272, P_EDGE);
  pxHomeBtn(6, 6);
  setHit(0, 0, 50, 44, H_HOME);
  pxU("设置", 46, 26, P_TEXT);
  for (int k = 0; k < 6; k++) {
    int i = SEC_ORDER[k], y = 48 + k * 34;
    bool on = i == setSec;
    if (on) pxPanel(6, y, SET_SB - 12, 32, P_ACCENT, P_ACCENT, false);
    drawSecIcon(i, 12, y + 5);
    pxText(SEC_NAME[i], 42, y + 21, on ? P_BG : P_TEXT, 1);
    setHit(0, y - 1, SET_SB, 34, H_SEC0 + i);
  }
  pxText(FW_VERSION, 12, 262, P_DIM, 1);
}

// 齿轮（首页状态栏，16×16）
void drawGearBadge(int x, int y) {
  const char *g[8] = {"...11...", ".1.11.1.", "..1111..", "111..111", "111..111", "..1111..", ".1.11.1.", "...11..."};
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if (g[r][c] == '1') gfx->fillRect(x + c * 2, y + r * 2, 2, 2, P_TEXT);
}

// ---------- 整页 ----------
void drawSettings(uint32_t now) {
  gfx->fillScreen(P_BG);
  setHitN = 0;
  int y = SET_TOP + 6 - setScroll[setSec];
  int h = 0;
  switch (setSec) {
    case 0: h = drawSoundPane(y); break;
    case 1: h = drawWifiPane(y, now); break;
    case 2: h = drawDisplayPane(y); break;
    case 3: h = drawRadarPane(y); break;
    case 5: h = drawPcPane(y, now); break;
    default: h = drawAboutPane(y); break;
  }
  setContentH[setSec] = h;
  // 标题栏盖住滚出去的内容
  gfx->fillRect(SET_SB, 0, 480 - SET_SB, SET_TOP, P_BG);
  pxU(SEC_NAME[setSec], SET_PX + 2, 28, P_TEXT);
  if (setSec == 1) drawWifiHeaderBtn();
  for (int x = SET_SB + 4; x < 480; x += 4) gfx->fillRect(x, SET_TOP - 2, 2, 2, P_GRID);
  drawSidebar();
}

void settingsDrag(int dy) {
  int maxS = max(0, setContentH[setSec] - (272 - SET_TOP - 6));
  setScroll[setSec] = constrain(setScroll[setSec] - dy, 0, maxS);
}

void slideTo(int x) {
  int v = constrain((x - SLIDE_X - 5) * 100 / (SLIDE_W - 10), 0, 100);
  if (setSlider == H_VOL) {
    spkVol = v;
    spkMute = false;  // 拖音量即取消静音（同 macOS）
  } else if (setSlider == H_BRIGHT) {
    cfgBright = max(10, v);
  }
}

// 按下：落在滑块上就接管之后的移动
bool settingsDown(int x, int y) {
  int id = setHitAt(x, y);
  if (id != H_VOL && id != H_BRIGHT) return false;
  setSlider = id;
  slideTo(x);
  return true;
}

void settingsSlide(int x) { slideTo(x); }

void settingsUp() {
  if (setSlider == H_VOL) {
    spkSetVolume(spkVol);
    sfxTone(988, 90, 0.3f, 0);  // 松手试听一声（不受提示音开关影响）
  }
  setSlider = 0;
  settingsSave();
}

void settingsTap(int x, int y) {
  int id = setHitAt(x, y);
  if (id == H_HOME) {
    setScreen(SCR_HOME);
  } else if (id >= H_SEC0 && id < H_SEC0 + 6) {
    setSec = id - H_SEC0;
    setScroll[setSec] = 0;
    settingsOpen();
  } else if (id >= H_PC0 && id < H_PC0 + NET_HOSTS) {
    pcSel = pcSel == id - H_PC0 ? -1 : id - H_PC0;
  } else if (id == H_PCADD) {
    netPairWindow(!pairOpen());
  } else if (id == H_PCALLOW || id == H_PCDENY) {
    netPairAnswer(id == H_PCALLOW);
  } else if (id == H_PCPREF) {
    netTogglePref(pcSel);
  } else if (id == H_PCDEL) {
    netRemoveHost(pcSel);
    pcSel = -1;
  } else if (id >= H_WSCAN) {
    wifiHit(id);
  } else if (id == H_MUTE) {
    spkMute = !spkMute;
    settingsSave();
  } else if (id == H_SFX) {
    cfgSfx = !cfgSfx;
    settingsSave();
    if (cfgSfx) sfxPlay("done");
  } else if (id == H_VOICE) {
    cfgVoice = !cfgVoice;
    settingsSave();
  } else if (id >= H_DSEC0 && id < H_DSEC0 + 4) {
    cfgDoneSec = DSEC_OPT[id - H_DSEC0];
    settingsSave();
  } else if (id == H_MREACT) {
    cfgMicReact = !cfgMicReact;
    settingsSave();
  } else if (id >= H_SLEEP0 && id < H_SLEEP0 + 4) {
    const int v[4] = {5, 15, 30, 0};
    cfgSleepMin = v[id - H_SLEEP0];
    settingsSave();
  } else if (id == H_NIGHT) {
    cfgNightDim = !cfgNightDim;
    settingsSave();
  } else if (id == H_MICPAUSE) {
    cfgMicPause = !cfgMicPause;
    settingsSave();
  } else if (id == H_RSEAT) {
    cfgRadarSeat = !cfgRadarSeat;
    settingsSave();
  } else if (id == H_PRIV) {
    cfgPrivacy = !cfgPrivacy;
    settingsSave();
  } else if (id == H_SEATCAL) {
    if (!calibUntil) seatCalibStart();
  } else if (id == H_SEATCLR) {
    seatCalibClear();
  } else if (id == H_ROPEN) {
    setScreen(SCR_RADAR);
  } else if (id == H_MMUTE) {
    cfgMeetMute = !cfgMeetMute;
    settingsSave();
  } else if (id == H_GREET) {
    cfgGreet = !cfgGreet;
    settingsSave();
  } else if (id == H_SCENE0 || id == H_SCENE0 + 1) {
    cfgHomeScene = id == H_SCENE0;
    settingsSave();
  } else if (id == H_CLOCK) {
    cfgClock = !cfgClock;
    settingsSave();
  }
}
