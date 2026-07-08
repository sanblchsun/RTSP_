#include "rdp_agent.h"
#include "capture_wgc.h"
#include "encoder_x264.h"
#include "rtsp_client.h"
#include "rtp/h264_rtp_packetizer.h"
#include <iostream>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <random>
#include <ctime>
#include <cstdarg>
#include <unordered_map>
#include <tlhelp32.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Secur32.lib")

#include <mstcpip.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "kernel32.lib")

std::atomic<int> RDPAgent::g_screen_w{1920};
std::atomic<int> RDPAgent::g_screen_h{1080};
std::atomic<int> RDPAgent::g_video_w{0};
std::atomic<int> RDPAgent::g_video_h{0};
std::atomic<int> RDPAgent::g_screen_origin_x{0};
std::atomic<int> RDPAgent::g_screen_origin_y{0};
std::atomic<int64_t> RDPAgent::g_last_frame_time{0};
std::mutex RDPAgent::g_clip_m;
std::string RDPAgent::g_last_clip;
std::mutex RDPAgent::g_monitors_m;
std::vector<MonitorInfo> RDPAgent::g_monitors;
std::atomic<int> RDPAgent::g_vscreen_x{0};
std::atomic<int> RDPAgent::g_vscreen_y{0};
std::atomic<int> RDPAgent::g_vscreen_w{0};
std::atomic<int> RDPAgent::g_vscreen_h{0};
std::atomic<int> RDPAgent::g_last_mouse_x{-1};
std::atomic<int> RDPAgent::g_last_mouse_y{-1};
std::atomic<bool> RDPAgent::g_input_pending{false};

// ─── Raw TCP helpers ────────────────────────────────────────

bool RDPAgent::send_all_raw(SOCKET s, const char *p, int n)
{
    while (n > 0) {
        int k = send(s, p, n, 0);
        if (k <= 0) return false;
        p += k; n -= k;
    }
    return true;
}

int RDPAgent::recv_n_raw(SOCKET s, char *p, int n)
{
    int got = 0;
    while (got < n) {
        int k = recv(s, p + got, n - got, 0);
        if (k <= 0) return got;
        got += k;
    }
    return got;
}

bool RDPAgent::sock_has_data(TlsConn *c, int timeout_ms)
{
    if (!c || c->sock == INVALID_SOCKET) return false;
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(c->sock, &readfds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(0, &readfds, NULL, NULL, &tv);
    return r > 0;
}

static void set_socket_keepalive(SOCKET s)
{
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char *)&one, sizeof(one));
    tcp_keepalive vals{};
    vals.onoff = 1;
    vals.keepalivetime = 5000;
    vals.keepaliveinterval = 3000;
    DWORD bytesReturned = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &vals, sizeof(vals), NULL, 0, &bytesReturned, NULL, NULL);
}

SOCKET RDPAgent::tcp_connect(const std::string &host, int port)
{
    addrinfo hints{}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string p = std::to_string(port);
    if (getaddrinfo(host.c_str(), p.c_str(), &hints, &res) != 0)
        return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (auto *a = res; a; a = a->ai_next)
    {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET)
    {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
        set_socket_keepalive(s);
    }
    return s;
}

// ─── TLS (Schannel) ─────────────────────────────────────────

void RDPAgent::tls_close(TlsConn *c)
{
    if (!c) return;
    if (c->ctx_ok) { DeleteSecurityContext(&c->ctx); c->ctx_ok = false; }
    if (c->cred_ok) { FreeCredentialHandle(&c->cred); c->cred_ok = false; }
    if (c->sock != INVALID_SOCKET) { closesocket(c->sock); c->sock = INVALID_SOCKET; }
}

