// rdp_agent.cpp — WebSocket/TLS infrastructure (static methods only)
// All RDP worker code removed — replaced by streaming worker in main.cpp
#include "rdp_agent.h"
#include <iostream>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <random>
#include <cstdarg>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Secur32.lib")
#pragma comment(lib, "crypt32.lib")

#include <mstcpip.h>

// ============ RAW TCP ============
static bool send_all_raw(SOCKET s, const char *p, int n)
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

static int recv_n_raw(SOCKET s, char *p, int n)
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

static SOCKET tcp_connect(const std::string &host, int port)
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

static bool tls_handshake(TlsConn *c, const std::string &host, bool verify_cert)
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
        return false;
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
        return false;

    std::vector<uint8_t> in_buf;
    char tmp[16384];

    while (true)
    {
        int n = recv(c->sock, tmp, sizeof tmp, 0);
        if (n <= 0)
            return false;
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
            in_buf.clear();

        if (ss == SEC_E_OK)
            break;
        if (ss == SEC_I_CONTINUE_NEEDED)
            continue;
        if (ss == SEC_E_INCOMPLETE_MESSAGE)
        {
            n = recv(c->sock, tmp, sizeof tmp, 0);
            if (n <= 0)
                return false;
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

static bool tls_send_all(TlsConn *c, const char *p, int n)
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

static int tls_recv_some(TlsConn *c, char *buf, int want)
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

static int tls_recv_n(TlsConn *c, char *p, int n)
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
static std::string b64(const unsigned char *d, size_t n)
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
