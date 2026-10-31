// HLK-LD2450 毫米波雷达：接板子 P1（UART0），256000 波特率，开机后持续上报最多 3 个目标的位置。
// 数据帧：AA FF 03 00 + 3×8 字节目标 + 55 CC；每个目标 x、y、速度各 2 字节（最高位 1 为正、0 为负），再 2 字节距离分辨率。
// P1 的 TXD / RXD 对应 GPIO43 / GPIO44；上电 3 秒内收不到数据就把收发脚对调再试。

HardwareSerial radarSerial(1);
int radarRx = 44, radarTx = 43;
uint32_t radarStartAt = 0, radarLogAt = 0, radarBytes = 0, radarModeAt = 0;
uint8_t radarModeTries = 0;
uint8_t radarBuf[30];
int radarLen = 0;

void radarBegin() {
  radarSerial.end();
  radarSerial.setRxBufferSize(1024);
  radarSerial.begin(256000, SERIAL_8N1, radarRx, radarTx);
  radarStartAt = radarModeAt = millis();
  radarLen = 0;
}

// 配置命令：FD FC FB FA + 长度(2) + 命令字(2) + 参数 + 04 03 02 01。回复（ACK）同样的帧头帧尾，命令字 | 0x0100
void radarCmd(uint16_t cmd, const uint8_t *val = nullptr, int n = 0);
void radarCmd(uint16_t cmd, const uint8_t *val, int n) {
  uint8_t f[32] = {0xFD, 0xFC, 0xFB, 0xFA, (uint8_t)(2 + n), 0, (uint8_t)(cmd & 0xFF), (uint8_t)(cmd >> 8)};
  int k = 8;
  for (int i = 0; i < n; i++) f[k++] = val[i];
  const uint8_t tail[4] = {0x04, 0x03, 0x02, 0x01};
  for (int i = 0; i < 4; i++) f[k++] = tail[i];
  radarSerial.write(f, k);
  radarSerial.flush();
  delay(60);
}

// 切到多目标跟踪（最多 3 人）：开启配置 → 多目标 → 查询模式 → 结束配置。出厂可能是单目标模式，只会报一个人
uint8_t radarMode = 0;  // 查询到的模式：1 单目标 / 2 多目标，0 未知
void radarMultiTarget() {
  radarMode = 0;
  const uint8_t en[2] = {0x01, 0x00};
  radarCmd(0x00FF, en, 2);
  radarCmd(0x0090);
  radarCmd(0x0091);
  radarCmd(0x00FE);
}

void radarInit() {
  radarBegin();
  radarMultiTarget();
}

int16_t ldVal(uint8_t lo, uint8_t hi) {
  uint16_t r = lo | (hi << 8);
  int16_t v = r & 0x7FFF;
  return (r & 0x8000) ? v : -v;
}