bool RDPAgent::tls_handshake(TlsConn *c, const std::string &host, bool verify_cert)
{
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS;
    sc.dwFlags |= verify_cert ? SCH_CRED_AUTO_CRED_VALIDATION : SCH_CRED_MANUAL_CRED_VALIDATION;
    SECURITY_STATUS ss = AcquireCredentialsHandleA(NULL, const_cast<char *>(UNISP_NAME_A),
        SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &c->cred, NULL);
    if (ss != SEC_E_OK) return false;
    c->cred_ok = true;

    const DWORD req_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
        ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR |
        ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    std::wstring whost(host.begin(), host.end());
    SecBuffer out_b = {0, SECBUFFER_TOKEN, NULL};
    SecBufferDesc out_d = {SECBUFFER_VERSION, 1, &out_b};
    DWORD ret_flags = 0;

    ss = InitializeSecurityContextW(&c->cred, NULL, const_cast<wchar_t *>(whost.c_str()),
        req_flags, 0, SECURITY_NATIVE_DREP, NULL, 0, &c->ctx, &out_d, &ret_flags, NULL);
    c->ctx_ok = true;

    if (out_b.pvBuffer && out_b.cbBuffer > 0)
    {
        bool ok = send_all_raw(c->sock, (const char *)out_b.pvBuffer, (int)out_b.cbBuffer);
        FreeContextBuffer(out_b.pvBuffer); out_b.pvBuffer = NULL;
        if (!ok) return false;
    }
    if (ss != SEC_I_CONTINUE_NEEDED) return false;

    std::vector<uint8_t> in_buf;
    char tmp[16384];

    while (true)
    {
        int n = recv(c->sock, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        in_buf.insert(in_buf.end(), tmp, tmp + n);

    retry:
        SecBuffer in_bufs[2] = {
            {(ULONG)in_buf.size(), SECBUFFER_TOKEN, in_buf.data()},
            {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc in_d = {SECBUFFER_VERSION, 2, in_bufs};
        out_b = {0, SECBUFFER_TOKEN, NULL};
        out_d = {SECBUFFER_VERSION, 1, &out_b};
        ret_flags = 0;

        ss = InitializeSecurityContextW(&c->cred, &c->ctx, NULL,
            req_flags, 0, SECURITY_NATIVE_DREP, &in_d, 0, NULL, &out_d, &ret_flags, NULL);

        if (out_b.pvBuffer && out_b.cbBuffer > 0)
        {
            bool ok = send_all_raw(c->sock, (const char *)out_b.pvBuffer, (int)out_b.cbBuffer);
            FreeContextBuffer(out_b.pvBuffer); out_b.pvBuffer = NULL;
            if (!ok) return false;
        }

        if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0)
        {
            size_t off = in_buf.size() - in_bufs[1].cbBuffer;
            std::vector<uint8_t> extra(in_buf.begin() + (ptrdiff_t)off, in_buf.end());
            in_buf = std::move(extra);
        }
        else if (ss != SEC_E_INCOMPLETE_MESSAGE)
            in_buf.clear();

        if (ss == SEC_E_OK) break;
        if (ss == SEC_I_CONTINUE_NEEDED) continue;
        if (ss == SEC_E_INCOMPLETE_MESSAGE)
        {
            n = recv(c->sock, tmp, sizeof tmp, 0);
            if (n <= 0) return false;
            in_buf.insert(in_buf.end(), tmp, tmp + n);
            goto retry;
        }
        return false;
    }

    if (!in_buf.empty()) c->raw = std::move(in_buf);
    QueryContextAttributes(&c->ctx, SECPKG_ATTR_STREAM_SIZES, &c->sizes);
    return true;
}

TlsConn *RDPAgent::tls_connect(const std::string &host, int port, bool verify_cert)
{
    SOCKET s = tcp_connect(host, port);
    if (s == INVALID_SOCKET) return nullptr;
    TlsConn *c = new TlsConn();
    c->sock = s;
    if (!tls_handshake(c, host, verify_cert)) { tls_close(c); delete c; return nullptr; }
    return c;
}

bool RDPAgent::tls_send_all(TlsConn *c, const char *p, int n)
{
    const int MAX_MSG = (int)c->sizes.cbMaximumMessage;
    while (n > 0)
    {
        int chunk = std::min(n, MAX_MSG);
        std::vector<uint8_t> msg(c->sizes.cbHeader + (size_t)chunk + c->sizes.cbTrailer);
        SecBuffer bufs[3] = {
            {c->sizes.cbHeader, SECBUFFER_STREAM_HEADER, msg.data()},
            {(ULONG)chunk, SECBUFFER_DATA, msg.data() + c->sizes.cbHeader},
            {c->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, msg.data() + c->sizes.cbHeader + chunk}};
        SecBufferDesc desc = {SECBUFFER_VERSION, 3, bufs};
        memcpy(bufs[1].pvBuffer, p, (size_t)chunk);
        SECURITY_STATUS ss = EncryptMessage(&c->ctx, 0, &desc, 0);
        if (ss != SEC_E_OK) return false;
        int total = (int)(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
        if (!send_all_raw(c->sock, (const char *)msg.data(), total)) return false;
        p += chunk; n -= chunk;
    }
    return true;
}

int RDPAgent::tls_recv_some(TlsConn *c, char *buf, int want)
{
    if (!c->plain.empty())
    {
        int n = (int)std::min((size_t)want, c->plain.size());
        memcpy(buf, c->plain.data(), (size_t)n);
        c->plain.erase(c->plain.begin(), c->plain.begin() + n);
        return n;
    }
    char tmp[16384];
    for (;;)
    {
        while (!c->raw.empty())
        {
            SecBuffer in_bufs[4] = {
                {(ULONG)c->raw.size(), SECBUFFER_DATA, c->raw.data()},
                {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}};
            SecBufferDesc in_desc = {SECBUFFER_VERSION, 4, in_bufs};
            SECURITY_STATUS ss = DecryptMessage(&c->ctx, &in_desc, 0, NULL);
            if (ss == SEC_E_INCOMPLETE_MESSAGE) break;
            if (ss == SEC_I_CONTEXT_EXPIRED) return 0;
            if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE) return -1;
            for (int i = 0; i < 4; ++i)
                if (in_bufs[i].BufferType == SECBUFFER_DATA && in_bufs[i].cbBuffer > 0)
                    c->plain.insert(c->plain.end(),
                        (uint8_t *)in_bufs[i].pvBuffer,
                        (uint8_t *)in_bufs[i].pvBuffer + in_bufs[i].cbBuffer);
            bool has_extra = false;
            for (int i = 1; i < 4; ++i)
            {
                if (in_bufs[i].BufferType == SECBUFFER_EXTRA && in_bufs[i].cbBuffer > 0)
                {
                    size_t off = c->raw.size() - in_bufs[i].cbBuffer;
                    std::vector<uint8_t> extra(c->raw.begin() + (ptrdiff_t)off, c->raw.end());
                    c->raw = std::move(extra);
                    has_extra = true; break;
                }
            }
            if (!has_extra) c->raw.clear();
            if (!c->plain.empty())
            {
                int n = (int)std::min((size_t)want, c->plain.size());
                memcpy(buf, c->plain.data(), (size_t)n);
                c->plain.erase(c->plain.begin(), c->plain.begin() + n);
                return n;
            }
        }
        int n = recv(c->sock, tmp, sizeof tmp, 0);
        if (n <= 0) return -1;
        c->raw.insert(c->raw.end(), tmp, tmp + n);
    }
}

int RDPAgent::tls_recv_n(TlsConn *c, char *p, int n)
{
    int got = 0;
    while (got < n) { int k = tls_recv_some(c, p + got, n - got); if (k <= 0) return got; got += k; }
    return got;
}

// ─── WebSocket ──────────────────────────────────────────────

std::string RDPAgent::b64(const unsigned char *d, size_t n)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    while (i < n)
    {
        uint32_t v = 0; int k = (int)std::min<size_t>(3, n - i);
        for (int j = 0; j < k; ++j) v |= d[i + j] << ((2 - j) * 8);
        for (int j = 0; j < 4; ++j) o += (j <= k) ? T[(v >> ((3 - j) * 6)) & 63] : '=';
        i += 3;
    }
    return o;
}

bool RDPAgent::ws_handshake(TlsConn *c, const std::string &host, int port, const std::string &path)
{
    unsigned char k[16];
    std::random_device rd;
    for (int i = 0; i < 16; ++i) k[i] = (unsigned char)(rd() & 0xFF);
    std::ostringstream r;
    r << "GET " << path << " HTTP/1.1\r\n"
      << "Host: " << host << ":" << port << "\r\n"
      << "Upgrade: websocket\r\nConnection: Upgrade\r\n"
      << "Sec-WebSocket-Key: " << b64(k, 16) << "\r\n"
      << "Sec-WebSocket-Version: 13\r\n\r\n";
    std::string rs = r.str();
    if (!tls_send_all(c, rs.data(), (int)rs.size())) return false;
    std::string h; char ch;
    while (h.size() < 8192)
    {
        if (tls_recv_n(c, &ch, 1) != 1) return false;
        h += ch;
        if (h.size() >= 4 && h.compare(h.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    return h.find(" 101") != std::string::npos;
}

bool RDPAgent::ws_send(TlsConn *c, int op, const void *data, size_t len)
{
    std::lock_guard<std::mutex> lk(c->send_m);
    std::vector<uint8_t> f;
    f.reserve(len + 14);
    f.push_back((uint8_t)(0x80 | op));
    uint8_t mask[4];
    std::random_device rd;
    for (int i = 0; i < 4; ++i) mask[i] = (uint8_t)(rd() & 0xFF);
    if (len < 126) f.push_back((uint8_t)(0x80 | len));
    else if (len < 65536) { f.push_back((uint8_t)(0x80 | 126)); f.push_back((uint8_t)((len >> 8) & 0xFF)); f.push_back((uint8_t)(len & 0xFF)); }
    else { f.push_back((uint8_t)(0x80 | 127)); for (int i = 7; i >= 0; --i) f.push_back((uint8_t)((len >> (i * 8)) & 0xFF)); }
    for (int i = 0; i < 4; ++i) f.push_back(mask[i]);
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) f.push_back(p[i] ^ mask[i & 3]);
    return tls_send_all(c, (const char *)f.data(), (int)f.size());
}

int RDPAgent::ws_recv(TlsConn *c, std::vector<uint8_t> &payload)
{
    uint8_t h[2];
    if (tls_recv_n(c, (char *)h, 2) != 2) return -1;
    int op = h[0] & 0x0F;
    bool masked = (h[1] & 0x80) != 0;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) { uint8_t b[2]; if (tls_recv_n(c, (char *)b, 2) != 2) return -1; len = ((uint64_t)b[0] << 8) | b[1]; }
    else if (len == 127) { uint8_t b[8]; if (tls_recv_n(c, (char *)b, 8) != 8) return -1; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | b[i]; }
    uint8_t mk[4] = {0,0,0,0};
    if (masked && tls_recv_n(c, (char *)mk, 4) != 4) return -1;
    if (len > (8u << 20)) return -1;
    payload.resize((size_t)len);
    if (len && tls_recv_n(c, (char *)payload.data(), (int)len) != (int)len) return -1;
    if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mk[i & 3];
    if (op == 0x8) return -1;
    if (op == 0x9) { ws_send(c, 0xA, payload.data(), payload.size()); return 0; }
    if (op == 0xA) return 0;
    return (op == 0x1) ? 1 : (op == 0x2) ? 2 : 0;
}

// ─── JSON helpers ───────────────────────────────────────────

std::string RDPAgent::json_escape(const std::string &s)
{
    std::string o;
    for (char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if ((unsigned char)c < 32) { char b[8]; sprintf_s(b, "\\u%04x", (unsigned char)c); o += b; }
        else o += c;
    }
    return o;
}

bool RDPAgent::json_str(const std::string &j, const std::string &k, std::string &out)
{
    std::string p = "\"" + k + "\":\"";
    size_t s = j.find(p);
    if (s == std::string::npos) return false;
    s += p.size();
    size_t e = j.find("\"", s);
    if (e == std::string::npos) return false;
    out = j.substr(s, e - s);
    return true;
}

bool RDPAgent::json_str_ex(const std::string &j, const std::string &k, std::string &out)
{
    std::string p = "\"" + k + "\":\"";
    size_t s = j.find(p);
    if (s == std::string::npos) { p = "\"" + k + "\": \""; s = j.find(p); }
    if (s == std::string::npos) return false;
    s += p.size();
    size_t e = j.find("\"", s);
    if (e == std::string::npos) return false;
    out = j.substr(s, e - s);
    return true;
}

bool RDPAgent::json_int(const std::string &j, const std::string &k, int &out)
{
    std::string p = "\"" + k + "\":";
    size_t s = j.find(p);
    if (s == std::string::npos) { p = "\"" + k + "\": "; s = j.find(p); }
    if (s == std::string::npos) return false;
    s += p.size();
    char *e = nullptr;
    long v = strtol(j.c_str() + s, &e, 10);
    if (e == j.c_str() + s) return false;
    out = (int)v;
    return true;
}

// ─── Screen metrics ─────────────────────────────────────────

bool RDPAgent::read_screen_metrics(int &w, int &h, int &ox, int &oy)
{
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    ox = GetSystemMetrics(SM_XVIRTUALSCREEN);
    oy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return w > 0 && h > 0;
}

void RDPAgent::init_screen_metrics()
{
    int w, h, ox, oy;
    if (read_screen_metrics(w, h, ox, oy)) {
        g_screen_w = w; g_screen_h = h;
        g_screen_origin_x = ox; g_screen_origin_y = oy;
    }
    refresh_vscreen_cache();
    g_last_mouse_x = -1; g_last_mouse_y = -1;
}

void RDPAgent::refresh_vscreen_cache()
{
    g_vscreen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vscreen_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vscreen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vscreen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

void RDPAgent::read_monitors(std::vector<MonitorInfo> &out)
{
    out.clear();
    DISPLAY_DEVICEA dd = {sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); ++i)
    {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
        DEVMODEA dm = {sizeof(dm)};
        if (EnumDisplaySettingsExA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0))
        {
            MonitorInfo mi;
            mi.x = dm.dmPosition.x;
            mi.y = dm.dmPosition.y;
            mi.w = dm.dmPelsWidth;
            mi.h = dm.dmPelsHeight;
            out.push_back(mi);
        }
    }
}

bool RDPAgent::mouse_to_abs(int x, int y, int monitor_id, LONG &dx, LONG &dy, int sw, int sh)
{
    int vx = g_vscreen_x.load(), vy = g_vscreen_y.load();
    int vw = g_vscreen_w.load(), vh = g_vscreen_h.load();
    if (vw < 2 || vh < 2) return false;

    MonitorInfo mi;
    {
        std::lock_guard<std::mutex> lk(g_monitors_m);
        if ((size_t)monitor_id < g_monitors.size())
            mi = g_monitors[monitor_id];
    }

    int abs_x = mi.x + x;
    int abs_y = mi.y + y;
    dx = (LONG)((int64_t)(abs_x - vx) * 65535 / (vw - 1));
    dy = (LONG)((int64_t)(abs_y - vy) * 65535 / (vh - 1));
    if (dx < 0) dx = 0; if (dx > 65535) dx = 65535;
    if (dy < 0) dy = 0; if (dy > 65535) dy = 65535;
    return true;
}

// ─── Mouse Input ────────────────────────────────────────────

void RDPAgent::do_mouse_move(int x, int y, int monitor_id)
{
    int sw = g_video_w.load(), sh = g_video_h.load();
    if (sw <= 1 || sh <= 1) sw = g_screen_w.load(), sh = g_screen_h.load();
    if (sw <= 1 || sh <= 1) return;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= sw) x = sw - 1; if (y >= sh) y = sh - 1;
    if (x == g_last_mouse_x.load() && y == g_last_mouse_y.load()) return;

    LONG dx, dy;
    if (!mouse_to_abs(x, y, monitor_id, dx, dy, sw, sh)) return;
    g_last_mouse_x = x; g_last_mouse_y = y;

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx; in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(INPUT));
}

