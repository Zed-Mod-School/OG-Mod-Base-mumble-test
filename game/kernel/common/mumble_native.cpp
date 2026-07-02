/*!
 * @file mumble_native.cpp
 * Native Mumble client embedded in gk. See mumble_native.h for the overview.
 *
 * Protocol notes (https://github.com/mumble-voip/mumble docs/dev):
 * - Control channel: TLS over TCP. Each message is framed as a 2-byte
 *   big-endian type id + 4-byte big-endian payload length + protobuf payload.
 * - We hand-roll the tiny protobuf subset we need (varints, length-delimited
 *   fields) instead of depending on libprotobuf.
 * - TLS comes from libcurl's CONNECT_ONLY mode - gk already links curl, and
 *   Mumble servers use self-signed certs, so peer verification is off (same
 *   trust model as the official client's default certificate prompt).
 */

#include "mumble_native.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "curl/curl.h"

#ifndef _WIN32
#include <sys/select.h>
#endif

namespace {

// ---------------- protocol constants ----------------

// control channel message type ids
enum MsgType : uint16_t {
  MT_Version = 0,
  MT_UDPTunnel = 1,
  MT_Authenticate = 2,
  MT_Ping = 3,
  MT_Reject = 4,
  MT_ServerSync = 5,
  MT_UserRemove = 8,
  MT_UserState = 9,
  MT_PluginDataTransmission = 26,
};

// same wire payload + dataID as the external plugin (mumble-plugin/), so the
// two paths interoperate
constexpr const char* kDataID = "opengoal-jak1-pos";
#pragma pack(push, 1)
struct PositionPayload {
  uint32_t magic;
  float pos[3];  // raw game units
};
#pragma pack(pop)

constexpr auto kSendInterval = std::chrono::milliseconds(200);
constexpr auto kPingInterval = std::chrono::seconds(15);

// ---------------- minimal protobuf writer/reader ----------------

struct PbWriter {
  std::vector<uint8_t> buf;

  void varint(uint64_t v) {
    while (v >= 0x80) {
      buf.push_back((uint8_t)(v | 0x80));
      v >>= 7;
    }
    buf.push_back((uint8_t)v);
  }
  void tag(int field, int wire) { varint((uint64_t)((field << 3) | wire)); }
  void field_varint(int field, uint64_t v) {
    tag(field, 0);
    varint(v);
  }
  void field_bytes(int field, const void* data, size_t len) {
    tag(field, 2);
    varint(len);
    const uint8_t* p = (const uint8_t*)data;
    buf.insert(buf.end(), p, p + len);
  }
  void field_string(int field, const char* s) { field_bytes(field, s, strlen(s)); }
};

struct PbReader {
  const uint8_t* p;
  const uint8_t* end;
  PbReader(const uint8_t* data, size_t len) : p(data), end(data + len) {}

  bool varint(uint64_t* out) {
    uint64_t v = 0;
    int shift = 0;
    while (p < end && shift < 64) {
      uint8_t b = *p++;
      v |= (uint64_t)(b & 0x7f) << shift;
      if (!(b & 0x80)) {
        *out = v;
        return true;
      }
      shift += 7;
    }
    return false;
  }
  // reads the next field; returns false at end of buffer. For wire type 2 the
  // value is the payload length and `data` points at it (reader advances past).
  bool next(int* field, int* wire, uint64_t* value, const uint8_t** data) {
    if (p >= end) {
      return false;
    }
    uint64_t key;
    if (!varint(&key)) {
      return false;
    }
    *field = (int)(key >> 3);
    *wire = (int)(key & 7);
    switch (*wire) {
      case 0:  // varint
        return varint(value);
      case 1:  // fixed64
        if (end - p < 8) {
          return false;
        }
        memcpy(value, p, 8);
        p += 8;
        return true;
      case 2: {  // length-delimited
        uint64_t len;
        if (!varint(&len) || (uint64_t)(end - p) < len) {
          return false;
        }
        *value = len;
        *data = p;
        p += len;
        return true;
      }
      case 5:  // fixed32
        if (end - p < 4) {
          return false;
        }
        *value = 0;
        memcpy(value, p, 4);
        p += 4;
        return true;
      default:
        return false;  // unsupported wire type - bail on the whole message
    }
  }
};

// ---------------- client state ----------------

struct Peer {
  std::string name;
  float pos[3] = {};
  bool has_pos = false;
  std::chrono::steady_clock::time_point last_pos_update{};
};

std::mutex g_mutex;  // guards everything below
MumbleNativeStatus g_status;
std::unordered_map<uint32_t, Peer> g_users;  // session -> user (everyone on the server)
float g_local_pos[3] = {};
uint32_t g_local_pos_tick = 0;

std::thread g_thread;
std::atomic<bool> g_run{false};

void set_status(MumbleNativeState state, const char* msg) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_status.state = state;
  snprintf(g_status.message, sizeof(g_status.message), "%s", msg);
}