// 雷达原样报来的目标先过一遍滤波，radarT 里只留「确实是一个人」的：
// - 最近 5 帧里有 3 帧报了才算：多径反射闪一下的鬼影不算；被你挡住一半、隔帧才报一次的人也能确认
// - 两个点相距 0.45 米以内当成一个人：近距离时雷达常把一个人的头和身子拆成两个点
// - 人静止时雷达偶尔丢几帧：1.5 秒内保留上次的位置，人数不跟着跳
// - 雷达跟丢后常把同一个人换到另一个槽位重新报：新目标出现时，0.8 秒内刚丢的、1.5 米内的旧位置当成同一个人，
//   不再保留。座位上的你只认 0.6 米内的（不然走过来的人一确认，就把正被保留的你当成换槽删掉了）
// 每条轨迹还记两件事：按位置算的移动速度（雷达报的速度只是朝向 / 远离雷达的那部分，横着走的人会是 0），
// 以及它有没有在离座位 1.3 米外出现过（从远处走过来的，不会是坐着的你）
const int RT_HITS = 3, RT_MERGE = 450, RT_JUMP = 1500, RT_JUMP_SEAT = 600, RT_FAR = 1300;
const uint32_t RT_HOLD = 1500, RT_SWAP = 800;
RadarTarget radarRaw[3];
uint8_t radarHist[3];   // 最近 5 帧有没有报（最低位是这一帧）
uint8_t radarAge[3];    // 轨迹确认后过了几帧：合并时留老的
bool radarConf[3];      // 轨迹已确认、还活着（含保留期）；radarT[i].on 再扣掉被合并掉的
bool radarFar[3];       // 这条轨迹在离座位 1.3 米外出现过
bool radarWasMe[3];     // 这条轨迹在座位上被认作过「你」（你起身走开，走到哪都还是你）
int16_t radarSpd[3];    // 最近约 1 秒按位置算的速度（厘米/秒），刚出现还算不出来时是 999
uint32_t radarSeenAt[3];
struct RtPos { int16_t x, y; uint32_t t; };
const int RT_PATH = 12;
RtPos radarPath[3][RT_PATH];
uint8_t radarPathN[3], radarPathHead[3];
int presRefX = 0, presRefY = 700;  // 你的位置：标定过是座位，否则是最近一次在座位区看到你的地方（presenceTick 更新）
bool presRefSeen = false;          // 没标定时，是不是真的在座位区看到过你
float behindScore = 0;             // 毫秒：看到「有人靠近」就往上加，看不到就慢慢往下减
int behindIdx = -1;                // 这一帧算作「有人靠近」的那个目标，没有是 -1
bool ghostOn = false;              // 盲区里记着一个人（见 ghostTick）
int ghostX = 0, ghostY = 0;
uint32_t ghostAt = 0;

// 记下这一帧的位置，用约 1 秒前的位置算速度（至少隔 0.5 秒才算，免得被雷达的抖动带偏）
void radarTrack(int i, uint32_t now) {
  RtPos *p = radarPath[i];
  p[radarPathHead[i]] = {radarRaw[i].x, radarRaw[i].y, now};
  radarPathHead[i] = (radarPathHead[i] + 1) % RT_PATH;
  if (radarPathN[i] < RT_PATH) radarPathN[i]++;
  for (int k = radarPathN[i] - 1; k >= 1; k--) {  // k 帧之前，从最老的往新的找
    const RtPos &o = p[(radarPathHead[i] + RT_PATH - 1 - k) % RT_PATH];
    uint32_t dt = now - o.t;
    if (dt > 1200) continue;
    if (dt >= 500) radarSpd[i] = (int16_t)min(999.0f, hypotf(radarRaw[i].x - o.x, radarRaw[i].y - o.y) * 100 / dt);
    break;
  }
}

