// Wi-Fi 链路的共享部分（实现见 net_link.ino）：主程序和各模块的输出都经 Host 送到当前链路，
// Arduino 把 .ino 拼成一个文件时主程序排在最前面，所以类、结构体和全局量放在头文件里。
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdh.h"
#include "freertos/message_buffer.h"

const size_t NET_MAX = 16384;      // 单条记录上限（magpie 一条 1~2KB，语音一块约 4KB）
const int NET_HOSTS = 4;            // 最多配对几台电脑（满了替换最久没连的）
const int NET_SESS = 3;             // 同时最多几条 Wi-Fi 连接（一台控制、其余待命）
volatile bool netUp = false;        // 有已认证、可以当控制者的 Wi-Fi 会话
volatile uint32_t serialRxAt = 0;   // 最近一次从串口收到数据
bool lineFromNet = false;           // handleLine 正在处理网络来的行（配对、改地址只认串口）
MessageBufferHandle_t netRx = nullptr, netTx = nullptr;
SemaphoreHandle_t hostMu = nullptr;

// 配对过的电脑（NVS 命名空间 "pair"）
struct PairHost {
  char hid[17];        // 电脑的 host_id（hex）
  char name[40];       // 显示名
  char dial[160];      // 板子去连它的地址（逗号分隔，IP 或 xxx.local）；空 = 由电脑来连板子（Windows）
  uint16_t port;
  uint8_t key[32];     // 共享密钥
  uint32_t use;        // 最近一次连上的序号（满了替换最小的）
  uint32_t nextDial;   // 运行时：下次拨号的时刻
  uint8_t back;        // 运行时：拨号退避秒数
};

// 一条 Wi-Fi 连接（板子拨出去的，或电脑连进来的），各自一个任务
struct NetSess {
  volatile bool used, authed, kill;
  volatile int8_t want;  // 该是什么角色：1 控制 / 0 待命（netTask 定）
  int8_t sent;           // 已经告诉对方的角色，-1 = 还没说
  WiFiClient c;
  char ip[16], hid[17], dialHid[17];  // 对方 IP、认证后的 host_id、拨号时要连的 host_id
  uint32_t since;
  uint8_t kTx[32], kRx[32];
  uint8_t *fb, *pb, *tb, *lb;  // 收：密文 / 解出的明文 / 发：密文 / 待发的行（PSRAM）
};

// 插着 USB 且最近 3 秒串口有数据（bridge 每 2 秒 ping）→ USB；否则有 Wi-Fi 控制者就走 Wi-Fi
inline bool hostNetActive() {
  return netUp && (!HWCDC::isPlugged() || millis() - serialRxAt > 3000);
}

// 输出：走 USB 时直接写 Serial；走 Wi-Fi 时按行排进发送队列（不写 Serial，免得没人读串口时被背压卡住）
class HostOut : public Print {
 public:
  String line;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t n) override {
    if (!hostNetActive()) return Serial.write(b, n);
    if (hostMu) xSemaphoreTake(hostMu, portMAX_DELAY);
    for (size_t i = 0; i < n; i++) {
      if (b[i] == '\n') {
        if (netTx && line.length()) xMessageBufferSend(netTx, line.c_str(), line.length(), 0);  // 满了就丢
        line = "";
      } else if (b[i] != '\r' && line.length() < NET_MAX) {
        line += (char)b[i];
      }
    }
    if (hostMu) xSemaphoreGive(hostMu);
    return n;
  }
};
HostOut Host;
