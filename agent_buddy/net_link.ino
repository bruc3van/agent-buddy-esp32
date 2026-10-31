// Wi-Fi 链路（docs/无线通讯方案.md）：两个方向都支持，连上以后完全一样。
//   板子 → 电脑：macOS 上 bridge 只监听 47824（接受入站连接不用「本地网络」授权，开机自启也稳定），板子按配对时拿到的地址去连；
//   电脑 → 板子：Windows 防火墙默认拦入站，bridge 主动连板子的 47824。板子用 mDNS（agent-buddy-<id>.local、_agentbuddy._tcp）
//               和 UDP 探测（"BUDDY? <id|*>" → "BUDDY! <id> <port> <配对窗口开着 1/0>"）让电脑找到自己。
// 上层协议不变：每条记录就是串口上的一行 JSON。连上后先双向 HMAC 认证，之后每帧 AES-256-GCM 加密：
//   B → H:  BUDDY/2 <board_id> <nB>          nB / nH：16 字节随机数（hex）。不管谁拨号，都是板子先说
//   H → B:  HOST <host_id> <nH> <macH>        macH = HMAC(K, "H|" board_id "|" nB "|" nH)
//   B → H:  OK <macB>                          macB = HMAC(K, "B|" host_id "|" nH "|" nB)
//   之后：u32 长度（小端）| 密文 | 16 字节 tag；nonce = 方向（"H2B\0" / "B2H\0"）+ u64 计数器；明文 = u8 类型 + 数据
//   类型 1 一行 JSON，4 心跳（板子每 2 秒发，10 秒收不到对方任何帧就断开重连）。
// 配对：插着 USB 时 bridge 自动发 {"t":"pair"}（只认串口）；没有 USB 时在 设置 → 电脑 → 添加电脑 打开 2 分钟窗口，
//   电脑回 "PAIR <host_id> <X25519 公钥> <port> <名字 base64> <地址|->"，板子回 "PAIRKEY <公钥>"，
//   两边各算 6 位验证码 = HMAC(共享密钥, "sas|" board_id "|" nB "|" PAIR 那一行 "|" 板子公钥) 前 4 字节 mod 10⁶，
//   用户在板子上点「允许」→ "PAIROK"，K = HKDF(共享密钥, salt = 电脑公钥 | 板子公钥, "buddy pair")。
// 多台电脑：同时只有一台是控制者（屏幕显示它的状态），其余待命，只收发心跳。板子发 {"t":"role","r":"active|standby"}。
//   优先级：USB 那台（串口在用时所有 Wi-Fi 连接都待命）> 设置页选的「首选电脑」> 先连上、一直连着的那台。
// 输出（CFG / PRES / HELLO / 调试）经 Host 送到当前链路；走 Wi-Fi 时不往 Serial 写。录屏只走 USB。
#include <ESPmDNS.h>
#include "esp_random.h"
#include "esp_mac.h"

const uint16_t NET_PORT_DEF = 47824;

String boardId;                     // MAC 后 3 字节，如 "a1b2c3"
PairHost hosts[NET_HOSTS];
int hostN = 0;
char prefHid[17] = "";              // 首选电脑
uint32_t useSeq = 0;
SemaphoreHandle_t pairMu = nullptr, sessMu = nullptr, netTxMu = nullptr, netRxMu = nullptr;
NetSess sess[NET_SESS];
volatile int netActive = -1;        // 控制者是哪条连接
volatile int netSt = 0;             // 设置页显示：0 未配对 / 1 等 Wi-Fi / 2 没连上 / 4 已连接（两个任务都要读，不用 String）
volatile uint32_t authFailAt = 0;   // 最近一次有电脑认证失败（密钥对不上：电脑那边重新配对过）
volatile bool mdnsOn = false;
char *netLineBuf = nullptr;
// 屏幕验证码配对
volatile uint32_t pairOpenUntil = 0;  // 配对窗口开到什么时候（0 = 关）
volatile int pairSas = -1;            // 等用户确认的验证码，-1 = 没有
volatile int pairAns = 0;             // 用户的回答：1 允许 / 2 拒绝
volatile bool pairBusy = false;
char pairReqName[40] = "";
char pairMsg[64] = "";                // 配对结果，设置页显示一会儿
uint32_t pairMsgAt = 0;

String hexOf(const uint8_t *b, int n) {
  static const char *H = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (int i = 0; i < n; i++) { s += H[b[i] >> 4]; s += H[b[i] & 15]; }
  return s;
}

