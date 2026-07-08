#include "rtsp_client.h"
#include <cstring>
#include <iostream>

extern void logf(const char *fmt, ...);

RtspClient::RtspClient() {}
RtspClient::~RtspClient() { Disconnect(); }

bool RtspClient::Connect(const std::string &host, int port)
{
    Disconnect();

    m_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (m_sock == INVALID_SOCKET)
        return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(m_sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    logf("[rtsp] TCP connected to %s:%d", host.c_str(), port);
    int one = 1;
    setsockopt(m_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    return true;
}

void RtspClient::Disconnect()
{
    m_handshake_done = false;
    if (m_sock != INVALID_SOCKET)
    {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
}

bool RtspClient::send_all(const uint8_t *buf, size_t len)
{
    while (len > 0)
    {
        int r = (int)send(m_sock, (const char *)buf, (int)len, 0);
        if (r <= 0) return false;
        buf += r;
        len -= (size_t)r;
    }
    return true;
}

void RtspClient::send_req(const std::string &req)
{
    send(m_sock, req.c_str(), (int)req.size(), 0);
}

bool RtspClient::recv_resp()
{
    int n = (int)recv(m_sock, m_recv_buf, sizeof(m_recv_buf) - 1, 0);
    if (n <= 0) return false;
    m_recv_buf[n] = 0;
    if (strstr(m_recv_buf, "200 OK")) return true;
    logf("[rtsp] unexpected response: %s", m_recv_buf);
    return false;
}

bool RtspClient::Handshake()
{
    send_req("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    if (!recv_resp()) return false;

    send_req("DESCRIBE rtsp://relay/stream RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n");
    if (!recv_resp()) return false;

    send_req("SETUP rtsp://relay/stream/trackID=0 RTSP/1.0\r\n"
             "CSeq: 3\r\nTransport: RTP/AVP/TCP;interleaved=0-1\r\n\r\n");
    if (!recv_resp()) return false;

    send_req("PLAY rtsp://relay/stream RTSP/1.0\r\nCSeq: 4\r\nSession: 12345678\r\n\r\n");
    if (!recv_resp()) return false;

    m_handshake_done = true;
    logf("[rtsp] handshake OK, RTP TCP interleaved");
    return true;
}

bool RtspClient::SendRtp(const uint8_t *data, size_t size)
{
    if (m_sock == INVALID_SOCKET) return false;
    uint8_t header[4];
    header[0] = '$';
    header[1] = 0;
    header[2] = (uint8_t)((size >> 8) & 0xFF);
    header[3] = (uint8_t)(size & 0xFF);
    if (!send_all(header, 4)) return false;
    return send_all(data, size);
}

void RtspClient::CheckForIncoming()
{
    if (m_sock == INVALID_SOCKET) return;
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(m_sock, &read_fds);
    struct timeval tv = {0, 0};
    if (select((int)m_sock + 1, &read_fds, nullptr, nullptr, &tv) > 0)
    {
        char buf[64];
        int n = (int)recv(m_sock, buf, sizeof(buf) - 1, 0);
        if (n > 0)
        {
            buf[n] = 0;
            if (strstr(buf, "!K"))
                m_keyframe_requested = true;
        }
    }
}
