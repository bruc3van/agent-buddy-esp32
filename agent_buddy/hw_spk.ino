// 板载 I2S 功放 NS4168（Speak 口）：BCLK=42、LRCLK=2、DOUT=41，必须 32bit + 单声道 + 左声道（同 xiaozhi-esp32 的配置）
// 音频引擎：常驻任务从 PSRAM 环形缓冲区取 16kHz/16bit PCM 写入 I2S；没有声音时持续写静音
// （停写会让 DMA 重复最后一块数据，变成嗡嗡声）。
// 声音来源：板上合成的 8-bit 音效（sfx），以及 bridge 发来的语音（{"t":"au","d":base64 PCM}）。
#include <ESP_I2S.h>
#include "mbedtls/base64.h"

I2SClass spkI2S;
int spkBclk = 42, spkWs = 2, spkDout = 41;
bool spkReady = false;
int spkVol = 50;  // 0~100，存 NVS

const uint32_t RB_N = 16000 * 20;  // 20 秒缓冲
int16_t *rb = nullptr;
volatile uint32_t rbHead = 0, rbTail = 0;  // 单写单读，递增计数，取模定位
volatile uint32_t spkLastAudioMs = 0;
TaskHandle_t spkTask = nullptr;
volatile bool spkHold = false, spkHeld = false;  // 换引脚时让播放任务先停手（它停在 write 里时不能关 I2S）
volatile bool spkDrop = false;  // 丢掉已排队的声音（只能由播放任务挪 rbTail）

void spkFlush() { spkDrop = true; }

uint32_t rbCount() { return rbHead - rbTail; }

void rbPush(const int16_t *s, uint32_t n) {
  if (!rb) return;
  for (uint32_t i = 0; i < n && rbCount() < RB_N; i++) {
    rb[rbHead % RB_N] = s[i];
    rbHead++;
  }
}

// 正在出声（含刚播完的 300ms 余音）：麦克风此时不做反应，避免把自己的声音当成外界声音
bool spkPlaying() { return rbCount() > 0 || millis() - spkLastAudioMs < 300; }

void spkLoop(void *) {
  static int32_t out[256];  // 16ms
  while (true) {
    if (spkHold || !spkReady) {
      spkHeld = true;
      vTaskDelay(10);
      continue;
    }
    spkHeld = false;
    if (spkDrop || spkMute) {  // 静音立刻生效：缓冲里最多还有 20 秒语音，不能接着念完
      spkDrop = false;
      rbTail = rbHead;
    }
    uint32_t avail = rbCount();
    if (avail) {
      float g = spkVol / 100.0f;
      uint32_t n = min(avail, (uint32_t)256);
      for (uint32_t i = 0; i < 256; i++) {
        int16_t v = i < n ? rb[(rbTail + i) % RB_N] : 0;
        out[i] = (int32_t)(v * g) << 16;
      }
      rbTail += n;
      spkLastAudioMs = millis();
    } else {
      memset(out, 0, sizeof(out));
    }
    spkI2S.write((uint8_t *)out, sizeof(out));
  }
}

void spkEnd() {
  if (!spkReady) return;
  spkI2S.end();
  spkReady = false;
}

bool spkBegin();
void spkSetPins(int bclk, int ws, int dout) {  // -1 表示保持不变
  spkHeld = false;
  spkHold = true;
  for (int i = 0; spkTask && !spkHeld && i < 100; i++) delay(2);  // 每次 write 最多 16ms
  spkEnd();
  if (bclk >= 0) spkBclk = bclk;
  if (ws >= 0) spkWs = ws;
  if (dout >= 0) spkDout = dout;
  spkBegin();
  spkHold = false;
}

bool spkBegin() {
  if (spkReady) return true;
  spkI2S.setPins(spkBclk, spkWs, spkDout);
  if (!spkI2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Host.println("SPK i2s 启动失败");
    return false;
  }
  spkReady = true;
  return true;
}

void audioInit() {
  spkVol = prefs.getUChar("vol", 50);
  rb = (int16_t *)ps_malloc(RB_N * 2);
  if (!rb || !spkBegin()) return;
  xTaskCreatePinnedToCore(spkLoop, "spk", 4096, nullptr, 2, &spkTask, 0);
}

void spkSetVolume(int v) {
  spkVol = constrain(v, 0, 100);
  prefs.putUChar("vol", spkVol);
}

// ---------- 8-bit 音效 ----------
// 方波（wave=0）或三角波（wave=1），3ms 淡入、15ms 淡出，避免爆音
void sfxTone(float f, int ms, float amp, int wave) {
  static int16_t buf[512];
  const int rate = 16000;
  int n = rate * ms / 1000, att = rate * 3 / 1000, rel = rate * 15 / 1000;
  for (int i = 0; i < n;) {
    int k = min(512, n - i);
    for (int j = 0; j < k; j++, i++) {
      float ph = fmodf((float)i * f / rate, 1.0f);
      float w = wave == 0 ? (ph < 0.5f ? 1.0f : -1.0f) : (ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph);
      float env = min(1.0f, min((float)i / att, (float)(n - i) / rel));
      buf[j] = (int16_t)(32767 * amp * env * w);
    }
    rbPush(buf, k);
  }
}

