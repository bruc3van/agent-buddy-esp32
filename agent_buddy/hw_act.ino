// 你在电脑前做什么：板子麦克风（敲键盘 / 说话 / 音乐 / 大声响）+ Mac 的键鼠空闲时间（bridge 每秒发来）
// 两边交叉判断，再变成 Clawd 的反应。只在 Clawd 待命 / 睡觉时反应，不打断 Claude 干活和等确认。
// 声音只在板子上算音量和节奏，不录音、不外传。


bool actFresh() { return actAt && millis() - actAt < 5000; }

// 最近 2 秒的敲击声频率（次/秒）
float micClickRate() {
  uint32_t now = millis();
  int n = 0;
  for (int i = 0; i < 16; i++)
    if (micClicks[i] && now - micClicks[i] < 2000) n++;
  return n / 2.0f;
}

// 麦克风听到的「持续敲键盘」：最近 3 秒内至少 8 下，从第一下到最后一下跨过 1.5 秒以上，
// 相邻两下都不超过 0.7 秒，而且最后一下就在 0.6 秒内。碰一下桌子、放个杯子这种零星响声都不算。
bool micTypingBurst(uint32_t now) {
  uint32_t t[16];
  int n = 0;
  for (int i = 0; i < 16; i++)
    if (micClicks[i] && now - micClicks[i] < 3000) t[n++] = micClicks[i];
  if (n < 8) return false;
  for (int i = 0; i < n; i++)  // 排序
    for (int j = i + 1; j < n; j++)
      if (t[j] < t[i]) { uint32_t x = t[i]; t[i] = t[j]; t[j] = x; }
  if (t[n - 1] - t[0] < 1500 || now - t[n - 1] > 600) return false;
  for (int i = 1; i < n; i++)
    if (t[i] - t[i - 1] > 700) return false;
  return true;
}

// Mac 上最近 ms 毫秒内按了几下键（真实按键，bridge 实时发来）
int macKeysIn(uint32_t now, uint32_t ms) {
  int n = 0;
  for (int i = 0; i < 32; i++)
    if (macKeys[i] && now - macKeys[i] < ms) n++;
  return n;
}

// 连着 bridge 时只认 Mac 的真实按键：2 秒内至少 4 下（单按一两个快捷键不算），停手 1.2 秒后收起键盘。
// 没连 bridge 时才退回麦克风听敲击声（更严格的持续敲击规则）。
uint32_t typingUntil = 0;

void typingTick(uint32_t now) {
  bool on;
  if (actFresh()) on = macKeysIn(now, 2000) >= 4 && now - macLastKeyAt < 800;
  else on = micOn && micTypingBurst(now);
  if (on) typingUntil = now + 1200;
}

bool userTyping() { return millis() < typingUntil; }

// 打字速度（键/秒）：优先用 Mac 的真实按键
float typingRate() {
  if (actFresh()) return macKeysIn(millis(), 2000) / 2.0f;
  return micClickRate();
}

// 离开电脑：Mac 5 分钟没有键鼠输入；没连 bridge 时看板子 10 分钟没人碰、也没声音
bool userAway() {
  uint32_t now = millis();
  if (lastTouchAt && now - lastTouchAt < 60000) return false;
  if (radarActive()) return radarSaysAway();  // 有雷达：座位区 60 秒没人才算离开（看视频不碰键盘也不算）
  if (actFresh()) return actInpSec >= 300;
  return now > 600000 && now - lastTouchAt > 600000 && now - micLastSoundMs > 600000;
}

// 有人在说话 / 开会：Mac 麦克风正被占用，或板子听到断断续续的人声（持续 3 秒以上，结束后保持 15 秒）
void talkTick(uint32_t now) {
  if (!micOn || spkPlaying()) return;
  int loud = 0, flips = 0, voiced = 0;
  uint32_t total = micBlockN, n = min(total, (uint32_t)192), b0 = total - n;
  bool prev = false;
  for (uint32_t k = 0; k < n; k++) {
    uint32_t b = (b0 + k) % 192;
    bool on = micLoudBits[b >> 3] & (1 << (b & 7));
    loud += on;
    voiced += (micVoiceBits[b >> 3] >> (b & 7)) & 1;
    if (on && !prev) flips++;
    prev = on;
  }
  float frac = n ? (float)loud / n : 0;
  // 说话：断断续续（有声 25%～90%、起伏多），而且有声的部分大多「像人声」（能量集中在 500～3000Hz）
  bool voice = n >= 150 && frac > 0.25f && frac < 0.9f && flips >= 6 && micClickRate() < 1.5f && !micDancing() &&
               loud > 0 && voiced * 10 >= loud * 6;
  if (voice) {
    if (!talkSince) talkSince = now;
    if (now - talkSince > 3000) talkUntil = now + 15000;
  } else if (now - micLastSoundMs > 1500) {
    talkSince = 0;
  }
}