bool unhex(const String &s, uint8_t *out, int n) {
  if ((int)s.length() != n * 2) return false;
  for (int i = 0; i < n; i++) {
    char h = s[2 * i], l = s[2 * i + 1];
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1)); };
    if (v(h) < 0 || v(l) < 0) return false;
    out[i] = v(h) * 16 + v(l);
  }
  return true;
}

void hmac256(const uint8_t *key, size_t kl, const uint8_t *msg, size_t ml, uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, kl, msg, ml, out);
}

// HKDF-SHA256，输出 32 字节（只要一块）
void hkdf32(const uint8_t *ikm, const uint8_t *salt, size_t sl, const char *info, uint8_t out[32]) {
  uint8_t prk[32], m[64];
  hmac256(salt, sl, ikm, 32, prk);
  size_t il = strlen(info);
  memcpy(m, info, il);
  m[il] = 1;
  hmac256(prk, 32, m, il + 1, out);
}

int netRng(void *, unsigned char *b, size_t n) {
  esp_fill_random(b, n);
  return 0;
}

bool validHid(const char *h) {
  size_t n = strlen(h);
  if (!n || n > 16) return false;
  for (size_t i = 0; i < n; i++)
    if (!isxdigit((unsigned char)h[i])) return false;
  return true;
}

// ---------- 配对表（改 hosts 要拿 pairMu）----------
void pairLock() { xSemaphoreTake(pairMu, portMAX_DELAY); }
void pairUnlock() { xSemaphoreGive(pairMu); }

void netSavePairs() {
  Preferences p;
  p.begin("pair", false);
  p.clear();
  char k[4];
  for (int i = 0; i < hostN; i++) {
    PairHost &h = hosts[i];
    snprintf(k, sizeof(k), "h%d", i), p.putString(k, h.hid);
    snprintf(k, sizeof(k), "m%d", i), p.putString(k, h.name);
    snprintf(k, sizeof(k), "d%d", i), p.putString(k, h.dial);
    snprintf(k, sizeof(k), "p%d", i), p.putUShort(k, h.port);
    snprintf(k, sizeof(k), "k%d", i), p.putBytes(k, h.key, 32);
    snprintf(k, sizeof(k), "u%d", i), p.putUInt(k, h.use);
  }
  p.putString("pref", prefHid);
  p.putUInt("seq", useSeq);
  p.end();
}

void netLoadPair() {
  Preferences p;
  p.begin("pair", false);
  hostN = 0;
  bool migrate = p.isKey("hid");  // v3.9 只存一台：hid / name / dial / port / key
  if (migrate) {
    PairHost &h = hosts[0];
    memset(&h, 0, sizeof(h));
    strlcpy(h.hid, p.getString("hid", "").c_str(), sizeof(h.hid));
    strlcpy(h.name, p.getString("name", "").c_str(), sizeof(h.name));
    strlcpy(h.dial, p.getString("dial", "").c_str(), sizeof(h.dial));
    h.port = p.getUShort("port", NET_PORT_DEF);
    if (h.hid[0] && p.getBytes("key", h.key, 32) == 32) hostN = 1;
  } else {
    char k[4];
    for (int i = 0; i < NET_HOSTS; i++) {
      snprintf(k, sizeof(k), "h%d", i);
      if (!p.isKey(k)) continue;
      PairHost &h = hosts[hostN];
      memset(&h, 0, sizeof(h));
      strlcpy(h.hid, p.getString(k, "").c_str(), sizeof(h.hid));
      snprintf(k, sizeof(k), "m%d", i), strlcpy(h.name, p.getString(k, "").c_str(), sizeof(h.name));
      snprintf(k, sizeof(k), "d%d", i), strlcpy(h.dial, p.getString(k, "").c_str(), sizeof(h.dial));
      snprintf(k, sizeof(k), "p%d", i), h.port = p.getUShort(k, NET_PORT_DEF);
      snprintf(k, sizeof(k), "u%d", i), h.use = p.getUInt(k, 0);
      snprintf(k, sizeof(k), "k%d", i);
      if (h.hid[0] && p.getBytes(k, h.key, 32) == 32) hostN++;
    }
  }
  strlcpy(prefHid, p.getString("pref", "").c_str(), sizeof(prefHid));
  useSeq = p.getUInt("seq", 0);
  p.end();
  for (int i = 0; i < hostN; i++) hosts[i].back = 1;
  if (migrate) netSavePairs();
}

int hostFind(const char *hid) {
  for (int i = 0; i < hostN; i++)
    if (!strcmp(hosts[i].hid, hid)) return i;
  return -1;
}