void RDPAgent::do_mouse_move_and_click(int x, int y, int button, bool down, int monitor_id)
{
    int sw = g_video_w.load(), sh = g_video_h.load();
    if (sw <= 1 || sh <= 1) sw = g_screen_w.load(), sh = g_screen_h.load();
    if (sw <= 1 || sh <= 1) return;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= sw) x = sw - 1; if (y >= sh) y = sh - 1;

    LONG dx, dy;
    if (!mouse_to_abs(x, y, monitor_id, dx, dy, sw, sh)) return;

    DWORD btn = 0;
    switch (button) {
        case 0: btn = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 1: btn = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 2: btn = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        default: return;
    }

    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dx = dx; in[0].mi.dy = dy;
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = btn;
    SendInput(2, in, sizeof(INPUT));
    g_last_mouse_x = x; g_last_mouse_y = y;
}

void RDPAgent::do_mouse_button(int button, bool down)
{
    INPUT in{}; in.type = INPUT_MOUSE;
    DWORD f = 0;
    switch (button) {
        case 0: f = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 1: f = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 2: f = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        default: return;
    }
    in.mi.dwFlags = f;
    SendInput(1, &in, sizeof(INPUT));
}

void RDPAgent::do_mouse_wheel(int delta)
{
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &in, sizeof(INPUT));
}

