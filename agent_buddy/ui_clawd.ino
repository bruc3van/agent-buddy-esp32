// Clawd 的官方动作：照 Claude 桌面端 Clawd 动画的关键帧重画（思考 Thinking、敲笔记本 Laptop、开心跳 JumpingHappy、
// 失望 Disappointed，待命的眼神路线照 LookingAroundEyesOnly）。官方 1 像素 = U/2，帧率 12 帧/秒。
// 坐标：正面站姿占 24×16 格（身体 4~19 列、0~11 行，手 0~3 / 20~23 列、4~7 行，腿 12~15 行），脚底贴着 BASE；
// 头顶往上是负数行。drawClawd 的比例和这里一样（1 格 = 2 个官方像素）。

#define C_CLAWD_SH 0xBB49  // 官方侧面 / 阴影的暗橙 #BE684D
#define C_THINK    0x6CD9  // 官方思考气泡蓝 #699ACB
#define C_LAPTOP   0x8C51  // 官方笔记本灰 #8B8B8B

int cpOX = 0, cpOY = 0;

// 原点：24×16 框的左上角，dx / dy 以官方像素计（可以为负）
void cpOrigin(int dx, int dy) {
  cpOX = CX - 6 * U + ((dx * U) >> 1);
  cpOY = BASE - 8 * U + ((dy * U) >> 1);
}

// 以官方像素为单位画方块（U 为奇数时按累计取整，整体大小不走样）
void cpR(int x, int y, int w, int h, uint16_t c) {
  int x0 = cpOX + ((x * U) >> 1), x1 = cpOX + (((x + w) * U) >> 1);
  int y0 = cpOY + ((y * U) >> 1), y1 = cpOY + (((y + h) * U) >> 1);
  gfx->fillRect(x0, y0, x1 - x0, y1 - y0, c);
}

void cpShadow(int lift) {  // lift：离地多少官方像素，越高影子越小
  int w = max(8, 70 - lift * 4) * U / 14;
  gfx->fillEllipse(CX, BASE + 4 * U / 14, w, max(2, 6 * U / 14), C_DIM);
}

void cpLegs(int y, int h, uint16_t c) {
  for (int x : {4, 8, 14, 18}) cpR(x, y, 2, h, c);
}

// 第几帧（12 帧/秒）
int cpFrame(uint32_t t) { return (int)(t * 12 / 1000); }

// ---------- 待命：眼睛慢慢转一圈（官方 LookingAroundEyesOnly，3.4 秒一圈，不眨眼）----------
// 右 → 右上 → 上 → 左上 → 左 → 左下 → 正下 → 右下 → 右 → 回中间，每个位置停 3 帧；中间多停 2 帧
void clawdLookAround(uint32_t now, int &lx, int &ly) {
  static const int8_t DX[13] = {0, 1, 1, 0, -1, -1, -1, -1, 0, 1, 1, 1, 0}, DY[13] = {0, 0, -1, -1, -1, 0, 1, 2, 2, 2, 1, 0, 0};
  int f = cpFrame(now) % 41;
  int i = f < 5 ? 0 : min(12, 1 + (f - 5) / 3);
  lx = DX[i] * U / 2;
  ly = DY[i] * U / 2;
}

// 思考气泡：大泡 9×7，尾巴朝左下（右边的泡）或右下（左边的泡）伸向头顶；dots 个点依次冒出来
void cpThinkBubble(int x, int y, bool big, bool tailLeft, int dots) {
  if (!big) {  // 刚冒出来的小泡
    cpR(x + 1, y + 2, 5, 1, C_THINK);
    cpR(x, y + 3, 7, 3, C_THINK);
    cpR(x + 2, y + 6, 3, 1, C_THINK);
    cpR(x + (tailLeft ? 2 : 4), y + 7, 1, 1, C_THINK);
    return;
  }
  cpR(x + 1, y, 7, 1, C_THINK);
  cpR(x, y + 1, 9, 5, C_THINK);
  cpR(x + 1, y + 6, 7, 1, C_THINK);
  int t = x + 3;
  cpR(t, y + 7, 3, 1, C_THINK);
  cpR(tailLeft ? t : t + 1, y + 8, 2, 1, C_THINK);
  cpR(tailLeft ? t : t + 2, y + 9, 1, 1, C_THINK);
  for (int i = 0; i < dots; i++) cpR(x + 2 + i * 2, y + 3, 1, 1, P_BG);
}