// 新增或更新一台（同一个 host_id 覆盖；满了替换最久没连的）。调用方拿着 pairMu
void hostPut(const char *hid, const char *name, const char *dial, uint16_t port, const uint8_t key[32]) {
  int i = hostFind(hid);
  if (i < 0) {
    if (hostN < NET_HOSTS) {
      i = hostN++;
    } else {
      i = 0;
      for (int j = 1; j < hostN; j++)
        if (hosts[j].use < hosts[i].use) i = j;
    }
  }
  PairHost &h = hosts[i];
  memset(&h, 0, sizeof(h));
  strlcpy(h.hid, hid, sizeof(h.hid));
  strlcpy(h.name, name && *name ? name : "电脑", sizeof(h.name));
  strlcpy(h.dial, dial, sizeof(h.dial));
  h.port = port ? port : NET_PORT_DEF;
  memcpy(h.key, key, 32);
  h.use = ++useSeq;
  h.back = 1;
  netSavePairs();
}

void sessKill(const char *hid) {
  for (int i = 0; i < NET_SESS; i++)
    if (sess[i].used && (!strcmp(sess[i].hid, hid) || !strcmp(sess[i].dialHid, hid))) sess[i].kill = true;
}

bool sessHas(const char *hid) {
  for (int i = 0; i < NET_SESS; i++)
    if (sess[i].used && !sess[i].kill && (!strcmp(sess[i].hid, hid) || !strcmp(sess[i].dialHid, hid))) return true;
  return false;
}

String joinDial(JsonDocument &doc) {
  String dial;
  for (const char *a : doc["dial"].as<JsonArray>())
    if (a && !strchr(a, ',')) dial += String(dial.length() ? "," : "") + a;
  return dial;
}

// {"t":"pair"}：插着 USB 时 bridge 自动发（只认串口）
void netPair(JsonDocument &doc) {
  size_t kl = 0;
  const char *k = doc["k"] | "";
  const char *hid = doc["id"] | "";
  uint8_t key[33];
  if (!validHid(hid) || mbedtls_base64_decode(key, sizeof(key), &kl, (const uint8_t *)k, strlen(k)) || kl != 32) return;
  String dial = joinDial(doc);
  pairLock();
  hostPut(hid, doc["name"] | "", dial.c_str(), doc["port"] | NET_PORT_DEF, key);
  pairUnlock();
  sessKill(hid);  // 换了密钥：旧连接作废，按新密钥重连
  Host.printf("PAIRED {\"id\":\"%s\",\"host\":\"%s\"}\n", boardId.c_str(), hid);
}

// {"t":"net","id":host_id,"dial":[...]}：电脑换了 IP（每次插着 USB 连上 bridge 都会发一次；空列表 = 由电脑来连板子）
void netSetDial(JsonDocument &doc) {
  if (!doc["dial"].is<JsonArray>()) return;
  const char *hid = doc["id"] | "";
  String dial = joinDial(doc);
  pairLock();
  int i = *hid ? hostFind(hid) : (hostN == 1 ? 0 : -1);  // 旧 bridge 不带 id
  if (i >= 0 && dial != hosts[i].dial) {
    strlcpy(hosts[i].dial, dial.c_str(), sizeof(hosts[i].dial));
    hosts[i].nextDial = 0;
    netSavePairs();
  }
  pairUnlock();
}

String netHelloFields() {
  String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
  String hs, last;
  uint32_t lu = 0;
  pairLock();
  for (int i = 0; i < hostN; i++) {
    hs += String(i ? ",\"" : "\"") + hosts[i].hid + "\"";
    if (hosts[i].use >= lu) { lu = hosts[i].use; last = hosts[i].hid; }
  }
  pairUnlock();
  return String(",\"proto\":2,\"net\":2,\"id\":\"") + boardId + "\",\"ip\":\"" + ip + "\",\"host\":\"" + last + "\",\"hosts\":[" + hs +
         "],\"wifi\":" + (netUp ? 1 : 0);
}

// ---------- 设置页「电脑」用 ----------
bool pairOpen() { return pairOpenUntil && (int32_t)(pairOpenUntil - millis()) > 0; }

void netPairWindow(bool on) {
  pairOpenUntil = on ? (millis() + 120000) | 1 : 0;
  if (!on && pairSas >= 0) pairAns = 2;
}

void netPairAnswer(bool yes) {
  if (pairSas >= 0) pairAns = yes ? 1 : 2;
}

