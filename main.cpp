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

        int one = 1;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        return true;
    }

    void Disconnect()
    {
        handshake_done_ = false;
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
        send_req("SETUP rtsp://relay/stream/trackID=0 RTSP/1.0\r\n"
                 "CSeq: 3\r\nTransport: RTP/AVP/TCP;interleaved=0-1\r\n\r\n");
        if (!recv_resp()) return false;

        // PLAY
        send_req("PLAY rtsp://relay/stream RTSP/1.0\r\nCSeq: 4\r\nSession: 12345678\r\n\r\n");
        if (!recv_resp()) return false;

        handshake_done_ = true;
        std::cout << "RTSP handshake OK" << std::endl;
        return true;
    }

    bool SendRtp(const uint8_t *data, size_t size)
    {
        if (sock_ == INVALID_SOCKET) return false;
        uint8_t header[4];
        header[0] = '$';
        header[1] = 0;                     // channel 0
        header[2] = (uint8_t)((size >> 8) & 0xFF);
        header[3] = (uint8_t)(size & 0xFF);
        int r = (int)send(sock_, (const char*)header, 4, 0);
        if (r <= 0) return false;
        r = (int)send(sock_, (const char*)data, (int)size, 0);
        return r > 0;
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
    bool handshake_done_ = false;
    char recv_buf_[4096];

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
              << "  " << prog << " push <vps_ip> [port]    # Push to VPS (TCP, default " << 8554 << ")\n";
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
            if (argc > 3)
                vps_port = std::atoi(argv[3]);
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
    packetizer.SetMaxPayloadSize(1400);
    packetizer.SetSsrc(0xDEADBEEF);

    RtspServer rtsp;
    RtspClient rtsp_client;

    if (push_mode)
    {
        std::cout << "Push mode: RTSP to " << vps_host << ":" << vps_port << std::endl;
    }
    else
    {
        std::cout << "RTSP server mode (local)" << std::endl;
        rtsp.SetLogCallback([](const std::string &msg) {
            std::cout << "[rtsp] " << msg << std::endl;
        });
        if (!sps.empty() && !pps.empty())
            rtsp.SetVideoParams(w, h, sps, pps, 30);
        if (rtsp.Start(8554))
            std::cout << "RTSP ready on port 8554" << std::endl;
        else
            std::cerr << "RTSP start failed" << std::endl;
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
    std::cout << "\nDone. " << frame_count << " frames" << std::endl;
    return 0;
}