// ---------- 思考（官方 Thinking）：先仰头托腮，右上冒泡；再压低身子往上看，泡挪到左上。各约 1.7 秒，来回 ----------
void drawClawdThink(uint32_t t) {
  int f = cpFrame(t) % 40;
  bool second = f >= 20;
  int k = f % 20;  // 这一段的第几帧
  int dots = k < 4 ? 0 : min(3, (k - 4) / 4 + 1);
  cpOrigin(0, 0);
  // 气泡顶最高到 y=48（Buddy 页标题栏到 42，再留 6px）；放不下就往下压，前半段同时往右挪，尾巴不戳进仰起的头
  int n2 = 2 * (48 - cpOY), yMin = n2 > 0 ? (n2 + U - 1) / U : n2 / U;  // 向上取整（负数时整除本来就是向上取整）
  int y1 = max(-13, yMin), y2 = max(-11, yMin);
  cpShadow(0);
  if (!second) {
    // 仰头：身体右上角翘起来，右手托着下巴，左手垂下，眯着眼（左眼低一格，头是歪的）
    cpR(16, -2, 4, 1, C_CLAUDE);
    cpR(8, -1, 12, 1, C_CLAUDE);
    cpR(4, 0, 16, 11, C_CLAUDE);
    cpR(4, 11, 8, 1, C_CLAUDE);
    cpLegs(12, 4, C_CLAUDE);
    cpR(14, 11, 2, 1, C_CLAUDE);
    cpR(18, 11, 2, 1, C_CLAUDE);
    cpR(20, 1, 2, 1, C_CLAUDE);  // 托腮的手
    cpR(21, 2, 2, 2, C_CLAUDE);
    cpR(20, 4, 3, 1, C_CLAUDE);
    cpR(0, 5, 4, 4, C_CLAUDE);   // 左手
    cpR(20, 5, 3, 1, C_CLAUDE);
    cpR(20, 6, 2, 1, C_CLAUDE);
    cpR(5, 2, 3, 1, C_BLACK);
    cpR(15, 1, 3, 1, C_BLACK);
    cpThinkBubble(12 + 2 * (y1 + 13), y1, k >= 2, true, dots);
  } else {
    // 压低身子往上看：头顶露出暗面，眼睛压到身子下半截，左手抬到脸边
    cpR(4, 1, 16, 2, C_CLAWD_SH);
    cpR(4, 3, 16, 10, C_CLAUDE);
    cpR(2, 7, 2, 1, C_CLAUDE);
    cpR(1, 8, 2, 2, C_CLAUDE);
    cpR(1, 10, 3, 2, C_CLAUDE);
    cpR(20, 8, 3, 4, C_CLAUDE);
    cpLegs(13, 1, C_CLAWD_SH);
    cpLegs(14, 2, C_CLAUDE);
    cpR(5, 8, 3, 1, C_BLACK);
    cpR(15, 8, 3, 1, C_BLACK);
    cpThinkBubble(0, y2, k >= 2, false, dots);
  }
}