void netRemoveHost(int i) {
  char hid[17] = "";
  pairLock();
  if (i >= 0 && i < hostN) {
    strlcpy(hid, hosts[i].hid, sizeof(hid));
    if (!strcmp(prefHid, hid)) prefHid[0] = 0;
    for (int j = i; j < hostN - 1; j++) hosts[j] = hosts[j + 1];
    hostN--;
    netSavePairs();
  }
  pairUnlock();
  if (hid[0]) sessKill(hid);
}

void netTogglePref(int i) {
  pairLock();
  if (i >= 0 && i < hostN) {
    if (!strcmp(prefHid, hosts[i].hid)) prefHid[0] = 0;
    else strlcpy(prefHid, hosts[i].hid, sizeof(prefHid));
    netSavePairs();
  }
  pairUnlock();
}

// 第 i 台电脑现在：2 控制中 / 1 待命 / 0 没连
int netHostState(int i) {
  char hid[17];
  pairLock();
  strlcpy(hid, i < hostN ? hosts[i].hid : "", sizeof(hid));
  pairUnlock();
  for (int s = 0; s < NET_SESS; s++)
    if (hid[0] && sess[s].used && sess[s].authed && !sess[s].kill && !strcmp(sess[s].hid, hid)) return sess[s].sent == 1 ? 2 : 1;
  return 0;
}

// ---------- 会话 ----------
bool readLine(WiFiClient &c, String &out, uint32_t ms, size_t maxLen) {
  out = "";
  uint32_t t0 = millis();
  while (millis() - t0 < ms && c.connected()) {
    int ch = c.read();
    if (ch < 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    if (ch == '\n') return true;
    if (out.length() >= maxLen) return false;
    out += (char)ch;
  }
  return false;
}

// 屏幕验证码配对：line = "PAIR <host_id> <电脑公钥> <port> <名字 base64> <地址|->"
bool netPairWifi(NetSess &S, const String &line, const String &nB) {
  if (!pairOpen() || pairBusy) {
    S.c.print("ERR closed\n");
    return false;
  }
  String tok[6];
  int n = 0, s = 0;
  while (n < 6 && s <= (int)line.length()) {
    int e = line.indexOf(' ', s);
    if (e < 0) e = line.length();
    tok[n++] = line.substring(s, e);
    s = e + 1;
  }
  uint8_t pubH[32];
  if (n != 6 || !validHid(tok[1].c_str()) || !unhex(tok[2], pubH, 32)) {
    S.c.print("ERR bad\n");
    return false;
  }
  pairBusy = true;
  uint8_t pubB[32], sh[32];
  size_t ol = 0;
  mbedtls_ecp_group grp;
  mbedtls_mpi d, z;
  mbedtls_ecp_point Q, Qp;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  mbedtls_ecp_point_init(&Q);
  mbedtls_ecp_point_init(&Qp);
  bool ok = !mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) && !mbedtls_ecp_gen_keypair(&grp, &d, &Q, netRng, nullptr) &&
            !mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &ol, pubB, 32) && ol == 32 &&
            !mbedtls_ecp_point_read_binary(&grp, &Qp, pubH, 32) && !mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d, netRng, nullptr) &&
            !mbedtls_mpi_write_binary_le(&z, sh, 32);
  mbedtls_ecp_point_free(&Qp);
  mbedtls_ecp_point_free(&Q);
  mbedtls_mpi_free(&z);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  if (!ok) {
    S.c.print("ERR crypto\n");
    pairBusy = false;
    return false;
  }
  String pubBh = hexOf(pubB, 32);
  S.c.print("PAIRKEY " + pubBh + "\n");
  String m = "sas|" + boardId + "|" + nB + "|" + line + "|" + pubBh;
  uint8_t mac[32];
  hmac256(sh, 32, (const uint8_t *)m.c_str(), m.length(), mac);
  uint8_t nm[40];
  size_t nl = 0;
  if (mbedtls_base64_decode(nm, sizeof(nm) - 1, &nl, (const uint8_t *)tok[4].c_str(), tok[4].length())) nl = 0;
  nm[nl] = 0;
  strlcpy(pairReqName, nl ? (const char *)nm : "电脑", sizeof(pairReqName));
  pairAns = 0;
  pairSas = (int)((((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) | ((uint32_t)mac[2] << 8) | mac[3]) % 1000000);
  uint32_t t0 = millis();
  while (!pairAns && millis() - t0 < 90000 && S.c.connected() && pairOpen()) vTaskDelay(pdMS_TO_TICKS(50));
  int ans = pairAns;
  pairSas = -1;
  if (ans == 1) {
    uint8_t salt[64], K[32];
    memcpy(salt, pubH, 32);
    memcpy(salt + 32, pubB, 32);
    hkdf32(sh, salt, 64, "buddy pair", K);
    pairLock();
    hostPut(tok[1].c_str(), pairReqName, tok[5] == "-" ? "" : tok[5].c_str(), tok[3].toInt(), K);
    pairUnlock();
    sessKill(tok[1].c_str());
    S.c.print("PAIROK\n");
    pairOpenUntil = 0;
    snprintf(pairMsg, sizeof(pairMsg), "已与 %s 配对", pairReqName);
  } else {
    S.c.print(ans == 2 ? "ERR denied\n" : "ERR timeout\n");
    snprintf(pairMsg, sizeof(pairMsg), ans == 2 ? "已拒绝 %s" : "%s 配对超时", pairReqName);
  }
  pairMsgAt = millis();
  memset(sh, 0, sizeof(sh));
  pairBusy = false;
  return ans == 1;
}