// ─── Keyboard Input ─────────────────────────────────────────

void RDPAgent::do_text_input(const std::string &utf8)
{
    if (utf8.empty()) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
    if (wlen <= 0) return;
    std::vector<wchar_t> w((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), w.data(), wlen);
    std::vector<INPUT> inputs;
    inputs.reserve(w.size() * 2);
    auto push_vk = [&](WORD vk) {
        INPUT d{}; d.type = INPUT_KEYBOARD; d.ki.wVk = vk;
        d.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        INPUT u = d; u.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(d); inputs.push_back(u);
    };
    for (wchar_t ch : w) {
        if (ch == L'\r') continue;
        if (ch == L'\n') { push_vk(VK_RETURN); continue; }
        if (ch == L'\t') { push_vk(VK_TAB); continue; }
        INPUT d{}; d.type = INPUT_KEYBOARD; d.ki.wVk = 0;
        d.ki.wScan = (WORD)ch; d.ki.dwFlags = KEYEVENTF_UNICODE;
        if ((ch & 0xFF00) == 0xE000) d.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        INPUT u = d; u.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(d); inputs.push_back(u);
    }
    if (inputs.empty()) return;
    const size_t BATCH = 128;
    for (size_t i = 0; i < inputs.size(); i += BATCH) {
        UINT n = (UINT)std::min(BATCH, inputs.size() - i);
        SendInput(n, inputs.data() + i, sizeof(INPUT));
    }
}

int RDPAgent::code_to_vk(const std::string &code)
{
    if (code.size() == 4 && code.compare(0, 3, "Key") == 0) {
        char c = code[3];
        if (c >= 'A' && c <= 'Z') return c;
    }
    if (code.size() == 6 && code.compare(0, 5, "Digit") == 0) {
        char c = code[5];
        if (c >= '0' && c <= '9') return c;
    }
    if (code.compare(0, 6, "Numpad") == 0) {
        if (code.size() == 7) { char c = code[6]; if (c >= '0' && c <= '9') return VK_NUMPAD0 + (c - '0'); }
        if (code == "NumpadAdd") return VK_ADD; if (code == "NumpadSubtract") return VK_SUBTRACT;
        if (code == "NumpadMultiply") return VK_MULTIPLY; if (code == "NumpadDivide") return VK_DIVIDE;
        if (code == "NumpadDecimal") return VK_DECIMAL; if (code == "NumpadEnter") return VK_RETURN;
    }
    if (!code.empty() && code[0] == 'F' && code.size() >= 2 && code.size() <= 3) {
        bool digits = true;
        for (size_t i = 1; i < code.size(); ++i) if (!std::isdigit((unsigned char)code[i])) { digits = false; break; }
        if (digits) { int n = std::atoi(code.c_str() + 1); if (n >= 1 && n <= 24) return VK_F1 + (n - 1); }
    }
    static const std::unordered_map<std::string, int> m = {
        {"Enter", VK_RETURN}, {"Backspace", VK_BACK}, {"Tab", VK_TAB},
        {"Space", VK_SPACE}, {"Escape", VK_ESCAPE},
        {"ArrowLeft", VK_LEFT}, {"ArrowRight", VK_RIGHT},
        {"ArrowUp", VK_UP}, {"ArrowDown", VK_DOWN},
        {"Home", VK_HOME}, {"End", VK_END},
        {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
        {"Insert", VK_INSERT}, {"Delete", VK_DELETE},
        {"ShiftLeft", VK_LSHIFT}, {"ShiftRight", VK_RSHIFT},
        {"ControlLeft", VK_LCONTROL}, {"ControlRight", VK_RCONTROL},
        {"AltLeft", VK_LMENU}, {"AltRight", VK_RMENU},
        {"MetaLeft", VK_LWIN}, {"MetaRight", VK_RWIN},
        {"OSLeft", VK_LWIN}, {"OSRight", VK_RWIN},
        {"CapsLock", VK_CAPITAL}, {"NumLock", VK_NUMLOCK},
        {"ScrollLock", VK_SCROLL}, {"PrintScreen", VK_SNAPSHOT},
        {"Pause", VK_PAUSE}, {"ContextMenu", VK_APPS},
        {"Minus", VK_OEM_MINUS}, {"Equal", VK_OEM_PLUS},
        {"BracketLeft", VK_OEM_4}, {"BracketRight", VK_OEM_6},
        {"Backslash", VK_OEM_5}, {"Semicolon", VK_OEM_1},
        {"Quote", VK_OEM_7}, {"Comma", VK_OEM_COMMA},
        {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2},
        {"Backquote", VK_OEM_3}, {"IntlBackslash", VK_OEM_102},
    };
    auto it = m.find(code);
    return it == m.end() ? 0 : it->second;
}

void RDPAgent::do_key(const std::string &code, bool down)
{
    int vk = code_to_vk(code);
    if (vk == 0) return;
    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    switch (vk) {
        case VK_RMENU: case VK_RCONTROL: case VK_LEFT: case VK_RIGHT:
        case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
        case VK_HOME: case VK_END: case VK_INSERT: case VK_DELETE:
        case VK_SNAPSHOT: case VK_APPS: case VK_LWIN: case VK_RWIN:
        case VK_NUMLOCK: in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY; break;
    }
    SendInput(1, &in, sizeof(INPUT));
}

void RDPAgent::release_modifier_keys()
{
    WORD mods[] = { VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                    VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
    for (WORD vk : mods) {
        INPUT in{}; in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk; in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));
    }
}

