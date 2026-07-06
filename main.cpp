#include "WinRT-API/capture_wgc.h"
#include "WinRT-API/encoder_x264.h"
#include "rtp/h264_rtp_packetizer.h"
#include "rtp/rtp_header.h"
#include "rtsp/rtsp_server.h"

#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <atomic>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
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

// ---- Tuning parameters ----
static const int kFps = 15;
// для kQp
// 23	отличное ~5-8 Mbps
// 25	хорошее	 ~3-5 Mbps
// 28	среднее	 ~2-3 Mbps
// 30	низкое	 ~1-2 Mbps
static const int kQp = 28;

static std::atomic<bool> g_running{true};

void signal_handler(int)
{
    std::cout << "\nStopping..." << std::endl;
    g_running.store(false);
}

#ifdef _WIN32
static bool ensure_winsock()
{
    static bool ok = false;
    if (!ok) { WSADATA w; ok = WSAStartup(MAKEWORD(2,2), &w) == 0; }
    return ok;
}
#else
static bool ensure_winsock() { return true; }
#endif

// ---- RTSP client (push mode) ----
class RtspClient
{
public:
    ~RtspClient() { Disconnect(); }

    bool HandshakeDone() const { return handshake_done_; }
    void SetUdpMode(bool on) { udp_mode_ = on; }

    bool Connect(const std::string &host, int port)
    {
        Disconnect();
        ensure_winsock();

        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == INVALID_SOCKET) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (connect(sock_, (sockaddr*)&addr, sizeof(addr)) < 0)
        {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            return false;
        }

        // Save server address for UDP target
        udp_target_ = addr;
        udp_target_.sin_port = 0;

