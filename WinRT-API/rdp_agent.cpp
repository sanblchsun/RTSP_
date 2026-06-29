// builder_cpp/agent/cmd/agent/rdp_agent.cpp
// Модуль RDP: всё, что должно работать в сессии пользователя.
// Запускается как отдельный процесс через --rdp-worker.
#include "rdp_agent.h"
#include "capture_wgc.h"
#include "encoder_x264.h"
#include "encoder_amf.h"
#include "encoder_ave.h"
#include "encoder_qsv.h"
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

// ============ STATIC MEMBERS ============
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

// ============ RAW TCP ============
bool RDPAgent::send_all_raw(SOCKET s, const char *p, int n)
{
    while (n > 0)
    {
        int k = send(s, p, n, 0);
        if (k <= 0)
            return false;
        p += k;
        n -= k;
    }
    return true;
}

int RDPAgent::recv_n_raw(SOCKET s, char *p, int n)
{
    int got = 0;
    while (got < n)
    {
        int k = recv(s, p + got, n - got, 0);
        if (k <= 0)
            return got;
        got += k;
    }
    return got;
}

bool RDPAgent::sock_has_data(TlsConn *c, int timeout_ms)
{
    if (!c || c->sock == INVALID_SOCKET)
        return false;

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
        if (s == INVALID_SOCKET)
            continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0)
            break;
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

// ============ TLS ============
void RDPAgent::tls_close(TlsConn *c)
{
    if (!c)
        return;
    if (c->ctx_ok)
    {
        DeleteSecurityContext(&c->ctx);
        c->ctx_ok = false;
    }
    if (c->cred_ok)
    {
        FreeCredentialHandle(&c->cred);
        c->cred_ok = false;
    }
    if (c->sock != INVALID_SOCKET)
    {
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
    }
}

bool RDPAgent::tls_handshake(TlsConn *c, const std::string &host, bool verify_cert)
{
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS;
    if (verify_cert)
        sc.dwFlags |= SCH_CRED_AUTO_CRED_VALIDATION;
    else
        sc.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION;

    SECURITY_STATUS ss = AcquireCredentialsHandleA(
        NULL, const_cast<char *>(UNISP_NAME_A),
        SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &c->cred, NULL);
    if (ss != SEC_E_OK)
    {
        return false;
    }
    c->cred_ok = true;

    const DWORD req_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                            ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR |
                            ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    std::wstring whost(host.begin(), host.end());
    SecBuffer out_b = {0, SECBUFFER_TOKEN, NULL};
    SecBufferDesc out_d = {SECBUFFER_VERSION, 1, &out_b};
    DWORD ret_flags = 0;

    ss = InitializeSecurityContextW(
        &c->cred, NULL, const_cast<wchar_t *>(whost.c_str()),
        req_flags, 0, SECURITY_NATIVE_DREP,
        NULL, 0, &c->ctx, &out_d, &ret_flags, NULL);
    c->ctx_ok = true;

    if (out_b.pvBuffer && out_b.cbBuffer > 0)
    {
        bool ok = send_all_raw(c->sock, (const char *)out_b.pvBuffer, (int)out_b.cbBuffer);
        FreeContextBuffer(out_b.pvBuffer);
        out_b.pvBuffer = NULL;
        if (!ok)
            return false;
    }
    if (ss != SEC_I_CONTINUE_NEEDED)
    {
        return false;
    }

    std::vector<uint8_t> in_buf;
    char tmp[16384];

    while (true)
    {
        int n = recv(c->sock, tmp, sizeof tmp, 0);
        if (n <= 0)
        {
            return false;
        }
        in_buf.insert(in_buf.end(), tmp, tmp + n);

    retry:
        SecBuffer in_bufs[2] = {
            {(ULONG)in_buf.size(), SECBUFFER_TOKEN, in_buf.data()},
            {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc in_d = {SECBUFFER_VERSION, 2, in_bufs};
        out_b = {0, SECBUFFER_TOKEN, NULL};
        out_d = {SECBUFFER_VERSION, 1, &out_b};
        ret_flags = 0;

        ss = InitializeSecurityContextW(
            &c->cred, &c->ctx, NULL,
            req_flags, 0, SECURITY_NATIVE_DREP,
            &in_d, 0, NULL, &out_d, &ret_flags, NULL);

        if (out_b.pvBuffer && out_b.cbBuffer > 0)
        {
            bool ok = send_all_raw(c->sock, (const char *)out_b.pvBuffer, (int)out_b.cbBuffer);
            FreeContextBuffer(out_b.pvBuffer);
            out_b.pvBuffer = NULL;
            if (!ok)
                return false;
        }

        if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0)
        {
            size_t off = in_buf.size() - in_bufs[1].cbBuffer;
            std::vector<uint8_t> extra(in_buf.begin() + (ptrdiff_t)off, in_buf.end());
            in_buf = std::move(extra);
        }
        else if (ss != SEC_E_INCOMPLETE_MESSAGE)
        {
            in_buf.clear();
        }

        if (ss == SEC_E_OK)
            break;
        if (ss == SEC_I_CONTINUE_NEEDED)
            continue;
        if (ss == SEC_E_INCOMPLETE_MESSAGE)
        {
            n = recv(c->sock, tmp, sizeof tmp, 0);
            if (n <= 0)
            {
                return false;
            }
            in_buf.insert(in_buf.end(), tmp, tmp + n);
            goto retry;
        }
        return false;
    }

    if (!in_buf.empty())
        c->raw = std::move(in_buf);
    QueryContextAttributes(&c->ctx, SECPKG_ATTR_STREAM_SIZES, &c->sizes);
    return true;
}

TlsConn *RDPAgent::tls_connect(const std::string &host, int port, bool verify_cert)
{
    SOCKET s = tcp_connect(host, port);
    if (s == INVALID_SOCKET)
        return nullptr;
    TlsConn *c = new TlsConn();
    c->sock = s;
    if (!tls_handshake(c, host, verify_cert))
    {
        tls_close(c);
        delete c;
        return nullptr;
    }
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
        if (ss != SEC_E_OK)
            return false;

        int total = (int)(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
        if (!send_all_raw(c->sock, (const char *)msg.data(), total))
            return false;
        p += chunk;
        n -= chunk;
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
                {0, SECBUFFER_EMPTY, NULL},
                {0, SECBUFFER_EMPTY, NULL},
                {0, SECBUFFER_EMPTY, NULL}};
            SecBufferDesc in_desc = {SECBUFFER_VERSION, 4, in_bufs};
            SECURITY_STATUS ss = DecryptMessage(&c->ctx, &in_desc, 0, NULL);
            if (ss == SEC_E_INCOMPLETE_MESSAGE)
                break;
            if (ss == SEC_I_CONTEXT_EXPIRED)
                return 0;
            if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE)
                return -1;
            for (int i = 0; i < 4; ++i)
            {
                if (in_bufs[i].BufferType == SECBUFFER_DATA && in_bufs[i].cbBuffer > 0)
                {
                    auto *ptr = (uint8_t *)in_bufs[i].pvBuffer;
                    c->plain.insert(c->plain.end(), ptr, ptr + in_bufs[i].cbBuffer);
                }
            }
            bool has_extra = false;
            for (int i = 1; i < 4; ++i)
            {
                if (in_bufs[i].BufferType == SECBUFFER_EXTRA && in_bufs[i].cbBuffer > 0)
                {
                    size_t off = c->raw.size() - in_bufs[i].cbBuffer;
                    std::vector<uint8_t> extra(c->raw.begin() + (ptrdiff_t)off, c->raw.end());
                    c->raw = std::move(extra);
                    has_extra = true;
                    break;
                }
            }
            if (!has_extra)
                c->raw.clear();
            if (!c->plain.empty())
            {
                int n = (int)std::min((size_t)want, c->plain.size());
                memcpy(buf, c->plain.data(), (size_t)n);
                c->plain.erase(c->plain.begin(), c->plain.begin() + n);
                return n;
            }
        }
        int n = recv(c->sock, tmp, sizeof tmp, 0);
        if (n <= 0)
            return -1;
        c->raw.insert(c->raw.end(), tmp, tmp + n);
    }
}

int RDPAgent::tls_recv_n(TlsConn *c, char *p, int n)
{
    int got = 0;
    while (got < n)
    {
        int k = tls_recv_some(c, p + got, n - got);
        if (k <= 0)
            return got;
        got += k;
    }
    return got;
}

// ============ HTTP GET ============
// ============ JSON ============
bool RDPAgent::json_str(const std::string &j, const std::string &k, std::string &out)
{
    std::string key = "\"" + k + "\"";
    auto p = j.find(key);
    if (p == std::string::npos)
        return false;
    p = j.find(':', p);
    if (p == std::string::npos)
        return false;
    ++p;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        ++p;
    if (p >= j.size() || j[p] != '"')
        return false;
    ++p;
    auto e = j.find('"', p);
    if (e == std::string::npos)
        return false;
    out = j.substr(p, e - p);
    return true;
}