// ─── Clipboard ──────────────────────────────────────────────

std::string RDPAgent::clipboard_read_utf8()
{
    std::string result;
    if (!OpenClipboard(NULL)) return result;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        wchar_t *ws = (wchar_t *)GlobalLock(h);
        if (ws) {
            int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
            if (len > 0) {
                std::vector<char> buf((size_t)len);
                WideCharToMultiByte(CP_UTF8, 0, ws, -1, buf.data(), len, NULL, NULL);
                result.assign(buf.data());
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return result;
}

void RDPAgent::clipboard_write_utf8(const std::string &utf8)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
    if (wlen <= 0) return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wlen + 1) * sizeof(wchar_t));
    if (!h) return;
    wchar_t *ws = (wchar_t *)GlobalLock(h);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), ws, wlen);
    ws[wlen] = 0;
    GlobalUnlock(h);
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, h);
        CloseClipboard();
    } else {
        GlobalFree(h);
    }
}

// ─── Control message handling ───────────────────────────────

static size_t hash_cursor_bitmap(HCURSOR hcur)
{
    ICONINFO ii{};
    if (!GetIconInfo(hcur, &ii)) return 0;
    size_t h = 0;
    if (ii.hbmMask) {
        BITMAP bm{}; GetObject(ii.hbmMask, sizeof(BITMAP), &bm);
        LONG n = bm.bmWidthBytes * bm.bmHeight;
        if (n > 0 && n < 65536) {
            std::vector<uint8_t> bits((size_t)n);
            GetBitmapBits(ii.hbmMask, n, bits.data());
            for (size_t i = 0; i < bits.size(); i++) h = h * 31 + (size_t)bits[i];
        }
        DeleteObject(ii.hbmMask);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    h ^= ((size_t)ii.xHotspot << 16) ^ (size_t)ii.yHotspot;
    return h;
}

void RDPAgent::handle_control(const std::string &j)
{
    std::string type;
    if (!json_str(j, "type", type)) return;

    // Track activity on any input
    if (shm) shm->last_activity_time = GetTickCount64();
    runtime.last_activity_time = std::chrono::steady_clock::now();

    g_input_pending = true;

    if (type == "mouse_move") {
        int x = 0, y = 0, mon = 0;
        json_int(j, "x", x); json_int(j, "y", y); json_int(j, "monitor_id", mon);
        do_mouse_move(x, y, mon);
    }
    else if (type == "mouse_down" || type == "mouse_up") {
        int x = 0, y = 0, button = 0, mon = 0;
        json_int(j, "x", x); json_int(j, "y", y);
        json_int(j, "button", button); json_int(j, "monitor_id", mon);
        bool down = (type == "mouse_down");
        if (x >= 0 && y >= 0) do_mouse_move_and_click(x, y, button, down, mon);
        else do_mouse_button(button, down);
    }
    else if (type == "mouse_wheel") {
        int delta = 0; json_int(j, "delta", delta);
        do_mouse_wheel(delta);
    }
    else if (type == "key_down" || type == "key_up") {
        std::string code; json_str(j, "code", code);
        do_key(code, type == "key_down");
    }
    else if (type == "text") {
        std::string text; json_str(j, "text", text);
        do_text_input(text);
    }
    else if (type == "clipboard") {
        std::string text; json_str(j, "text", text);
        clipboard_write_utf8(text);
    }
    else if (type == "release_modifiers") {
        release_modifier_keys();
    }
    else if (type == "video_size") {
        int w = 0, h = 0;
        if (json_int(j, "w", w)) g_video_w = w;
        if (json_int(j, "h", h)) g_video_h = h;
    }
    else if (type == "config_update") {
        std::string enc, qual; int fps = 0;
        std::lock_guard<std::mutex> lk(runtime.m);
        if (json_str(j, "encoder", enc)) runtime.encoder = enc;
        if (json_str(j, "quality_preset", qual)) { runtime.quality_preset = qual; runtime.quality_qp = preset_to_qp(qual); }
        if (json_int(j, "framerate", fps) && fps > 0) runtime.framerate = fps;
        if (json_int(j, "timeout_min", fps) && fps > 0) { runtime.timeout_min = fps; if (shm) shm->timeout_min = fps; }
        if (json_str(j, "quality_preset", qual)) runtime.quality_preset = qual;
    }
    else if (type == "config_get") {
        // Config is sent in hello
    }

    g_input_pending = false;
}

// ─── Control send helpers ───────────────────────────────────

void RDPAgent::ctrl_send_hello()
{
    std::string j = make_hello_json();
    std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
    if (runtime.ctrl_conn)
        ws_send(runtime.ctrl_conn, 1, j.data(), j.size());
}

void RDPAgent::ctrl_send_clipboard(const std::string &text)
{
    std::string j = "{\"type\":\"clipboard\",\"text\":\"" + json_escape(text) + "\"}";
    std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
    if (runtime.ctrl_conn)
        ws_send(runtime.ctrl_conn, 1, j.data(), j.size());
}

void RDPAgent::ctrl_send_cursor(int shape_id)
{
    std::string j = "{\"type\":\"cursor\",\"shape\":" + std::to_string(shape_id) + "}";
    std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
    if (runtime.ctrl_conn)
        ws_send(runtime.ctrl_conn, 1, j.data(), j.size());
}

void RDPAgent::ctrl_send_monitor_status(int monitor_id, int bitrate_kbps, int fps, bool overloaded)
{
    std::string j = "{\"type\":\"monitor_status\",\"monitor_id\":" + std::to_string(monitor_id) +
        ",\"bitrate_kbps\":" + std::to_string(bitrate_kbps) +
        ",\"fps\":" + std::to_string(fps) +
        ",\"overloaded\":" + (overloaded ? "true" : "false") + "}";
    std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
    if (runtime.ctrl_conn)
        ws_send(runtime.ctrl_conn, 1, j.data(), j.size());
}

std::string RDPAgent::make_hello_json() const
{
    std::vector<MonitorInfo> monitors;
    read_monitors(monitors);

    std::string mon_json = "[";
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (i > 0) mon_json += ",";
        mon_json += "{\"id\":" + std::to_string(i) +
            ",\"x\":" + std::to_string(monitors[i].x) +
            ",\"y\":" + std::to_string(monitors[i].y) +
            ",\"w\":" + std::to_string(monitors[i].w) +
            ",\"h\":" + std::to_string(monitors[i].h) + "}";
    }
    mon_json += "]";

    return "{\"type\":\"hello\",\"agent_id\":\"" + json_escape(config.agent_id) +
        "\",\"monitors\":" + mon_json +
        ",\"fps\":" + std::to_string(config.framerate) +
        ",\"quality_preset\":\"" + json_escape(config.quality_preset) + "\"}";
}

