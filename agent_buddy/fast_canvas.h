// 画布填色加速：Arduino_Canvas 填矩形是一个像素一个像素写 16 位；这里改成每次写 32 位（两个像素），
// 画布在 PSRAM 里，写入次数减半，清屏和大面板都快不少。只处理不旋转的画布（我们的画布都不旋转）。
#pragma once
#include <Arduino_GFX_Library.h>

class FastCanvas : public Arduino_Canvas {
public:
  using Arduino_Canvas::Arduino_Canvas;
  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    uint16_t *fb = getFramebuffer();
    if (!fb || getRotation() != 0) {
      Arduino_Canvas::writeFillRectPreclipped(x, y, w, h, color);
      return;
    }
    const int W = width();
    const uint32_t c2 = ((uint32_t)color << 16) | color;
    for (int j = 0; j < h; j++) {
      uint16_t *p = fb + (y + j) * W + x;
      int n = w;
      if (((uintptr_t)p & 2) && n) { *p++ = color; n--; }
      uint32_t *q = (uint32_t *)p;
      int m = n >> 1;
      for (int i = 0; i < m; i++) q[i] = c2;
      if (n & 1) p[n - 1] = color;
    }
  }
};
