// 全局通知：等你确认、完成、出错、有人靠近，在任何页面右上角滑出一张通知卡片，点一下跳到对应页面。
// 同时只显示最要紧的一条：等你确认 > 有人靠近 > 出错 > 完成。已经在对应页面上时不重复弹。

const char *const IC_CHECK[8] = {"........", ".......1", "......11", ".....11.", "1...11..", "11.11...", ".111....", "..1....."};
const char *const IC_CROSS[8] = {"11....11", "111..111", ".111111.", "..1111..", "..1111..", ".111111.", "111..111", "11....11"};
const char *const IC_HAND[8] = {"..1.1...", ".11.11..", ".11.111.", ".111111.", "1111111.", "1111111.", ".11111..", "..1111.."};

const int AG_N = 3;  // Claude / Codex / DeepSeek
uint32_t toastDoneAt[AG_N] = {0, 0, 0}, toastErrAt[AG_N] = {0, 0, 0};
int toastLastMode[AG_N] = {-1, -1, -1};
long doneSeq[AG_N] = {-1, -1, -1};  // bridge 发来的完成次数（"dn"）；-1 = 开机后还没收到，或 bridge 太旧不发

void doneAdd(int a, int k) {
  uint32_t e = epochNow();
  int day = e ? (int)(((int64_t)e + tzOff) / 86400) : -1;
  if (day != doneDay) {  // 新的一天：清零
    doneDay = day;
    prefs.putInt("dnday", day);
    for (int i = 0; i < AG_N; i++) {
      doneToday[i] = 0;
      prefs.putInt(("dn" + String(i)).c_str(), 0);
    }
  }
  doneToday[a] += k;
  prefs.putInt(("dn" + String(a)).c_str(), doneToday[a]);
}

// 完成次数的起点按来源分开记：bi = bridge 进程的随机编号（每台电脑、每次启动都不同；旧 bridge 不发，记成 0）。
// 切到另一台电脑再切回来时，各自和自己上一次的值比，不会把 A、B 两边的计数差当成新完成（DoneSrc 在 hw_state.h）
const int DONE_SRC_N = 6;
DoneSrc doneSrc[DONE_SRC_N];
int doneSrcN = 0;
uint32_t doneSrcUse = 0;

DoneSrc *doneSrcOf(uint32_t bi) {
  for (int i = 0; i < doneSrcN; i++)
    if (doneSrc[i].bi == bi) { doneSrc[i].use = ++doneSrcUse; return &doneSrc[i]; }
  int k = doneSrcN < DONE_SRC_N ? doneSrcN++ : 0;
  if (k == 0 && doneSrcN == DONE_SRC_N)  // 满了：换掉最久没用的
    for (int i = 1; i < DONE_SRC_N; i++)
      if (doneSrc[i].use < doneSrc[k].use) k = i;
  doneSrc[k].bi = bi;
  doneSrc[k].use = ++doneSrcUse;
  for (int a = 0; a < AG_N; a++) doneSrc[k].seq[a] = -1;
  return &doneSrc[k];
}

// 按 bridge 的完成次数记「今天完成」：并发完成、连续完成都不漏。每个来源第一次收到只当起点，不会重复计数
void doneSeqSet(int a, long n, uint32_t bi) {
  if (n < 0) return;
  DoneSrc *s = doneSrcOf(bi);
  if (s->seq[a] >= 0) {
    long k = n >= s->seq[a] ? n - s->seq[a] : n;  // 变小：bridge 重启过，从 0 重新数（只有不发 bi 的旧 bridge 会走到）
    if (k > 0) doneAdd(a, min(k, 100L));
  }
  s->seq[a] = n;
  doneSeq[a] = n;
}
int16_t toastX = 0, toastW = 0;
const int TOAST_RADAR = 9;
int toastTarget = -1;  // 点提示条去哪：0 Claude / 1 Codex / 2 DeepSeek / TOAST_RADAR 雷达

// 记下完成 / 出错的时刻（显示 4 秒 / 6 秒）
void toastTick(uint32_t now) {
  for (int a = 0; a < AG_N; a++) {
    int m = agMode(a);
    if (m != toastLastMode[a]) {
      // 开机后收到的第一个状态不算：bridge 会把「刚完成」保持一小会儿，重启时会被重复计数
      if (m == M_DONE && toastLastMode[a] != -1) {
        toastDoneAt[a] = now;
        if (doneSeq[a] < 0) doneAdd(a, 1);  // 旧 bridge 不发完成次数：退回按状态跳变计
      }
      if (m == M_ERROR) toastErrAt[a] = now;
      toastLastMode[a] = m;
    }
  }
}

// 通知卡片，照 macOS 通知横幅：右上角从屏幕外滑进来，左边是来源的图标，右边两行——来源名（亮）+ 内容（暗），右上角「现在」。
// 完成 / 出错到时间后再滑出去；等你确认、有人靠近一直挂着，直到状态解除。
const int NT_W = 216, NT_H = 50, NT_Y = 6, NT_X = 480 - 8 - NT_W;
String ntKey;
uint32_t ntShownAt = 0;