int RDPAgent::preset_to_qp(const std::string &preset)
{
    if (preset == "ultra") return 18;
    if (preset == "high") return 21;
    if (preset == "medium") return 23;
    if (preset == "low") return 28;
    if (preset == "ultra_low") return 35;
    return 23;
}

// ─── Loops ──────────────────────────────────────────────────

void RDPAgent::control_loop()
{
    while (!runtime.stop)
    {
        TlsConn *c = tls_connect(config.server_host, config.server_port, config.verify_cert);
        if (!c) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
        std::string path = "/relay/ws/control/worker/" + config.agent_id + "?token=" + config.agent_token;
        if (!ws_handshake(c, config.server_host, config.server_port, path))
        {
            tls_close(c); delete c;
            std::this_thread::sleep_for(std::chrono::seconds(3)); continue;
        }
        {
            std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
            runtime.ctrl_conn = c;
        }
        release_modifier_keys();
        ctrl_send_hello();

        std::vector<uint8_t> buf;
        while (!runtime.stop)
        {
            int r = ws_recv(c, buf);
            if (r < 0) break;
            if (r == 1) { std::string msg(buf.begin(), buf.end()); handle_control(msg); }
        }
        {
            std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
            runtime.ctrl_conn = nullptr;
        }
        tls_close(c); delete c;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void RDPAgent::resolution_watch_loop()
{
    while (!runtime.stop)
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!m_monitor) continue;
        RECT r; GetWindowRect(GetDesktopWindow(), &r);
        int nw = r.right - r.left, nh = r.bottom - r.top;
        if (nw != m_monitor->w || nh != m_monitor->h)
        {
            logf("[res] resolution changed: %dx%d -> %dx%d, restarting pipeline", m_monitor->w, m_monitor->h, nw, nh);
            runtime.restart = true;
            return;
        }
    }
}

void RDPAgent::clipboard_watch_loop()
{
    {
        std::string cur = clipboard_read_utf8();
        std::lock_guard<std::mutex> lk(g_clip_m);
        g_last_clip = cur;
    }
    while (!runtime.stop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (runtime.stop) break;
        std::string cur = clipboard_read_utf8();
        if (cur.empty()) continue;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(g_clip_m);
            if (cur != g_last_clip) { g_last_clip = cur; changed = true; }
        }
        if (!changed) continue;
        if (cur.size() > 512 * 1024) continue;
        ctrl_send_clipboard(cur);
    }
}

void RDPAgent::cursor_watch_loop()
{
    MSG msg;
    PeekMessageA(&msg, NULL, 0, 0, PM_NOREMOVE);

    struct CursorEntry { int id; HCURSOR hcur; size_t hash; };
    auto load_cur = [](int id, LPCSTR name) -> CursorEntry {
        HCURSOR h = LoadCursorA(NULL, name); return {id, h, hash_cursor_bitmap(h)};
    };

    std::vector<CursorEntry> table;
    table.push_back(load_cur(32512, IDC_ARROW));
    table.push_back(load_cur(32513, IDC_IBEAM));
    table.push_back(load_cur(32514, IDC_WAIT));
    table.push_back(load_cur(32515, IDC_CROSS));
    table.push_back(load_cur(32516, IDC_UPARROW));
    table.push_back(load_cur(32642, IDC_SIZENWSE));
    table.push_back(load_cur(32643, IDC_SIZENESW));
    table.push_back(load_cur(32644, IDC_SIZEWE));
    table.push_back(load_cur(32645, IDC_SIZENS));
    table.push_back(load_cur(32646, IDC_SIZEALL));
    table.push_back(load_cur(32648, IDC_NO));
    table.push_back(load_cur(32649, IDC_HAND));

    int last_shape = 32512;
    while (!runtime.stop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (runtime.stop) break;
        CURSORINFO ci = { sizeof(CURSORINFO) };
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) continue;
        int shape = 0;
        for (auto &e : table) { if (ci.hCursor == e.hcur) { shape = e.id; break; } }
        if (!shape) {
            size_t cur_hash = hash_cursor_bitmap(ci.hCursor);
            if (cur_hash) for (auto &e : table) { if (e.hash == cur_hash) { shape = e.id; break; } }
        }
        if (!shape) shape = 32512;
        if (shape != last_shape) { last_shape = shape; ctrl_send_cursor(shape); }
    }
}

void RDPAgent::session_keepalive_loop()
{
    bool nudge_right = true;
    while (!runtime.stop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        auto t_now = std::chrono::steady_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
        if (t_now - g_last_frame_time.load(std::memory_order_relaxed) < 30) continue;

        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw < 2 || vh < 2) continue;
        POINT pt; GetCursorPos(&pt);
        int vx_org = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy_org = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int cx = std::max(vx_org, std::min((int)pt.x, vx_org + vw - 1));
        int cy = std::max(vy_org, std::min((int)pt.y, vy_org + vh - 1));
        int nx = nudge_right ? (cx + 1 < vx_org + vw ? cx + 1 : cx - 1) : (cx - 1 >= vx_org ? cx - 1 : cx + 1);
        nudge_right = !nudge_right;

        LONG dx = (LONG)((int64_t)(nx - vx_org) * 65535 / (vw - 1));
        LONG dy = (LONG)((int64_t)(cy - vy_org) * 65535 / (vh - 1));
        INPUT in{}; in.type = INPUT_MOUSE;
        in.mi.dx = dx; in.mi.dy = dy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        SendInput(1, &in, sizeof(INPUT));
    }
}