void radarFilter(uint32_t now) {
  for (int i = 0; i < 3; i++) radarT[i].on = radarConf[i];  // 上一帧被合并掉的先放回来，下面重新合并
  for (int i = 0; i < 3; i++) {
    const RadarTarget &r = radarRaw[i];
    radarHist[i] = ((radarHist[i] << 1) | r.on) & 0x1F;
    if (r.on && (radarConf[i] || __builtin_popcount(radarHist[i]) >= RT_HITS)) {
      if (!radarConf[i]) {  // 新轨迹：看是不是刚丢的那个人换了个槽位
        radarConf[i] = true;
        radarAge[i] = 0;
        radarFar[i] = radarWasMe[i] = false;
        radarSpd[i] = 999;
        radarPathN[i] = 0;
        int k = -1;
        long best = (long)RT_JUMP * RT_JUMP;
        for (int j = 0; j < 3; j++) {
          if (j == i || !radarConf[j] || radarRaw[j].on || now - radarSeenAt[j] > RT_SWAP) continue;
          long dx = radarT[j].x - r.x, dy = radarT[j].y - r.y, d2 = dx * dx + dy * dy;
          long lim = inSeat(radarT[j]) ? (long)RT_JUMP_SEAT * RT_JUMP_SEAT : (long)RT_JUMP * RT_JUMP;
          if (d2 < lim && d2 < best) { best = d2; k = j; }
        }
        if (k >= 0) {
          radarConf[k] = radarT[k].on = false;
          radarFar[i] = radarFar[k];
          radarWasMe[i] = radarWasMe[k];
        }
      }
      if (radarAge[i] < 255) radarAge[i]++;
      radarT[i] = r;
      radarSeenAt[i] = now;
      radarTrack(i, now);
      long dx = r.x - presRefX, dy = r.y - presRefY;
      if (dx * dx + dy * dy > (long)RT_FAR * RT_FAR) radarFar[i] = true;
    } else if (!r.on && radarConf[i] && now - radarSeenAt[i] > RT_HOLD) {
      radarConf[i] = radarT[i].on = false;
    }
  }
  // 合并：留下正在被看到的、跟得久的那个
  for (int i = 0; i < 3; i++)
    for (int j = i + 1; j < 3; j++) {
      RadarTarget &a = radarT[i], &b = radarT[j];
      if (!a.on || !b.on || abs(a.x - b.x) > RT_MERGE || abs(a.y - b.y) > RT_MERGE) continue;
      if ((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) > RT_MERGE * RT_MERGE) continue;
      int sa = radarAge[i] + (radarRaw[i].on ? 1000 : 0), sb = radarAge[j] + (radarRaw[j].on ? 1000 : 0);
      (sa >= sb ? b : a).on = false;
    }
}

void radarParse() {
  for (int i = 0; i < 3; i++) {
    const uint8_t *b = radarBuf + 4 + i * 8;
    RadarTarget &t = radarRaw[i];
    t.x = ldVal(b[0], b[1]);
    t.y = ldVal(b[2], b[3]);
    t.v = ldVal(b[4], b[5]);
    t.on = b[0] || b[1] || b[2] || b[3];
  }
  radarFrameAt = millis();
  radarFrames++;
  radarFilter(radarFrameAt);
}