// 返回 1 认证通过 / 2 刚配对完（关掉，等对方按新密钥重连）/ 0 失败
int netHandshake(NetSess &S) {
  uint8_t nb[16], nh[16], mac[32], key[32];
  esp_fill_random(nb, 16);
  String nB = hexOf(nb, 16);
  S.c.print("BUDDY/2 " + boardId + " " + nB + "\n");
  String l;
  if (!readLine(S.c, l, 5000, 400)) return 0;
  if (l.startsWith("PAIR ")) return netPairWifi(S, l, nB) ? 2 : 0;
  if (!l.startsWith("HOST ")) return 0;
  int a = l.indexOf(' ', 5), b = l.indexOf(' ', a + 1);
  if (a < 0 || b < 0) return 0;
  String hid = l.substring(5, a), nH = l.substring(a + 1, b), macH = l.substring(b + 1);
  pairLock();
  int i = hostFind(hid.c_str());
  if (i >= 0) memcpy(key, hosts[i].key, 32);
  pairUnlock();
  if (i < 0 || !unhex(nH, nh, 16)) {
    S.c.print("ERR unknown\n");
    authFailAt = millis() | 1;
    return 0;
  }
  String m = "H|" + boardId + "|" + nB + "|" + nH;
  hmac256(key, 32, (const uint8_t *)m.c_str(), m.length(), mac);
  String want = hexOf(mac, 32);
  uint8_t diff = macH.length() != want.length();
  for (int j = 0; j < (int)want.length() && j < (int)macH.length(); j++) diff |= want[j] ^ macH[j];
  if (diff) {
    S.c.print("ERR auth\n");
    authFailAt = millis() | 1;
    return 0;
  }
  m = "B|" + hid + "|" + nH + "|" + nB;
  hmac256(key, 32, (const uint8_t *)m.c_str(), m.length(), mac);
  S.c.print("OK " + hexOf(mac, 32) + "\n");
  uint8_t salt[32];
  memcpy(salt, nb, 16);
  memcpy(salt + 16, nh, 16);
  hkdf32(key, salt, 32, "buddy b2h", S.kTx);
  hkdf32(key, salt, 32, "buddy h2b", S.kRx);
  strlcpy(S.hid, hid.c_str(), sizeof(S.hid));
  return 1;
}

void nonceOf(uint8_t iv[12], const char *dir, uint64_t n) {
  memcpy(iv, dir, 4);
  memcpy(iv + 4, &n, 8);
}

bool netSend(NetSess &S, mbedtls_gcm_context &g, uint64_t &n, uint8_t type, const uint8_t *d, size_t len) {
  uint8_t *pb = S.pb, *tb = S.tb;
  pb[0] = type;
  if (len) memcpy(pb + 1, d, len);
  uint8_t iv[12];
  nonceOf(iv, "B2H\0", n++);
  uint32_t fl = len + 1 + 16;
  memcpy(tb, &fl, 4);
  if (mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len + 1, iv, 12, nullptr, 0, pb, tb + 4, 16, tb + 4 + len + 1)) return false;
  return S.c.write(tb, fl + 4) == fl + 4;
}

