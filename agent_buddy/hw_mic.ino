// INMP441 麦克风（I2S）：P3 的 IO6=SCK、IO7=WS、IO15=SD，IO16 拉低当作 L/R 接地（左声道）
// 开机常开；后台任务每 16ms 算一次音量，并自动学习房间底噪，给 Clawd 的声音反应用：
//   突然大声 → 吓一跳；持续有节拍 → 跟着摇摆；长时间安静 → 打瞌睡
// 喇叭出声时不做分析（避免把自己的声音当成外界声音）。首页状态栏显示麦克风图标；调试时可让它经串口报音量（{"t":"ui","mic":1}）
#include <ESP_I2S.h>

I2SClass micI2S;
TaskHandle_t micTask = nullptr;
uint32_t micLogAt = 0;

void micAnalyze(double rmsDb, double peakDb, bool voiceLike) {
  uint32_t now = millis();
  micDb = rmsDb;
  micPeakDb = max((float)micPeakDb - 1.5f, (float)peakDb);
  if (spkPlaying()) return;
  float base = micBase;
  if (rmsDb < base + 6) base = base * 0.995f + rmsDb * 0.005f;  // 只用安静的片段慢慢学底噪
  micBase = constrain(base, -80.0f, -30.0f);
  if (rmsDb > micBase + 8) micLastSoundMs = now;
  // 有声位图：给「有人在说话」用（说话 = 断断续续、有起伏）
  uint32_t b = micBlockN % 192;
  if (rmsDb > micBase + 10) micLoudBits[b >> 3] |= 1 << (b & 7);
  else micLoudBits[b >> 3] &= ~(1 << (b & 7));
  if (rmsDb > micBase + 10 && voiceLike) micVoiceBits[b >> 3] |= 1 << (b & 7);
  else micVoiceBits[b >> 3] &= ~(1 << (b & 7));
  micBlockN++;
  // 键盘敲击声：前一块安静、这一块峰值突起、两三块内又安静下来（说话的音节要长得多）
  static float prevRms = -90;
  static uint32_t candAt = 0;
  static int candAge = 0;
  if (candAt) {
    if (rmsDb < micBase + 8) {
      micClicks[micClickN % 16] = candAt;
      micClickN++;
      candAt = 0;
    } else if (++candAge >= 3) {
      candAt = 0;
    }
  } else if (peakDb > micBase + 14 && prevRms < micBase + 8) {
    candAt = now;
    candAge = 0;
  }
  prevRms = rmsDb;
  if (peakDb > max(micBase + 30, -25.0f) && now - micStartleAt > 1500) micStartleAt = now;  // 突然大声
  // 节拍点：能量明显高于最近平均且高于底噪，间隔至少 180ms
  static float avg = -60;
  if (rmsDb > avg + 5 && rmsDb > micBase + 10 && now - micLastOnsetMs > 180) {
    micLastOnsetMs = now;
    micOnsets[micOnsetN % 8] = now;
    micOnsetN++;
  }
  avg = avg * 0.9f + rmsDb * 0.1f;
}

// 跟着音乐摇摆：最近 4 秒内至少 5 个节拍点、间隔比较均匀（说话的音节忽快忽慢），且声音持续不断
bool micDancing() {
  uint32_t now = millis(), t[8];
  int n = 0;
  for (int i = 0; i < 8; i++)
    if (micOnsets[i] && now - micOnsets[i] < 4000) t[n++] = micOnsets[i];
  if (n < 5 || now - micLastSoundMs > 600) return false;
  for (int i = 0; i < n; i++)  // 排序
    for (int j = i + 1; j < n; j++)
      if (t[j] < t[i]) { uint32_t x = t[i]; t[i] = t[j]; t[j] = x; }
  float mean = (float)(t[n - 1] - t[0]) / (n - 1), dev = 0;
  for (int i = 1; i < n; i++) dev += fabsf((t[i] - t[i - 1]) - mean);
  return dev / (n - 1) < 0.4f * mean;
}