void radarTick() {
  static const uint8_t HEAD[4] = {0xAA, 0xFF, 0x03, 0x00}, ACKH[4] = {0xFD, 0xFC, 0xFB, 0xFA};
  static uint8_t ack[32];
  static int ackLen = 0;
  while (radarSerial.available()) {
    uint8_t c = radarSerial.read();
    radarBytes++;
    // 配置回复：FD FC FB FA + 长度(2) + 内容 + 04 03 02 01，按长度收完整一帧（各命令回复长短不一，
    // 按固定长度收会吞掉下一条回复的帧头）。查询模式（0x0191）的回复里带模式值
    if (ackLen < 4) {
      if (c == ACKH[ackLen]) ack[ackLen++] = c;
      else ackLen = c == ACKH[0] ? 1 : 0;
    } else {
      ack[ackLen++] = c;
      int total = ackLen >= 6 ? 10 + ack[4] : 0;
      if (total > (int)sizeof(ack)) ackLen = 0;
      else if (ackLen == total) {
        if (radarLog) {  // 调试：打出配置回复的原始字节
          Host.print("RADAR ack");
          for (int i = 4; i < total - 4; i++) Host.printf(" %02X", ack[i]);
          Host.println();
        }
        if (ack[6] == 0x91 && ack[7] == 0x01 && total >= 16) {
          radarMode = ack[10];
          Host.printf("RADAR mode=%s\n", radarMode == 2 ? "多目标" : (radarMode == 1 ? "单目标" : "?"));
        }
        ackLen = 0;
      }
      continue;
    }
    if (radarLen < 4) {  // 找帧头
      if (c == HEAD[radarLen]) radarBuf[radarLen++] = c;
      else radarLen = (c == HEAD[0]) ? 1 : 0;
      if (radarLen == 1) radarBuf[0] = c;
      continue;
    }
    radarBuf[radarLen++] = c;
    if (radarLen == 30) {
      if (radarBuf[28] == 0x55 && radarBuf[29] == 0xCC) radarParse();
      radarLen = 0;
    }
  }
  uint32_t now = millis();
  if (dbgRadarDemo && now < dbgUntil) {  // 演示：你坐在前面轻轻晃，另一个人从后面走过
    float t = now / 1000.0f;
    radarT[0] = {(int16_t)(60 * sinf(t)), (int16_t)(420 + 30 * sinf(t * 1.3f)), 0, true};
    radarT[1] = {(int16_t)(-1200 + (int)(fmodf(t * 500, 2400))), 1900, 50, true};
    radarT[2].on = false;
    radarSpd[0] = 5;  // 演示数据不经过滤波，速度直接给：路过的那个人走 0.5 米/秒，不算驻足
    radarSpd[1] = 50;
    radarFrameAt = now;
    radarFrames++;
  }
  if (!radarFrames && now - radarStartAt > 3000) {  // 一直没数据：收发脚对调再试
    int t = radarRx; radarRx = radarTx; radarTx = t;
    radarBegin();
  }
  // 开机时雷达可能还没准备好、或者收发脚对调过，那次设置没生效：收到数据后确认一下是不是多目标，不是就再设（最多 3 次）。
  // 单目标模式下两个人只会报一个
  if (radarFrames && radarMode != 2 && radarModeTries < 3 && now - radarModeAt > 3000) {
    radarModeTries++;
    radarModeAt = now;
    radarMultiTarget();
  }
  if (radarLog && now - radarLogAt > 1000) {  // 原始目标；* 表示滤波后算作一个人，s 按位置算的速度，F 从远处来的，G 盲区里记着的人
    radarLogAt = now;
    Host.printf("RADAR rx=%d bytes=%lu frames=%lu mode=%d", radarRx, radarBytes, radarFrames, radarMode);
    for (int i = 0; i < 3; i++)
      if (radarRaw[i].on || radarT[i].on)
        Host.printf(" t%d%s=(%d,%d,%d) s=%d%s", i, radarT[i].on ? "*" : "", radarRaw[i].x, radarRaw[i].y, radarRaw[i].v,
                      radarSpd[i], radarFar[i] ? "F" : "");
    if (ghostOn) Host.printf(" G=(%d,%d)", ghostX, ghostY);
    Host.printf(" ref=(%d,%d) score=%d\n", presRefX, presRefY, (int)behindScore);
  }
}

// ---------- 在不在座位、有没有人靠近 ----------
// 座位区：标定过就以你的座位为中心、左右各 0.5 米，从屏幕前一直到座位后 0.5 米（你往前凑近看屏幕也还算在座位上）；
// 没标定时用雷达正前方 1.5 米内、左右各 0.8 米。人完全静止时雷达偶尔会丢目标，所以 8 秒内看到过就算在。
// 有人靠近：见 behindTarget() 和 ghostTick()（被你挡住的人）。只提醒，不遮挡屏幕上的内容。
const int SEAT_Y = 1500, SEAT_X = 800, SEAT_R = 500;
uint32_t behindGoneAt = 0, presLogAt = 0;
int lastPresP = -1, lastPresPv = -1, lastPresN = -1;

bool radarFresh() { return radarFrameAt && millis() - radarFrameAt < 2000; }

int radarCount() {
  int n = 0;
  for (auto &t : radarT) n += t.on;
  return n;
}

bool inSeat(const RadarTarget &t) {
  if (!t.on) return false;
  if (seatCal) return abs(t.x - seatCalX) < SEAT_R && t.y > 0 && t.y < seatCalY + SEAT_R;
  return t.y > 0 && t.y < SEAT_Y && abs(t.x) < SEAT_X;
}

