// Codex 的桌面宠物（ChatGPT / Codex 客户端里的云朵小机器人），按官方形象手工整理成 32×39 的像素图（云朵头）
// 头上的屏幕是它的脸：表情由程序画（>_、睡觉、思考、开心……），其余像素来自这张图。
// 字母：K 描边 / N 屏幕 / B 蓝 / L 亮蓝 / D 暗蓝 / C 青；小写 k b e 是手臂（单独一层，能举起来）
#pragma once
#include <Arduino.h>

#define C_CODEX 0x541E     // Codex 蓝 #5783F4
#define C_CODEX_HI 0x751F  // 亮蓝（等你确认时闪烁）

const char *const CODEX_PET[39] = {
  "............KKKK................",
  "..........KKLLLLKK..KKKK........",
  ".........KLLLLLLLLKKLLLLKK......",
  "........KLLLLLLLLLLLLLLLLLK.....",
  ".......KLLLLLLLLLLLLLLLLLLK.....",
  ".......KLLLLLLLLLLLLLLLLLLLK....",
  "......KKLLLLLLLLLLLLLLLLLLLK....",
  "....KKLLLLLLLLLLLLLLLLLLLLLKK...",
  "...KLLLLLLLLLLLLLLLLLLLLLLLLLK..",
  "..KLLLLLLLLLLLLLLLLLLLLLLLLLLLK.",
  ".KBBBLLLLKKKKKKKKKKKKKKKKKLLLLLK",
  ".KBBBBBLKNNNNNNNNNNNNNNNNNKLLLLK",
  "KDBBBBBBKNNNNNNNNNNNNNNNNNKLLLLK",
  "KDBBBBBBKNNNNNNNNNNNNNNNNNKBLLLK",
  "KDBBBBBBKNNNNNNNNNNNNNNNNNKBBBBK",
  "KDBBBBBBKNNNNNNNNNNNNNNNNNKBBBBK",
  "KDDDBBBBKNNNNNNNNNNNNNNNNNKBBBBK",
  ".KDDBBBBKNNNNNNNNNNNNNNNNNKBBBBK",
  ".KKDBBBBKNNNNNNNNNNNNNNNNNKBBBK.",
  "..KDBBBBKNNNNNNNNNNNNNNNNNKBBBK.",
  "..KDBBBBKNNNNNNNNNNNNNNNNNKBBBK.",
  "..KDDBBBKNNNNNNNNNNNNNNNNNKBBDK.",
  "...KDDDBBKNNNNNNNNNNNNNNNKBBBK..",
  "...KDDDDDBKKKKKKKKKKKKKKKBBBBK..",
  "....KDDDDDBBBBBBBBBBBBBBBBDDK...",
  ".....KKDDDDDDDBBBBBBBBBBDDKK....",
  ".......KKKKKKKKKKKKKKKKKKK......",
  "......kkkKBBBBBBBBBBBBKkkk......",
  ".....kbbbKBBBBBBBBBBBBKbbbk.....",
  ".....kbbbKBBBCBBBBBBBBKbbbk.....",
  "....kbbbbKBBBBCBBCCCBBKbbbbk....",
  "....kbbbbKBBBCBBBBBBBBKbbbbk....",
  "....kebbbKDBBBBBBBBBBDKbbbek....",
  ".....keeeKDDDDDDDDDDDDKeeek.....",
  "......kkk.KDDDDDDDDDDK.kkk......",
  "..........KDDDKKKKDDDK..........",
  "..........KBBBK..KBBBK..........",
  "..........KDBDK..KDBDK..........",
  "..........KKKK....KKKK..........",
};

// 干活时坐着：头往下挪 2 行，下面换成缩成一团坐着的身体（第 28~37 行），右前方一台笔记本（程序画，屏幕往后斜）
const char *const CODEX_SIT[10] = {
  "......KKKKKKKKKKKKKKKKKKK.........",
  ".....KBBBBBBBBBBBBBBBBBBBK........",
  "....KBBBBBBBBBBBBBBBBBBBBBK.......",
  "...KBBBBBBBBBBBBBBBBBBBBBBK.......",
  "...KDBBBBBBBBBBBBBBBBBBBBBK.......",
  "...KDDDDDDDDDDDDDDDDDDDDDK........",
  "..KBBBBKKKKKKKKKKKKKKKKKK.........",
  ".KBBBBBBK.KBBBBBBK................",
  ".KDDBBBBK.KDDBBBBK................",
  "..KKKKKK...KKKKKK.................",
};
// 搭在键盘上的手（相对第 31 行、第 12 列）
const char *const CODEX_HAND[4] = {
  ".KKKK.",
  "KBBBLK",
  "KDBBBK",
  ".KKKK.",
};

// 屏幕（脸）的范围：第 11~22 行、第 9~25 列
enum CxFace { CF_PROMPT, CF_TYPING, CF_SLEEP, CF_DOTS, CF_HAPPY, CF_X, CF_ASK, CF_SQUEEZE, CF_OFF, CF_EYES, CF_LOOKUP, CF_WINCE, CF_SQUINT };

struct CxPose {
  int dx = 0, dy = 0;      // 整体偏移（屏幕像素）
  int armL = 0, armR = 0;  // 手臂抬高（贴图像素）
  int face = CF_PROMPT;
  bool grey = false;       // 未连接：灰色
  bool sit = false;        // 坐着敲笔记本（干活）
};