bool RDPAgent::json_int(const std::string &j, const std::string &k, int &out)
{
    std::string key = "\"" + k + "\"";
    auto p = j.find(key);
    if (p == std::string::npos)
        return false;
    p = j.find(':', p);
    if (p == std::string::npos)
        return false;
    ++p;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        ++p;
    if (p >= j.size())
        return false;
    int sign = 1;
    if (j[p] == '-')
    {
        sign = -1;
        ++p;
    }
    if (p >= j.size() || !std::isdigit((unsigned char)j[p]))
        return false;
    int v = 0;
    while (p < j.size() && std::isdigit((unsigned char)j[p]))
    {
        v = v * 10 + (j[p] - '0');
        ++p;
    }
    out = sign * v;
    return true;
}

bool RDPAgent::json_str_ex(const std::string &j, const std::string &k, std::string &out)
{
    std::string key = "\"" + k + "\"";
    auto p = j.find(key);
    if (p == std::string::npos)
        return false;
    p = j.find(':', p);
    if (p == std::string::npos)
        return false;
    ++p;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        ++p;
    if (p >= j.size() || j[p] != '"')
        return false;
    ++p;
    out.clear();
    auto hex = [](char c, unsigned &v)
    {
        if (c >= '0' && c <= '9')
        {
            v = c - '0';
            return true;
        }
        if (c >= 'a' && c <= 'f')
        {
            v = c - 'a' + 10;
            return true;
        }
        if (c >= 'A' && c <= 'F')
        {
            v = c - 'A' + 10;
            return true;
        }
        return false;
    };
    auto emit_cp = [&](unsigned cp)
    {
        if (cp < 0x80)
            out += (char)cp;
        else if (cp < 0x800)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    };
    while (p < j.size())
    {
        char c = j[p];
        if (c == '"')
            return true;
        if (c == '\\' && p + 1 < j.size())
        {
            char n = j[p + 1];
            if (n == '"' || n == '\\' || n == '/')
            {
                out += n;
                p += 2;
                continue;
            }
            if (n == 'n')
            {
                out += '\n';
                p += 2;
                continue;
            }
            if (n == 't')
            {
                out += '\t';
                p += 2;
                continue;
            }
            if (n == 'r')
            {
                out += '\r';
                p += 2;
                continue;
            }
            if (n == 'b')
            {
                out += '\b';
                p += 2;
                continue;
            }
            if (n == 'f')
            {
                out += '\f';
                p += 2;
                continue;
            }
            if (n == 'u' && p + 5 < j.size())
            {
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i)
                {
                    unsigned v;
                    if (!hex(j[p + 2 + i], v))
                        return false;
                    cp = (cp << 4) | v;
                }
                p += 6;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 5 < j.size() && j[p] == '\\' && j[p + 1] == 'u')
                {
                    unsigned low = 0;
                    bool ok = true;
                    for (int i = 0; i < 4; ++i)
                    {
                        unsigned v;
                        if (!hex(j[p + 2 + i], v))
                        {
                            ok = false;
                            break;
                        }
                        low = (low << 4) | v;
                    }
                    if (ok && low >= 0xDC00 && low <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        p += 6;
                    }
                }
                emit_cp(cp);
                continue;
            }
            return false;
        }
        out += c;
        ++p;
    }
    return false;
}

std::string RDPAgent::json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            }
            else
                out += (char)c;
        }
    }
    out += '"';
    return out;
}

// ============ WEBSOCKET ============
std::string RDPAgent::b64(const unsigned char *d, size_t n)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    while (i < n)
    {
        uint32_t v = 0;
        int k = (int)std::min<size_t>(3, n - i);
        for (int j = 0; j < k; ++j)
            v |= d[i + j] << ((2 - j) * 8);
        for (int j = 0; j < 4; ++j)
            o += (j <= k) ? T[(v >> ((3 - j) * 6)) & 63] : '=';
        i += 3;
    }
    return o;
}

bool RDPAgent::ws_handshake(TlsConn *c, const std::string &host, int port, const std::string &path)
{
    unsigned char k[16];
    std::random_device rd;
    for (int i = 0; i < 16; ++i)
        k[i] = (unsigned char)(rd() & 0xFF);
    std::ostringstream r;
    r << "GET " << path << " HTTP/1.1\r\n"
      << "Host: " << host << ":" << port << "\r\n"
      << "Upgrade: websocket\r\nConnection: Upgrade\r\n"
      << "Sec-WebSocket-Key: " << b64(k, 16) << "\r\n"
      << "Sec-WebSocket-Version: 13\r\n\r\n";
    std::string rs = r.str();
    if (!tls_send_all(c, rs.data(), (int)rs.size()))
        return false;
    std::string h;
    char ch;
    while (h.size() < 8192)
    {
        if (tls_recv_n(c, &ch, 1) != 1)
            return false;
        h += ch;
        if (h.size() >= 4 && h.compare(h.size() - 4, 4, "\r\n\r\n") == 0)
            break;
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
    for (int i = 0; i < 4; ++i)
        mask[i] = (uint8_t)(rd() & 0xFF);
    if (len < 126)
        f.push_back((uint8_t)(0x80 | len));
    else if (len < 65536)
    {
        f.push_back((uint8_t)(0x80 | 126));
        f.push_back((uint8_t)((len >> 8) & 0xFF));
        f.push_back((uint8_t)(len & 0xFF));
    }
    else
    {
        f.push_back((uint8_t)(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            f.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
    }
    for (int i = 0; i < 4; ++i)
        f.push_back(mask[i]);
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i)
        f.push_back(p[i] ^ mask[i & 3]);
    return tls_send_all(c, (const char *)f.data(), (int)f.size());
}

int RDPAgent::ws_recv(TlsConn *c, std::vector<uint8_t> &payload)
{
    uint8_t h[2];
    if (tls_recv_n(c, (char *)h, 2) != 2)
        return -1;
    int op = h[0] & 0x0F;
    bool masked = (h[1] & 0x80) != 0;
    uint64_t len = h[1] & 0x7F;
    if (len == 126)
    {
        uint8_t b[2];
        if (tls_recv_n(c, (char *)b, 2) != 2)
            return -1;
        len = ((uint64_t)b[0] << 8) | b[1];
    }
    else if (len == 127)
    {
        uint8_t b[8];
        if (tls_recv_n(c, (char *)b, 8) != 8)
            return -1;
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | b[i];
    }
    uint8_t mk[4] = {0, 0, 0, 0};
    if (masked && tls_recv_n(c, (char *)mk, 4) != 4)
        return -1;
    if (len > (8u << 20))
        return -1;
    payload.resize((size_t)len);
    if (len && tls_recv_n(c, (char *)payload.data(), (int)len) != (int)len)
        return -1;
    if (masked)
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= mk[i & 3];
    if (op == 0x8)
        return -1;
    if (op == 0x9)
    {
        ws_send(c, 0xA, payload.data(), payload.size());
        return 0;
    }
    if (op == 0xA)
        return 0;
    if (op == 0x1)
        return 1;
    if (op == 0x2)
        return 2;
    return 0;
}

// ============ SCREEN ============
static bool get_desktop_window_size(int &w, int &h)
{
    // Query the actual desktop window client area — the exact region
    // that gdigrab can capture. GetClipBox on the desktop window DC
    // returns the bounding visible rectangle, which in RDP sessions
    // may be the RDP client's viewport (smaller than the virtual screen).
    HWND hwnd = GetDesktopWindow();
    HDC hdc = GetDC(hwnd);
    if (!hdc)
        return false;
    RECT clip;
    if (!GetClipBox(hdc, &clip))
    {
        ReleaseDC(hwnd, hdc);
        return false;
    }
    w = clip.right - clip.left;
    h = clip.bottom - clip.top;
    ReleaseDC(hwnd, hdc);
    return w > 0 && h > 0;
}

bool RDPAgent::read_screen_metrics(int &w, int &h, int &ox, int &oy)
{
    // Step 1: Clamp to the desktop window client area (what gdigrab can actually capture)
    int dw = 0, dh = 0;
    get_desktop_window_size(dw, dh);

    // Step 2: Get the full monitor bounding box from EnumDisplayMonitors
    struct Bbox { int min_x; int min_y; int max_x; int max_y; };
    Bbox bbox = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    EnumDisplayMonitors(NULL, NULL,
        [](HMONITOR, HDC, LPRECT rect, LPARAM lparam) -> BOOL {
            Bbox *b = reinterpret_cast<Bbox *>(lparam);
            if (rect->left < b->min_x) b->min_x = rect->left;
            if (rect->top < b->min_y) b->min_y = rect->top;
            if (rect->right > b->max_x) b->max_x = rect->right;
            if (rect->bottom > b->max_y) b->max_y = rect->bottom;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&bbox));

    int vw, vh, vox, voy;
    if (bbox.min_x != INT_MAX)
    {
        vw = bbox.max_x - bbox.min_x;
        vh = bbox.max_y - bbox.min_y;
        vox = bbox.min_x;
        voy = bbox.min_y;
    }
    else
    {
        vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        vox = GetSystemMetrics(SM_XVIRTUALSCREEN);
        voy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    }

    // Use the desktop window size if available and smaller than virtual
    if (dw > 0 && dh > 0 && (dw < vw || dh < vh))
    {
        w = dw;
        h = dh;
        ox = vox;
        oy = voy;
    }
    else
    {
        w = vw;
        h = vh;
        ox = vox;
        oy = voy;
    }

    return w > 0 && h > 0;
}

void RDPAgent::init_screen_metrics()
{
    int w, h, ox, oy;
    if (read_screen_metrics(w, h, ox, oy))
    {
        g_screen_w = w;
        g_screen_h = h;
        g_screen_origin_x = ox;
        g_screen_origin_y = oy;
    }
    refresh_vscreen_cache();
    g_last_mouse_x = -1;
    g_last_mouse_y = -1;
    //logf("[screen] init: %dx%d origin=%d,%d", w, h, ox, oy);
}

void RDPAgent::refresh_vscreen_cache()
{
    g_vscreen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vscreen_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vscreen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vscreen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

// ============ MONITORS ============

// Callback for EnumDisplayMonitors — collects monitor rects
static BOOL CALLBACK monitor_enum_proc(HMONITOR, HDC, LPRECT r, LPARAM data)
{
    auto *out = (std::vector<MonitorInfo> *)data;
    MonitorInfo mi;
    mi.x = r->left;
    mi.y = r->top;
    mi.w = r->right - r->left;
    mi.h = r->bottom - r->top;
    out->push_back(mi);
    return TRUE;
}

void RDPAgent::read_monitors(std::vector<MonitorInfo> &out)
{
    out.clear();
    EnumDisplayMonitors(NULL, NULL, monitor_enum_proc, (LPARAM)&out);
    if (out.empty())
    {
        // Fallback: single monitor at (0,0) with current screen size
        MonitorInfo mi;
        mi.w = g_screen_w.load();
        mi.h = g_screen_h.load();
        out.push_back(mi);
    }
}

// ============ MOUSE ============

// Convert client (x,y) within capture frame (sw,sh) to normalized absolute coords
bool RDPAgent::mouse_to_abs(int x, int y, int monitor_id,
                            LONG &out_dx, LONG &out_dy,
                            int sw, int sh)
{
    int x_left = g_vscreen_x.load();
    int y_top  = g_vscreen_y.load();
    int vw = g_vscreen_w.load();
    int vh = g_vscreen_h.load();
    if (vw <= 1 || vh <= 1)
        vw = sw, vh = sh;

    int ox = g_screen_origin_x.load();
    int oy = g_screen_origin_y.load();
    {
        std::lock_guard<std::mutex> lk(g_monitors_m);
        if (monitor_id >= 0 && monitor_id < (int)g_monitors.size())
        {
            ox = g_monitors[monitor_id].x;
            oy = g_monitors[monitor_id].y;
        }
    }
    int vx = ox + x;
    int vy = oy + y;
    if (vx < x_left) vx = x_left;
    if (vy < y_top)  vy = y_top;
    int vw_max = x_left + vw;
    int vh_max = y_top  + vh;
    if (vx >= vw_max) vx = vw_max - 1;
    if (vy >= vh_max) vy = vh_max - 1;
    out_dx = (LONG)((int64_t)(vx - x_left) * 65535 / (vw - 1));
    out_dy = (LONG)((int64_t)(vy - y_top)  * 65535 / (vh - 1));
    return true;
}

void RDPAgent::do_mouse_move(int x, int y, int monitor_id)
{
    int sw = g_video_w.load(), sh = g_video_h.load();
    if (sw <= 1 || sh <= 1)
        sw = g_screen_w.load(), sh = g_screen_h.load();
    if (sw <= 1 || sh <= 1)
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= sw) x = sw - 1;
    if (y >= sh) y = sh - 1;

    // Skip if same as last position (reduces SendInput calls from coalesced events)
    if (x == g_last_mouse_x.load() && y == g_last_mouse_y.load())
        return;

    LONG dx, dy;
    if (!mouse_to_abs(x, y, monitor_id, dx, dy, sw, sh))
        return;

    g_last_mouse_x = x;
    g_last_mouse_y = y;

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(INPUT));
}

// Combined move+click in a single SendInput batch — atomic, lower latency
void RDPAgent::do_mouse_move_and_click(int x, int y, int button, bool down, int monitor_id)
{
    int sw = g_video_w.load(), sh = g_video_h.load();
    if (sw <= 1 || sh <= 1)
        sw = g_screen_w.load(), sh = g_screen_h.load();
    if (sw <= 1 || sh <= 1)
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= sw) x = sw - 1;
    if (y >= sh) y = sh - 1;

    LONG dx, dy;
    if (!mouse_to_abs(x, y, monitor_id, dx, dy, sw, sh))
        return;

    DWORD btn = 0;
    switch (button)
    {
    case 0: btn = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;   break;
    case 1: btn = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case 2: btn = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;   break;
    default: return;
    }

    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dx = dx;
    in[0].mi.dy = dy;
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = btn;
    SendInput(2, in, sizeof(INPUT));

    g_last_mouse_x = x;
    g_last_mouse_y = y;
}