// 「你」：在座位区里、离座位中心最近的那个，座位区没人返回 -1
int radarSeatMe() {
  int k = -1, best = 1 << 30;
  for (int i = 0; i < 3; i++) {
    if (!inSeat(radarT[i])) continue;
    int dx = radarT[i].x - (seatCal ? seatCalX : 0), dy = radarT[i].y - (seatCal ? seatCalY : 0);
    int d = dx * dx / 1000 + dy * dy / 1000;
    if (d < best) { best = d; k = i; }
  }
  return k;
}

// 界面上标「你」的那个：座位区没人时退回离雷达最近的，都没有返回 -1
int radarNearest() {
  int k = radarSeatMe();
  if (k >= 0) return k;
  for (int i = 0; i < 3; i++)
    if (radarT[i].on && (k < 0 || radarT[i].y < radarT[k].y)) k = i;
  return k;
}

bool radarSeatPresent() { return radarFresh() && seatSeenAt && millis() - seatSeenAt < 8000; }

// 标定座位：5 秒内取离雷达最近（直线距离 2.5 米内）那个人的平均位置。
// 只用新收到的雷达帧、而且是这一帧真看到的人（不用滤波保留的旧位置），够 20 帧（约 2 秒）才算数
void seatCalibStart() {
  calibUntil = millis() + 5000;
  calibSumX = calibSumY = calibN = 0;
  calibMsg = "坐好别动";
}

void seatCalibTick(uint32_t now) {
  static uint32_t lastFrame = 0;
  if (!calibUntil) return;
  if (radarFrames != lastFrame) {
    lastFrame = radarFrames;
    int k = -1;
    long best = 2500L * 2500;
    for (int i = 0; i < 3; i++) {
      const RadarTarget &t = radarT[i];
      long d = (long)t.x * t.x + (long)t.y * t.y;
      if (t.on && radarRaw[i].on && d < best) { best = d; k = i; }
    }
    if (k >= 0) { calibSumX += radarT[k].x; calibSumY += radarT[k].y; calibN++; }
  }
  if (now < calibUntil) return;
  calibUntil = 0;
  if (calibN < 20) { calibMsg = "没看到人，再试一次"; return; }
  seatCal = true;
  seatCalX = calibSumX / calibN;
  seatCalY = calibSumY / calibN;
  prefs.putBool("seatcal", true);
  prefs.putShort("seatx", seatCalX);
  prefs.putShort("seaty", seatCalY);
  calibMsg = "已标定";
}

void seatCalibClear() {
  seatCal = false;
  prefs.putBool("seatcal", false);
  calibMsg = "";
}

// 用雷达判断「离开」：座位区 60 秒没人，而且 Mac 键鼠 2 分钟没动（人坐着不动时雷达偶尔会丢目标）
bool radarSaysAway() {
  if (!cfgRadarSeat || !radarFresh()) return false;
  if (actFresh() && actInpSec >= 0 && actInpSec < 120) return false;
  return !seatSeenAt || millis() - seatSeenAt > 60000;
}
bool radarActive() { return cfgRadarSeat && radarFresh(); }

// 你在不在：雷达 8 秒内在座位区见过人，或者 Mac 键鼠 30 秒内有操作。
// 人坐着不动时 LD2450 会把人跟丢（实测能整整两分钟一个目标都不报），只认「这一帧看到你」会漏掉大半时间
bool youPresent() { return radarSeatPresent() || (actFresh() && actInpSec >= 0 && actInpSec < 30); }
bool youTyping() { return actFresh() && actInpSec >= 0 && actInpSec < 5; }

// 你身后的盲区：24GHz 毫米波穿不过人体，从雷达看过去，你左右各约 0.25 米宽，
// 身后就是一个越往远越宽的「阴影锥」（多留 4°），里面的人雷达基本看不到。返回半角（弧度）
float shadowHalf(int rx, int ry) {
  float d0 = sqrtf((float)rx * rx + (float)ry * ry);
  return atanf(250.0f / max(d0, 250.0f)) + 0.07f;
}
bool inShadow(int x, int y, int rx, int ry) {
  float d0 = sqrtf((float)rx * rx + (float)ry * ry), d = sqrtf((float)x * x + (float)y * y);
  if (d < d0 + 150) return false;
  return fabsf(atan2f(x, y) - atan2f(rx, ry)) < shadowHalf(rx, ry);
}
long dist2(int ax, int ay, int bx, int by) { return (long)(ax - bx) * (ax - bx) + (long)(ay - by) * (ay - by); }