void RDPAgent::wake_dwm_now()
{
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0) vw = 1; if (vh <= 0) vh = 1;
    POINT pt; GetCursorPos(&pt);
    int vx_org = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy_org = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cx = std::max(vx_org, std::min((int)pt.x, vx_org + vw - 1));
    int cy = std::max(vy_org, std::min((int)pt.y, vy_org + vh - 1));
    LONG x_orig = (LONG)((int64_t)(cx - vx_org) * 65535 / (vw - 1));
    LONG y_fixed = (LONG)((int64_t)(cy - vy_org) * 65535 / (vh - 1));
    int nx = (cx + 1 < vx_org + vw) ? cx + 1 : cx - 1;
    LONG x_nudge = (LONG)((int64_t)nx * 65535 / (vw - 1));

    INPUT in_nudge = {}; in_nudge.type = INPUT_MOUSE;
    in_nudge.mi.dx = x_nudge; in_nudge.mi.dy = y_fixed;
    in_nudge.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in_nudge, sizeof(INPUT));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    INPUT in_restore = {}; in_restore.type = INPUT_MOUSE;
    in_restore.mi.dx = x_orig; in_restore.mi.dy = y_fixed;
    in_restore.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in_restore, sizeof(INPUT));
}

// ─── Capture & Encode (RTSP_ pipeline - single monitor) ─────

bool RDPAgent::init_capture()
{
    try {
        m_capture = std::make_unique<WGCAdapter>();
        if (!m_capture->Initialize()) {
            log("[capture] WGC Init failed");
            m_capture.reset();
            return false;
        }
        log("[capture] WGC initialized");
    } catch (const std::exception &e) {
        logf("[capture] WGC Init exception: %s", e.what());
        return false;
    }

    int w = g_screen_w.load(), h = g_screen_h.load();
    if (w <= 0 || h <= 0) { w = 1920; h = 1080; }

    auto ms = std::make_unique<PerMonitorState>();
    ms->id = 0;
    ms->w = w;
    ms->h = h;
    ms->fps = config.framerate > 0 ? config.framerate : 30;
    ms->quality_qp = preset_to_qp(config.quality_preset);
    ms->t0 = std::chrono::steady_clock::now();

    try {
        ms->encoder = std::make_unique<X264EncoderAdapter>();
        if (!ms->encoder->Initialize(w, h, ms->fps, ms->quality_qp)) {
            log("[encoder] x264 Init failed");
            return false;
        }
        logf("[encoder] x264 initialized: %dx%d @ %d fps, qp=%d", w, h, ms->fps, ms->quality_qp);
    } catch (const std::exception &e) {
        logf("[encoder] x264 Init exception: %s", e.what());
        return false;
    }

    m_monitor = std::move(ms);
    return true;
}

void RDPAgent::shutdown_capture()
{
    if (m_monitor) {
        {
            std::lock_guard<std::mutex> lk(m_monitor->m);
            m_monitor->stop = true;
        }
        m_monitor->cv.notify_all();
    }
    if (m_capture) {
        m_capture->Shutdown();
        m_capture.reset();
    }
    m_monitor.reset();
}