// 现在听到的是什么（设置页的实时显示用）
String soundClass() {
  if (!micOn) return "麦克风已暂停";
  uint32_t now = millis();
  if (now < talkUntil || talkSince) return "说话";
  if (micDancing()) return "音乐";
  if (micClickRate() >= 2) return "敲击";
  if (now - micStartleAt < 1500) return "大声响";
  if (now - micLastSoundMs < 800) return "有声音";
  return "安静";
}

bool userTalking() { return (actFresh() && actMacMic) || millis() < talkUntil; }

// 现在该播语音吗（会议 / 有人说话时不打断）
bool voiceAllowed() { return cfgVoice && !spkMute && !(cfgMeetMute && userTalking()); }

// 刚变成不该播（关掉语音 / 开始开会）：已经排队的语音也不念了
void voiceGateTick() {
  static bool was = true;
  bool ok = voiceAllowed();
  if (was && !ok) spkFlush();
  was = ok;
}

int clawdReact(int m, uint32_t now) {
  if (now < dbgUntil && dbgReact) return dbgReact;
  if (m != M_IDLE && m != M_SLEEP) return R_NONE;
  if (backAt && now - backAt < 8000) return R_WAVE;
  if (!cfgMicReact) return R_NONE;
  bool typing = userTyping();
  if (micOn && now - micStartleAt < 900 && !typing) return R_STARTLE;
  if (userTalking()) return R_SHH;
  if (typing) return R_TYPE;
  if (micOn && micDancing()) return R_DANCE;
  bool macBusy = actFresh() && actInpSec >= 0 && actInpSec < 120;
  if (userAway() || (m == M_IDLE && micOn && now - micLastSoundMs > 120000 && !macBusy)) return R_SLEEP;
  return R_NONE;
}

// 反应的姿势，以及画在 Clawd 身后的东西（按 U 缩放，首页小 Clawd 也能用）
void reactPre(Pose &p, int r, uint32_t now) {
  float s = U / 14.0f;
  if (r == R_STARTLE) {  // 吓一跳：猛地跳起、瞪大眼、双手举高，头顶冒感叹号
    float k = min(1.0f, (now - micStartleAt) / 500.0f);
    p.dy = -(int)(26 * s * sinf(PI * k));
    p.dx = 0;
    p.eye = 5;
    p.lookX = p.lookY = 0;
    p.armL = p.armR = 2;
    p.squash = 0;
    int ex = CX + 4 * U, ey = BASE - 12.4f * U;
    gfx->fillRect(ex, ey, max(3, U * 3 / 7), U * 11 / 7, C_YELLOW);
    gfx->fillRect(ex, ey + U * 13 / 7, max(3, U * 3 / 7), max(3, U * 3 / 7), C_YELLOW);
  } else if (r == R_DANCE) {  // 跟着节拍摇摆：每个节拍换一边，双手轮流举起，开心眯眼
    int side = (micOnsetN % 2) ? 1 : -1;
    float k = min(1.0f, (now - micLastOnsetMs) / 300.0f);
    p.dx = side * (int)(12 * s * (1 - 0.4f * k));
    p.dy = -(int)(8 * s * (1 - k));
    p.armL = side > 0 ? 2 : 0;
    p.armR = side > 0 ? 0 : 2;
    p.legLift[0] = p.legLift[2] = side > 0 ? U * 3 / 7 : 0;
    p.legLift[1] = p.legLift[3] = side > 0 ? 0 : U * 3 / 7;
    p.eye = 2;
    p.squash = 0;
    for (int i = 0; i < 3; i++) {  // 往上飘的像素音符
      uint32_t nt = (now + i * 700) % 2100;
      int nx = CX - 5 * U + i * 5 * U + (int)(8 * s * sinf(nt / 300.0f)), ny = BASE - 6.7f * U - (int)(nt / 25 * s);
      uint16_t nc = i == 1 ? C_CLAUDE : C_YELLOW;
      int q = max(1, U / 7);
      gfx->fillRect(nx, ny, 2 * q, 12 * q, nc);
      gfx->fillRect(nx - 5 * q, ny + 9 * q, 6 * q, 5 * q, nc);
      gfx->fillRect(nx, ny, 6 * q, 2 * q, nc);
    }
  } else if (r == R_TYPE) {  // 跟你一起敲键盘：低头看键盘，两手交替，打得快就更快
    // 连着 bridge 时一键一下：每按一次键换一只手、身体轻轻一点；否则按节奏自己敲
    int ph = actFresh() ? (int)(macKeyN & 1) : (int)((now / (typingRate() >= 5 ? 80 : 140)) % 2);
    p.armL = ph ? 1 : 0;
    p.armR = ph ? 0 : 1;
    p.dy = (actFresh() ? now - macLastKeyAt < 90 : ph) ? -max(1, U / 7) : 0;
    p.lookX = 0;
    p.lookY = U / 3;
    p.eye = 0;
    p.squash = 0;
  } else if (r == R_SHH) {  // 有人说话：安静下来，侧耳听，头顶一个静音小喇叭
    p.dy = (int)(2 * s * sinf(now / 1200.0f));
    p.armL = 1;
    p.lookX = -U / 4;
    p.eye = (now % 4200) < 160 ? 1 : 0;
    p.squash = 0;
    int q = max(2, U / 4), x = CX + 3 * U, y = BASE - 12 * U;
    gfx->fillRect(x, y + 2 * q, 2 * q, 2 * q, P_DIM);  // 喇叭
    gfx->fillRect(x + 2 * q, y + q, q, 4 * q, P_DIM);
    gfx->fillRect(x + 3 * q, y, q, 6 * q, P_DIM);
    for (int k = 0; k < 4; k++) {  // ×
      gfx->fillRect(x + 5 * q + k * q, y + q + k * q, q, q, P_RED);
      gfx->fillRect(x + 8 * q - k * q, y + q + k * q, q, q, P_RED);
    }
  } else if (r == R_WAVE) {  // 欢迎回来：开心挥手、小跳
    int ph = (now / 220) % 2;
    p.armR = ph ? 2 : 1;
    p.armL = 0;
    p.eye = 2;
    p.dy = ph ? -U / 3 : 0;
    p.squash = 0;
  }
}