void sfxRest(int ms) {
  static int16_t z[256] = {0};
  for (int n = 16000 * ms / 1000; n > 0; n -= 256) rbPush(z, min(n, 256));
}

void sfxPlay(const char *name) {
  if (!cfgSfx || spkMute) return;
  if (!strcmp(name, "start")) {  // 开始思考：轻轻一声
    sfxTone(1320, 45, 0.18f, 1);
  } else if (!strcmp(name, "done")) {  // 完成：上行琶音
    const float notes[4] = {784, 988, 1175, 1568};
    for (int i = 0; i < 4; i++) sfxTone(notes[i], i == 3 ? 180 : 70, 0.22f, 0);
  } else if (!strcmp(name, "alert")) {  // 等你确认：叮咚
    sfxTone(988, 130, 0.25f, 0);
    sfxRest(50);
    sfxTone(1319, 200, 0.25f, 0);
  } else if (!strcmp(name, "error")) {  // 出错：下行
    sfxTone(660, 120, 0.22f, 0);
    sfxTone(523, 120, 0.22f, 0);
    sfxTone(392, 240, 0.22f, 0);
  } else if (!strcmp(name, "cstart")) {  // Codex 用三角波，音色更软、音高不同，一听就分得出是谁
    sfxTone(880, 50, 0.25f, 1);
  } else if (!strcmp(name, "cdone")) {  // Codex 完成：大三和弦往下再往上
    const float notes[4] = {1047, 784, 1047, 1319};
    for (int i = 0; i < 4; i++) sfxTone(notes[i], i == 3 ? 200 : 80, 0.3f, 1);
  } else if (!strcmp(name, "calert")) {  // Codex 等确认：嘀嘀嘀
    for (int i = 0; i < 3; i++) {
      sfxTone(1175, 70, 0.3f, 1);
      sfxRest(40);
    }
  } else if (!strcmp(name, "cerror")) {
    sfxTone(523, 150, 0.3f, 1);
    sfxTone(370, 260, 0.3f, 1);
  } else if (!strcmp(name, "cpoke")) {  // 点 Codex：哔啵
    sfxTone(1568, 40, 0.25f, 1);
    sfxTone(1175, 70, 0.25f, 1);
  } else if (!strcmp(name, "dstart")) {  // DeepSeek 用快速上滑的三角波，像冒一个水泡
    sfxTone(660, 25, 0.25f, 1);
    sfxTone(990, 35, 0.25f, 1);
  } else if (!strcmp(name, "ddone")) {  // DeepSeek 完成：一串泡泡往上冒，最后一声长的
    const float notes[4] = {880, 1175, 1397, 1760};
    for (int i = 0; i < 4; i++) {
      sfxTone(notes[i] * 0.75f, 20, 0.25f, 1);
      sfxTone(notes[i], i == 3 ? 200 : 50, 0.28f, 1);
    }
  } else if (!strcmp(name, "dalert")) {  // DeepSeek 等确认：咕噜、咕噜
    for (int i = 0; i < 2; i++) {
      sfxTone(587, 40, 0.3f, 1);
      sfxTone(880, 90, 0.3f, 1);
      sfxRest(90);
    }
  } else if (!strcmp(name, "derror")) {
    sfxTone(494, 140, 0.3f, 1);
    sfxTone(330, 260, 0.3f, 1);
  } else if (!strcmp(name, "dpoke")) {  // 点小鲸鱼：噗（喷水）
    sfxTone(700, 30, 0.25f, 1);
    sfxTone(1050, 30, 0.25f, 1);
    sfxTone(1400, 70, 0.25f, 1);
  } else if (!strcmp(name, "poke")) {  // 点 Clawd：啵
    sfxTone(1047, 50, 0.2f, 0);
    sfxTone(1568, 80, 0.2f, 0);
  }
}

// 测试用：响 n 声 880Hz
void spkBeep(int n, int vol) {
  if (vol >= 0) spkSetVolume(vol);
  for (int k = 0; k < n; k++) {
    sfxTone(880, 250, 0.6f, 1);
    sfxRest(200);
  }
  Host.printf("SPK 已播放 %d 声（音量 %d）\n", n, spkVol);
}

// bridge 发来的语音片段：base64 编码的 16kHz / 16bit / 单声道 PCM
void spkPushB64(const char *b64) {
  static uint8_t pcm[6144];
  size_t olen = 0;
  if (mbedtls_base64_decode(pcm, sizeof(pcm), &olen, (const unsigned char *)b64, strlen(b64)) == 0)
    rbPush((int16_t *)pcm, olen / 2);
}
