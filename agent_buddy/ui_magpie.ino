// magpie 面板：Agent / 额度 / 用量 / 路由，浅色主题，参照 magpie 菜单栏面板
// 数据结构见 magpie_data.h；文字由 bridge 预先格式化、截断，这里只负责排版

#define M_BG      0xF79E  // #F2F2F7
#define M_CARD    0xFFFF
#define M_BORDER  0xE73D  // #E5E5EA
#define M_SEG     0xEF5D  // #E8E8ED
#define M_TEXT    0x18E3  // #1C1C1E
#define M_SUB     0x8C72  // #8E8E93
#define M_PURPLE  0x523C  // #5046E5
#define M_GREEN   0x1509  // #16A34A
#define M_ORANGE  0xC201  // #C2410C
#define M_RING    0x4A49  // #48484A
#define M_LOWTRK  0xF71A  // #F3E1D6
#define M_RED     0xD924  // #DC2626
#define M_CARD2   0xF7BE  // #F5F5F7

const int M_TOP = 42;      // 内容区起点
int magScroll[4] = {0, 0, 0, 0};
int magContentH[4] = {0, 0, 0, 0};
int magViewH[4] = {0, 0, 0, 0};

// ---------- 串口数据 ----------
bool magpieHandle(const char *t, JsonDocument &doc) {
  if (!strcmp(t, "mst")) {
    magpieOnline = doc["on"] | false;
  } else if (!strcmp(t, "mq")) {
    JsonArray gs = doc["g"].as<JsonArray>();
    mqCount = 0;
    for (JsonObject g : gs) {
      if (mqCount >= 8) break;
      MqGroup &G = mqGroups[mqCount++];
      G.name = String(g["n"] | "");
      G.icon = String(g["i"] | "");
      G.plan = String(g["plan"] | "");
      G.nc = 0;
      for (JsonObject c : g["c"].as<JsonArray>()) {
        if (G.nc >= 6) break;
        MqCard &C = G.cards[G.nc++];
        C.user = String(c["u"] | "");
        C.reset = String(c["r"] | "");
        C.nw = 0;
        for (JsonObject w : c["w"].as<JsonArray>()) {
          if (C.nw >= 3) break;
          C.w[C.nw].label = String(w["l"] | "");
          C.w[C.nw].left = w["left"] | 0;
          C.nw++;
        }
      }
    }
    magpieOnline = true;
  } else if (!strcmp(t, "mu")) {
    int i = constrain((int)(doc["p"] | 0), 0, 3);
    MuPeriod &P = muPeriods[i];
    P.has = true;
    P.tok = String(doc["tok"] | "");
    P.tsub = String(doc["tsub"] | "");
    P.calls = String(doc["calls"] | "");
    P.csub = String(doc["csub"] | "");
    P.cost = String(doc["cost"] | "");
    P.gcalls = String(doc["gcalls"] | "");  // 旧 bridge 不发 gcalls：沿用总数
    P.gcost = String(doc["gcost"] | "");
    if (!P.gcalls.length()) { P.gcalls = P.calls; P.gcost = P.cost; }
    P.hit = String(doc["hit"] | "");
    P.ymax = String(doc["ymax"] | "");
    P.nb = 0;
    for (int v : doc["bars"].as<JsonArray>()) {
      if (P.nb >= 60) break;
      P.bars[P.nb++] = constrain(v, 0, 1000);
    }
    P.nxl = 0;
    for (const char *s : doc["xl"].as<JsonArray>()) {
      if (P.nxl >= 5) break;
      P.xl[P.nxl++] = String(s);
    }
  } else if (!strcmp(t, "mr")) {
    mrListDirty = true;
    mrHead = String(doc["head"] | "");
    mrCount = 0;
    for (JsonObject r : doc["rows"].as<JsonArray>()) {
      if (mrCount >= 10) break;
      MrRow &R = mrRows[mrCount++];
      R.agent = String(r["a"] | "");
      R.icon = String(r["i"] | "");
      R.model = String(r["m"] | "");
      R.kind = String(r["k"] | "");
      R.tm = String(r["tm"] | "");
      R.served = String(r["sv"] | "");
      R.info = String(r["info"] | "");
      R.err = r["err"] | false;
    }
    msAgN = 0;
    for (JsonObject a : doc["ags"].as<JsonArray>()) {
      if (msAgN >= 3) break;
      MsNode &N = msAgs[msAgN++];
      N.name = String(a["n"] | "");
      N.icon = String(a["i"] | "");
      N.color = brandColor(N.icon, N.name, a["c"] | M_PURPLE);  // 用 logo 主题色
    }
    msDstN = 0;
    for (JsonObject x : doc["dst"].as<JsonArray>()) {
      if (msDstN >= 3) break;
      MsNode &N = msDst[msDstN++];
      N.name = String(x["n"] | "");
      N.sub = String(x["s"] | "");
      N.st = x["st"] | 1;
    }
    uint32_t now = millis();
    for (Trip &T : trips)  // 飞完的槽平时由路由页绘制时释放；不在路由页时这里释放，否则 4 个槽占满后新请求全被丢掉
      if (T.on && (int32_t)(now - T.t0) > TRIP_MS) T.on = false;
    for (JsonObject tr : doc["trips"].as<JsonArray>()) {  // 新请求：排进空闲的飞行槽，依次错开起飞
      int a = tr["a"] | 0, d = tr["d"] | 0;
      if (a < 0 || a >= 3 || d < 0 || d >= 3) continue;
      for (Trip &T : trips) {
        if (T.on) continue;
        T.on = true;
        T.a = a;
        T.d = d;
        T.ok = tr["ok"] | true;
        T.t0 = now;
        now += 350;
        break;
      }
    }
  } else if (!strcmp(t, "ma")) {
    maCount = 0;
    maVisible = 0;
    for (JsonObject a : doc["a"].as<JsonArray>()) {
      if (maCount >= 24) break;
      MaAgent &A = maAgents[maCount++];
      A.name = String(a["n"] | "");
      A.icon = String(a["i"] | "");
      A.model = String(a["m"] | "");
      A.hidden = a["h"] | false;
      if (!A.hidden) maVisible++;
    }
  } else {
    return false;
  }
  return true;
}