void RDPAgent::do_mouse_button(int button, bool down)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    DWORD f = 0;
    switch (button)
    {
    case 0:
        f = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 1:
        f = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case 2:
        f = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    default:
        return;
    }
    in.mi.dwFlags = f;
    SendInput(1, &in, sizeof(INPUT));
}

void RDPAgent::do_mouse_wheel(int delta)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &in, sizeof(INPUT));
}

// ============ KEYBOARD ============
void RDPAgent::do_text_input(const std::string &utf8)
{
    if (utf8.empty())
        return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
    if (wlen <= 0)
        return;
    std::vector<wchar_t> w((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), w.data(), wlen);
    std::vector<INPUT> inputs;
    inputs.reserve(w.size() * 2);
    auto push_vk = [&](WORD vk)
    {
        INPUT d{};
        d.type = INPUT_KEYBOARD;
        d.ki.wVk = vk;
        d.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        d.ki.dwFlags = 0;
        INPUT u = d;
        u.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(d);
        inputs.push_back(u);
    };
    for (wchar_t ch : w)
    {
        if (ch == L'\r')
            continue;
        if (ch == L'\n')
        {
            push_vk(VK_RETURN);
            continue;
        }
        if (ch == L'\t')
        {
            push_vk(VK_TAB);
            continue;
        }
        INPUT d{};
        d.type = INPUT_KEYBOARD;
        d.ki.wVk = 0;
        d.ki.wScan = (WORD)ch;
        d.ki.dwFlags = KEYEVENTF_UNICODE;
        if ((ch & 0xFF00) == 0xE000)
            d.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        INPUT u = d;
        u.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(d);
        inputs.push_back(u);
    }
    if (inputs.empty())
        return;
    const size_t BATCH = 128;
    for (size_t i = 0; i < inputs.size(); i += BATCH)
    {
        UINT n = (UINT)std::min(BATCH, inputs.size() - i);
        SendInput(n, inputs.data() + i, sizeof(INPUT));
    }
}

int RDPAgent::code_to_vk(const std::string &code)
{
    if (code.size() == 4 && code.compare(0, 3, "Key") == 0)
    {
        char c = code[3];
        if (c >= 'A' && c <= 'Z')
            return c;
    }
    if (code.size() == 6 && code.compare(0, 5, "Digit") == 0)
    {
        char c = code[5];
        if (c >= '0' && c <= '9')
            return c;
    }
    if (code.compare(0, 6, "Numpad") == 0)
    {
        if (code.size() == 7)
        {
            char c = code[6];
            if (c >= '0' && c <= '9')
                return VK_NUMPAD0 + (c - '0');
        }
        if (code == "NumpadAdd")
            return VK_ADD;
        if (code == "NumpadSubtract")
            return VK_SUBTRACT;
        if (code == "NumpadMultiply")
            return VK_MULTIPLY;
        if (code == "NumpadDivide")
            return VK_DIVIDE;
        if (code == "NumpadDecimal")
            return VK_DECIMAL;
        if (code == "NumpadEnter")
            return VK_RETURN;
    }
    if (!code.empty() && code[0] == 'F' && code.size() >= 2 && code.size() <= 3)
    {
        bool digits = true;
        for (size_t i = 1; i < code.size(); ++i)
            if (!std::isdigit((unsigned char)code[i]))
            {
                digits = false;
                break;
            }
        if (digits)
        {
            int n = std::atoi(code.c_str() + 1);
            if (n >= 1 && n <= 24)
                return VK_F1 + (n - 1);
        }
    }
    static const std::unordered_map<std::string, int> m = {
        {"Enter", VK_RETURN},
        {"Backspace", VK_BACK},
        {"Tab", VK_TAB},
        {"Space", VK_SPACE},
        {"Escape", VK_ESCAPE},
        {"ArrowLeft", VK_LEFT},
        {"ArrowRight", VK_RIGHT},
        {"ArrowUp", VK_UP},
        {"ArrowDown", VK_DOWN},
        {"Home", VK_HOME},
        {"End", VK_END},
        {"PageUp", VK_PRIOR},
        {"PageDown", VK_NEXT},
        {"Insert", VK_INSERT},
        {"Delete", VK_DELETE},
        {"ShiftLeft", VK_LSHIFT},
        {"ShiftRight", VK_RSHIFT},
        {"ControlLeft", VK_LCONTROL},
        {"ControlRight", VK_RCONTROL},
        {"AltLeft", VK_LMENU},
        {"AltRight", VK_RMENU},
        {"MetaLeft", VK_LWIN},
        {"MetaRight", VK_RWIN},
        {"OSLeft", VK_LWIN},
        {"OSRight", VK_RWIN},
        {"CapsLock", VK_CAPITAL},
        {"NumLock", VK_NUMLOCK},
        {"ScrollLock", VK_SCROLL},
        {"PrintScreen", VK_SNAPSHOT},
        {"Pause", VK_PAUSE},
        {"ContextMenu", VK_APPS},
        {"Minus", VK_OEM_MINUS},
        {"Equal", VK_OEM_PLUS},
        {"BracketLeft", VK_OEM_4},
        {"BracketRight", VK_OEM_6},
        {"Backslash", VK_OEM_5},
        {"Semicolon", VK_OEM_1},
        {"Quote", VK_OEM_7},
        {"Comma", VK_OEM_COMMA},
        {"Period", VK_OEM_PERIOD},
        {"Slash", VK_OEM_2},
        {"Backquote", VK_OEM_3},
        {"IntlBackslash", VK_OEM_102},
    };
    auto it = m.find(code);
    return it == m.end() ? 0 : it->second;
}