// ---------- 干活（官方 Laptop 的循环段）：侧过身坐着，对着右边的笔记本两手轮流敲 ----------
// 身子看到左侧的暗面和朝右的脸；手有三个姿势，每个停 2 帧；敲第二下时笔记本跟着弹一下。laptop=false：只画人（首页对着显示器）
void drawClawdLaptop(uint32_t t, int dx, bool laptop) {
  int ph = (cpFrame(t) / 2) % 3;
  cpOrigin(dx, 0);
  cpShadow(0);
  cpR(4, 2, 4, 11, C_CLAWD_SH);  // 侧面
  cpR(8, 2, 12, 11, C_CLAUDE);   // 脸朝右前方
  cpR(10, 5, 2, 2, C_BLACK);
  cpR(18, 5, 2, 2, C_BLACK);
  // 坐着：腿弯一下，往左收
  for (int i = 0; i < 4; i++) {
    int x = i == 0 ? 4 : (i == 1 ? 8 : (i == 2 ? 14 : 18));
    uint16_t c = i == 0 ? C_CLAWD_SH : C_CLAUDE;
    cpR(x, 13, 2, 1, c);
    cpR(x - 1, 14, 3, 1, c);
    cpR(x - 1, 15, 2, 1, c);
  }
  if (ph == 0) {         // 近手抬起，远手在键盘上
    cpR(20, 7, 3, 4, C_CLAUDE);
    cpR(20, 12, 4, 3, C_CLAWD_SH);
  } else if (ph == 1) {  // 近手按下去
    cpR(20, 8, 3, 4, C_CLAUDE);
    cpR(23, 9, 1, 3, C_CLAWD_SH);
    cpR(20, 12, 4, 1, C_CLAWD_SH);
  } else {               // 远手抬起
    cpR(20, 7, 4, 2, C_CLAWD_SH);
    cpR(20, 9, 3, 3, C_CLAUDE);
    cpR(23, 9, 1, 3, C_CLAWD_SH);
    cpR(20, 12, 3, 2, C_CLAUDE);
  }
  if (!laptop) return;
  int b = ph == 1 ? -1 : 0;  // 敲下去时笔记本弹一下
  cpR(21, 15 + b, 6, 1, C_LAPTOP);  // 底座
  for (int i = 0; i < 5; i++) cpR(26 + i, 14 - i + b, i == 4 ? 1 : 2, 1, C_LAPTOP);  // 斜着打开的屏幕
  for (int i = 1; i < 4; i++) cpR(25 + i, 14 - i + b, 1, 1, C_THINK);  // 屏幕朝 Clawd 的那面亮着，一眼认得出是笔记本
}

// ---------- 完成（官方 JumpingHappy）：下蹲 → 双手举过头顶蹦起来 → 顶点 ^^ 晃手 → 落地压扁 → 站稳，约 1.6 秒一次 ----------
void cpFront(int top, int armY, int eyeY, int legY, int legH) {  // 正面：身体 12 行，从 top 开始
  cpR(4, top, 16, 12, C_CLAUDE);
  cpR(0, armY, 4, 4, C_CLAUDE);
  cpR(20, armY, 4, 4, C_CLAUDE);
  cpR(6, eyeY, 2, 2, C_BLACK);
  cpR(16, eyeY, 2, 2, C_BLACK);
  cpLegs(legY, legH, C_CLAUDE);
}

void cpHappyEyes(int y) {  // ^ ^
  cpR(7, y, 2, 1, C_BLACK); cpR(6, y + 1, 1, 1, C_BLACK); cpR(9, y + 1, 1, 1, C_BLACK);
  cpR(14, y, 2, 1, C_BLACK); cpR(13, y + 1, 1, 1, C_BLACK); cpR(16, y + 1, 1, 1, C_BLACK);
}