// ---------------- framed IO over curl ----------------

struct Connection {
  CURL* curl = nullptr;
  curl_socket_t sock = CURL_SOCKET_BAD;
  std::vector<uint8_t> rxbuf;

  bool open(const char* host, int port, char* err, size_t errlen) {
    curl = curl_easy_init();
    if (!curl) {
      snprintf(err, errlen, "curl init failed");
      return false;
    }
    char url[256];
    snprintf(url, sizeof(url), "https://%s:%d", host, port);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    // Mumble servers use self-signed certificates
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
      snprintf(err, errlen, "connect failed: %s", curl_easy_strerror(rc));
      return false;
    }
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);
    return true;
  }

  void close() {
    if (curl) {
      curl_easy_cleanup(curl);
      curl = nullptr;
    }
    sock = CURL_SOCKET_BAD;
  }

  bool send_all(const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
      size_t sent = 0;
      CURLcode rc = curl_easy_send(curl, data + off, len - off, &sent);
      if (rc == CURLE_AGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      if (rc != CURLE_OK) {
        return false;
      }
      off += sent;
    }
    return true;
  }

  bool send_message(uint16_t type, const std::vector<uint8_t>& payload) {
    uint8_t hdr[6] = {
        (uint8_t)(type >> 8),           (uint8_t)(type & 0xff),
        (uint8_t)(payload.size() >> 24), (uint8_t)(payload.size() >> 16),
        (uint8_t)(payload.size() >> 8),  (uint8_t)(payload.size() & 0xff),
    };
    std::vector<uint8_t> frame;
    frame.reserve(6 + payload.size());
    frame.insert(frame.end(), hdr, hdr + 6);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return send_all(frame.data(), frame.size());
  }

  // pull whatever is available into rxbuf; returns false on connection error
  bool pump_recv() {
    uint8_t tmp[4096];
    for (;;) {
      size_t n = 0;
      CURLcode rc = curl_easy_recv(curl, tmp, sizeof(tmp), &n);
      if (rc == CURLE_AGAIN) {
        return true;  // nothing more right now
      }
      if (rc != CURLE_OK || n == 0) {
        return false;  // closed or errored
      }
      rxbuf.insert(rxbuf.end(), tmp, tmp + n);
      if (n < sizeof(tmp)) {
        return true;
      }
    }
  }

  // extract one complete frame from rxbuf if present
  bool pop_frame(uint16_t* type, std::vector<uint8_t>* payload) {
    if (rxbuf.size() < 6) {
      return false;
    }
    uint16_t t = (uint16_t)((rxbuf[0] << 8) | rxbuf[1]);
    uint32_t len = ((uint32_t)rxbuf[2] << 24) | ((uint32_t)rxbuf[3] << 16) |
                   ((uint32_t)rxbuf[4] << 8) | rxbuf[5];
    if (len > 8 * 1024 * 1024) {
      return false;  // insane length - treat as desync (caller will drop connection)
    }
    if (rxbuf.size() < 6 + (size_t)len) {
      return false;
    }
    *type = t;
    payload->assign(rxbuf.begin() + 6, rxbuf.begin() + 6 + len);
    rxbuf.erase(rxbuf.begin(), rxbuf.begin() + 6 + len);
    return true;
  }
};

// ---------------- message handling ----------------

