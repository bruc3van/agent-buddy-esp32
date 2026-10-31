// DeepSeek 的小鲸鱼（照 DeepSeek logo 的蓝鲸：头朝左、尾巴往上卷），手工整理成 32×19 的像素图
// 眼睛、胸鳍、尾鳍、喷水由程序画，所以能有表情和动作。
// 字母：K 描边 / B 蓝 / D 暗蓝 / L 肚皮；尾鳍（第 0~3 行）单独两帧，摆尾时交替
#pragma once
#include <Arduino.h>

#define C_DS 0x4B5F     // DeepSeek 蓝 #4D6BFE
#define C_DS_HI 0x8D3F  // 亮蓝（等你确认时闪烁）
#define C_DS_WATER 0x6E1E  // 水花 / 水面 #6FC3F0

#define DS_THINKING "深度求索中"  // 思考时的文字，同 DeepSeek 官方
typedef const char *const *Icon8;  // 8×8 小图标（drawIcon8 用）

const char *const DS_WHALE[19] = {
  "................................",
  "................................",
  "................................",
  "................................",
  "...........KKKKKKK.........KBBK.",
  ".........KKBBBBBBBKK.......KBBK.",
  ".......KKBBBBBBBBBBBKK....KBBK..",
  "......KBBBBBBBBBBBBBBBK..KBBBK..",
  ".....KBBBBBBBBBBBBBBBBBBKBBBK...",
  "....KBBBBBBBBBBBBBBBBBBBBBBK....",
  "...KBBBBBBBBBBBBBBBBBBBBBBK.....",
  "..KBBBBBBBBBBBBBBBBBBBBBDK......",
  "..KBBBBBBBBBBBBBBBBBBBBDDK......",
  ".KLLLBBBBBBBBBBBBBBBBBDDK.......",
  ".KLLLLLLBBBBBBBBBBBBDDDK........",
  "..KLLLLLLLLLLBBBBDDDDDK.........",
  "...KLLLLLLLLLLLDDDDDKK..........",
  "....KKLLLLLLLLLLKKKKK...........",
  "......KKKKKKKKKK................",
};

// 尾鳍：0 往上卷（同 logo），1 放平；画在第 0~3 行
const char *const DS_FLUKE[2][4] = {
  {"..........................KK.KK.",
   ".........................KBBKBBK",
   "..........................KBBBBK",
   "...........................KBBK."},
  {"................................",
   "............................KK..",
   "..........................KKBBKK",
   "...........................KBBBK"},
};

// 胸鳍：0 垂着（画在第 14 行、第 11 列）、1 举起来挥（第 11 行、第 11 列）
const char *const DS_FIN[2][4] = {
  {"KK..", "KDDK", ".KDK", "..KK"},
  {"..KK", ".KDK", "KDDK", "KK.."},
};

// 8×8 小图标：头朝左的鲸鱼，尾巴翘起来（眼睛是空出来的一格）
const char *const IC_DS8[8] = {"........", "......11", ".1111.1.", "111111..", "1.11111.", "1111111.", ".11111..", "........"};

// 甩尾巴（DeepSeek 官方「深度求索中」的动画）：尾鳍左倾 / 正 / 右倾三帧，按 正 → 左 → 正 → 右 播
const char *const IC_DSTAIL8[3][8] = {
  {"........", "........", "1......1", "11....11", ".111111.", "...11...", "...11...", "...11..."},
  {"........", "11......", ".11...11", "..11111.", "...111..", "...11...", "..11....", "..11...."},
  {"........", "......11", "11...11.", ".11111..", "..111...", "...11...", "....11..", "....11.."},
};

// 眼睛的样子（眼睛在第 10~11 行、第 8~9 列）
enum DsEye { DE_OPEN, DE_BLINK, DE_HAPPY, DE_X, DE_SLEEP, DE_BIG, DE_UP };

struct DsPose {
  int dx = 0, dy = 0;  // 整体偏移（屏幕像素）
  int eye = DE_OPEN;
  int fin = 0;         // 胸鳍：0 垂着 1 举起
  int fluke = 0;       // 尾鳍帧
  int spout = 0;       // 喷水：0 不喷 1 小水花 2 大喷泉
  bool blush = false;  // 开心时脸红
  bool grey = false;   // 未连接：灰色
  bool flip = false;   // 头朝右（往右游）
  float wave = 0;      // 游动：尾巴上下摆的幅度（贴图像素），0 = 不摆
  float wph = 0;       // 游动的相位
  float tilt = 0;      // 抬头 / 低头（> 0 抬头）：跳出水面时起跳抬头、落水低头
};
