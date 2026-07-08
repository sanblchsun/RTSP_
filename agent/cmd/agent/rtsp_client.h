#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket(x) close(x)
#endif

class RtspClient
{
public:
    RtspClient();
    ~RtspClient();

    bool Connect(const std::string &host, int port);
    void Disconnect();
    bool Handshake();

    bool HandshakeDone() const { return m_handshake_done; }
    bool SendRtp(const uint8_t *data, size_t size);

    bool KeyframeRequested() const { return m_keyframe_requested; }
    void ClearKeyframeFlag() { m_keyframe_requested = false; }
    void CheckForIncoming();

private:
    SOCKET m_sock = INVALID_SOCKET;
    bool m_handshake_done = false;
    bool m_keyframe_requested = false;
    char m_recv_buf[4096] = {};

    bool send_all(const uint8_t *buf, size_t len);
    void send_req(const std::string &req);
    bool recv_resp();
};