void micLoop(void *) {
  static int32_t buf[256];  // 16kHz 下 16ms 一块
  while (true) {
    if (!micOn) { vTaskDelay(50); continue; }
    size_t n = micI2S.readBytes((char *)buf, sizeof(buf)) / 4;
    if (!n) { vTaskDelay(5); continue; }
    double sum = 0, mean = 0;
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) mean += (buf[i] >> 8);  // 24bit 数据放在 32bit 的高位
    mean /= n;
    for (size_t i = 0; i < n; i++) {
      double v = (buf[i] >> 8) - mean;
      sum += v * v;
      int32_t a = abs((int32_t)v);
      if (a > peak) peak = a;
    }
    double rms = sqrt(sum / n) / 8388608.0;
    // 频段：Goertzel 算 6 个频点的能量。人声的能量集中在 500～3000Hz，低频（250）和高频（5000）相对少
    float e[6], tot = 0;
    for (int b = 0; b < 6; b++) {
      float k = 2 * cosf(2 * PI * MIC_FREQ[b] / 16000), s1 = 0, s2 = 0;
      for (size_t i = 0; i < n; i++) {
        float s0 = ((buf[i] >> 8) - (float)mean) / 8388608.0f + k * s1 - s2;
        s2 = s1;
        s1 = s0;
      }
      e[b] = s1 * s1 + s2 * s2 - k * s1 * s2;
      tot += e[b];
      micBand[b] = e[b] > 0 ? 10 * log10f(e[b] / n) : -90;
    }
    bool voiceLike = tot > 0 && (e[1] + e[2] + e[3] + e[4]) / tot > 0.75f && e[5] / tot < 0.1f;
    micAnalyze(rms > 0 ? 20 * log10(rms) : -90, peak > 0 ? 20 * log10(peak / 8388608.0) : -90, voiceLike);
  }
}

void micStart() {
  if (micOn) return;
  pinMode(16, OUTPUT);
  digitalWrite(16, LOW);  // L/R = 低：左声道
  micI2S.setPins(6, 7, -1, 15);
  micI2S.setTimeout(100);
  if (!micI2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Host.println("MIC i2s 启动失败");
    return;
  }
  if (!micTask) xTaskCreatePinnedToCore(micLoop, "mic", 4096, nullptr, 1, &micTask, 0);
  micLastSoundMs = millis();
  micOn = true;
}

// 像素麦克风图标（7×8，同 Wi-Fi 图标的格子风格）：1 = 话筒，2 = 支架
const char *MIC_ICON[8] = {
  "..111..",
  "..111..",
  "..111..",
  "2.111.2",
  "2.....2",
  ".22222.",
  "...2...",
  ".22222.",
};

// 首页状态栏：放大 2 倍（14×16），(x, y) 为左上角。听到声音时话筒亮绿色；麦克风没启动时灰色 + 红斜杠
void drawMicBadge(int x, int y) {
  const int sc = 2;
  bool hear = micOn && !spkPlaying() && millis() - micLastSoundMs < 400;
  uint16_t body = !micOn ? P_DIM : (userTalking() ? C_CLAUDE : (hear ? P_GREEN : P_TEXT)), stand = micOn ? P_TEXT : P_DIM;
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 7; c++) {
      char ch = MIC_ICON[r][c];
      if (ch != '.') gfx->fillRect(x + c * sc, y + r * sc, sc, sc, ch == '1' ? body : stand);
    }
  if (!micOn)
    for (int k = 0; k < 7; k++) gfx->fillRect(x + k * sc, y + (k * 8 / 7) * sc, sc, sc, P_RED);
}

// 调试用（{"t":"ui","mic":1}）：每 0.5 秒经串口报 "MIC ..."，不在屏幕上画
void micLogTick() {
  if (!micOn || !micBar || millis() - micLogAt < 500) return;
  micLogAt = millis();
  Host.printf("MIC rms=%.1f peak=%.1f base=%.1f dance=%d\n", (float)micDb, (float)micPeakDb, (float)micBase, micDancing());
}

// 暂停采集（你离开时）：先让采集任务停手，再关 I2S
void micStop() {
  if (!micOn) return;
  micOn = false;
  delay(150);  // 等采集任务这一轮 readBytes 结束（超时 100ms）
  micI2S.end();
}
