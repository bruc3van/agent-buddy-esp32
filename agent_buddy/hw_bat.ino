// 锂电池（板子下沿 BAT 口）：电池电压经 160K / 220K 分压接 GPIO5（ADC1_CH4），系数 380/220 ≈ 1.73
// （原理图画的是 33K/100K，实际焊的是 160K/220K，同 xiaozhi-esp32 的 jc4827w543 配置）。
// 充电芯片的状态脚没接到 ESP32；这个点上还有充电 / 升压芯片的开关纹波，单次读数上下差 600 mV。
// 实测插着 USB 时，接不接电池读数都一样（芯片空载也把它撑到 4.2V 左右），所以：
//   插着 USB（有主机）→ 只显示「外接供电」，不显示电量；拔掉 USB 板子还在跑 → 一定是电池在供电，显示电量。
// 拔掉 USB 时板子会重启一次（3.3V 在升压接手前掉电），属正常。
// 充电 / 升压芯片是 U2 IP5306（不带 I2C 的版本）。SW1 接它的 KEY 脚，固件读不到：靠电池时短按开升压、
// 1 秒内连按两下关升压（整块板子断电）；插着 USB 时按了没用。IP5306 负载低于约 45mA 持续 32 秒会自己关掉升压，
// 低电量深睡时负载远低于这个数，半分钟后整块板子就真断电了。P1 的 +5V（雷达）只在插着 USB 时有电，靠电池时雷达不工作。
const int BAT_PIN = 5;
const float BAT_K = 380.0f / 220.0f;

float batV = 0;       // 平滑后的电池电压（V），0 = 还没采样
bool batUsb = false;  // 插着 USB（有主机）
bool batHas = false;  // 靠电池在跑
int batPct = -1;      // 电量 0~100，-1 = 不知道
uint32_t batAt = 0, batLogAt = 0;

void batInit() {
  analogSetPinAttenuation(BAT_PIN, ADC_11db);  // 量程约 0~3.1V，满电 4.2V 分压后约 2.43V
}

float batRead() {
  uint32_t mv = 0;
  for (int i = 0; i < 64; i++) mv += analogReadMilliVolts(BAT_PIN);
  return mv / 64.0f / 1000.0f * BAT_K;
}

// 低电量保护：电池快没电时一开屏幕 / Wi-Fi，升压芯片就扛不住，板子反复掉电重启，每次重启喇叭都「滴」一声。
// 所以电压太低就关背光、深睡，每分钟醒来量一次（插上 USB 后读数回到 4.2V 左右，就正常开机）。
// 开机门槛比运行时的关机线高一点：卸掉负载后电池电压会回弹，免得刚开机又掉下去来回循环。
// 光靠这点回差不够：静置后电压又能弹回 3.45V 以上，开机一带载就掉电重启，
// 滴滴声停不下来。所以低电量关过一次就在 NVS 里记下，之后要到 3.80V（充上电 / 插 USB）才开机；
// 靠电池时开机没撑过 20 秒就掉电（欠压复位，或连着两次没跑满 20 秒）也当成电池扛不住，同样处理。
const float BAT_CUT_V = 3.30f, BAT_BOOT_V = 3.45f, BAT_RESUME_V = 3.80f;
const float BAT_USB_V = 4.05f;  // 读数高于这个：插着 USB / 充电头（或电池是满的），不用防掉电重启
bool batBootPending = false;    // 这次开机记了「没跑满 20 秒」，跑满后清掉

// 功放 NS4168 没有使能脚，板子有电它就在工作，I2S 三根脚一悬空就会把噪声放出来（xiaozhi-esp32 的这块板子也有同样的说明）。
// 所以深睡前把这三根脚和背光脚都压低并锁住。LRCK（GPIO2）和背光在 RTC 域，锁定会保持到 batBootGuard 决定开机；
// 41、42 是数字域的脚，醒来后可能回到默认状态，这时靠 LRCK 被压住让功放收不到声道时钟（未实测）。
// 整块板子断过电时锁定就没了，从上电到 setup 这一小段固件管不到。
void batHoldPins(bool on) {
  const int pins[] = {GFX_BL, spkPin(0), spkPin(1), spkPin(2)};
  for (int pin : pins) {
    if (on) gpio_hold_en((gpio_num_t)pin);
    else gpio_hold_dis((gpio_num_t)pin);
  }
  if (on) gpio_deep_sleep_hold_en();
  else gpio_deep_sleep_hold_dis();
}

void batSleep() {
  spkShutdown();  // 运行中关机：先停掉播放，免得 DMA 反复播最后一块数据
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, LOW);
  batHoldPins(true);
  esp_sleep_enable_timer_wakeup(60ULL * 1000000);
  esp_deep_sleep_start();
}