void handle_user_state(const uint8_t* data, size_t len) {
  PbReader r(data, len);
  int field, wire;
  uint64_t value;
  const uint8_t* bytes;
  uint32_t session = 0;
  std::string name;
  bool have_session = false;
  while (r.next(&field, &wire, &value, &bytes)) {
    if (field == 1 && wire == 0) {
      session = (uint32_t)value;
      have_session = true;
    } else if (field == 3 && wire == 2) {
      name.assign((const char*)bytes, (size_t)value);
    }
  }
  if (!have_session) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& user = g_users[session];
  if (!name.empty()) {
    user.name = name;
  }
  g_status.user_count = (int)g_users.size();
}

void handle_user_remove(const uint8_t* data, size_t len) {
  PbReader r(data, len);
  int field, wire;
  uint64_t value;
  const uint8_t* bytes;
  while (r.next(&field, &wire, &value, &bytes)) {
    if (field == 1 && wire == 0) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_users.erase((uint32_t)value);
      g_status.user_count = (int)g_users.size();
    }
  }
}

void handle_plugin_data(const uint8_t* data, size_t len) {
  PbReader r(data, len);
  int field, wire;
  uint64_t value;
  const uint8_t* bytes;
  uint32_t sender = 0;
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  std::string data_id;
  while (r.next(&field, &wire, &value, &bytes)) {
    if (field == 1 && wire == 0) {
      sender = (uint32_t)value;
    } else if (field == 3 && wire == 2) {
      payload = bytes;
      payload_len = (size_t)value;
    } else if (field == 4 && wire == 2) {
      data_id.assign((const char*)bytes, (size_t)value);
    }
  }
  if (data_id != kDataID || payload_len != sizeof(PositionPayload)) {
    return;
  }
  PositionPayload pp;
  memcpy(&pp, payload, sizeof(pp));
  if (pp.magic != kMumblePeersMagic) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& user = g_users[sender];
  memcpy(user.pos, pp.pos, sizeof(user.pos));
  user.has_pos = true;
  user.last_pos_update = std::chrono::steady_clock::now();
  g_status.positions_received++;
}

// returns the reject reason, or empty if not a reject we could parse
std::string parse_reject(const uint8_t* data, size_t len) {
  PbReader r(data, len);
  int field, wire;
  uint64_t value;
  const uint8_t* bytes;
  std::string reason = "rejected by server";
  while (r.next(&field, &wire, &value, &bytes)) {
    if (field == 2 && wire == 2) {
      reason.assign((const char*)bytes, (size_t)value);
    }
  }
  return reason;
}

// ---------------- client thread ----------------