// unifont 里没有 "·"：遇到就手画一个小圆点
void printDots(const String &s, int x, int y, uint16_t color) {
  gfx->setTextColor(color);
  int from = 0;
  while (true) {
    int k = s.indexOf("·", from);
    String part = s.substring(from, k < 0 ? s.length() : k);
    gfx->setCursor(x, y);
    gfx->print(part);
    x += textW(part);
    if (k < 0) break;
    gfx->fillCircle(x + 4, y - 5, 1, color);
    x += 9;
    from = k + strlen("·");
  }
}

// ---------- 路由 ----------
// 舞台布局：左列 Agent 右对齐、中间 magpie、右列服务方；连线是三次贝塞尔曲线（同 routing.js 的 pLayout）
const int ST_Y = 68, HUB_X = 240;
int ST_H = 92, HUB_R = 22, ST_CY = ST_Y + 92 / 2;  // 随收起程度变化（像素风路由页可吸附收起）

bool stageCompact() { return routeCol > 0.5f; }

// 每帧把收起程度往目标值靠拢（约 0.2 秒），并算出舞台高度、中枢大小
void updateStageGeom() {  // 收起程度 routeCol 由滚动量直接决定（跟手），见 scrollBy
  ST_H = 92 - (int)(54 * routeCol);  // 92 → 38
  HUB_R = 22 - (int)(7 * routeCol);  // 22 → 15
  ST_CY = ST_Y + ST_H / 2;
}
int agX1[3], agY[3], dsX0[3], dsY[3];

int nodeY(int i, int n) {  // 收起时几个节点先向中间汇拢
  float k = 1 - min(1.0f, routeCol * 2);
  return ST_CY + (int)((2 * i - (n - 1)) * 15 * k);
}

void bez(int x1, int y1, int x2, int y2, float t, int &x, int &y) {
  float mx = (x1 + x2) / 2.0, u = 1 - t;
  x = lroundf(u * u * u * x1 + 3 * u * u * t * mx + 3 * u * t * t * mx + t * t * t * x2);
  y = lroundf(u * u * u * y1 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y2);
}

// 某条线（左 i / 右 i）上的位置；fwd=true 从 Agent 往服务方方向
void wirePos(bool left, int i, float t, int &x, int &y) {
  if (pixelStyle) {
    pxPathPos(left, i, t, x, y);
    return;
  }
  if (left) bez(agX1[i] + 3, agY[i], HUB_X - HUB_R - 2, ST_CY, t, x, y);
  else bez(HUB_X + HUB_R + 2, ST_CY, dsX0[i] - 3, dsY[i], t, x, y);
}

float easeIO(float k) { return k < .5 ? 2 * k * k : 1 - (-2 * k + 2) * (-2 * k + 2) / 2; }