// 开机时读 5 次取中位数，隔开一点时间，避开升压芯片的纹波和刚上电的抖动
float batReadStable() {
  float r[5];
  for (int i = 0; i < 5; i++) {
    r[i] = batRead();
    delay(5);
  }
  for (int i = 1; i < 5; i++)
    for (int j = i; j > 0 && r[j] < r[j - 1]; j--) {
      float t = r[j];
      r[j] = r[j - 1];
      r[j - 1] = t;
    }
  return r[2];
}

// 靠电池时每次上电记一笔（复位原因、电压、是否开机），插回 USB 后从串口打印，用来分辨「滴」声出在哪一步
const size_t BAT_TRACE_MAX = 120;  // 每条 2 字节，最多 60 条，写满就不再写
void batTraceAdd(Preferences &p, int reason, float v, bool sleep) {
  uint8_t buf[BAT_TRACE_MAX];
  size_t n = p.isKey("btrace") ? p.getBytes("btrace", buf, sizeof(buf)) : 0;
  if (n + 2 > sizeof(buf)) return;
  buf[n++] = (uint8_t)(reason & 0x7F) | (sleep ? 0x80 : 0);
  buf[n++] = (uint8_t)constrain((int)(v * 50), 0, 255);
  p.putBytes("btrace", buf, n);
}

void batTraceDump() {
  if (!prefs.isKey("btrace")) return;
  uint8_t buf[BAT_TRACE_MAX];
  size_t n = prefs.getBytes("btrace", buf, sizeof(buf));
  Host.printf("BATTRACE %u 条（r=复位原因 1上电 3软件 4崩溃 8深睡唤醒 9欠压；S=继续深睡 B=开机）:", (unsigned)(n / 2));
  for (size_t i = 0; i + 1 < n; i += 2) Host.printf(" r%d/%.2f/%c", buf[i] & 0x7F, buf[i + 1] / 50.0f, buf[i] & 0x80 ? 'S' : 'B');
  Host.println();
  prefs.remove("btrace");
}

// setup 一开始调用，此时屏幕、喇叭、Wi-Fi 都还没上电
void batBootGuard() {
  pinMode(GFX_BL, OUTPUT);  // 断过电时锁定已经没了：先压低，确定要开机才放开
  digitalWrite(GFX_BL, LOW);
  spkPinsLow();
  batInit();
  float v = batReadStable();
  Preferences p;
  p.begin("buddy", false);
  bool low = p.getBool("batlow", false);
  int fails = p.getUChar("batboot", 0);  // 靠电池开机、没跑满 20 秒就断电的次数
  int reason = esp_reset_reason();
  bool weak = v < BAT_USB_V && (reason == ESP_RST_BROWNOUT || fails >= 2);
  // 没有掉电记录时，读数 ≤ 2.8V 不可信（没接电池等），照常开机，但照样计入失败次数；
  // 已经掉过电就不再放行，不然电池彻底没电时读数更低，反而会一直开机、一直响
  bool sleep = (low || weak) ? v < BAT_RESUME_V : (v > 2.8f && v < BAT_BOOT_V);
  if (v < BAT_USB_V) batTraceAdd(p, reason, v, sleep);
  if (sleep) {
    if (!low) p.putBool("batlow", true);
    if (fails) p.remove("batboot");
    if (p.isKey("bsec")) {  // 一轮续航还没记完就扛不住了（运行中掉电重启）：到此为止，记成上次续航
      p.putUInt("blife", p.getUInt("bsec", 0));
      p.putChar("blpct", p.getChar("bpct", -1));
      p.remove("bsec");
      if (p.isKey("bpct")) p.remove("bpct");
    }
    p.end();
    batSleep();
  }
  batHoldPins(false);  // 确定开机了才放开，I2S 启动前三根脚一直是低电平
  if (low) p.remove("batlow");
  if (v < BAT_USB_V) {
    p.putUChar("batboot", min(fails + 1, 9));
    batBootPending = true;
  } else if (fails) {
    p.remove("batboot");
  }
  p.end();
}

// 单节锂电的放电曲线（带载电压 → 电量），中间段很平，只能当大概
int batPctOf(float v) {
  static const float V[] = {3.30, 3.50, 3.61, 3.67, 3.71, 3.75, 3.80, 3.86, 3.92, 4.00, 4.10, 4.20};
  static const int P[] = {0, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
  if (v <= V[0]) return 0;
  for (int i = 1; i < 12; i++)
    if (v < V[i]) return P[i - 1] + (int)((v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]));
  return 100;
}

// 续航：从拔掉 USB 开始累计靠电池跑的时间，一直到低电量关机，记成「上次续航」。
// 中途重启（拔 USB 那一下、掉电）接着累计：每分钟存一次 NVS，断电最多丢 1 分钟；插回 USB 就作废重新算。
// 同时记下起始电量，没从满电开始用的话，续航自然短。
uint32_t batRunBase = 0, batRunAt = 0, batRunSaveAt = 0;  // NVS 里已累计的秒数；这次开机从哪一刻开始算
int batRunPct = -1;                                       // 这一轮开始时的电量，-1 = 还没记
bool batRunOn = false;
uint32_t batLifeSec = 0;  // 上次续航（秒），0 = 还没有完整记录
int batLifePct = -1;      // 上次续航是从多少电量开始的