void RDPAgent::do_key(const std::string &code, bool down)
{
    int vk = code_to_vk(code);
    if (vk == 0)
        return;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    switch (vk)
    {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_HOME:
    case VK_END:
    case VK_INSERT:
    case VK_DELETE:
    case VK_SNAPSHOT:
    case VK_APPS:
    case VK_LWIN:
    case VK_RWIN:
    case VK_NUMLOCK:
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        break;
    }
    SendInput(1, &in, sizeof(INPUT));
}

// ============ MODIFIER KEY RELEASE ============
void RDPAgent::release_modifier_keys()
{
    WORD mods[] = { VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                    VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
    for (WORD vk : mods)
    {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));
    }
}

// ============ TASK MANAGER ============
void RDPAgent::run_taskmgr()
{
    // Use ShellExecute via shell32.dll to launch in user session
    HMODULE hmod = LoadLibraryA("shell32.dll");
    if (!hmod) return;
    typedef HINSTANCE (WINAPI *ShellExecuteW_t)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
    ShellExecuteW_t pShellExecuteW = (ShellExecuteW_t)GetProcAddress(hmod, "ShellExecuteW");
    if (pShellExecuteW)
    {
        pShellExecuteW(NULL, L"open", L"taskmgr.exe", NULL, NULL, SW_SHOWNORMAL);
    }
    FreeLibrary(hmod);
}

// ============ CLIPBOARD ============
std::string RDPAgent::clipboard_read_utf8()
{
    bool opened = false;
    for (int i = 0; i < 10; ++i)
    {
        if (OpenClipboard(NULL))
        {
            opened = true;
            break;
        }
        Sleep(15);
    }
    if (!opened)
    {
        // log("[clipboard] read: OpenClipboard failed after 10 retries");
        return {};
    }
    std::string result;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h)
    {
        const wchar_t *w = (const wchar_t *)GlobalLock(h);
        if (w)
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            if (n > 1)
            {
                result.resize((size_t)n - 1);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, &result[0], n, NULL, NULL);
            }
            GlobalUnlock(h);
        }
    }
    // else
    // {
    //     log("[clipboard] read: GetClipboardData returned NULL");
    // }
    CloseClipboard();
    return result;
}

void RDPAgent::clipboard_write_utf8(const std::string &utf8)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size() + 1, NULL, 0);
    if (wlen <= 0)
        return;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (size_t)wlen * sizeof(wchar_t));
    if (!mem)
        return;
    wchar_t *dst = (wchar_t *)GlobalLock(mem);
    if (!dst)
    {
        GlobalFree(mem);
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size() + 1, dst, wlen);
    GlobalUnlock(mem);
    bool opened = false;
    for (int i = 0; i < 10; ++i)
    {
        if (OpenClipboard(NULL))
        {
            opened = true;
            break;
        }
        Sleep(15);
    }
    if (!opened)
    {
        GlobalFree(mem);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, mem))
        GlobalFree(mem);
    CloseClipboard();
    std::lock_guard<std::mutex> lk(g_clip_m);
    g_last_clip = utf8;
}

void RDPAgent::handle_control(const std::string &j)
{
    std::string type;
    if (!json_str(j, "type", type))
        return;

    // Update activity timestamp (mouse_move excluded — too frequent, unnecessary for inactivity detection)
    bool is_input = (type == "mouse_down" || type == "mouse_up" ||
                     type == "text" || type == "key_down" ||
                     type == "key_up" || type == "mouse_wheel");
    if (is_input)
    {
        runtime.last_activity_time = std::chrono::steady_clock::now();
        if (shm)
            shm->last_activity_time = GetTickCount64();
        g_input_pending.store(true, std::memory_order_relaxed);
    }
    if (type == "mouse_move")
        g_input_pending.store(true, std::memory_order_relaxed);

    if (type == "mouse_move")
    {
        int x = 0, y = 0, mid = 0;
        json_int(j, "monitor_id", mid);
        if (json_int(j, "x", x) && json_int(j, "y", y))
        {
            do_mouse_move(x, y, mid);
        }
    }
    else if (type == "mouse_down" || type == "mouse_up")
    {
        int btn = 0, mx = 0, my = 0, mid = 0;
        json_int(j, "monitor_id", mid);
        json_int(j, "button", btn);
        if (json_int(j, "x", mx) && json_int(j, "y", my))
            do_mouse_move_and_click(mx, my, btn, type == "mouse_down", mid);
        else
            do_mouse_button(btn, type == "mouse_down");
        wake_dwm_now();
    }
    else if (type == "mouse_wheel")
    {
        int d = 0;
        if (json_int(j, "delta", d))
            do_mouse_wheel(d);
        wake_dwm_now();
    }
    else if (type == "text")
    {
        std::string text;
        if (json_str_ex(j, "text", text))
            do_text_input(text);
        wake_dwm_now();
    }
    else if (type == "key_down" || type == "key_up")
    {
        std::string code;
        if (json_str(j, "code", code))
            do_key(code, type == "key_down");
        wake_dwm_now();
    }
    else if (type == "clipboard")
    {
        std::string text;
        if (json_str_ex(j, "text", text))
        {
            // logf("[clipboard] received from viewer: %zu bytes", text.size());
            clipboard_write_utf8(text);
        }
    }
    else if (type == "video_size")
    {
        int w = 0, h = 0;
        if (json_int(j, "w", w) && json_int(j, "h", h) && w > 0 && h > 0)
        {
            g_video_w = w;
            g_video_h = h;
        }
    }
    else if (type == "run_taskmgr")
    {
        run_taskmgr();
    }
    else if (type == "release_modifiers")
    {
        release_modifier_keys();
    }
    else if (type == "command")
    {
        std::string cmd;
        if (json_str(j, "cmd", cmd))
        {
            if (cmd == "disable-uac")
            {
                disable_uac();
            }
            else if (cmd == "start-rdp-worker")
            {
            }
            else if (cmd == "stop-rdp-worker")
            {
            }
        }
    }
    else if (type == "config")
    {
        std::string codec, encoder, quality_preset, mode;
        int fps = 0, mq = 0, new_timeout = -1;
        json_str(j, "codec", codec);
        json_str(j, "encoder", encoder);
        json_str(j, "quality", quality_preset);
        json_str(j, "mode", mode);
        json_int(j, "fps", fps);
        json_int(j, "rdp_timeout", new_timeout);

        //logf("[config] RECEIVED: codec=%s encoder=%s quality=%s fps=%d mode=%s",
        //     codec.c_str(), encoder.c_str(), quality_preset.c_str(), fps, mode.c_str());

        if (codec.empty())
            return;

        std::string sig = codec + "|" + encoder + "|" + quality_preset + "|" +
                          std::to_string(fps) + "|" + mode;
        if (sig == last_config_sig)
        {
            //logf("[config] DUPLICATE, ignored");
            return;
        }
        last_config_sig = sig;

        {
            std::lock_guard<std::mutex> lk(runtime.m);
            runtime.codec = codec;
            runtime.encoder = encoder.empty() ? "cpu" : encoder;
            if (!quality_preset.empty())
            {
                runtime.quality_preset = quality_preset;
                runtime.quality_qp = preset_to_qp(quality_preset);
            }
            if (fps > 0)
            {
                runtime.framerate = fps;
                runtime.auto_adj_fps = fps;
            }
            bool old_manual = runtime.manual_mode;
            runtime.manual_mode = (mode == "manual");
            if (runtime.manual_mode != old_manual)
            {
                //logf("[config] mode changed: %s -> %s", old_manual ? "manual" : "auto", mode.c_str());
                if (!runtime.manual_mode && old_manual)
                {
                    // Fresh start for auto ramp
                    runtime.auto_adj_fps = 5;
                    runtime.auto_adj_next = std::chrono::steady_clock::now();
                }
            }
            runtime.restart = true;
            //logf("[config] APPLIED: encoder=%s quality=%s(%d) fps=%d manual_mode=%d auto_adj_fps=%d restart=1",
            //     runtime.encoder.c_str(), runtime.quality_preset.c_str(), runtime.quality_qp,
            //     runtime.framerate, (int)runtime.manual_mode, runtime.auto_adj_fps);
        }

        if (new_timeout >= 0 && shm && new_timeout != shm->timeout_min)
        {
            shm->timeout_min = new_timeout;
        }
    }
}