void drawClawdJump(uint32_t t) {
  int f = (int)(t * 14 / 1000) % 22;  // 官方这一段 14 帧/秒；最后 8 帧站着歇一下
  // 顶点时双手高出脚底 30 个官方像素；Buddy 页上方是标签栏，放不下就按离地高度把腾空的几帧往下压，跳得矮一点
  static const uint8_t LIFT[9] = {0, 3, 9, 10, 10, 10, 10, 10, 6};
  int over = 46 - (BASE - 15 * U), sink = over > 0 ? (over * 2 + U - 1) / U : 0;
  cpOrigin(0, f < 9 ? sink * LIFT[f] / 10 : 0);
  switch (f) {
    case 0:  // 下蹲：身子往下沉 2 格，腿缩短
      cpShadow(0);
      cpR(4, 2, 16, 12, C_CLAUDE);
      cpR(0, 8, 4, 4, C_CLAUDE); cpR(20, 8, 4, 4, C_CLAUDE);
      cpR(6, 7, 2, 2, C_BLACK); cpR(16, 7, 2, 2, C_BLACK);
      cpLegs(14, 1, C_CLAWD_SH); cpLegs(15, 1, C_CLAUDE);
      break;
    case 1:  // 起跳：双手张开举起，眼睛拉长
      cpShadow(3);
      cpR(2, -8, 4, 2, C_CLAUDE); cpR(18, -8, 4, 2, C_CLAUDE); cpR(2, -6, 20, 2, C_CLAUDE);
      cpR(4, -4, 16, 12, C_CLAUDE);
      cpR(6, -4, 2, 4, C_BLACK); cpR(16, -4, 2, 4, C_BLACK);
      cpLegs(8, 5, C_CLAUDE);
      break;
    case 2:  // 往上冲：双手举过头顶
      cpShadow(9);
      cpR(5, -13, 4, 4, C_CLAUDE); cpR(15, -13, 4, 4, C_CLAUDE);
      cpR(4, -9, 16, 12, C_CLAUDE);
      cpR(6, -7, 2, 2, C_BLACK); cpR(16, -7, 2, 2, C_BLACK);
      cpLegs(3, 4, C_CLAUDE);
      break;
    case 3: case 4: case 5: case 6: case 7: {  // 顶点：^^，双手一里一外晃
      cpShadow(10);
      int ax = (f % 2) ? 6 : 5;
      cpR(ax, -14, 4, 4, C_CLAUDE); cpR(24 - ax - 4, -14, 4, 4, C_CLAUDE);
      cpR(4, -10, 16, 12, C_CLAUDE);
      cpHappyEyes(-8);
      cpLegs(2, 4, C_CLAUDE);
      break;
    }
    case 8:  // 落下：手往两边放
      cpShadow(6);
      cpR(3, -9, 4, 3, C_CLAUDE); cpR(17, -9, 4, 3, C_CLAUDE); cpR(3, -6, 18, 1, C_CLAUDE);
      cpR(4, -5, 16, 11, C_CLAUDE);
      cpR(6, -4, 2, 2, C_BLACK); cpR(16, -4, 2, 2, C_BLACK);
      cpLegs(6, 4, C_CLAUDE);
      break;
    case 9:  // 落地压扁：身子变宽变矮，腿往外撇
      cpShadow(0);
      cpR(4, -3, 15, 1, C_CLAUDE); cpR(4, -2, 16, 4, C_CLAUDE); cpR(0, 2, 24, 5, C_CLAUDE);
      cpR(1, 7, 22, 1, C_CLAUDE); cpR(2, 8, 20, 1, C_CLAUDE); cpR(4, 9, 16, 3, C_CLAUDE);
      cpR(6, -1, 2, 5, C_BLACK); cpR(16, -1, 2, 5, C_BLACK);
      for (int x : {3, 7, 14, 18}) { cpR(x, 12, 3, 1, C_CLAUDE); cpR(x, 14, 3, 1, C_CLAUDE); }
      for (int x : {3, 7, 15, 19}) cpR(x, 13, 2, 1, C_CLAUDE);
      cpLegs(15, 1, C_CLAUDE);
      break;
    case 10:  // 回弹
      cpShadow(0);
      cpR(4, 1, 16, 11, C_CLAUDE); cpR(0, 7, 24, 4, C_CLAUDE);
      cpR(6, 3, 2, 3, C_BLACK); cpR(16, 3, 2, 3, C_BLACK);
      cpR(3, 12, 18, 1, C_CLAUDE);
      cpR(3, 13, 2, 1, C_CLAUDE); cpR(7, 13, 10, 1, C_CLAUDE); cpR(19, 13, 2, 1, C_CLAUDE);
      for (int x : {3, 7, 14, 18}) cpR(x, 14, 3, 1, C_CLAUDE);
      cpLegs(15, 1, C_CLAUDE);
      break;
    case 11:  // 站直，手还没抬回来
      cpShadow(0);
      cpR(4, -1, 16, 12, C_CLAUDE); cpR(0, 7, 24, 4, C_CLAUDE);
      cpR(6, 1, 2, 2, C_BLACK); cpR(16, 1, 2, 2, C_BLACK);
      cpLegs(11, 5, C_CLAUDE);
      break;
    default:  // 站好，歇一下再跳
      cpShadow(0);
      cpFront(0, 4, 2, 12, 4);
      break;
  }
}