void client_thread_main(MumbleNativeConfig cfg) {
  set_status(MumbleNativeState::Connecting, "connecting...");

  curl_global_init(CURL_GLOBAL_DEFAULT);  // ref-counted, safe if gk already did

  Connection conn;
  char err[128];
  if (!conn.open(cfg.host, cfg.port, err, sizeof(err))) {
    set_status(MumbleNativeState::Failed, err);
    return;
  }

  // handshake: Version then Authenticate
  {
    PbWriter v;
    v.field_varint(1, (1u << 16) | (5u << 8) | 0u);  // version_v1: 1.5.0
    v.field_string(2, "OpenGOAL Jak 1 native client");
    v.field_string(3, "OpenGOAL");
    conn.send_message(MT_Version, v.buf);

    PbWriter a;
    a.field_string(1, cfg.username);
    if (cfg.password[0]) {
      a.field_string(2, cfg.password);
    }
    a.field_varint(5, 1);  // opus = true
    conn.send_message(MT_Authenticate, a.buf);
  }

  auto last_ping = std::chrono::steady_clock::now();
  auto last_pos_send = std::chrono::steady_clock::now();
  uint32_t last_sent_tick = 0;
  bool synced = false;

  while (g_run.load()) {
    // wait for readability (or timeout so we can run timers)
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(conn.sock, &rfds);
    timeval tv{0, 100 * 1000};  // 100 ms
    select((int)conn.sock + 1, &rfds, nullptr, nullptr, &tv);

    if (!conn.pump_recv()) {
      set_status(MumbleNativeState::Failed, "connection lost");
      break;
    }

    uint16_t type;
    std::vector<uint8_t> payload;
    while (conn.pop_frame(&type, &payload)) {
      switch (type) {
        case MT_ServerSync: {
          PbReader r(payload.data(), payload.size());
          int field, wire;
          uint64_t value;
          const uint8_t* bytes;
          while (r.next(&field, &wire, &value, &bytes)) {
            if (field == 1 && wire == 0) {
              std::lock_guard<std::mutex> lock(g_mutex);
              g_status.session = (uint32_t)value;
            }
          }
          synced = true;
          set_status(MumbleNativeState::Connected, "connected");
          break;
        }
        case MT_Reject:
          set_status(MumbleNativeState::Failed,
                     parse_reject(payload.data(), payload.size()).c_str());
          g_run = false;
          break;
        case MT_UserState:
          handle_user_state(payload.data(), payload.size());
          break;
        case MT_UserRemove:
          handle_user_remove(payload.data(), payload.size());
          break;
        case MT_PluginDataTransmission:
          handle_plugin_data(payload.data(), payload.size());
          break;
        case MT_Ping:
        case MT_UDPTunnel:  // voice - phase 2
        default:
          break;  // ignore everything else (channels, ACLs, codecs, ...)
      }
    }
    if (!g_run.load()) {
      break;
    }

    auto now = std::chrono::steady_clock::now();

    if (now - last_ping > kPingInterval) {
      last_ping = now;
      PbWriter p;
      p.field_varint(1, (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count());
      if (!conn.send_message(MT_Ping, p.buf)) {
        set_status(MumbleNativeState::Failed, "connection lost");
        break;
      }
    }

    if (synced && now - last_pos_send > kSendInterval) {
      last_pos_send = now;
      PositionPayload pp;
      pp.magic = kMumblePeersMagic;
      uint32_t tick;
      std::vector<uint32_t> targets;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        tick = g_local_pos_tick;
        memcpy(pp.pos, g_local_pos, sizeof(pp.pos));
        for (auto& [session, user] : g_users) {
          if (session != g_status.session) {
            targets.push_back(session);
          }
        }
      }
      // frozen tick = not in game; don't broadcast
      if (tick != last_sent_tick && !targets.empty()) {
        last_sent_tick = tick;
        PbWriter d;
        for (uint32_t t : targets) {
          d.field_varint(2, t);  // receiverSessions (non-packed is valid protobuf)
        }
        d.field_bytes(3, &pp, sizeof(pp));
        d.field_string(4, kDataID);
        if (!conn.send_message(MT_PluginDataTransmission, d.buf)) {
          set_status(MumbleNativeState::Failed, "connection lost");
          break;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.positions_sent++;
      }
    }
  }

  conn.close();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_users.clear();
    g_status.user_count = 0;
    g_status.session = 0;
    if (g_status.state == MumbleNativeState::Connected ||
        g_status.state == MumbleNativeState::Connecting) {
      g_status.state = MumbleNativeState::Disconnected;
      snprintf(g_status.message, sizeof(g_status.message), "disconnected");
    }
  }
}

}  // namespace

// ---------------- public interface ----------------

MumbleNativeConfig g_mumble_native_config;

void mumble_native_connect() {
  mumble_native_disconnect();
  if (!g_mumble_native_config.username[0]) {
    set_status(MumbleNativeState::Failed, "set a username first");
    return;
  }
  g_run = true;
  g_thread = std::thread(client_thread_main, g_mumble_native_config);
}

void mumble_native_disconnect() {
  g_run = false;
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

bool mumble_native_connected() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_status.state == MumbleNativeState::Connected;
}

MumbleNativeStatus mumble_native_get_status() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_status;
}

void mumble_native_update_position(const float pos[3]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  memcpy(g_local_pos, pos, sizeof(g_local_pos));
  g_local_pos_tick++;
}

int mumble_native_get_peers(MumbleLinkPeer* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_status.state != MumbleNativeState::Connected) {
    return 0;
  }
  const auto now = std::chrono::steady_clock::now();
  int count = 0;
  for (auto& [session, user] : g_users) {
    if (count >= kMaxMumblePeers) {
      break;
    }
    if (session == g_status.session || !user.has_pos) {
      continue;
    }
    if (now - user.last_pos_update > std::chrono::seconds(3)) {
      continue;
    }
    auto& peer = out[count++];
    snprintf(peer.name, sizeof(peer.name), "%s", user.name.c_str());
    memcpy(peer.pos, user.pos, sizeof(peer.pos));
  }
  return count;
}