// ============ (ffmpeg removed — replaced by DDA + libx264) ============

// ============ CONTROL MESSAGES ============

int RDPAgent::preset_to_qp(const std::string &preset)
{
    if (preset == "best")   return 18;
    if (preset == "high")   return 21;
    if (preset == "medium") return 23;
    if (preset == "low")    return 28;
    return 23;
}

std::string RDPAgent::make_hello_json()
{
    std::ostringstream hs;
    hs << "{\"type\":\"hello\""
       << ",\"screen_w\":" << g_screen_w.load()
       << ",\"screen_h\":" << g_screen_h.load()
       << ",\"screen_origin_x\":" << g_screen_origin_x.load()
       << ",\"screen_origin_y\":" << g_screen_origin_y.load()
       << "}";
    return hs.str();
}

void RDPAgent::ctrl_send_hello()
{
    std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
    if (!runtime.ctrl_conn)
        return;
    std::string h = make_hello_json();
    ws_send(runtime.ctrl_conn, 0x1, h.data(), h.size());
}

void RDPAgent::ctrl_send_clipboard(const std::string &text)
{
    std::string msg = std::string("{\"type\":\"clipboard\",\"text\":") + json_escape(text) + "}";
    std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
    if (!runtime.ctrl_conn)
    {
        // logf("[clipboard] send failed: no ctrl_conn");
        return;
    }
    // logf("[clipboard] sending %zu bytes via ws", msg.size());
    ws_send(runtime.ctrl_conn, 0x1, msg.data(), msg.size());
}

void RDPAgent::ctrl_send_cursor(int shape_id)
{
    std::string msg = "{\"type\":\"cursor\",\"shape_id\":" + std::to_string(shape_id) + "}";
    std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
    if (!runtime.ctrl_conn)
        return;
    ws_send(runtime.ctrl_conn, 0x1, msg.data(), msg.size());
}

void RDPAgent::ctrl_send_monitor_status(int monitor_id, int bitrate_kbps, int fps, bool overloaded)
{
    std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
    if (!runtime.ctrl_conn)
        return;
    std::string quality_preset;
    {
        std::lock_guard<std::mutex> lk2(runtime.m);
        quality_preset = runtime.quality_preset;
    }
    std::string msg = "{\"type\":\"monitor_status\",\"monitor_id\":" +
                      std::to_string(monitor_id) +
                      ",\"bitrate_kbps\":" + std::to_string(bitrate_kbps) +
                      ",\"fps\":" + std::to_string(fps) +
                      ",\"quality\":\"" + quality_preset + "\"" +
                      ",\"encoder_overloaded\":" + (overloaded ? "true" : "false") + "}";
    ws_send(runtime.ctrl_conn, 0x1, msg.data(), msg.size());
}

// ============ THREAD LOOPS ============
void RDPAgent::control_loop()
{
    while (!runtime.stop)
    {
        TlsConn *c = tls_connect(config.server_host, config.server_port, config.verify_cert);
        if (!c)
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        std::string path = "/relay/ws/control/worker/" + config.agent_id + "?token=" + config.agent_token;
        if (!ws_handshake(c, config.server_host, config.server_port, path))
        {
            tls_close(c);
            delete c;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
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
            if (r < 0)
                break;
            if (r == 1)
            {
                std::string msg(buf.begin(), buf.end());
                handle_control(msg);
            }
        }

        {
            std::lock_guard<std::mutex> lk(runtime.ctrl_sock_m);
            runtime.ctrl_conn = nullptr;
        }
        tls_close(c);
        delete c;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void RDPAgent::resolution_watch_loop()
{
    using namespace std::chrono;
    int last_vw = 0, last_vh = 0, last_ox = 0, last_oy = 0;
    read_screen_metrics(last_vw, last_vh, last_ox, last_oy);
    while (!runtime.stop)
    {
        std::this_thread::sleep_for(seconds(2));
        if (runtime.stop)
            break;

        int vw, vh, ox, oy;
        read_screen_metrics(vw, vh, ox, oy);
        bool changed = (vw != last_vw || vh != last_vh || ox != last_ox || oy != last_oy);
        if (changed)
        {
            last_vw = vw;
            last_vh = vh;
            last_ox = ox;
            last_oy = oy;
            //logf("[capture] resolution changed: %dx%d origin=%d,%d — restart", vw, vh, ox, oy);
        }

        // Sync dimensions from actual monitor bounding box
        {
            g_screen_w = vw;
            g_screen_h = vh;
            g_screen_origin_x = ox;
            g_screen_origin_y = oy;
        }

        // Refresh virtual-screen cache for fast mouse coordinate mapping
        refresh_vscreen_cache();
        g_last_mouse_x = -1;
        g_last_mouse_y = -1;

        // Refresh per-monitor info for mouse coordinate mapping
        {
            std::lock_guard<std::mutex> lk(g_monitors_m);
            read_monitors(g_monitors);
        }

        if (!changed)
            continue;

        std::this_thread::sleep_for(seconds(3));
        runtime.restart = true;
        ctrl_send_hello();
    }
}

void RDPAgent::clipboard_watch_loop()
{
    {
        std::string cur = clipboard_read_utf8();
        std::lock_guard<std::mutex> lk(g_clip_m);
        g_last_clip = cur;
    }
    // log("[clipboard] watch loop started");
    while (!runtime.stop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (runtime.stop)
            break;
        std::string cur = clipboard_read_utf8();
        if (cur.empty())
            continue;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(g_clip_m);
            if (cur != g_last_clip)
            {
                // logf("[clipboard] change detected: %zu bytes", cur.size());
                g_last_clip = cur;
                changed = true;
            }
        }
        if (!changed)
            continue;
        if (cur.size() > 512 * 1024)
        {
            // logf("[clipboard] skipped: too large (%zu bytes)", cur.size());
            continue;
        }
        ctrl_send_clipboard(cur);
    }
    // log("[clipboard] watch loop stopped");
}

static size_t hash_cursor_bitmap(HCURSOR hcur)
{
    ICONINFO ii{};
    if (!GetIconInfo(hcur, &ii))
        return 0;
    size_t h = 0;
    if (ii.hbmMask)
    {
        BITMAP bm{};
        GetObject(ii.hbmMask, sizeof(BITMAP), &bm);
        LONG n = bm.bmWidthBytes * bm.bmHeight;
        if (n > 0 && n < 65536)
        {
            std::vector<uint8_t> bits((size_t)n);
            GetBitmapBits(ii.hbmMask, n, bits.data());
            for (size_t i = 0; i < bits.size(); i++)
                h = h * 31 + (size_t)bits[i];
        }
        DeleteObject(ii.hbmMask);
    }
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    // Mix in hotspot for additional discrimination
    h ^= ((size_t)ii.xHotspot << 16) ^ (size_t)ii.yHotspot;
    return h;
}

void RDPAgent::cursor_watch_loop()
{
    MSG msg;
    PeekMessageA(&msg, NULL, 0, 0, PM_NOREMOVE);

    // Build cursor lookup: HCURSOR handle + bitmap hash
    struct CursorEntry { int id; HCURSOR hcur; size_t hash; };
    auto load_cur = [](int id, LPCSTR name) -> CursorEntry {
        HCURSOR h = LoadCursorA(NULL, name);
        return {id, h, hash_cursor_bitmap(h)};
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
    logf("[cursor] watch loop started");
    while (!runtime.stop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (runtime.stop) break;

        CURSORINFO ci = { sizeof(CURSORINFO) };
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING))
            continue;

        int shape = 0;
        // Fast path: HCURSOR handle match
        for (auto &e : table)
        {
            if (ci.hCursor == e.hcur)
            {
                shape = e.id;
                break;
            }
        }
        // Slow path: bitmap hash match (handles themed cursors)
        if (!shape)
        {
            size_t cur_hash = hash_cursor_bitmap(ci.hCursor);
            if (cur_hash)
            {
                for (auto &e : table)
                {
                    if (e.hash == cur_hash)
                    {
                        shape = e.id;
                        break;
                    }
                }
            }
        }
        if (!shape)
            shape = 32512;

        if (shape != last_shape)
        {
            last_shape = shape;
            ctrl_send_cursor(shape);
        }
    }
    logf("[cursor] watch loop stopped");
}