// 一个目标是不是「靠近你的另一个人」（在你身后或身边）。参照点是你的位置（presRefX / presRefY）：
// - 离你 0.6 米以内的不算：近距离时雷达常把一个人拆成两个点（雷达本身的距离分辨率也只有约 0.6 米）
// - 和你几乎同一方向、距离几乎正好是你两倍的不算：雷达波经你身体再反射回来的「镜像」。
//   范围收得很窄（偏 5%、差 0.15 米内）：站在你椅子后面的人也常在这条线上，放宽了会把真人一起滤掉
// - 从雷达看过去比你远 0.3～3 米、偏离你那条视线不超过 1 米（越远放得越宽），算「身后」
// - 离座位 1.2 米以内的也算：站在你身边一样看得到屏幕
bool behindTarget(const RadarTarget &t, int rx, int ry) {
  float d0 = sqrtf((float)rx * rx + (float)ry * ry);
  if (d0 < 100) return false;
  float ux = rx / d0, uy = ry / d0;
  float dx = t.x - rx, dy = t.y - ry;
  float dMe = sqrtf(dx * dx + dy * dy);
  if (dMe < 600) return false;
  float along = t.x * ux + t.y * uy, lat = fabsf(t.x * uy - t.y * ux);
  float dT = sqrtf((float)t.x * t.x + (float)t.y * t.y);
  if (lat < dT * 0.05f && fabsf(dT - 2 * d0) < 150) return false;
  if (dMe < 1200) return true;
  return along > d0 + 300 && along < d0 + 3000 && lat < 1000 + 0.3f * (along - d0);
}

const int LINGER_MS = 2500, LINGER_V = 40;  // 驻足：累计 2.5 秒、移动速度 40 厘米/秒以下才算，路过的不算
const int GHOST_NEAR = 900, GHOST_SEEN = 1000, NEAR_ZONE = 1500;
const uint32_t GHOST_MS = 15000;
uint32_t lastPresFrame = 0, lastPresAt = 0;
bool nearLast[3];
int16_t nearX[3], nearY[3];

// 盲区记忆：从远处走过来、走到你 1.5 米内的人，如果在你身后的阴影锥里、或者贴到你身边 0.9 米内后不见了，
// 多半是被你挡住了或者和你并成了一个点，不是走了：记住这个位置 15 秒，当作「有人在你身后」。
// 又在附近看到人（换了个槽位重新报出来）就交回正常判断；你离开座位、超时也忘掉。
// 只看「从远处来的」、没被认作过你的：你自己站起来走动再坐回去，不会被当成别人
void ghostTick(uint32_t now, int me, bool here) {
  for (int i = 0; i < 3; i++) {
    const RadarTarget &t = radarT[i];
    if (nearLast[i] && !t.on && here &&
        (inShadow(nearX[i], nearY[i], presRefX, presRefY) || dist2(nearX[i], nearY[i], presRefX, presRefY) < (long)GHOST_NEAR * GHOST_NEAR)) {
      ghostOn = true;
      ghostX = nearX[i];
      ghostY = nearY[i];
      ghostAt = now;
    }
    nearLast[i] = t.on && i != me && radarFar[i] && !radarWasMe[i] && dist2(t.x, t.y, presRefX, presRefY) < (long)NEAR_ZONE * NEAR_ZONE;
    if (nearLast[i]) { nearX[i] = t.x; nearY[i] = t.y; }
  }
  if (!ghostOn) return;
  if (!here || now - ghostAt > GHOST_MS) { ghostOn = false; return; }
  for (int i = 0; i < 3; i++) {
    const RadarTarget &t = radarT[i];
    if (!t.on || i == me || dist2(t.x, t.y, ghostX, ghostY) > (long)GHOST_SEEN * GHOST_SEEN) continue;
    radarFar[i] = true;  // 就是刚才那个人
    ghostOn = false;
  }
}