// 画在 Clawd 前面的东西
void reactPost(int r, uint32_t now) {
  if (r != R_TYPE) return;
  // 小键盘：挡住腿的下半截
  int kw = 9 * U, kx = CX - kw / 2, ky = BASE - U - 2, kh = max(6, U * 4 / 5);
  gfx->fillRect(kx, ky, kw, kh, P_EDGE);
  gfx->fillRect(kx, ky + kh, kw, 2, P_SHADOW);
  int q = max(2, U / 4), cols = (kw - q) / (q * 2);
  bool real = actFresh();
  uint32_t hit = real ? macKeyN : now / (typingRate() >= 5 ? 80 : 140);
  bool flash = !real || now - macLastKeyAt < 150;  // 真实按键：按下后亮 0.15 秒
  for (int row = 0; row < 2 && ky + q + row * 2 * q + q <= ky + kh; row++)
    for (int c = 0; c < cols; c++) {
      bool lit = flash && (hit * 7 + c * 3 + row * 5) % 11 == 0;
      gfx->fillRect(kx + q + c * 2 * q, ky + q / 2 + row * 2 * q + 1, q, q, lit ? C_CLAUDE : P_DIM);
    }
  if (typingRate() >= 5) {  // 打得飞快：键盘上蹦火花
    for (int i = 0; i < 3; i++) {
      if (((now / 90) + i) % 3 == 0) continue;
      int sx = kx + (int)((hit * 37 + i * 53) % kw), sy = ky - 4 - (int)((now / 30 + i * 7) % (2 * U));
      gfx->fillRect(sx, sy, 2, 2, C_YELLOW);
    }
  }
}

// ---------- 节能与唤醒 ----------
// 你离开（雷达看到座位没人 / 没雷达时看键鼠和声音）→ 麦克风暂停；离开满 N 分钟 → 开着「离开时显示时钟」就把时钟
// 调暗到约 10%（远远还能看时间），夜里 23:00～7:00 或者关了时钟才熄屏（背光关、不再画）。
// 你回来（雷达看到你、碰一下屏幕、Mac 上有键鼠操作）→ 马上亮屏、恢复麦克风。雷达本身一直开着，用来唤醒。
void wakeUp(uint32_t now) {
  if (screenOff) {
    screenOff = false;
    lastWakeAt = now;
    blLevel = -1;  // 强制下一帧重设背光
  }
}

void powerTick(uint32_t now) {
  bool away = userAway();
  if (away) {
    if (!awayAt) awayAt = now;
  } else {
    awayAt = 0;
    wakeUp(now);
  }
  bool idle = away && cfgSleepMin && now - awayAt > (uint32_t)cfgSleepMin * 60000;
  int lm = localMinute();
  bool night = lm >= 0 && (lm >= 23 * 60 || lm < 7 * 60);
  bool off = idle && (!cfgClock || night);
  clockDim = idle && !off;
  if (off && !screenOff) {
    screenOff = true;
    setBacklight(0);
  } else if (clockDim && screenOff) {  // 夜里熄的屏，到早上 7 点人还没回来：亮回调暗的时钟
    screenOff = false;
    blLevel = -1;
  }
  // 麦克风：离开时暂停，回来恢复（关掉「离开时暂停」就一直开着）
  if (cfgMicPause && away && micOn) micStop();
  else if ((!away || !cfgMicPause) && !micOn) micStart();
}