// ============ DWM KEEPALIVE ============
// DWM засыпает через ~100ms без визуальных изменений.
// Держим DWM всегда в тонусе — дёргаем курсор на 1px каждые 15ms,
// чередуя влево-вправо. Курсор из видео убран (IsCursorCaptureEnabled),
// так что осцилляция невидима. Sleep(1ms) не нужен — чередование
// направлений само даёт DWM время заметить движение.
void RDPAgent::session_keepalive_loop()
{
    bool nudge_right = true;
    while (!runtime.stop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));

        // Skip nudge if frames are flowing (active movement, video playback)
        auto t_now = std::chrono::steady_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
        if (t_now - g_last_frame_time.load(std::memory_order_relaxed) < 30)
            continue;

        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw < 2 || vh < 2) continue;
        POINT pt;
        GetCursorPos(&pt);
        int vx_org = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy_org = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int cx = std::max(vx_org, std::min((int)pt.x, vx_org + vw - 1));
        int cy = std::max(vy_org, std::min((int)pt.y, vy_org + vh - 1));

        int nx = nudge_right
            ? (cx + 1 < vx_org + vw ? cx + 1 : cx - 1)
            : (cx - 1 >= vx_org ? cx - 1 : cx + 1);
        nudge_right = !nudge_right;

        LONG dx = (LONG)((int64_t)(nx - vx_org) * 65535 / (vw - 1));
        LONG dy = (LONG)((int64_t)(cy - vy_org) * 65535 / (vh - 1));

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = dx;
        in.mi.dy = dy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        SendInput(1, &in, sizeof(INPUT));
    }
}

// ============ DWM WAKE (immediate) ============
// Однократный nudge курсора на 1px и обратно, чтобы разбудить DWM
// перед входом в цикл захвата.
void RDPAgent::wake_dwm_now()
{
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0) vw = 1;
    if (vh <= 0) vh = 1;
    POINT pt;
    GetCursorPos(&pt);
    int vx_org = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy_org = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cx = std::max(vx_org, std::min((int)pt.x, vx_org + vw - 1));
    int cy = std::max(vy_org, std::min((int)pt.y, vy_org + vh - 1));
    LONG x_orig = (LONG)((int64_t)(cx - vx_org) * 65535 / (vw - 1));
    LONG y_fixed = (LONG)((int64_t)(cy - vy_org) * 65535 / (vh - 1));
    int nx = (cx + 1 < vx_org + vw) ? cx + 1 : cx - 1;
    LONG x_nudge = (LONG)((int64_t)nx * 65535 / (vw - 1));

    INPUT in_nudge = {};
    in_nudge.type = INPUT_MOUSE;
    in_nudge.mi.dx = x_nudge;
    in_nudge.mi.dy = y_fixed;
    in_nudge.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in_nudge, sizeof(INPUT));

    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    INPUT in_restore = {};
    in_restore.type = INPUT_MOUSE;
    in_restore.mi.dx = x_orig;
    in_restore.mi.dy = y_fixed;
    in_restore.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in_restore, sizeof(INPUT));
}

bool RDPAgent::init_capture()
{
    shutdown_capture();

    // Determine base config before creating capture
    std::string enc_type = "cpu";
    int base_fps = 5;
    int base_qp = 23;
    {
        std::lock_guard<std::mutex> lk(runtime.m);
        enc_type = runtime.encoder.empty() ? "cpu" : runtime.encoder;
        base_fps = runtime.manual_mode ? runtime.framerate : runtime.auto_adj_fps;
        base_qp = runtime.quality_qp;
    }

    // Create capture (WGC only)
    auto cap = std::make_unique<WGCCapture>();
    if (!cap->Initialize(base_fps))
    {
        logf("[capture] WGC initialization failed");
        return false;
    }
    logf("[capture] WGC initialized: %d outputs", cap->GetMonitorCount());

    int n = cap->GetMonitorCount();
    if (n <= 0)
    {
        logf("[capture] no monitors available");
        return false;
    }

    // Create per-monitor states
    m_monitors.clear();
    m_monitors.resize(n);

    for (int i = 0; i < n; i++)
    {
        auto &ms = m_monitors[i];
        ms = std::make_unique<PerMonitorState>();
        ms->id = i;
        ms->stop = false;
        ms->has_new = false;
        ms->encoder_type = enc_type;
        ms->t0 = std::chrono::steady_clock::now();
        ms->auto_adj_next = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        // Get monitor dimensions
        int mx = 0, my = 0;
        if (!cap->GetMonitorInfo(i, ms->w, ms->h, mx, my))
        {
            logf("[capture] GetMonitorInfo(%d) failed", i);
            shutdown_capture();
            return false;
        }
        ms->origin_x = mx;
        ms->origin_y = my;
        // Quality (QP, constant)
        ms->quality_qp = base_qp;
        ms->measured_bitrate_kbps = 0;

        // FPS
        ms->fps = base_fps;
        ms->auto_adj_fps = base_fps;

        // Create encoder
        bool encoder_ok = false;

        auto try_encoder = [&](auto &&factory, const char *name) -> bool {
            auto enc = factory();
            if (!enc) return false;
            if (!enc->Initialize(ms->w, ms->h, ms->fps, ms->quality_qp))
                return false;
            ms->encoder = std::move(enc);
            return true;
        };

        if (enc_type == "amf")
        {
            if (AMFEncoder::IsAvailable())
                encoder_ok = try_encoder([]() { return std::make_unique<AMFEncoder>(); }, "h264_amf");
            if (!encoder_ok)
            {
                logf("[capture] monitor %d: AMF unavailable, fallback to libx264", i);
                encoder_ok = try_encoder([]() { return std::make_unique<X264Encoder>(); }, "libx264");
            }
        }
        else if (enc_type == "ave")
        {
            if (AVEEncoder::IsAvailable())
                encoder_ok = try_encoder([]() { return std::make_unique<AVEEncoder>(); }, "h264_ave");
            if (!encoder_ok)
            {
                logf("[capture] monitor %d: AVE unavailable, fallback to libx264", i);
                encoder_ok = try_encoder([]() { return std::make_unique<X264Encoder>(); }, "libx264");
            }
        }
        else if (enc_type == "qsv")
        {
            encoder_ok = try_encoder([]() { return std::make_unique<QSVEncoder>(); }, "h264_qsv");
            if (!encoder_ok)
            {
                logf("[capture] monitor %d: QSV init failed", i);
            }
        }
        else
        {
            encoder_ok = try_encoder([]() { return std::make_unique<X264Encoder>(); }, "libx264");
        }

        if (!encoder_ok)
        {
            logf("[capture] monitor %d: no encoder available", i);
            shutdown_capture();
            return false;
        }
    }

    m_capture = std::move(cap);

    // Start per-monitor ingest threads
    for (int i = 0; i < n; i++)
    {
        m_ingest_threads.emplace_back([this, i]()
            { encode_ingest_loop(i); });
    }

    // Populate global monitor list for mouse coordinate mapping
    {
        std::lock_guard<std::mutex> lk(g_monitors_m);
        read_monitors(g_monitors);
    }

    logf("[capture] init OK: %d monitors, %d ingest threads", n, n);
    return true;
}

void RDPAgent::shutdown_capture()
{
    // Signal stop to all per-monitor states
    for (auto &ms : m_monitors)
    {
        if (ms)
        {
            {
                std::lock_guard<std::mutex> lk(ms->m);
                ms->stop = true;
                ms->has_new = false;
            }
            ms->cv.notify_all();
        }
    }

    // Join ingest threads before destroying state
    for (auto &t : m_ingest_threads)
        if (t.joinable())
            t.join();
    m_ingest_threads.clear();

    // Destroy encoders
    for (auto &ms : m_monitors)
    {
        if (ms && ms->encoder)
        {
            ms->encoder->Shutdown();
            ms->encoder.reset();
        }
    }
    m_monitors.clear();

    // Destroy capture
    if (m_capture)
    {
        m_capture->Shutdown();
        m_capture.reset();
    }

    logf("[capture] pipeline shut down");
}