void drawToast(uint32_t now) {
  toastW = 0;
  toastTarget = -1;
  int kind = -1, agent = 0;  // 0 等你确认 / 1 有人靠近 / 2 出错 / 3 完成
  uint32_t endAt = 0;        // 会自己消失的通知：什么时候开始滑出
  bool onBuddy = screen == SCR_BUDDY;
  for (int a = 0; a < AG_N && kind < 0; a++) {
    int m = agMode(a);
    if (m == M_WAITING && !(onBuddy && buddyAgent == a)) { kind = 0; agent = a; }
  }
  if (kind < 0 && privacyOn && screen != SCR_RADAR) kind = 1;
  for (int a = 0; a < AG_N && kind < 0; a++)
    if (toastErrAt[a] && now - toastErrAt[a] < 6000 && !(onBuddy && buddyAgent == a)) { kind = 2; agent = a; endAt = toastErrAt[a] + 6000; }
  for (int a = 0; a < AG_N && kind < 0; a++)
    if (toastDoneAt[a] && now - toastDoneAt[a] < 4000 && !(onBuddy && buddyAgent == a)) { kind = 3; agent = a; endAt = toastDoneAt[a] + 4000; }
  if (kind < 0) { ntKey = ""; return; }

  String key = String(kind) + agent;
  if (key != ntKey) { ntKey = key; ntShownAt = now; }
  // 滑入 / 滑出：缓动 220ms
  float k = min(1.0f, (now - ntShownAt) / 220.0f);
  if (endAt && endAt - now < 220) k = min(k, (endAt - now) / 220.0f);
  k = 1 - (1 - k) * (1 - k) * (1 - k);
  int x = NT_X + (int)((1 - k) * (NT_W + 12)), y = NT_Y;

  String title, body;
  uint16_t tile;
  if (kind == 1) {
    title = "雷达";
    body = "有人靠近 · " + behindWhere();
    tile = C_YELLOW;
    toastTarget = TOAST_RADAR;
  } else {
    title = agTitle(agent);
    body = kind == 0 ? "需要你确认，点这里查看" : (kind == 2 ? "运行出错" : "完成了任务");
    tile = agColor(agent);
    toastTarget = agent;
  }
  // 卡片：等你确认 / 有人靠近用来源色描边，表示「还在等你」
  pxPanel(x, y, NT_W, NT_H, P_PANEL, kind <= 1 ? tile : P_EDGE, true);
  // 图标：同 Buddy 页状态卡——深色底块上画来源色图标（亮色底配白图标太糊）
  pxPanel(x + 9, y + 9, 32, 32, P_BG, P_BG, false);
  if (kind == 1) {
    drawBehindIcon(x + 9, y + 9, 2, tile, (now / 500) % 2 ? mix565(tile, P_BG, 110) : tile);  // 背后那个人一闪一闪
  } else {
    drawIcon8(agIcon8(agent), x + 13, y + 13, 3, agHi(agent));  // 高亮色：深底上 Codex 的蓝太暗
    // 角标：完成 ✓ / 出错 × / 等你确认 小手，画在图标右下角；亮色底 + 深色图形，外面一圈卡片底色和图标隔开
    const char *const *badge = kind == 0 ? IC_HAND : (kind == 2 ? IC_CROSS : IC_CHECK);
    uint16_t bc = kind == 0 ? agHi(agent) : (kind == 2 ? P_RED : P_GREEN);
    pxPanel(x + 30, y + 30, 16, 16, P_PANEL, P_PANEL, false);  // 角标 12×12，压在图标右下角
    pxPanel(x + 32, y + 32, 12, 12, bc, bc, false);
    int bob = kind == 0 && (now / 300) % 2 ? -1 : 0;
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++)
        if (badge[r][c] == '1') gfx->fillRect(x + 34 + c, y + 34 + r + bob, 1, 1, P_BG);
  }
  int tx = x + 50, tw = NT_W - 50 - 10;
  String when = "现在";
  pxText(when, x + NT_W - 10 - pxW(when, 1), y + 19, P_DIM, 1);
  pxMarquee(title, tx, y + 19, P_TEXT, 1, tw - pxW(when, 1) - 8);
  pxMarquee(body, tx, y + 38, kind == 2 ? P_RED : P_DIM, 1, tw);
  toastX = x;
  toastW = NT_W;
}

// 点到提示条：跳到对应页面；返回 true 表示这次点击被提示条用掉了
bool toastTap(int x, int y) {
  if (!toastW || y < NT_Y || y > NT_Y + NT_H || x < toastX || x > toastX + toastW || toastTarget < 0) return false;
  if (toastTarget == TOAST_RADAR) setScreen(SCR_RADAR);
  else {
    buddyAgent = toastTarget;
    setScreen(SCR_BUDDY);
  }
  return true;
}
