#include "desktop_streamer.h"
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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
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

static std::atomic<bool> g_running{true};

void signal_handler(int)
{
    std::cout << "\nStopping..." << std::endl;
    g_running.store(false);
}

// ---- H.264 NAL over TCP sender ----
class TcpPushSender
{
public:
    TcpPushSender() = default;
    ~TcpPushSender() { Disconnect(); }

    bool Connect(const std::string &host, int port)
    {
        Disconnect();

        // init winsock (safe to call multiple times in Win)
#ifdef _WIN32
        static bool wsock = false;
        if (!wsock) { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); wsock = true; }
#endif

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

        int one = 1;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        return true;
    }

    void Disconnect()
    {
        if (sock_ != INVALID_SOCKET)
        {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    bool SendNal(const uint8_t *data, size_t size)
    {
        if (sock_ == INVALID_SOCKET) return false;
        // [4-byte big-endian size][NAL data]
        uint32_t net_size = htonl((uint32_t)size);
        int r = (int)send(sock_, (const char*)&net_size, 4, 0);
        if (r <= 0) return false;
        r = (int)send(sock_, (const char*)data, (int)size, 0);
        return r > 0;
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
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
              << "  " << prog << "                  # RTSP server mode (local)\n"
              << "  " << prog << " push <vps_ip>    # Push to VPS (H.264 over TCP)\n";
}

int main(int argc, char *argv[])
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    bool push_mode = false;
    std::string vps_host;
    int vps_port = 8554;

    if (argc > 1)
    {
        std::string mode = argv[1];
        if (mode == "push" && argc > 2)
        {
            push_mode = true;
            vps_host = argv[2];
            if (argc > 3) vps_port = std::atoi(argv[3]);
        }
        else
        {
            print_usage(argv[0]);
            return -1;
        }
    }

    // ---- Init capture ----
    auto capture = std::make_unique<WGCCapture>();
    if (!capture->Initialize(30))
    {
        std::cerr << "Capture init failed" << std::endl;
        return -1;
    }

    int w = 0, h = 0, mx = 0, my = 0;
    capture->GetMonitorInfo(0, w, h, mx, my);
    std::cout << "Monitor: " << w << "x" << h << std::endl;

    // ---- Init encoder ----
    auto encoder = std::make_unique<X264Encoder>();
    if (!encoder->Initialize(w, h, 30, 23))
    {
        std::cerr << "Encoder init failed" << std::endl;
        return -1;
    }

    // ---- Capture first frame for SPS/PPS ----
    auto capture_first_frame = [&]() -> bool {
        std::vector<uint8_t> bgra, nals;
        int fw = 0, fh = 0;
        for (int i = 0; i < 60 && g_running.load(); i++)
        {
            if (capture->CaptureFrame(0, bgra, fw, fh))
            {
                nals.clear();
                if (encoder->EncodeFrame(bgra, nals) && !nals.empty())
                    return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        return false;
    };

    std::vector<uint8_t> sps, pps;
    {
        std::vector<uint8_t> bgra2, nals2;
        int fw2, fh2;
        if (capture_first_frame())
        {
            capture->CaptureFrame(0, bgra2, fw2, fh2);
            encoder->EncodeFrame(bgra2, nals2);
            extract_sps_pps(nals2, sps, pps);
        }
    }
    std::cout << "SPS: " << sps.size() << " PPS: " << pps.size() << std::endl;

    // ---- Setup ----
    H264RtpPacketizer packetizer;
    packetizer.SetMaxPayloadSize(1400);
    packetizer.SetSsrc(0xDEADBEEF);

    RtspServer rtsp;
    TcpPushSender pusher;
    bool rtsp_ready = false;
    bool push_ready = false;

    if (push_mode)
    {
        std::cout << "Connecting to VPS " << vps_host << ":" << vps_port << std::endl;
        push_ready = pusher.Connect(vps_host, vps_port);
        if (!push_ready)
            std::cerr << "VPS connection failed, will retry" << std::endl;
    }
    else
    {
        rtsp.SetLogCallback([](const std::string &msg) {
            std::cout << "[rtsp] " << msg << std::endl;
        });
        if (!sps.empty() && !pps.empty())
            rtsp.SetVideoParams(w, h, sps, pps, 30);
        rtsp_ready = rtsp.Start(8554);
        if (rtsp_ready)
            std::cout << "RTSP ready on port 8554" << std::endl;
    }

    // ---- Main loop ----
    std::vector<uint8_t> bgra, nals;
    std::vector<std::vector<uint8_t>> rtp_packets;
    int fw = 0, fh = 0;
    uint32_t rtp_ts = 0;
    const uint32_t rtp_ts_step = 90000 / 30;
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

        if (push_mode)
        {
            // Send raw H.264 NALs to VPS
            if (!pusher.SendNal(nals.data(), nals.size()))
            {
                // Reconnect
                std::cout << "VPS disconnected, reconnecting..." << std::endl;
                pusher.Disconnect();
                std::this_thread::sleep_for(std::chrono::seconds(1));
                push_ready = pusher.Connect(vps_host, vps_port);
                if (push_ready)
                    pusher.SendNal(nals.data(), nals.size());
            }
        }
        else
        {
            // RTSP mode: RTP packetize + send
            packetizer.Packetize(nals.data(), nals.size(), rtp_ts, rtp_packets);
            rtp_ts += rtp_ts_step;

            if (rtsp.IsClientConnected())
            {
                for (const auto &pkt : rtp_packets)
                    rtsp.SendRtp(pkt.data(), pkt.size());
            }
        }

        frame_count++;
        if (frame_count % 30 == 0)
        {
            std::cout << "Frames: " << frame_count << " ("
                      << nals.size() << " bytes)\r" << std::flush;
        }
    }

    rtsp.Stop();
    pusher.Disconnect();
    encoder->Shutdown();
    capture->Shutdown();
    std::cout << "\nDone. " << frame_count << " frames" << std::endl;
    return 0;
}