// ============ CAPTURE LOOP (1 thread) ============
void RDPAgent::capture_loop()
{
    while (!runtime.stop)
    {
        // (Re)init capture if needed
        if (!m_capture || runtime.restart)
        {
            bool was_restart = runtime.restart.exchange(false);
            shutdown_capture();
            if (runtime.stop)
                break;
            if (!init_capture())
            {
                logf("[capture] init_capture failed, retry in 3s");
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
        }

        auto &monitors = m_monitors;

        // Check secure desktop (UAC/lock)
        {
            static auto last_lock_check = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (now - last_lock_check >= std::chrono::seconds(5))
            {
                last_lock_check = now;
                if (is_secure_desktop_active())
                {
                    logf("[capture] session locked, shutting down pipeline");
                    shutdown_capture();
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    continue;
                }
            }
        }

        bool any_frame = false;

        for (int i = 0; i < (int)monitors.size(); i++)
        {
            if (!monitors[i] || monitors[i]->stop)
                continue;

            std::vector<uint8_t> bgra;
            int cw = 0, ch = 0;
            bool ok = m_capture->CaptureFrame(i, bgra, cw, ch);
            if (!ok)
                continue;

            // Resolution change check
            if (cw != monitors[i]->w || ch != monitors[i]->h)
            {
                if (monitors[i]->encoder)
                {
                    monitors[i]->encoder->Shutdown();
                    if (!monitors[i]->encoder->Initialize(cw, ch, monitors[i]->fps, monitors[i]->quality_qp))
                    {
                        logf("[capture] monitor %d: encoder reinit failed", i);
                        runtime.restart = true;
                        break;
                    }
                }
                monitors[i]->w = cw;
                monitors[i]->h = ch;
            }

            // Push frame to per-monitor queue
            {
                std::lock_guard<std::mutex> lk(monitors[i]->m);
                monitors[i]->latest_frame = std::move(bgra);
                monitors[i]->has_new = true;
            }
            monitors[i]->cv.notify_one();

            g_last_frame_time.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count(),
                std::memory_order_relaxed);

            any_frame = true;
        }

        if (!any_frame)
        {
            // Timeout: no frames from any monitor for 30s → restart
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    shutdown_capture();
    logf("[capture] capture_loop ended");
}

// ============ ENCODE + INGEST LOOP (1 thread per monitor) ============
void RDPAgent::encode_ingest_loop(int monitor_id)
{
    logf("[ingest %d] thread started", monitor_id);

    while (!runtime.stop)
    {
        // Get current monitor state — if stopped, exit immediately
        PerMonitorState *ms = nullptr;
        {
            if (monitor_id < (int)m_monitors.size() && m_monitors[monitor_id])
            {
                if (m_monitors[monitor_id]->stop)
                    break;
                ms = m_monitors[monitor_id].get();
            }
        }
        if (!ms)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Wait for capture_loop to deliver a frame
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(ms->m);
            ms->cv.wait(lk, [ms]() {
                return ms->has_new || ms->stop;
            });
            if (ms->stop || runtime.stop)
                continue;
            frame = std::move(ms->latest_frame);
            ms->has_new = false;
        }

        // Force keyframe before first encode (decoder needs SPS/PPS)
        if (ms->encoder)
            ms->encoder->Initialize(ms->w, ms->h, ms->fps, ms->quality_qp);

        // Encode
        std::vector<uint8_t> nal;
        if (!ms->encoder || !ms->encoder->EncodeFrame(frame, nal))
        {
            logf("[ingest %d] encode failed, reinit encoder", monitor_id);
            if (ms->encoder)
            {
                ms->encoder->Shutdown();
                if (!ms->encoder->Initialize(ms->w, ms->h, ms->fps, ms->quality_qp))
                {
                    ms->encoder.reset();
                    logf("[ingest %d] encoder reinit failed, requesting restart", monitor_id);
                    runtime.restart = true;
                    continue;
                }
            }
            continue;
        }
        if (nal.empty())
            continue;

        // TLS connect + HTTP ingest
        TlsConn *c = tls_connect(config.server_host, config.server_port, config.verify_cert);
        if (!c)
        {
            logf("[ingest %d] TLS connect failed, retry in 1s", monitor_id);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        std::ostringstream req;
        req << "POST /relay/ingest/" << config.agent_id
            << "?token=" << config.agent_token
            << "&monitor=" << monitor_id
            << " HTTP/1.1\r\n"
            << "Host: " << config.server_host << ":" << config.server_port << "\r\n"
            << "Content-Type: video/h264\r\n"
            << "X-Agent-Encoder: " << ms->encoder_type << "\r\n"
            << "X-Agent-Quality: " << runtime.quality_preset << "\r\n"
            << "X-Agent-Bitrate-Measured: " << (ms->measured_bitrate_kbps / 1000) << "\r\n"
            << "X-Agent-Width: " << ms->w << "\r\n"
            << "X-Agent-Height: " << ms->h << "\r\n"
            << "X-Agent-FPS: " << ms->fps << "\r\n"
            << "X-Agent-Mode: " << (runtime.manual_mode ? "manual" : "auto") << "\r\n"
            << "Transfer-Encoding: chunked\r\n"
            << "Connection: close\r\n\r\n";

        std::string req_str = req.str();
        if (!tls_send_all(c, req_str.data(), (int)req_str.size()))
        {
            logf("[ingest %d] HTTP request failed", monitor_id);
            tls_close(c);
            delete c;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // Send h264 data in a chunked loop
        ms->t0 = std::chrono::steady_clock::now();
        ms->ingest_frames_5s = 0;
        ms->ingest_bytes_5s = 0;
        uint64_t bytes_sent = 0;
        int frames_sent = 0;
        auto t_start = std::chrono::steady_clock::now();
        bool conn_ok = true;

        while (!runtime.stop && conn_ok)
        {
            // Re-check monitor validity each iteration
            PerMonitorState *mon = nullptr;
            {
                if (monitor_id < (int)m_monitors.size() && m_monitors[monitor_id])
                    mon = m_monitors[monitor_id].get();
            }
            if (!mon || mon->stop)
                break;

            // Send current NAL
            char chdr[32];
            int chdr_len = std::snprintf(chdr, sizeof chdr, "%X\r\n", (unsigned)nal.size());
            if (!tls_send_all(c, chdr, chdr_len) ||
                !tls_send_all(c, (const char *)nal.data(), (int)nal.size()) ||
                !tls_send_all(c, "\r\n", 2))
            {
                conn_ok = false;
                break;
            }

            bytes_sent += nal.size();
            frames_sent++;
            mon->frame_count++;
            mon->bytes_total += nal.size();
            mon->ingest_frames_5s++;
            mon->ingest_bytes_5s += nal.size();

            auto t_now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_now - mon->t0).count();
            if (elapsed_ms >= 5000)
            {
                double dt_sec = elapsed_ms / 1000.0;
                double kbps = (double)mon->ingest_bytes_5s * 8.0 / 1000.0 / dt_sec;
                mon->measured_bitrate_kbps = (int)(kbps + 0.5);
                int budget_us = 1000000 / mon->fps;
                bool overloaded = mon->avg_encode_us > budget_us;
                ctrl_send_monitor_status(monitor_id, mon->measured_bitrate_kbps, mon->fps, overloaded);
                mon->t0 = t_now;
                mon->ingest_frames_5s = 0;
                mon->ingest_bytes_5s = 0;
            }

            // Auto-fps ramp (every 30s, non-manual mode)
            if (!runtime.manual_mode)
            {
                auto now_a = std::chrono::steady_clock::now();
                if (now_a >= mon->auto_adj_next)
                {
                    mon->auto_adj_next = now_a + std::chrono::seconds(30);
                    int new_fps = mon->fps + 5;
                    if (new_fps <= 60 && new_fps != mon->fps)
                    {
                        if (mon->encoder && mon->encoder->Initialize(mon->w, mon->h, new_fps, mon->quality_qp))
                        {
                            mon->fps = new_fps;
                            mon->auto_adj_fps = mon->fps;
                            ctrl_send_monitor_status(monitor_id, mon->measured_bitrate_kbps, mon->fps, false);
                        }
                        else
                        {
                            int down = mon->fps - 1;
                            if (down >= 5)
                            {
                                mon->fps = down;
                                mon->auto_adj_fps = mon->fps;
                                if (mon->encoder)
                                    mon->encoder->Initialize(mon->w, mon->h, mon->fps, mon->quality_qp);
                                ctrl_send_monitor_status(monitor_id, mon->measured_bitrate_kbps, mon->fps, false);
                            }
                        }
                    }
                }
            }

            // Throttle to target fps (skip if input pending — deliver feedback ASAP)
            if (mon->fps > 0 && !g_input_pending.load(std::memory_order_relaxed))
            {
                auto frame_interval = std::chrono::microseconds(1000000 / mon->fps);
                auto elapsed = std::chrono::steady_clock::now() - t_start;
                if (elapsed < frame_interval)
                {
                    std::this_thread::sleep_for(frame_interval - elapsed);
                }
            }
            g_input_pending.store(false, std::memory_order_relaxed);

            // Wait for next frame from capture_loop
            {
                std::unique_lock<std::mutex> lk(mon->m);
                mon->cv.wait(lk, [mon]() {
                    return mon->has_new || mon->stop;
                });
                if (mon->stop || runtime.stop)
                    break;
                frame = std::move(mon->latest_frame);
                mon->has_new = false;
            }

            // Encode
            nal.clear();
            auto encode_t0 = std::chrono::steady_clock::now();
            bool enc_ok = mon->encoder && mon->encoder->EncodeFrame(frame, nal);
            auto encode_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - encode_t0).count();
            mon->avg_encode_us = mon->avg_encode_us > 0
                ? (mon->avg_encode_us * 3 + encode_elapsed) / 4
                : encode_elapsed;
            if (!enc_ok)
            {
                logf("[ingest %d] encode failed during ingest loop", monitor_id);
                break;
            }
        }

        // Flush encoder
        PerMonitorState *mon = nullptr;
        {
            if (monitor_id < (int)m_monitors.size() && m_monitors[monitor_id])
                mon = m_monitors[monitor_id].get();
        }
        if (mon && mon->encoder)
        {
            std::vector<uint8_t> flush_nal;
            mon->encoder->Flush(flush_nal);
            if (!flush_nal.empty())
            {
                char chdr[32];
                int chdr_len = std::snprintf(chdr, sizeof chdr, "%X\r\n", (unsigned)flush_nal.size());
                tls_send_all(c, chdr, chdr_len);
                tls_send_all(c, (const char *)flush_nal.data(), (int)flush_nal.size());
                tls_send_all(c, "\r\n", 2);
            }
        }

        // End chunked
        tls_send_all(c, "0\r\n\r\n", 5);
        tls_close(c);
        delete c;

        if (runtime.stop)
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    logf("[ingest %d] loop ended", monitor_id);
}

// ============ LIFECYCLE ============
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
            if (shm)
            {
                shm->last_activity_time = GetTickCount64();
                shm->timeout_min = config.timeout_min;
            }
            else
            {
                CloseHandle(shm_handle);
                shm_handle = nullptr;
            }
        }
    }
}