void presenceTick(uint32_t now) {
  if (!radarFresh()) {
    privacyOn = ghostOn = false;
    behindScore = 0;
    behindIdx = -1;
    return;
  }
  int me = radarSeatMe();
  for (auto &t : radarT)
    if (inSeat(t)) seatSeenAt = now;
  bool seat = radarSeatPresent();
  // 只在收到新的一帧雷达数据时判断（雷达约 11 帧/秒，界面 30 帧/秒，不重复计数）
  if (radarFrames != lastPresFrame) {
    uint32_t dt = lastPresAt ? min<uint32_t>(now - lastPresAt, 300) : 0;
    lastPresFrame = radarFrames;
    lastPresAt = now;
    // 参照点：标定过用座位；没标定用你在座位区的位置；你暂时没被看到（坐着不动时会丢）就沿用上次的位置
    if (seatCal) { presRefX = seatCalX; presRefY = seatCalY; }
    else if (me >= 0) { presRefX = radarT[me].x; presRefY = radarT[me].y; presRefSeen = true; }
    if (me >= 0) radarWasMe[me] = true;
    bool here = youPresent();
    ghostTick(now, me, here);
    // 你在座位上（不要求这一帧看到你）时，你自己（座位区里离座位最近的那个）以外、在你身后或身边、
    // 走得慢的目标都算。在座位上被认作过你的轨迹，是你自己起身走动，不算。
    // 这一帧座位区没看到你时，剩下那个也可能就是你自己，所以只有它是从远处走过来的、
    // 或者你正在敲键盘（你肯定在键盘前）才算别人
    bool raw = false, typing = youTyping() && (seatCal || presRefSeen);
    behindIdx = -1;
    if (here)
      for (int i = 0; i < 3; i++) {
        const RadarTarget &t = radarT[i];
        if (!t.on || i == me || radarSpd[i] > LINGER_V || radarWasMe[i]) continue;
        if (me < 0 && !typing && !radarFar[i]) continue;
        if (behindTarget(t, presRefX, presRefY)) { raw = true; behindIdx = i; }
      }
    if (ghostOn) raw = true;
    behindScore = raw ? min(behindScore + dt, (float)LINGER_MS + 500) : max(behindScore - dt * 0.4f, 0.0f);
    if (raw) behindGoneAt = 0;
    else if (!behindGoneAt) behindGoneAt = now;
  }
  // 累计驻足约 2.5 秒就提醒（偶尔丢一两帧不会清零）；收起只看「连续 3 秒看不到」（从滤波后的目标消失算起，
  // 雷达真丢目标后还有 1.5 秒保留，盲区里记着的人另算 15 秒），收起时积分清零，免得马上又弹出来
  if (cfgPrivacy && behindScore > LINGER_MS) privacyOn = true;
  if (!cfgPrivacy || (behindGoneAt && now - behindGoneAt > 3000)) {
    privacyOn = false;
    behindScore = 0;
  }

  // 告诉 bridge：在不在座位、几个人、有没有人靠近（变了马上报，平时 10 秒一次）
  int p = seat ? 1 : 0, pv = privacyOn ? 1 : 0, n = radarCount();
  if (p != lastPresP || pv != lastPresPv || n != lastPresN || now - presLogAt > 10000) {
    lastPresP = p; lastPresPv = pv; lastPresN = n; presLogAt = now;
    Host.printf("PRES {\"p\":%d,\"n\":%d,\"pv\":%d,\"g\":%d,\"seat\":%d}\n", p, n, pv, ghostOn, cfgRadarSeat);
  }
}