        std::cout << "TCP: connected to " << host << ":" << port << " (RTSP control)" << std::endl;
        int one = 1;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        return true;
    }

    void Disconnect()
    {
        handshake_done_ = false;
        if (rtp_sock_ != INVALID_SOCKET)
        {
            closesocket(rtp_sock_);
            rtp_sock_ = INVALID_SOCKET;
        }
        if (sock_ != INVALID_SOCKET)
        {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    bool Handshake()
    {
        // OPTIONS
        send_req("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n");
        if (!recv_resp()) return false;

        // DESCRIBE
        send_req("DESCRIBE rtsp://relay/stream RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n");
        if (!recv_resp()) return false;

        // SETUP
        if (udp_mode_)
        {
            if (!SetupUdpSocket())
            {
                std::cout << "UDP: failed to bind socket" << std::endl;
                return false;
            }
            char setup_req[256];
            snprintf(setup_req, sizeof(setup_req),
                "SETUP rtsp://relay/stream/trackID=0 RTSP/1.0\r\n"
                "CSeq: 3\r\nTransport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n\r\n",
                local_rtp_port_, local_rtp_port_ + 1);
            send_req(setup_req);
        }
        else
        {
            send_req("SETUP rtsp://relay/stream/trackID=0 RTSP/1.0\r\n"
                     "CSeq: 3\r\nTransport: RTP/AVP/TCP;interleaved=0-1\r\n\r\n");
        }
        if (!recv_resp())
        {
            if (udp_mode_)
                std::cout << "UDP: SETUP failed (server may not support UDP transport)" << std::endl;
            return false;
        }
        if (udp_mode_ && !ParseUdpTransport())
        {
            std::cout << "UDP: failed to parse server_port from SETUP response" << std::endl;
            return false;
        }
        if (udp_mode_)
            std::cout << "UDP: server RTP port " << ntohs(udp_target_.sin_port) << std::endl;

        // PLAY
        send_req("PLAY rtsp://relay/stream RTSP/1.0\r\nCSeq: 4\r\nSession: 12345678\r\n\r\n");
        if (!recv_resp()) return false;

        handshake_done_ = true;
        if (udp_mode_)
            std::cout << "RTSP handshake OK | RTP UDP client->server port " << ntohs(udp_target_.sin_port) << " | local UDP port " << local_rtp_port_ << std::endl;
        else
            std::cout << "RTSP handshake OK | RTP TCP interleaved" << std::endl;
        return true;
    }

    bool SendRtp(const uint8_t *data, size_t size)
    {
        if (sock_ == INVALID_SOCKET) return false;
        if (udp_mode_)
        {
            if (rtp_sock_ == INVALID_SOCKET || udp_target_.sin_port == 0)
                return false;
            int r = sendto(rtp_sock_, (const char*)data, (int)size, 0,
                          (sockaddr*)&udp_target_, sizeof(udp_target_));
            return r > 0;
        }
        uint8_t header[4];
        header[0] = '$';
        header[1] = 0;                     // channel 0
        header[2] = (uint8_t)((size >> 8) & 0xFF);
        header[3] = (uint8_t)(size & 0xFF);
        if (!send_all(header, 4)) return false;
        return send_all(data, size);
    }

    bool send_all(const uint8_t *buf, size_t len)
    {
        while (len > 0) {
            int r = (int)send(sock_, (const char*)buf, (int)len, 0);
            if (r <= 0) return false;
            buf += r;
            len -= (size_t)r;
        }
        return true;
    }

    bool KeyframeRequested() const { return keyframe_requested_; }
    void ClearKeyframeFlag() { keyframe_requested_ = false; }

    void CheckForIncoming()
    {
        if (sock_ == INVALID_SOCKET) return;
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock_, &read_fds);
        struct timeval tv = {0, 0};
        if (select((int)sock_ + 1, &read_fds, nullptr, nullptr, &tv) > 0)
        {
            char buf[64];
            int n = (int)recv(sock_, buf, sizeof(buf) - 1, 0);
            if (n > 0)
            {
                buf[n] = 0;
                if (strstr(buf, "!K"))
                    keyframe_requested_ = true;
            }
        }
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
    bool handshake_done_ = false;
    bool keyframe_requested_ = false;
    char recv_buf_[4096] = {};

    // UDP mode
    bool udp_mode_ = false;
    SOCKET rtp_sock_ = INVALID_SOCKET;
    int local_rtp_port_ = 0;
    sockaddr_in udp_target_{};

    void send_req(const std::string &req)
    {
        send(sock_, req.c_str(), (int)req.size(), 0);
    }

    bool recv_resp()
    {
        int n = (int)recv(sock_, recv_buf_, sizeof(recv_buf_) - 1, 0);
        if (n <= 0) return false;
        recv_buf_[n] = 0;
        // Accept any 200 OK response
        if (strstr(recv_buf_, "200 OK")) return true;
        return false;
    }

    bool SetupUdpSocket()
    {
        rtp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (rtp_sock_ == INVALID_SOCKET) return false;

        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = 0;

        if (bind(rtp_sock_, (sockaddr*)&bind_addr, sizeof(bind_addr)) < 0)
        {
            closesocket(rtp_sock_);
            rtp_sock_ = INVALID_SOCKET;
            return false;
        }

        sockaddr_in name;
        socklen_t name_len = sizeof(name);
        if (getsockname(rtp_sock_, (sockaddr*)&name, &name_len) == 0)
            local_rtp_port_ = ntohs(name.sin_port);

        std::cout << "UDP: local RTP port " << local_rtp_port_ << std::endl;
        return true;
    }

    bool ParseUdpTransport()
    {
        const char *p = strstr(recv_buf_, "server_port=");
        if (!p) return false;
        int port = 0;
        if (sscanf(p, "server_port=%d", &port) < 1) return false;
        udp_target_.sin_port = htons((uint16_t)port);
        return udp_target_.sin_port != 0;
    }
};

// Extract SPS/PPS from encoded NAL data
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

        // Find end of this NAL (next start code or end)
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

        if (nal_type == 7) // SPS
            sps.assign(nal_data.begin() + nal_start, nal_data.begin() + nal_end);
        else if (nal_type == 8) // PPS
            pps.assign(nal_data.begin() + nal_start, nal_data.begin() + nal_end);

        if (!sps.empty() && !pps.empty())
            return true;

        i = nal_end;
    }
    return !sps.empty() && !pps.empty();
}

static void print_usage(const char *prog)
{
    std::cout << "Usage:\n"
              << "  " << prog << "                         # RTSP server mode (local, ffplay)\n"
              << "  " << prog << " push <vps_ip> [port] [udp]    # Push to VPS (default " << 8554 << ", tcp)\n";
}