RDPAgent::~RDPAgent()
{
    stop();
    if (shm)
    {
        UnmapViewOfFile((LPVOID)shm);
        shm = nullptr;
    }
    if (shm_handle)
    {
        CloseHandle(shm_handle);
        shm_handle = nullptr;
    }
}

void RDPAgent::start()
{
    if (running)
        return;
    runtime.stop = false;

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0)
    {
        return;
    }

    init_screen_metrics();
    release_modifier_keys();

    threads.emplace_back([this]()
                         { control_loop(); });
    threads.emplace_back([this]()
                         { resolution_watch_loop(); });
    threads.emplace_back([this]()
                         { clipboard_watch_loop(); });
    threads.emplace_back([this]()
                         { cursor_watch_loop(); });
    threads.emplace_back([this]()
                         { session_keepalive_loop(); });
    threads.emplace_back([this]()
                         { capture_loop(); });

    running = true;
    logf("RDPAgent started");
}

void RDPAgent::stop()
{
    if (!running)
        return;
    log("Stopping RDPAgent...");
    runtime.stop = true;

    // Signal all monitors to wake ingest threads
    for (auto &ms : m_monitors)
    {
        if (ms)
        {
            {
                std::lock_guard<std::mutex> lk(ms->m);
                ms->stop = true;
            }
            ms->cv.notify_all();
        }
    }

    // Join control/capture threads — capture_loop calls shutdown_capture
    // internally which joins ingest threads
    for (auto &t : threads)
        if (t.joinable())
            t.join();
    threads.clear();

    // Safety: join any remaining ingest threads (if capture_loop didn't clean up)
    for (auto &t : m_ingest_threads)
        if (t.joinable())
            t.join();
    m_ingest_threads.clear();

    m_monitors.clear();
    if (m_capture)
    {
        m_capture->Shutdown();
        m_capture.reset();
    }

    WSACleanup();
    running = false;
    log("RDPAgent stopped");
}

bool RDPAgent::isRunning() const { return running; }

RDPAgent::Status RDPAgent::getStatus() const
{
    Status st;
    st.screen_w = g_screen_w.load();
    st.screen_h = g_screen_h.load();
    st.is_connected = (runtime.ctrl_conn != nullptr);
    return st;
}

// ============ SECURE DESKTOP / WORKSTATION LOCK DETECTION ============
// Detects if the Secure Desktop (UAC prompt) is active OR workstation is locked (Win+L)
// When locked, screen capture fails with error 5 (access denied)

typedef enum tagQUERY_USER_NOTIFICATION_STATE
{
    QUNS_NOT_PRESENT = 1,
    QUNS_BUSY = 2,
    QUNS_RUNNING_D3D_FULL_SCREEN = 3,
    QUNS_PRESENTATION_MODE = 4,
    QUNS_ACCEPTS_NOTIFICATIONS = 5,
    QUNS_QUIET_TIME = 6
} QUERY_USER_NOTIFICATION_STATE;

typedef HRESULT(WINAPI *SHQueryUserNotificationStateFn)(QUERY_USER_NOTIFICATION_STATE*);

bool RDPAgent::is_secure_desktop_active()
{
    // Method 1: SHQueryUserNotificationState - most reliable for Windows Vista+
    // Returns QUNS_NOT_PRESENT when session is locked
    HMODULE hShell32 = LoadLibraryA("shell32.dll");
    if (hShell32)
    {
        SHQueryUserNotificationStateFn pQuery = (SHQueryUserNotificationStateFn)
            GetProcAddress(hShell32, "SHQueryUserNotificationState");
        if (pQuery)
        {
            QUERY_USER_NOTIFICATION_STATE state;
            HRESULT hr = pQuery(&state);
            if (SUCCEEDED(hr))
            {
                if (state == QUNS_NOT_PRESENT)
                {
                    FreeLibrary(hShell32);
                    return true;
                }
            }
        }
        FreeLibrary(hShell32);
    }

    // Method 2: Try to open the input desktop with full access
    // If workstation is locked or on Secure Desktop, this will fail with Access Denied
    HDESK hDesk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | 
                                      DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP);
    if (!hDesk)
    {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
        {
            return true;
        }
    }
    else
    {
        // Check the desktop name
        char desk_name[256] = {0};
        DWORD len = 0;
        if (GetUserObjectInformationA(hDesk, UOI_NAME, desk_name, sizeof(desk_name) - 1, &len))
        {
            desk_name[len] = 0;
            std::string name(desk_name);
            // "Winlogon" = workstation locked
            // "Screen-saver" = secure screensaver active
            // "Secure Desktop" / "Default" with special prefix = UAC prompt
            if (name == "Winlogon" || 
                name.find("Screen-saver") != std::string::npos ||
                name.find("Secure") != std::string::npos)
            {
                CloseDesktop(hDesk);
                return true;
            }
        }
        CloseDesktop(hDesk);
    }

    // Method 3: Check current thread desktop
    HDESK threadDesk = GetThreadDesktop(GetCurrentThreadId());
    if (threadDesk)
    {
        char desk_name[256] = {0};
        DWORD len = 0;
        if (GetUserObjectInformationA(threadDesk, UOI_NAME, desk_name, sizeof(desk_name) - 1, &len))
        {
            desk_name[len] = 0;
            std::string name(desk_name);
            if (name == "Winlogon" || 
                name.find("Screen-saver") != std::string::npos ||
                name.find("Secure") != std::string::npos)
            {
                return true;
            }
        }
    }

    // Fallback: check if consent.exe is running (UAC prompt)
    return is_consent_exe_running();
}

bool RDPAgent::is_consent_exe_running()
{
    // Check if UAC consent.exe process is running
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32 pe{sizeof(pe)};
    bool found = false;

    if (Process32First(snap, &pe))
    {
        do
        {
            if (_stricmp(pe.szExeFile, "consent.exe") == 0)
            {
                found = true;
                break;
            }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return found;
}

// ============ USER-SESSION WORKER ENTRYPOINT ============
int run_rdp_worker(const std::string &host, int port,
                   const std::string &agent_id,
                   const std::string &agent_token,
                   bool verify_cert,
                   int timeout_min, const std::string &shm_name,
                   const std::string &codec,
                   const std::string &encoder,
                   const std::string &quality_preset,
                   int fps)
{
    // Make the process DPI-aware so GetSystemMetrics returns physical pixels
    // at any display scaling (100%, 125%, 150%, etc.).
    // Without this, on Windows 8.1+ at non-100% scaling, GetSystemMetrics returns
    // virtualized (scaled) pixel values (e.g. 1536x864 at 125% on a 1920x1080 display),
    // causing -video_size mismatch and wrong mouse coordinates.
    // Use SetProcessDpiAwareness first (Windows 8.1+), then fallback to SetProcessDPIAware.
    HMODULE hShcore = LoadLibraryA("SHCORE.DLL");
    if (hShcore)
    {
        typedef HRESULT(WINAPI *SetProcessDpiAwarenessFn)(int);
        SetProcessDpiAwarenessFn pSetProcessDpiAwareness =
            (SetProcessDpiAwarenessFn)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (pSetProcessDpiAwareness)
        {
            // PROCESS_PER_MONITOR_DPI_AWARE = 2
            pSetProcessDpiAwareness(2);
        }
        FreeLibrary(hShcore);
    }
    else
    {
        SetProcessDPIAware();
    }

    RDPConfig cfg;
    cfg.server_host = host;
    cfg.server_port = port;
    cfg.agent_id = agent_id;
    cfg.agent_token = agent_token;
    cfg.verify_cert = verify_cert;
    cfg.timeout_min = timeout_min;
    cfg.shm_name = shm_name;
    cfg.codec = codec.empty() ? "h264" : codec;
    cfg.encoder = encoder.empty() ? "cpu" : encoder;
    cfg.quality_preset = quality_preset.empty() ? "medium" : quality_preset;
    cfg.framerate = fps > 0 ? fps : 15;

    RDPAgent agent(cfg);
    log("[worker] rdp_worker init OK, starting agent...");
    agent.start();

    // Блокируемся до завершения (родительский процесс убьёт через TerminateProcess
    // при таймауте неактивности или по команде stop-rdp-worker).
    while (agent.isRunning())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    agent.stop();
    return 0;
}
