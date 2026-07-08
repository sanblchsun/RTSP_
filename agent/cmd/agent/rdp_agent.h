#pragma once

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <chrono>
#include <condition_variable>

// ============ LOGGING (реализуется в main.cpp) ============
extern void log(const char *msg);
extern void log(const std::string &msg);
extern void logf(const char *fmt, ...);

// ============ INTERNAL STRUCTURES ============

struct MonitorInfo
{
    int x = 0, y = 0, w = 0, h = 0;
};

// Shared memory between main process (SYSTEM) and worker
struct ActivityShm
{
    volatile LONG64 last_activity_time;
    volatile LONG timeout_min;
};

struct TlsConn
{
    SOCKET sock = INVALID_SOCKET;
    CredHandle cred = {};
    CtxtHandle ctx = {};
    bool cred_ok = false;
    bool ctx_ok = false;
    SecPkgContext_StreamSizes sizes = {};
    std::vector<uint8_t> raw;
    std::vector<uint8_t> plain;
    std::mutex send_m;
};

// ============ RDP AGENT CLASS (static WebSocket/TLS infrastructure only) ============

class RDPAgent
{
public:
    // TLS
    static TlsConn *tls_connect(const std::string &host, int port, bool verify_cert);
    static void tls_close(TlsConn *c);

    // WebSocket
    static bool ws_handshake(TlsConn *c, const std::string &host, int port, const std::string &path);
    static int ws_recv(TlsConn *c, std::vector<uint8_t> &payload);
    static bool ws_send(TlsConn *c, int op, const void *data, size_t len);
    static bool sock_has_data(TlsConn *c, int timeout_ms);

    // JSON
    static bool json_str(const std::string &j, const std::string &k, std::string &out);
    static bool json_str_ex(const std::string &j, const std::string &k, std::string &out);
    static bool json_int(const std::string &j, const std::string &k, int &out);
    static std::string json_escape(const std::string &s);
};