void RDPAgent::capture_loop()
{
    log("[capture] capture_loop started");

    while (!runtime.stop)
    {
        if (!m_capture) {
            if (!init_capture()) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
        }

        std::vector<uint8_t> frame_data;
        int cap_w = 0, cap_h = 0;
        if (!m_capture->CaptureFrame(0, frame_data, cap_w, cap_h)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (m_monitor) {
            std::lock_guard<std::mutex> lk(m_monitor->m);
            m_monitor->latest_frame = std::move(frame_data);
            m_monitor->has_new = true;
        }
        m_monitor->cv.notify_one();
    }
    log("[capture] capture_loop stopped");
}

// ─── SPS/PPS extraction from encoded NAL stream ─────────────

static bool extract_sps_pps(const std::vector<uint8_t> &nal_data,
                            std::vector<uint8_t> &sps,
                            std::vector<uint8_t> &pps)
{
    size_t i = 0;
    while (i < nal_data.size())
    {
        size_t sc_len = 0;
        if (i + 4 <= nal_data.size() && nal_data[i] == 0 && nal_data[i+1] == 0 && nal_data[i+2] == 0 && nal_data[i+3] == 1)
            sc_len = 4;
        else if (i + 3 <= nal_data.size() && nal_data[i] == 0 && nal_data[i+1] == 0 && nal_data[i+2] == 1)
            sc_len = 3;
        if (sc_len == 0) { i++; continue; }

        size_t nal_start = i + sc_len;
        if (nal_start >= nal_data.size()) break;

        uint8_t nal_type = nal_data[nal_start] & 0x1F;
        size_t nal_end = nal_start;
        for (size_t j = nal_start + 1; j < nal_data.size(); j++)
        {
            if ((j + 4 <= nal_data.size() && nal_data[j] == 0 && nal_data[j+1] == 0 && nal_data[j+2] == 0 && nal_data[j+3] == 1) ||
                (j + 3 <= nal_data.size() && nal_data[j] == 0 && nal_data[j+1] == 0 && nal_data[j+2] == 1))
            {
                nal_end = j;
                break;
            }
            nal_end = nal_data.size();
        }

        if (nal_type == 7)
            sps.assign(nal_data.begin() + nal_start, nal_data.begin() + nal_end);
        else if (nal_type == 8)
            pps.assign(nal_data.begin() + nal_start, nal_data.begin() + nal_end);

        if (!sps.empty() && !pps.empty())
            return true;
        i = nal_end;
    }
    return !sps.empty() && !pps.empty();
}

void RDPAgent::encode_and_push_loop()
{
    log("[encode] encode_and_push_loop started");
    int fps_counter = 0, last_fps_reset = (int)GetTickCount64();

    RtspClient rtsp_client;
    H264RtpPacketizer packetizer;
    packetizer.SetMaxPayloadSize(8000);
    packetizer.SetSsrc(0xDEADBEEF);

    bool sps_pps_sent = false;
    std::vector<uint8_t> sps, pps;
    uint32_t rtp_ts = 0;
    uint32_t rtp_ts_step = 3000; // 90000/30 default, updated once monitor is ready

    while (!runtime.stop)
    {
        if (!m_monitor) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::vector<uint8_t> frame_data;
        {
            std::unique_lock<std::mutex> lk(m_monitor->m);
            if (!m_monitor->has_new) {
                m_monitor->cv.wait_for(lk, std::chrono::milliseconds(100));
            }
            if (!m_monitor->has_new) continue;
            frame_data = std::move(m_monitor->latest_frame);
            m_monitor->has_new = false;
        }

        if (frame_data.empty()) continue;

        rtp_ts_step = 90000 / std::max(m_monitor->fps, 1);

        // Encode with RTSP_ reference x264 encoder
        auto encode_start = std::chrono::steady_clock::now();
        std::vector<uint8_t> encoded;
        if (!m_monitor->encoder->EncodeFrame(frame_data, encoded)) continue;
        auto encode_end = std::chrono::steady_clock::now();
        int64_t encode_us = std::chrono::duration_cast<std::chrono::microseconds>(encode_end - encode_start).count();
        m_monitor->avg_encode_us = encode_us;

        if (encoded.empty()) continue;

        // Extract SPS/PPS from first frame if not done yet
        if (!sps_pps_sent)
        {
            if (extract_sps_pps(encoded, sps, pps) && !sps.empty() && !pps.empty())
            {
                logf("[rtsp] SPS=%zu PPS=%zu", sps.size(), pps.size());
                sps_pps_sent = true;
            }
        }

        // RTP packetize + RTSP push
        std::vector<std::vector<uint8_t>> rtp_packets;
        packetizer.Packetize(encoded.data(), encoded.size(), rtp_ts, rtp_packets);
        rtp_ts += rtp_ts_step;

        // Ensure RTSP connection
        if (!rtsp_client.HandshakeDone())
        {
            std::string vps_host = config.server_host;
            int vps_port = 8554;
            if (rtsp_client.Connect(vps_host, vps_port))
            {
                if (!rtsp_client.Handshake())
                {
                    log("[rtsp] handshake failed, retrying...");
                    rtsp_client.Disconnect();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }

        // Check for incoming keyframe requests
        rtsp_client.CheckForIncoming();
        if (rtsp_client.KeyframeRequested())
        {
            // x264 intra-refresh handles periodic IDR; clear flag
            rtsp_client.ClearKeyframeFlag();
        }

        // Push RTP packets
        for (const auto &pkt : rtp_packets)
        {
            if (!rtsp_client.SendRtp(pkt.data(), pkt.size()))
            {
                log("[rtsp] VPS disconnected, reconnecting...");
                rtsp_client.Disconnect();
                break;
            }
        }

        m_monitor->frame_count++;
        m_monitor->bytes_total += (uint64_t)encoded.size();

        int now = (int)GetTickCount64();
        if (now - last_fps_reset >= 5000) {
            int real_fps = m_monitor->frame_count;
            logf("[encode] %d fps, %d bitrate_kbps, %.1f ms encode",
                 real_fps,
                 (int)(m_monitor->bytes_total * 8 / 5 / 1000),
                 encode_us / 1000.0);
            m_monitor->frame_count = 0;
            m_monitor->bytes_total = 0;
            last_fps_reset = now;
        }
    }
    rtsp_client.Disconnect();
    log("[encode] encode_and_push_loop stopped");
}

// ─── Constructor / Destructor / start / stop ────────────────

RDPAgent::RDPAgent(const RDPConfig &cfg) : config(cfg)
{
    runtime.codec = config.codec;
    runtime.encoder = config.encoder;
    runtime.quality_preset = config.quality_preset;
    runtime.quality_qp = preset_to_qp(config.quality_preset);
    runtime.framerate = config.framerate;
    runtime.timeout_min = config.timeout_min;
    runtime.last_activity_time = std::chrono::steady_clock::now();
    log("RDPAgent created: agent_id=" + config.agent_id +
        " timeout=" + std::to_string(config.timeout_min) + " min");

    if (!config.shm_name.empty())
    {
        shm_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, config.shm_name.c_str());
        if (shm_handle)
        {
            shm = (ActivityShm *)MapViewOfFile(shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ActivityShm));
            if (shm) { shm->last_activity_time = GetTickCount64(); shm->timeout_min = config.timeout_min; }
            else { CloseHandle(shm_handle); shm_handle = nullptr; }
        }
    }
}

RDPAgent::~RDPAgent()
{
    stop();
    if (shm) { UnmapViewOfFile((LPVOID)shm); shm = nullptr; }
    if (shm_handle) { CloseHandle(shm_handle); shm_handle = nullptr; }
}

void RDPAgent::start()
{
    if (running) return;
    runtime.stop = false;

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return;

    init_screen_metrics();
    release_modifier_keys();

    threads.emplace_back([this]() { control_loop(); });
    threads.emplace_back([this]() { resolution_watch_loop(); });
    threads.emplace_back([this]() { clipboard_watch_loop(); });
    threads.emplace_back([this]() { cursor_watch_loop(); });
    threads.emplace_back([this]() { session_keepalive_loop(); });
    threads.emplace_back([this]() { capture_loop(); });
    threads.emplace_back([this]() { encode_and_push_loop(); });

    running = true;
    logf("RDPAgent started with %zu threads", threads.size());
}

void RDPAgent::stop()
{
    if (!running) return;
    log("Stopping RDPAgent...");
    runtime.stop = true;

    if (m_monitor) {
        std::lock_guard<std::mutex> lk(m_monitor->m);
        m_monitor->stop = true;
        m_monitor->cv.notify_all();
    }

    for (auto &t : threads)
        if (t.joinable()) t.join();
    threads.clear();

    shutdown_capture();
    WSACleanup();
    running = false;
    log("RDPAgent stopped");
}

bool RDPAgent::isRunning() const { return running; }

RDPAgent::Status RDPAgent::getStatus() const
{
    Status s;
    s.screen_w = g_screen_w.load();
    s.screen_h = g_screen_h.load();
    s.is_connected = (runtime.ctrl_conn != nullptr);
    return s;
}

bool RDPAgent::is_secure_desktop_active()
{
    return false;
}

bool RDPAgent::is_consent_exe_running()
{
    HANDLE ss = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (ss == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {sizeof(pe)};
    bool found = false;
    if (Process32FirstW(ss, &pe)) do {
        if (_wcsicmp(pe.szExeFile, L"consent.exe") == 0) { found = true; break; }
    } while (Process32NextW(ss, &pe));
    CloseHandle(ss);
    return found;
}

// ─── Worker Entry Point ─────────────────────────────────────

int run_rdp_worker(const std::string &host, int port,
                   const std::string &agent_id,
                   const std::string &agent_token,
                   bool verify_cert,
                   int timeout_min,
                   const std::string &shm_name,
                   const std::string &codec,
                   const std::string &encoder,
                   const std::string &quality_preset,
                   int fps)
{
    RDPConfig cfg;
    cfg.server_host = host;
    cfg.server_port = port;
    cfg.agent_id = agent_id;
    cfg.agent_token = agent_token;
    cfg.verify_cert = verify_cert;
    cfg.timeout_min = timeout_min;
    cfg.shm_name = shm_name;
    cfg.codec = codec;
    cfg.encoder = encoder;
    cfg.quality_preset = quality_preset;
    cfg.framerate = fps > 0 ? fps : 30;

    RDPAgent agent(cfg);
    agent.start();

    // Wait until stop
    while (!agent.isStopRequested())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    agent.stop();
    return 0;
}