// JC4827W543 屏幕 + 触摸测试：显示芯片信息，手指触摸处画点
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>

// 屏幕：NV3041A，QSPI，480x272
#define GFX_BL 1
Arduino_DataBus *bus = new Arduino_ESP32QSPI(45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */);
Arduino_GFX *gfx = new Arduino_NV3041A(bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, true /* IPS */);

// 触摸：GT911，I2C
#define TOUCH_SDA 8
#define TOUCH_SCL 4
#define TOUCH_INT 3
#define TOUCH_RST 38
TAMC_GT911 tp(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, 480, 272);

const uint16_t colors[] = {RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_YELLOW, RGB565_CYAN};

void drawHeader() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(10, 10);
  gfx->println("JC4827W543 OK!");
  gfx->setTextSize(1);
  gfx->setCursor(10, 40);
  gfx->printf("%s rev%d  %dMHz  Flash %luMB  PSRAM %luMB\n",
              ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz(),
              ESP.getFlashChipSize() / (1024 * 1024), ESP.getPsramSize() / (1024 * 1024));
  gfx->setCursor(10, 55);
  gfx->println("Touch the screen to draw. Touch top-right box to clear.");
  gfx->drawRect(420, 0, 60, 30, RGB565_WHITE);
  gfx->setCursor(432, 11);
  gfx->print("CLEAR");
}

void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("boot");

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  Serial.println("gfx ok");
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  Wire.setBufferSize(256);  // GT911 配置块 185 字节，超过默认 128 字节缓冲
  tp.begin();
  tp.setRotation(ROTATION_NORMAL);  // X、Y 都翻转，与屏幕方向一致
  Wire.setClock(400000);  // I2C 提到 400kHz，减少每次读触摸的耗时

  drawHeader();
  Serial.println("Ready.");
}

// 每个触点上一次的位置，用于连线；-1 表示上一帧该触点未按下
int lastX[5] = {-1, -1, -1, -1, -1};
int lastY[5] = {-1, -1, -1, -1, -1};

// 两点之间按 2 像素步长补圆点，画出连续的粗线
void drawStroke(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = x1 - x0, dy = y1 - y0;
  int steps = max(abs(dx), abs(dy)) / 2 + 1;
  for (int s = 0; s <= steps; s++) {
    gfx->fillCircle(x0 + dx * s / steps, y0 + dy * s / steps, 4, color);
  }
}

void loop() {
  tp.read();
  int n = tp.isTouched ? tp.touches : 0;
  for (int i = 0; i < n && i < 5; i++) {
    int x = tp.points[i].x;
    int y = tp.points[i].y;
    if (x > 420 && y < 30) {
      drawHeader();
      delay(300);
      return;
    }
    if (lastX[i] < 0) {
      gfx->fillCircle(x, y, 4, colors[i]);
    } else {
      drawStroke(lastX[i], lastY[i], x, y, colors[i]);
    }
    lastX[i] = x;
    lastY[i] = y;
  }
  for (int i = n; i < 5; i++) {
    lastX[i] = -1;
  }
}