uint32_t batRunSec(uint32_t now) { return batRunOn ? batRunBase + (now - batRunAt) / 1000 : 0; }

void batRunClear() {
  if (prefs.isKey("bsec")) prefs.remove("bsec");
  if (prefs.isKey("bpct")) prefs.remove("bpct");
}

void batLifeLoad() {
  batLifeSec = prefs.getUInt("blife", 0);
  batLifePct = prefs.getChar("blpct", -1);
}

void batRunTick(uint32_t now) {
  if (batUsb) {  // 插着 USB：这一轮作废
    if (batRunOn) batRunClear();
    batRunOn = false;
    batRunBase = 0;
    batRunPct = -1;
    return;
  }
  if (!batHas) return;
  if (!batRunOn) {
    batRunOn = true;
    batRunBase = prefs.getUInt("bsec", 0);
    batRunPct = batRunBase ? prefs.getChar("bpct", -1) : -1;
    batRunAt = batRunSaveAt = now;
  }
  if (batRunPct < 0 && now > 15000) {  // 等平滑稳定了再记起始电量
    batRunPct = batPct;
    prefs.putChar("bpct", batRunPct);
  }
  if (now - batRunSaveAt >= 60000) {
    batRunSaveAt = now;
    prefs.putUInt("bsec", batRunSec(now));
  }
}

// 低电量关机前调用：这一轮记成上次续航
void batRunFinish(uint32_t now) {
  if (!batRunOn) return;
  batLifeSec = batRunSec(now);
  batLifePct = batRunPct;
  prefs.putUInt("blife", batLifeSec);
  prefs.putChar("blpct", batLifePct);
  batRunClear();
  Host.printf("BAT 续航 %lu 秒（从 %d%% 开始）\n", (unsigned long)batLifeSec, batLifePct);
}

// 每秒采 64 次取平均，再做慢速指数平滑（约 10 秒），把纹波压下去
void batTick(uint32_t now) {
  if (batAt && now - batAt < 1000) return;
  batAt = now;
  float v = batRead();
  batV = batV == 0 ? v : batV * 0.9f + v * 0.1f;
  batUsb = HWCDC::isPlugged();
  batHas = !batUsb && batV > 2.8f && batV < 4.5f;
  batPct = batHas ? batPctOf(batV) : -1;
  spkBatLow = batHas && batV < BAT_BOOT_V;  // 快没电时不出声：喇叭峰值电流最大，一响就可能把升压拉垮
  batRunTick(now);
  if (batUsb && now > 20000) batTraceDump();
  if (batHas && batV < BAT_CUT_V && now > 15000) {  // 开机先等平滑稳定，单次纹波不会触发
    batRunFinish(now);
    prefs.putBool("batlow", true);
    batSleep();
  }
  if (batBootPending && now > 20000) {  // 带着屏幕和 Wi-Fi 撑过 20 秒：电池扛得住
    batBootPending = false;
    prefs.remove("batboot");
  }

  if (now - batLogAt >= 30000 || !batLogAt) {  // bridge 日志里留一份（只在插着 USB 时看得到）
    batLogAt = now;
    Host.printf("BAT v=%.3f pct=%d usb=%d\n", batV, batPct, (int)batUsb);
  }
}

String batState() {
  if (batUsb) return "外接供电（USB）";
  return batHas ? "电池供电" : "—";
}

// 首页状态栏：靠电池时画 20×12 的像素电池（4 格电量，低于 20% 红色）；插着 USB 时画绿色插头
#define BAT_GREEN 0x3708  // #30E040：比界面里的 P_GREEN 更绿、更亮
void drawBatBadge(int x, int y) {
  if (batUsb) {
    const char *G[8] = {".1..1...", ".1..1...", "111111..", "111111..", "111111..", ".1111...", "..11....", "..11...."};
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++)
        if (G[r][c] == '1') gfx->fillRect(x + 4 + c * 2, y - 2 + r * 2, 2, 2, BAT_GREEN);
    return;
  }
  if (!batHas) return;
  uint16_t c = batPct < 20 ? P_RED : P_TEXT;
  gfx->fillRect(x, y, 18, 2, P_TEXT);  // 外壳
  gfx->fillRect(x, y + 10, 18, 2, P_TEXT);
  gfx->fillRect(x, y, 2, 12, P_TEXT);
  gfx->fillRect(x + 16, y, 2, 12, P_TEXT);
  gfx->fillRect(x + 18, y + 4, 2, 4, P_TEXT);  // 正极
  int bars = batPct < 20 ? 1 : (batPct + 24) / 25;  // 20~25% 一格 … 76~100% 四格
  for (int i = 0; i < bars; i++) gfx->fillRect(x + 3 + i * 3, y + 3, 2, 6, c);
}