void netSession(NetSess &S) {
  mbedtls_gcm_context ge, gd;
  mbedtls_gcm_init(&ge);
  mbedtls_gcm_init(&gd);
  mbedtls_gcm_setkey(&ge, MBEDTLS_CIPHER_ID_AES, S.kTx, 256);
  mbedtls_gcm_setkey(&gd, MBEDTLS_CIPHER_ID_AES, S.kRx, 256);
  uint64_t txN = 0, rxN = 0;
  uint32_t lastRx = millis(), lastTx = 0;
  uint8_t hdr[4];
  int hdrN = 0;
  uint32_t need = 0, got = 0;
  S.sent = -1;
  while (S.c.connected() && WiFi.status() == WL_CONNECTED && !S.kill) {
    bool busy = false;
    // 收：长度 + 密文 + tag
    while (S.c.available() > 0) {
      busy = true;
      if (hdrN < 4) {
        int ch = S.c.read();
        if (ch < 0) break;
        hdr[hdrN++] = ch;
        if (hdrN == 4) {
          memcpy(&need, hdr, 4);
          got = 0;
          if (need < 17 || need > NET_MAX + 17) goto out;
        }
        continue;
      }
      int r = S.c.read(S.fb + got, need - got);
      if (r <= 0) break;
      got += r;
      if (got < need) continue;
      uint8_t iv[12];
      nonceOf(iv, "H2B\0", rxN++);
      size_t pl = need - 16;
      if (mbedtls_gcm_auth_decrypt(&gd, pl, iv, 12, nullptr, 0, S.fb + pl, 16, S.fb, S.pb)) goto out;  // 校验失败：断开
      lastRx = millis();
      if (S.sent == 1 && S.pb[0] == 1 && pl > 1) {  // 待命的电脑发来的行不处理
        xSemaphoreTake(netRxMu, portMAX_DELAY);
        xMessageBufferSend(netRx, S.pb + 1, pl - 1, pdMS_TO_TICKS(200));
        xSemaphoreGive(netRxMu);
      }
      hdrN = 0;
    }
    // 角色变了：告诉对方
    int8_t w = S.want;
    if (w != S.sent) {
      const char *r = w ? "{\"t\":\"role\",\"r\":\"active\"}" : "{\"t\":\"role\",\"r\":\"standby\"}";
      if (!netSend(S, ge, txN, 1, (const uint8_t *)r, strlen(r))) goto out;
      S.sent = w;
      lastTx = millis();
    }
    // 发：主循环排进来的行（只有控制者发）
    if (S.sent == 1) {
      size_t n;
      bool fail = false;
      xSemaphoreTake(netTxMu, portMAX_DELAY);
      while ((n = xMessageBufferReceive(netTx, S.lb, NET_MAX, 0)) > 0) {
        busy = true;
        if (!netSend(S, ge, txN, 1, S.lb, n)) { fail = true; break; }
        lastTx = millis();
      }
      xSemaphoreGive(netTxMu);
      if (fail) goto out;
    }
    if (millis() - lastTx > 2000) {
      if (!netSend(S, ge, txN, 4, nullptr, 0)) goto out;
      lastTx = millis();
    }
    if (millis() - lastRx > 10000) goto out;  // 对方 10 秒没动静
    if (!busy) vTaskDelay(pdMS_TO_TICKS(5));
  }
out:
  mbedtls_gcm_free(&ge);
  mbedtls_gcm_free(&gd);
}

void sessTask(void *arg) {
  NetSess &S = *(NetSess *)arg;
  S.c.setNoDelay(true);
  int r = netHandshake(S);
  if (r == 1) {
    for (int j = 0; j < NET_SESS; j++)  // 同一台电脑的旧连接让位
      if (&sess[j] != &S && sess[j].used && sess[j].authed && !strcmp(sess[j].hid, S.hid)) sess[j].kill = true;
    pairLock();
    int i = hostFind(S.hid);
    if (i >= 0) {
      hosts[i].use = ++useSeq;
      hosts[i].back = 1;
      Preferences p;
      p.begin("pair", false);
      char k[4];
      snprintf(k, sizeof(k), "u%d", i);
      p.putUInt(k, hosts[i].use);
      p.putUInt("seq", useSeq);
      p.end();
    }
    pairUnlock();
    S.since = millis();
    S.authed = true;
    netSession(S);
  } else if (r == 0) {
    vTaskDelay(pdMS_TO_TICKS(1000));  // 认证失败：慢一点再放下一个，防暴力试
  }
  S.authed = false;
  S.c.stop();
  const char *h = S.hid[0] ? S.hid : S.dialHid;
  if (*h) {  // 断开后隔 1 秒再拨
    pairLock();
    int i = hostFind(h);
    if (i >= 0) hosts[i].nextDial = millis() + 1000;
    pairUnlock();
  }
  S.used = false;
  vTaskDelete(nullptr);
}

