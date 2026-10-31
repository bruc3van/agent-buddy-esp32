// 锂电池（板子下沿 BAT 口）：电池电压经 160K / 220K 分压接 GPIO5（ADC1_CH4），系数 380/220 ≈ 1.73
// （原理图画的是 33K/100K，实际焊的是 160K/220K，同 xiaozhi-esp32 的 jc4827w543 配置）。
// 充电芯片的状态脚没接到 ESP32；这个点上还有充电 / 升压芯片的开关纹波，单次读数上下差 600 mV。
// 实测插着 USB 时，接不接电池读数都一样（芯片空载也把它撑到 4.2V 左右），所以：
//   插着 USB（有主机）→ 只显示「外接供电」，不显示电量；拔掉 USB 板子还在跑 → 一定是电池在供电，显示电量。
// 拔掉 USB 时板子会重启一次（3.3V 在升压接手前掉电），属正常。SW1 接充电芯片的 KEY 脚（唤醒升压），固件读不到。
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

// 单节锂电的放电曲线（带载电压 → 电量），中间段很平，只能当大概
int batPctOf(float v) {
  static const float V[] = {3.30, 3.50, 3.61, 3.67, 3.71, 3.75, 3.80, 3.86, 3.92, 4.00, 4.10, 4.20};
  static const int P[] = {0, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
  if (v <= V[0]) return 0;
  for (int i = 1; i < 12; i++)
    if (v < V[i]) return P[i - 1] + (int)((v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]));
  return 100;
}

// 每秒采 64 次取平均，再做慢速指数平滑（约 10 秒），把纹波压下去
void batTick(uint32_t now) {
  if (batAt && now - batAt < 1000) return;
  batAt = now;
  uint32_t mv = 0;
  for (int i = 0; i < 64; i++) mv += analogReadMilliVolts(BAT_PIN);
  float v = mv / 64.0f / 1000.0f * BAT_K;
  batV = batV == 0 ? v : batV * 0.9f + v * 0.1f;
  batUsb = HWCDC::isPlugged();
  batHas = !batUsb && batV > 2.8f && batV < 4.5f;
  batPct = batHas ? batPctOf(batV) : -1;

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
