// GT911 触摸（I2C：SDA=8、SCL=4，INT=3、RST=38，地址 0x5D）。
// 自己读，不用 TAMC_GT911 库的 read() / begin()，因为这两处都有问题：
// - read() 不看 0x814E 的「数据就绪」位。读完清零后，下一份数据（约 10ms 一份）到之前读到的都是 0，
//   主循环空闲时每秒轮询几百次，手指按着也会被当成一下下抬起：滑动被切碎、一次点击触发好几次。
//   （有一块板子实测如此，另一块没出现过，原因没比对过；hwScan 会打印芯片信息，可以对照）
// - begin() 每次开机都写一次配置刷新标志，而校验和是用没初始化的变量算的，可能写进错的校验和。
//   板子出厂配置已经是 480×272，只有分辨率不对时才写配置，而且校验和按手册重新算。
const uint8_t GT_ADDR = 0x5D;
const int GT_SDA = 8, GT_SCL = 4, GT_INT = 3, GT_RST = 38, GT_W = 480, GT_H = 272;
const uint16_t GT_CFG = 0x8047, GT_CHK = 0x80FF, GT_FRESH = 0x8100, GT_STATUS = 0x814E, GT_POINT = 0x814F;
const int GT_CFG_N = GT_CHK - GT_CFG;  // 参与校验的配置字节数
const uint32_t GT_LOST_MS = 150;  // 按着却这么久没新数据：抬手那份丢了，当成抬手

bool gtRead(uint16_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(GT_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 255);
  if (Wire.endTransmission() != 0 || Wire.requestFrom(GT_ADDR, n) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void gtWrite(uint16_t reg, const uint8_t *buf, size_t n) {
  Wire.beginTransmission(GT_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 255);
  Wire.write(buf, n);
  Wire.endTransmission();
}

uint8_t gtChecksum(const uint8_t *cfg) {
  uint8_t sum = 0;
  for (int i = 0; i < GT_CFG_N; i++) sum += cfg[i];
  return (uint8_t)(~sum + 1);
}

void gtFixResolution() {
  uint8_t cfg[GT_CFG_N];
  if (!gtRead(GT_CFG, cfg, sizeof(cfg))) {
    Host.println("TOUCH 读配置失败");
    return;
  }
  if ((cfg[1] | cfg[2] << 8) == GT_W && (cfg[3] | cfg[4] << 8) == GT_H) return;
  cfg[1] = GT_W & 255;
  cfg[2] = GT_W >> 8;
  cfg[3] = GT_H & 255;
  cfg[4] = GT_H >> 8;
  gtWrite(GT_CFG + 1, cfg + 1, 4);
  uint8_t tail[2] = {gtChecksum(cfg), 1};  // 校验和 + 刷新标志
  gtWrite(GT_CHK, tail, 2);
}

void touchInit() {
  Wire.setBufferSize(256);  // 配置块 184 字节，超过默认 128 字节缓冲
  Wire.begin(GT_SDA, GT_SCL);
  // 复位时序：INT 拉低选地址 0x5D
  pinMode(GT_INT, OUTPUT);
  pinMode(GT_RST, OUTPUT);
  digitalWrite(GT_INT, LOW);
  digitalWrite(GT_RST, LOW);
  delay(10);
  digitalWrite(GT_RST, HIGH);
  delay(55);
  pinMode(GT_INT, INPUT);
  delay(50);
  gtFixResolution();
  Wire.setClock(400000);
}

// 有新状态时返回 true；没有新数据返回 false，调用方保持上一次的状态
bool touchRead(uint32_t now, bool &down, int &x, int &y) {
  static bool held = false;
  static uint32_t lastAt = 0;
  uint8_t st;
  bool ok = gtRead(GT_STATUS, &st, 1);
  if (!ok || !(st & 0x80)) {
    if (!held || now - lastAt <= GT_LOST_MS) return false;
    held = false;
    down = false;
    return true;
  }
  int n = st & 0x0F;
  bool has = n >= 1 && n <= 5;
  uint8_t p[7];
  if (has) has = gtRead(GT_POINT, p, sizeof(p));
  uint8_t zero = 0;
  gtWrite(GT_STATUS, &zero, 1);
  if (n > 5 || (n >= 1 && !has)) return false;  // 坏数据：丢掉，不当成抬手
  lastAt = now;
  held = down = has;
  if (has) {  // 屏幕转了 180°，坐标跟着翻转
    x = GT_W - (p[1] | p[2] << 8);
    y = GT_H - (p[3] | p[4] << 8);
  }
  return true;
}

// 硬件扫描时打印：不同批次板子的触摸表现不一样时，用来对照芯片固件和配置
void touchInfo() {
  uint8_t c[GT_CFG_N + 1], id[6];
  if (!gtRead(GT_CFG, c, sizeof(c)) || !gtRead(0x8140, id, 6)) {
    Host.println("SCAN touch 读取失败");
    return;
  }
  Host.printf("SCAN touch pid=%c%c%c%c fw=%02X%02X cfg=%02X x=%d y=%d lvl=%d/%d rate=%d chk=%02X calc=%02X\n", id[0], id[1], id[2], id[3], id[5], id[4], c[0],
              c[1] | c[2] << 8, c[3] | c[4] << 8, c[0x53 - 0x47], c[0x54 - 0x47], c[0x56 - 0x47], c[GT_CFG_N], gtChecksum(c));
}