bool sessStart(WiFiClient &c, const char *dialHid) {
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 40 * 1024) return false;
  bool ok = false;
  xSemaphoreTake(sessMu, portMAX_DELAY);
  for (int i = 0; i < NET_SESS && !ok; i++) {
    NetSess &S = sess[i];
    if (S.used || !S.fb) continue;
    S.c = c;
    strlcpy(S.ip, c.remoteIP().toString().c_str(), sizeof(S.ip));
    strlcpy(S.dialHid, dialHid ? dialHid : "", sizeof(S.dialHid));
    S.hid[0] = 0;
    S.authed = S.kill = false;
    S.want = 0;
    S.sent = -1;
    S.used = true;
    ok = xTaskCreatePinnedToCore(sessTask, "nets", 8192, &S, 1, nullptr, 0) == pdPASS;
    if (!ok) S.used = false;
  }
  xSemaphoreGive(sessMu);
  return ok;
}

// 挑控制者，并把每条连接该是什么角色记下来（会话任务自己去通知对方）
void netPick() {
  bool usbAct = HWCDC::isPlugged() && serialRxAt && millis() - serialRxAt <= 3000;
  int cur = netActive, pref = -1, first = -1;
  for (int i = 0; i < NET_SESS; i++) {
    NetSess &S = sess[i];
    if (!S.used || !S.authed || S.kill) continue;
    if (prefHid[0] && !strcmp(S.hid, prefHid)) pref = i;
    if (first < 0 || (int32_t)(S.since - sess[first].since) < 0) first = i;
  }
  bool curOk = cur >= 0 && sess[cur].used && sess[cur].authed && !sess[cur].kill;
  int pick = pref >= 0 ? pref : (curOk ? cur : first);
  netActive = pick;
  netUp = pick >= 0;
  for (int i = 0; i < NET_SESS; i++) sess[i].want = (i == pick && !usbAct) ? 1 : 0;
  netSt = !hostN ? 0 : WiFi.status() != WL_CONNECTED ? 1 : pick >= 0 ? 4 : 2;
}

// 管理任务：监听 47824（电脑来连）、回 UDP 探测、挑控制者
void netTask(void *) {
  WiFiServer srv(NET_PORT_DEF);
  WiFiUDP udp;
  bool srvOn = false;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(20));
    if (WiFi.status() != WL_CONNECTED) {
      if (srvOn) {
        srv.end();
        udp.stop();
        srvOn = false;
      }
      netPick();
      continue;
    }
    if (!mdnsOn && MDNS.begin(("agent-buddy-" + boardId).c_str())) {
      MDNS.addService("agentbuddy", "tcp", NET_PORT_DEF);
      mdnsOn = true;
    }
    if (!srvOn) {
      srv.begin();
      udp.begin(NET_PORT_DEF);
      srvOn = true;
    }
    WiFiClient nc = srv.accept();
    if (nc && !sessStart(nc, nullptr)) nc.stop();
    if (udp.parsePacket() > 0) {
      char b[48];
      int l = udp.read(b, sizeof(b) - 1);
      b[max(l, 0)] = 0;
      String q(b);
      q.trim();
      if (q.startsWith("BUDDY? ") && (q.substring(7) == "*" || q.substring(7) == boardId)) {
        udp.beginPacket(udp.remoteIP(), udp.remotePort());
        udp.printf("BUDDY! %s %u %d\n", boardId.c_str(), NET_PORT_DEF, pairOpen() ? 1 : 0);
        udp.endPacket();
      }
    }
    netPick();
  }
}

