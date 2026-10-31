// 硬件扫描：板载 I2C、扩展口（P2 / P3 / P4）上的 I2C 器件、TF 卡；结果以 "SCAN ..." 行经串口报给 bridge
// 扩展口先被动检测外部上拉（只读），只在有上拉的引脚上做 I2C 扫描，避免误触继电器等普通 GPIO 器件
#include <SD.h>
#include <SPI.h>

const char *i2cName(uint8_t a) {
  switch (a) {
    case 0x14: case 0x5D: return "GT911 触摸芯片（板载）";
    case 0x23: case 0x5C: return "BH1750 光照";
    case 0x29: return "VL53L0X 测距 / TSL2591 光照";
    case 0x38: return "AHT10/AHT20 温湿度";
    case 0x39: return "APDS9960 手势 / TSL2561 光照";
    case 0x3C: case 0x3D: return "SSD1306 OLED 屏";
    case 0x40: return "INA219 电流 / HTU21D / SHT20 温湿度";
    case 0x44: case 0x45: return "SHT3x 温湿度";
    case 0x48: case 0x49: case 0x4A: case 0x4B: return "ADS1115 ADC / TMP102 温度";
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: return "AT24C EEPROM";
    case 0x5A: return "MLX90614 红外测温 / CCS811 空气质量";
    case 0x62: return "SCD40/SCD41 CO2";
    case 0x68: return "MPU6050/MPU9250 陀螺仪 / DS3231 时钟";
    case 0x69: return "MPU6050（AD0 高）";
    case 0x6A: case 0x6B: return "LSM6DS 加速度陀螺仪 / QMI8658";
    case 0x76: case 0x77: return "BME280/BMP280 气压温湿度";
    default: return "未知器件";
  }
}

void scanBus(TwoWire &w, const String &where) {
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    w.beginTransmission(a);
    if (w.endTransmission() == 0) {
      Host.printf("SCAN i2c %s 0x%02X %s\n", where.c_str(), a, i2cName(a));
      n++;
    }
  }
  if (!n) Host.printf("SCAN i2c %s 无器件\n", where.c_str());
}

void hwScan() {
  Host.println("SCAN begin");
  bool mic = micOn;  // P3 上接着麦克风，扫描会改动它的引脚：先停，扫完重开
  micStop();
  // 1. 板载 I2C（触摸芯片所在，GPIO8 SDA / GPIO4 SCL）
  scanBus(Wire, "板载(SDA8,SCL4)");

  // 2. 扩展口：开内部下拉后读电平，高电平说明外面有上拉电阻（I2C 模块常见）或被器件拉高
  struct Port { const char *name; int pins[4]; int n; };
  Port ports[3] = {{"P2", {46, 9, 14, 5}, 4}, {"P3", {6, 7, 15, 16}, 4}, {"P4", {17, 18, -1, -1}, 2}};
  for (Port &p : ports) {
    int up[4], nu = 0;
    String lv = "";
    for (int i = 0; i < p.n; i++) {
      pinMode(p.pins[i], INPUT_PULLDOWN);
      delay(3);
      int v = digitalRead(p.pins[i]);
      lv += " IO" + String(p.pins[i]) + "=" + (v ? "高" : "低");
      if (v) up[nu++] = p.pins[i];
      pinMode(p.pins[i], INPUT);
    }
    Host.printf("SCAN pins %s%s\n", p.name, lv.c_str());
    // 3. 只在两两都有外部上拉的引脚上试 I2C（SDA/SCL 两种接法都试）
    for (int i = 0; i < nu; i++)
      for (int j = 0; j < nu; j++) {
        if (i == j) continue;
        Wire1.begin(up[i], up[j], 100000);
        scanBus(Wire1, String(p.name) + "(SDA" + up[i] + ",SCL" + up[j] + ")");
        Wire1.end();
        pinMode(up[i], INPUT);
        pinMode(up[j], INPUT);
      }
  }

  // 4. TF 卡（SPI：CS10 MOSI11 SCK12 MISO13），用 HSPI，避开屏幕占用的 SPI2
  static SPIClass sdSpi(HSPI);
  sdSpi.begin(12, 13, 11, 10);
  if (SD.begin(10, sdSpi)) {
    uint8_t t = SD.cardType();
    Host.printf("SCAN sd 已插卡 类型=%s 容量=%lluMB\n", t == CARD_SDHC ? "SDHC" : t == CARD_SD ? "SD" : t == CARD_MMC ? "MMC" : "未知",
                  SD.cardSize() / (1024 * 1024));
    SD.end();
  } else {
    Host.println("SCAN sd 未检测到卡");
  }
  sdSpi.end();
  if (mic) micStart();
  Host.println("SCAN end");
}
