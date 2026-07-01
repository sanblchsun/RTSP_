#include "rtsp_server.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket(x) close(x)
#endif

#include <cstdio>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>

static void default_rtsp_log(const std::string &msg)
{
    fprintf(stderr, "[rtsp] %s\n", msg.c_str());
}

static bool winsock_initialized = false;

static void ensure_winsock()
{
#ifdef _WIN32
    if (!winsock_initialized)
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        winsock_initialized = true;
    }
#endif
}

RtspServer::RtspServer() = default;
RtspServer::~RtspServer() { Stop(); }

void RtspServer::Log(const std::string &msg)
{
    if (log_)
        log_(msg);
    else
        default_rtsp_log(msg);
}

void RtspServer::SetVideoParams(int width, int height,
                                 const std::vector<uint8_t> &sps,
                                 const std::vector<uint8_t> &pps,
                                 int fps)
{
    video_width_ = width;
    video_height_ = height;
    sps_ = sps;
    pps_ = pps;
    fps_ = fps;
}

void RtspServer::SetLogCallback(LogCallback cb) { log_ = std::move(cb); }

bool RtspServer::Start(uint16_t port)
{
    Stop();
    port_ = port;
    ensure_winsock();

    server_fd_ = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0)
    {
        Log("Failed to create socket");
        return false;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd_, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        Log("Failed to bind to port " + std::to_string(port));
        closesocket(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 1) < 0)
    {
        Log("Failed to listen");
        closesocket(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread(&RtspServer::AcceptLoop, this);
    Log("RTSP server listening on port " + std::to_string(port));
    return true;
}

void RtspServer::Stop()
{
    running_.store(false);
    client_connected_.store(false);

#ifdef _WIN32
    if (client_fd_ >= 0) { closesocket(client_fd_); client_fd_ = -1; }
    if (server_fd_ >= 0) { closesocket(server_fd_); server_fd_ = -1; }
#else
    if (client_fd_ >= 0) { close(client_fd_); client_fd_ = -1; }
    if (server_fd_ >= 0) { close(server_fd_); server_fd_ = -1; }
#endif

    if (accept_thread_.joinable())
        accept_thread_.join();
    if (client_thread_.joinable())
        client_thread_.join();
}

void RtspServer::AcceptLoop()
{
    while (running_.load())
    {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int addrlen = sizeof(client_addr);
#else
        socklen_t addrlen = sizeof(client_addr);
#endif
        int fd = (int)accept(server_fd_, (sockaddr *)&client_addr, &addrlen);
        if (fd < 0)
        {
            if (running_.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char client_ip[64] = {};
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        client_address_ = client_ip;
        Log("Client connected: " + client_address_);

        // Close previous client if any
        if (client_fd_ >= 0)
        {
            closesocket(client_fd_);
            client_fd_ = -1;
        }
        client_fd_ = fd;
        client_connected_.store(true);

        if (client_thread_.joinable())
            client_thread_.join();
        client_thread_ = std::thread(&RtspServer::HandleClient, this);
    }
}

void RtspServer::HandleClient()
{
    char buf[4096];
    int fd = client_fd_;

    while (running_.load() && client_connected_.load())
    {
        // Read RTSP request (read until \r\n\r\n)
        std::string req;
        bool got_request = false;
        size_t total_read = 0;

        while (total_read < sizeof(buf) - 1)
        {
            int n = (int)recv(fd, buf + total_read,
                              (int)(sizeof(buf) - 1 - total_read), 0);
            if (n <= 0)
            {
                Log("Client disconnected");
                client_connected_.store(false);
                closesocket(fd);
                client_fd_ = -1;
                return;
            }
            total_read += (size_t)n;
            buf[total_read] = 0;

            // Check for end of headers
            std::string data(buf, total_read);
            if (data.find("\r\n\r\n") != std::string::npos)
            {
                req = data;
                got_request = true;
                break;
            }
        }

        if (!got_request)
            continue;

        Log("RTSP request: " + req.substr(0, req.find("\r\n")));

        // Parse and handle
        std::string response;
        if (req.find("OPTIONS") == 0)
            response = HandleOptions(req);
        else if (req.find("DESCRIBE") == 0)
            response = HandleDescribe(req);
        else if (req.find("SETUP") == 0)
            response = HandleSetup(req);
        else if (req.find("PLAY") == 0)
            response = HandlePlay(req);
        else if (req.find("TEARDOWN") == 0)
            response = HandleTeardown(req);
        else
        {
            // Extract CSeq from request for error response
            size_t cseq_pos = req.find("CSeq:");
            std::string cseq_str;
            if (cseq_pos != std::string::npos)
            {
                size_t end = req.find("\r\n", cseq_pos);
                cseq_str = req.substr(cseq_pos + 5, end - cseq_pos - 5);
                // trim
                while (!cseq_str.empty() && (cseq_str[0] == ' ' || cseq_str[0] == '\t'))
                    cseq_str = cseq_str.substr(1);
            }
            response = "RTSP/1.0 405 Method Not Allowed\r\nCSeq: " + cseq_str + "\r\n\r\n";
        }

        if (!response.empty())
            SendResponse(response);
    }
}

void RtspServer::SendResponse(const std::string &response)
{
    if (client_fd_ < 0)
        return;
    send(client_fd_, response.c_str(), (int)response.size(), 0);
}

std::string RtspServer::HandleOptions(const std::string &req)
{
    // Extract CSeq
    size_t cseq_pos = req.find("CSeq:");
    std::string cseq = "1";
    if (cseq_pos != std::string::npos)
    {
        size_t end = req.find("\r\n", cseq_pos);
        cseq = req.substr(cseq_pos + 5, end - cseq_pos - 5);
        while (!cseq.empty() && (cseq[0] == ' ' || cseq[0] == '\t'))
            cseq = cseq.substr(1);
    }

    std::ostringstream resp;
    resp << "RTSP/1.0 200 OK\r\n"
         << "CSeq: " << cseq << "\r\n"
         << "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n"
         << "\r\n";
    return resp.str();
}

std::string RtspServer::HandleDescribe(const std::string &req)
{
    size_t cseq_pos = req.find("CSeq:");
    std::string cseq = "1";
    if (cseq_pos != std::string::npos)
    {
        size_t end = req.find("\r\n", cseq_pos);
        cseq = req.substr(cseq_pos + 5, end - cseq_pos - 5);
        while (!cseq.empty() && (cseq[0] == ' ' || cseq[0] == '\t'))
            cseq = cseq.substr(1);
    }

    std::string sdp = GenerateSdp();
    std::ostringstream resp;
    resp << "RTSP/1.0 200 OK\r\n"
         << "CSeq: " << cseq << "\r\n"
         << "Content-Type: application/sdp\r\n"
         << "Content-Length: " << sdp.size() << "\r\n"
         << "\r\n"
         << sdp;
    return resp.str();
}

std::string RtspServer::HandleSetup(const std::string &req)
{
    size_t cseq_pos = req.find("CSeq:");
    std::string cseq = "1";
    if (cseq_pos != std::string::npos)
    {
        size_t end = req.find("\r\n", cseq_pos);
        cseq = req.substr(cseq_pos + 5, end - cseq_pos - 5);
        while (!cseq.empty() && (cseq[0] == ' ' || cseq[0] == '\t'))
            cseq = cseq.substr(1);
    }

    // Parse transport: prefer TCP interleaved
    size_t transport_pos = req.find("Transport:");
    std::string transport = "RTP/AVP/TCP;interleaved=0-1";
    if (transport_pos != std::string::npos)
    {
        size_t end = req.find("\r\n", transport_pos);
        std::string client_transport = req.substr(transport_pos + 10, end - transport_pos - 10);
        // Trim
        while (!client_transport.empty() && (client_transport[0] == ' ' || client_transport[0] == '\t'))
            client_transport = client_transport.substr(1);

        if (client_transport.find("RTP/AVP/TCP") != std::string::npos)
        {
            // Use interleaved mode
            size_t interleaved_pos = client_transport.find("interleaved=");
            if (interleaved_pos != std::string::npos)
            {
                transport = client_transport; // echo back client's transport
            }
            else
            {
                transport = "RTP/AVP/TCP;interleaved=0-1";
            }
        }
        else if (client_transport.find("RTP/AVP/UDP") != std::string::npos)
        {
            // Extract client ports for UDP
            transport = client_transport;
        }
        else
        {
            transport = "RTP/AVP/TCP;interleaved=0-1";
        }
    }

    std::string session_id = "12345678";
    std::ostringstream resp;
    resp << "RTSP/1.0 200 OK\r\n"
         << "CSeq: " << cseq << "\r\n"
         << "Transport: " << transport << "\r\n"
         << "Session: " << session_id << "\r\n"
         << "\r\n";
    return resp.str();
}

std::string RtspServer::HandlePlay(const std::string &req)
{
    size_t cseq_pos = req.find("CSeq:");
    std::string cseq = "1";
    if (cseq_pos != std::string::npos)
    {
        size_t end = req.find("\r\n", cseq_pos);
        cseq = req.substr(cseq_pos + 5, end - cseq_pos - 5);
        while (!cseq.empty() && (cseq[0] == ' ' || cseq[0] == '\t'))
            cseq = cseq.substr(1);
    }

    std::string session_id = "12345678";
    // Use RTP-Info for timestamp and sequence sync
    std::ostringstream resp;
    resp << "RTSP/1.0 200 OK\r\n"
         << "CSeq: " << cseq << "\r\n"
         << "Session: " << session_id << "\r\n"
         << "RTP-Info: url=trackID=0\r\n"
         << "\r\n";
    return resp.str();
}

std::string RtspServer::HandleTeardown(const std::string &req)
{
    size_t cseq_pos = req.find("CSeq:");
    std::string cseq = "1";
    if (cseq_pos != std::string::npos)
    {
        size_t end = req.find("\r\n", cseq_pos);
        cseq = req.substr(cseq_pos + 5, end - cseq_pos - 5);
        while (!cseq.empty() && (cseq[0] == ' ' || cseq[0] == '\t'))
            cseq = cseq.substr(1);
    }

    std::ostringstream resp;
    resp << "RTSP/1.0 200 OK\r\n"
         << "CSeq: " << cseq << "\r\n"
         << "\r\n";
    return resp.str();
}

std::string RtspServer::GenerateSdp() const
{
    // Base64 encode SPS/PPS for fmtp line
    // For simplicity, use profile-level-id from SPS
    std::string sps_hex;
    char hex[4];
    for (size_t i = 0; i < sps_.size() && i < 4; i++)
    {
        snprintf(hex, sizeof(hex), "%02X", sps_[i]);
        sps_hex += hex;
    }
    // profile-level-id is bytes 1,2,3 of SPS (after NAL header)
    std::string profile_level = "42C01F"; // default baseline 3.1
    if (sps_.size() >= 4)
    {
        // SPS: [NAL header, profile, profile_compat, level, ...]
        profile_level = "";
        for (int i = 1; i <= 3 && i < (int)sps_.size(); i++)
        {
            snprintf(hex, sizeof(hex), "%02X", sps_[i]);
            profile_level += hex;
        }
    }

    std::string sprop;
    if (!sps_.empty() && !pps_.empty())
    {
        sprop = "sprop-parameter-sets=";
        // Base64 encode SPS and PPS
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        auto base64_encode = [&](const std::vector<uint8_t> &data) -> std::string {
            std::string out;
            size_t i = 0;
            while (i < data.size())
            {
                uint32_t b = (uint32_t)data[i++] << 16;
                if (i < data.size()) b |= (uint32_t)data[i++] << 8;
                if (i < data.size()) b |= data[i++];
                out += b64[(b >> 18) & 0x3F];
                out += b64[(b >> 12) & 0x3F];
                out += (i - 1 < data.size()) ? b64[(b >> 6) & 0x3F] : '=';
                out += (i < data.size()) ? b64[b & 0x3F] : '=';
            }
            return out;
        };

        sprop += base64_encode(sps_);
        sprop += ",";
        sprop += base64_encode(pps_);
    }

    std::ostringstream sdp;
    sdp << "v=0\r\n"
        << "o=- " << std::chrono::system_clock::now().time_since_epoch().count()
        << " 1 IN IP4 0.0.0.0\r\n"
        << "s=Desktop Stream\r\n"
        << "c=IN IP4 0.0.0.0\r\n"
        << "t=0 0\r\n"
        << "a=tool:desktop_streamer\r\n"
        << "m=video 0 RTP/AVP 96\r\n"
        << "a=rtpmap:96 H264/90000\r\n"
        << "a=fmtp:96 packetization-mode=1;profile-level-id=" << profile_level;
    if (!sprop.empty())
        sdp << ";" << sprop;
    sdp << "\r\n"
        << "a=control:trackID=0\r\n"
        << "\r\n";
    return sdp.str();
}

void RtspServer::SendRtp(const uint8_t *data, size_t size)
{
    if (client_fd_ < 0 || !client_connected_.load())
        return;
    SendInterleavedRtp(data, size);
}

void RtspServer::SendInterleavedRtp(const uint8_t *data, size_t size)
{
    if (client_fd_ < 0)
        return;

    // Interleaved format: $<channel(1)><length(2)><data>
    uint8_t header[4];
    header[0] = '$';
    header[1] = (uint8_t)rtp_channel_;
    header[2] = (uint8_t)((size >> 8) & 0xFF);
    header[3] = (uint8_t)(size & 0xFF);

    send(client_fd_, (const char *)header, 4, 0);
    send(client_fd_, (const char *)data, (int)size, 0);
}