int main(int argc, char *argv[])
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Server's frame queue size (webrtc_server.py: deque maxlen) = 20

    bool push_mode = false;
    bool udp_mode = false;
    std::string vps_host;
    int vps_port = 8554;

    if (argc > 1)
    {
        std::string mode = argv[1];
        if (mode == "push" && argc > 2)
        {
            push_mode = true;
            vps_host = argv[2];
            if (argc > 3)
            {
                if (strcmp(argv[3], "udp") == 0)
                    udp_mode = true;
                else
                {
                    vps_port = std::atoi(argv[3]);
                    if (argc > 4 && strcmp(argv[4], "udp") == 0)
                        udp_mode = true;
                }
            }
        }
        else
        {
            print_usage(argv[0]);
            return -1;
        }
    }

    // ---- Init capture ----
    auto capture = std::make_unique<WGCCapture>();
    if (!capture->Initialize(kFps))
    {
        std::cerr << "Capture init failed" << std::endl;
        return -1;
    }

    int w = 0, h = 0, mx = 0, my = 0;
    capture->GetMonitorInfo(0, w, h, mx, my);
    std::cout << "Monitor: " << w << "x" << h << std::endl;

    // ---- Init encoder ----
    auto encoder = std::make_unique<X264Encoder>();
    if (!encoder->Initialize(w, h, kFps, kQp))
    {
        std::cerr << "Encoder init failed" << std::endl;
        return -1;
    }

    // ---- Get SPS/PPS from encoder headers ----
    std::vector<uint8_t> sps = encoder->GetSps();
    std::vector<uint8_t> pps = encoder->GetPps();
    if (sps.empty() || pps.empty())
    {
        // Fallback: capture first frame and extract
        std::vector<uint8_t> bgra, first_nals;
        int fw = 0, fh = 0;
        for (int i = 0; i < 60 && g_running.load(); i++)
        {
            if (capture->CaptureFrame(0, bgra, fw, fh))
            {
                first_nals.clear();
                if (encoder->EncodeFrame(bgra, first_nals) && !first_nals.empty())
                {
                    extract_sps_pps(first_nals, sps, pps);
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
    std::cout << "SPS: " << sps.size() << " PPS: " << pps.size() << std::endl;

    // ---- Setup ----
    H264RtpPacketizer packetizer;
    if (udp_mode)
        packetizer.SetMaxPayloadSize(1400);  // avoid IP fragmentation over UDP
    else
        packetizer.SetMaxPayloadSize(8000);  // TCP stream, fragmentation is fine
    packetizer.SetSsrc(0xDEADBEEF);

    RtspServer rtsp;
    RtspClient rtsp_client;

    if (push_mode)
    {
        rtsp_client.SetUdpMode(udp_mode);
        if (udp_mode)
            std::cout << "Push mode: RTSP " << (udp_mode ? "UDP" : "TCP") << " to " << vps_host << ":" << vps_port << std::endl;
    }
    else
    {
        std::cout << "RTSP server mode (local)" << std::endl;
        rtsp.SetLogCallback([](const std::string &msg) {
            std::cout << "[rtsp] " << msg << std::endl;
        });
        if (!sps.empty() && !pps.empty())
            rtsp.SetVideoParams(w, h, sps, pps, kFps);
        if (rtsp.Start(8554))
            std::cout << "RTSP ready on port 8554" << std::endl;
        else
            std::cerr << "RTSP start failed" << std::endl;
    }

    // ---- Main loop ----
    timeBeginPeriod(1);
    std::vector<uint8_t> bgra, nals;
    std::vector<std::vector<uint8_t>> rtp_packets;
    int fw = 0, fh = 0;
    uint32_t rtp_ts = 0;
    const uint32_t rtp_ts_step = 90000 / kFps;
    const auto frame_duration = std::chrono::nanoseconds(1000000000LL / kFps);
    auto next_frame = std::chrono::steady_clock::now();
    int64_t frame_count = 0;

    while (g_running.load())
    {
        if (!capture->CaptureFrame(0, bgra, fw, fh))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        nals.clear();
        if (!encoder->EncodeFrame(bgra, nals))
        {
            std::cerr << "Encode error" << std::endl;
            break;
        }
        if (nals.empty())
            continue;

        // RTP packetize (both modes)
        packetizer.Packetize(nals.data(), nals.size(), rtp_ts, rtp_packets);
        rtp_ts += rtp_ts_step;

        if (push_mode)
        {
            // Auto-connect on first frame
            if (!rtsp_client.HandshakeDone())
            {
                if (rtsp_client.Connect(vps_host, vps_port))
                {
                    if (!rtsp_client.Handshake())
                    {
                        std::cout << "RTSP handshake failed, retrying in 1s..." << std::endl;
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

            // Check for incoming keyframe requests from server
            rtsp_client.CheckForIncoming();
            if (rtsp_client.KeyframeRequested())
            {
                encoder->RequestKeyframe();
                rtsp_client.ClearKeyframeFlag();
            }

            for (const auto &pkt : rtp_packets)
            {
                if (!rtsp_client.SendRtp(pkt.data(), pkt.size()))
                {
                    std::cout << "VPS disconnected, reconnecting..." << std::endl;
                    rtsp_client.Disconnect();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    break;
                }
            }
        }
        else if (rtsp.IsClientConnected())
        {
            for (const auto &pkt : rtp_packets)
                rtsp.SendRtp(pkt.data(), pkt.size());
        }

        frame_count++;

        next_frame += frame_duration;
        std::this_thread::sleep_until(next_frame);
        if (next_frame < std::chrono::steady_clock::now())
            next_frame = std::chrono::steady_clock::now();

        if (frame_count % 30 == 0)
        {
            std::cout << "Frames: " << frame_count << " ("
                      << nals.size() << " bytes)\r" << std::flush;
        }
    }

    rtsp.Stop();
    rtsp_client.Disconnect();
    encoder->Shutdown();
    capture->Shutdown();
    timeEndPeriod(1);
    std::cout << "\nDone. " << frame_count << " frames" << std::endl;
    return 0;
}