// ---------- 出错（官方 Disappointed）：眼睛眯成线 → 皱眉 → 耸一下肩 → 垮下去 → 摇摇头 → 站起来，约 3.5 秒一轮 ----------
void cpSlump(int y, int band, int eyeX) {  // 垮下去的样子：y 身子顶；band -1 / 1 左 / 右侧露出暗面（摇头）
  cpR(4, y, 16, 11, C_CLAUDE);
  cpR(1, y + 6, 22, 4, C_CLAUDE);
  if (band < 0) { cpR(4, y, 2, 11, C_CLAWD_SH); cpR(1, y + 6, 2, 4, C_CLAWD_SH); }
  if (band > 0) { cpR(18, y, 2, 11, C_CLAWD_SH); cpR(21, y + 6, 2, 4, C_CLAWD_SH); }
  cpR(eyeX, y + 4, 3, 1, C_BLACK);
  cpR(eyeX + 9, y + 4, 3, 1, C_BLACK);
}

void cpFrown(int y) {  // 皱眉：一横下面垂一点
  cpR(5, y, 3, 1, C_BLACK); cpR(6, y + 1, 1, 1, C_BLACK);
  cpR(16, y, 3, 1, C_BLACK); cpR(17, y + 1, 1, 1, C_BLACK);
}

void drawClawdSad(uint32_t t) {
  int f = cpFrame(t) % 44;
  cpOrigin(0, 0);
  cpShadow(0);
  if (f < 5 || f >= 40) {  // 站着
    cpFront(0, 4, 2, 12, 4);
  } else if (f < 6) {      // 眼睛眯成一条线
    cpR(4, 0, 16, 12, C_CLAUDE); cpR(0, 4, 4, 4, C_CLAUDE); cpR(20, 4, 4, 4, C_CLAUDE); cpLegs(12, 4, C_CLAUDE);
    cpR(5, 3, 4, 1, C_BLACK); cpR(14, 3, 4, 1, C_BLACK);
  } else if (f < 9) {      // 皱眉
    cpR(4, 0, 16, 12, C_CLAUDE); cpR(0, 4, 4, 4, C_CLAUDE); cpR(20, 4, 4, 4, C_CLAUDE); cpLegs(12, 4, C_CLAUDE);
    cpFrown(2);
  } else if (f < 12) {     // 耸肩：身子往上提，手抬到眼睛旁边
    int up = f < 10 ? 1 : 2, ay = up == 1 ? 3 : 1;
    cpR(4, -up, 16, 11 + up, C_CLAUDE); cpR(0, ay, 4, 4, C_CLAUDE); cpR(20, ay, 4, 4, C_CLAUDE);
    cpLegs(11, 5, C_CLAUDE);
    cpFrown(2 - up);
  } else if (f < 13) {     // 泄气：手垂成一大片
    cpR(4, -1, 16, 12, C_CLAUDE); cpR(0, 3, 24, 6, C_CLAUDE); cpLegs(11, 5, C_CLAUDE);
    cpR(5, 3, 3, 1, C_BLACK); cpR(16, 3, 3, 1, C_BLACK);
  } else if (f < 17) {     // 垮下去，弹一下
    int y = f == 13 ? 3 : 2;
    cpSlump(y, 0, 6);
    cpLegs(y + 11, 1, C_CLAWD_SH);
    cpLegs(y + 12, 16 - y - 12, C_CLAUDE);
  } else if (f < 38) {     // 摇摇头：先转左（看到左侧暗面、眼睛往右挪），再转右
    bool left = f < 21;
    cpSlump(2, left ? -1 : 1, left ? 8 : 4);
    cpLegs(13, 1, C_CLAWD_SH);
    cpLegs(14, 2, C_CLAUDE);
  } else {                 // 回到正面，准备站起来
    cpSlump(3, 0, 6);
    cpLegs(14, 1, C_CLAWD_SH);
    cpLegs(15, 1, C_CLAUDE);
  }
}