// 拨号任务：板子去连有地址的电脑（macOS 那种）；每台按 1、2、4、5…… 秒退避
void dialTask(void *) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(300));
    if (WiFi.status() != WL_CONNECTED || !mdnsOn) continue;
    char hid[17] = "", dial[160];
    uint16_t port = NET_PORT_DEF;
    pairLock();
    for (int i = 0; i < hostN && !hid[0]; i++) {
      PairHost &h = hosts[i];
      if (!h.dial[0] || (int32_t)(millis() - h.nextDial) < 0 || sessHas(h.hid)) continue;
      strlcpy(hid, h.hid, sizeof(hid));
      strlcpy(dial, h.dial, sizeof(dial));
      port = h.port;
    }
    pairUnlock();
    if (!hid[0]) continue;
    bool ok = false;
    String ds = dial;
    int s = 0;
    while (!ok && s <= (int)ds.length()) {
      int e = ds.indexOf(',', s);
      if (e < 0) e = ds.length();
      String a = ds.substring(s, e);
      s = e + 1;
      if (!a.length()) continue;
      IPAddress ip;
      if (!ip.fromString(a)) {  // 主机名：xxx.local 用 mDNS 解析
        if (!a.endsWith(".local")) continue;
        ip = MDNS.queryHost(a.substring(0, a.length() - 6), 1500);
        if (ip == IPAddress()) continue;
      }
      WiFiClient c;
      if (c.connect(ip, port, 2000)) ok = sessStart(c, hid);
      if (!ok) c.stop();
    }
    pairLock();
    int i = hostFind(hid);
    if (i >= 0 && !ok) {
      hosts[i].nextDial = millis() + hosts[i].back * 1000;
      hosts[i].back = min(hosts[i].back * 2, 5);
    }
    pairUnlock();
  }
}

void netInit() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  boardId = hexOf(mac + 3, 3);
  pairMu = xSemaphoreCreateMutex();
  sessMu = xSemaphoreCreateMutex();
  netTxMu = xSemaphoreCreateMutex();
  netRxMu = xSemaphoreCreateMutex();
  hostMu = xSemaphoreCreateMutex();
  netLoadPair();
  static StaticMessageBuffer_t rxS, txS;
  const size_t RXN = 64 * 1024, TXN = 32 * 1024;
  uint8_t *rxB = (uint8_t *)heap_caps_malloc(RXN, MALLOC_CAP_SPIRAM), *txB = (uint8_t *)heap_caps_malloc(TXN, MALLOC_CAP_SPIRAM);
  netLineBuf = (char *)heap_caps_malloc(NET_MAX + 1, MALLOC_CAP_SPIRAM);
  if (!rxB || !txB || !netLineBuf) return;
  netRx = xMessageBufferCreateStatic(RXN, rxB, &rxS);
  netTx = xMessageBufferCreateStatic(TXN, txB, &txS);
  for (int i = 0; i < NET_SESS; i++) {
    NetSess &S = sess[i];
    S.fb = (uint8_t *)heap_caps_malloc(NET_MAX + 64, MALLOC_CAP_SPIRAM);
    S.pb = (uint8_t *)heap_caps_malloc(NET_MAX + 64, MALLOC_CAP_SPIRAM);
    S.tb = (uint8_t *)heap_caps_malloc(NET_MAX + 64, MALLOC_CAP_SPIRAM);
    S.lb = (uint8_t *)heap_caps_malloc(NET_MAX + 64, MALLOC_CAP_SPIRAM);
    if (!S.pb || !S.tb || !S.lb) S.fb = nullptr;  // 这一格不用
  }
  xTaskCreatePinnedToCore(netTask, "net", 6144, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(dialTask, "netdial", 6144, nullptr, 1, nullptr, 0);
}

// 主循环里取出网络来的行，交给同一个 handleLine（界面状态仍然只在主循环里改）
void pollNet() {
  if (!netRx) return;
  if (pairSas >= 0 && !(screen == SCR_SET && setSec == 5)) {  // 有电脑请求配对：跳到「电脑」页给用户确认
    setSec = 5;
    setScreen(SCR_SET);
  }
  size_t n;
  int k = 0;
  while (k++ < 32 && (n = xMessageBufferReceive(netRx, netLineBuf, NET_MAX, 0)) > 0) {
    netLineBuf[n] = 0;
    lineFromNet = true;
    handleLine(String(netLineBuf));
    lineFromNet = false;
  }
}

String netHostName(const char *hid) {
  String s;
  pairLock();
  int i = hostFind(hid);
  if (i >= 0) s = hosts[i].name;
  pairUnlock();
  return s;
}

// 设置页「关于」里的一行：现在走哪条链路
String netLinkText() {
  if (lastMsgAt && millis() - lastMsgAt < 10000 && !hostNetActive()) return netUp ? "USB（Wi-Fi 备用）" : "USB";
  int a = netActive;
  if (hostNetActive() && a >= 0) {
    String n = netHostName(sess[a].hid);
    return "Wi-Fi · " + (n.length() ? n : String(sess[a].ip));
  }
  if (authFailAt && millis() - authFailAt < 30000) return "认证失败，请重新配对";
  static const char *const S[5] = {"未配对（插 USB 或在「电脑」里添加）", "等 Wi-Fi", "没连上电脑", "", "Wi-Fi"};
  return String(S[netSt]);
}