// 一次飞行各段：去程 420ms 到 magpie，460ms 到服务方，等回答 900ms，回程 420+420ms（失败：变红、歇 260ms、回 360ms）
TripPose tripPose(const Trip &T, uint32_t now) {
  TripPose P = {false, 0, 0, msAgs[T.a].color, false, -1, -1};
  int t = now - T.t0;
  if (t < 0) return P;
  P.vis = true;
  int seg[6] = {420, 460, 900, T.ok ? 0 : 260, T.ok ? 420 : 360, 420};
  for (int k = 0; k < 6; k++) {
    if (t >= seg[k]) { t -= seg[k]; continue; }
    float e = easeIO((float)t / seg[k]);
    switch (k) {
      case 0: wirePos(true, T.a, e, P.x, P.y); P.litA = T.a; break;
      case 1: wirePos(false, T.d, e, P.x, P.y); P.litD = T.d; break;
      case 2: wirePos(false, T.d, 1, P.x, P.y); P.waiting = true; P.litD = T.d; break;
      case 3: wirePos(false, T.d, 1, P.x, P.y); P.c = M_RED; P.litD = T.d; break;
      case 4: wirePos(false, T.d, 1 - e, P.x, P.y); P.c = T.ok ? M_GREEN : M_RED; P.litD = T.d; break;
      case 5: wirePos(true, T.a, 1 - e, P.x, P.y); P.c = T.ok ? M_GREEN : M_RED; P.litA = T.a; break;
    }
    return P;
  }
  P.vis = false;
  return P;
}

// ---------- 入口 ----------
void drawMagpie(uint32_t now) {
  magpieTick(now);
  drawMagpiePx(now);
}

void magpieTap(int x, int y) {
  if (y < M_TOP) {
    // 与标题栏的画法一致：home 按钮 + 喜鹊 + 标签（TAB_X / TAB_W）；home 按钮的触控区比画出来的大一圈
    int homeW = 48, tabX = 82, tabW = 98;
    if (x < homeW) setScreen(SCR_HOME);
    else if (x >= tabX) {
      int t = constrain((x - tabX) / tabW, 0, 3);
      if (t != magTab) magWagAt = millis();
      magTab = t;
    }
    return;
  }
  if (magTab == 3 && pixelStyle && stageCompact() && y >= ST_Y && y <= ST_Y + ST_H) {  // 点收起的细条：展开
    routeS = ROUTE_FOLD / 2 - 1;  // 回到列表顶部，再由吸附动画展开
    magScroll[3] = 0;
    return;
  }
  if (magTab == 2 && pixelStyle && y >= 44 && y <= 78 && x >= 8 && x < 240) {  // 像素风的周期按钮更高
    muPeriod = constrain((x - 8) / 58, 0, 3);
    return;
  }
  if (magTab == 2 && y >= 46 && y <= 74 && x >= 8 && x < 240) {
    muPeriod = constrain((x - 8) / 58, 0, 3);
  }
}

// 滚动：手指移动多少内容就动多少；路由页（像素风）先用前 ROUTE_FOLD 像素收起舞台，再滚列表
float flingV = 0;     // 惯性速度（px/ms），正值 = 内容往下走
uint32_t flingT = 0;

void scrollBy(float d) {
  if (magTab == 3 && pixelStyle) {
    int maxList = max(0, magContentH[3] - (272 - (ST_Y + 92 - ROUTE_FOLD + 6)));
    routeS = constrain(routeS - d, 0.0f, (float)(ROUTE_FOLD + maxList));
    magScroll[3] = max(0, (int)routeS - ROUTE_FOLD);
    routeCol = min(routeS, (float)ROUTE_FOLD) / ROUTE_FOLD;
  } else {
    int &s = magScroll[magTab];
    s = constrain((int)lroundf(s - d), 0, max(0, magContentH[magTab] - magViewH[magTab]));
  }
}

void magpieDrag(int dy) {
  flingV = 0;
  scrollBy(dy);
}

void magpieFling(float v) {
  flingV = constrain(v, -3.0f, 3.0f);
  flingT = millis();
}

// 每帧：惯性减速；路由页停在半收起时吸附到最近的一端（过半收起，否则展开）
void magpieTick(uint32_t now) {
  uint32_t dt = min((uint32_t)100, now - flingT);
  flingT = now;
  if (!tDown && fabs(flingV) > 0.03f) {
    scrollBy(flingV * dt);
    flingV *= powf(0.995f, dt);
  } else if (!tDown) {
    flingV = 0;
  }
  if (magTab == 3 && pixelStyle && !tDown && flingV == 0 && routeS > 0 && routeS < ROUTE_FOLD) {
    float target = routeS > ROUTE_FOLD / 2 ? ROUTE_FOLD : 0;
    routeS += (target - routeS) * 0.35f;
    if (fabs(target - routeS) < 1) routeS = target;
    routeCol = routeS / ROUTE_FOLD;
    magScroll[3] = 0;
  }
  routeCol = pixelStyle ? min(routeS, (float)ROUTE_FOLD) / ROUTE_FOLD : 0;  // 收起程度始终由滚动量决定
}
