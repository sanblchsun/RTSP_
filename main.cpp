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
static std::string vps_host;
static int vps_port = 8554;
static bool use_udp = false;

void signal_handler(int)
{
    std::cout << "\nStopping..." << std::endl;
    g_running.store(false);
}

// ---- init winsock (safe to call multiple times) ----
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

// ---- Interface for push transport ----
struct IPushSender
{
    virtual bool Connect(const std::string &host, int port) = 0;
    virtual void Disconnect() = 0;
    virtual bool SendNal(const uint8_t *data, size_t size) = 0;
    virtual ~IPushSender() = default;
};

// ---- H.264 NAL over TCP sender ----
class TcpPushSender : public IPushSender
{
public:
    ~TcpPushSender() override { Disconnect(); }

    bool Connect(const std::string &host, int port) override
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

    void Disconnect() override
    {
        if (sock_ != INVALID_SOCKET)
        {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    bool SendNal(const uint8_t *data, size_t size) override
    {
        if (sock_ == INVALID_SOCKET) return false;
        uint32_t net_size = htonl((uint32_t)size);
        int r = (int)send(sock_, (const char*)&net_size, 4, 0);
        if (r <= 0) return false;
        r = (int)send(sock_, (const char*)data, (int)size, 0);
        return r > 0;
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
};

// ---- H.264 NAL over UDP sender (fragmented) ----
class UdpPushSender : public IPushSender
{
public:
    ~UdpPushSender() override { Disconnect(); }

    bool Connect(const std::string &host, int port) override
    {
        Disconnect();
        ensure_winsock();

        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == INVALID_SOCKET) return false;

        dest_addr_.sin_family = AF_INET;
        dest_addr_.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, host.c_str(), &dest_addr_.sin_addr);
        return true;
    }

    void Disconnect() override
    {
        if (sock_ != INVALID_SOCKET)
        {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    bool SendNal(const uint8_t *data, size_t size) override
    {
        if (sock_ == INVALID_SOCKET) return false;

        const int frag_payload = MAX_PAYLOAD - HEADER_SIZE;
        uint16_t total_frags = (uint16_t)((size + frag_payload - 1) / frag_payload);
        if (total_frags == 0) total_frags = 1;

        uint8_t buf[MAX_PAYLOAD];

        for (uint16_t i = 0; i < total_frags; i++)
        {
            size_t off = i * frag_payload;
            size_t chunk = (std::min)(size - off, (size_t)frag_payload);

            // Header: seq, index, total
            uint32_t nbo_seq = htonl(seq_);
            uint16_t nbo_idx = htons(i);
            uint16_t nbo_tot = htons(total_frags);
            memcpy(buf, &nbo_seq, 4);
            memcpy(buf + 4, &nbo_idx, 2);
            memcpy(buf + 6, &nbo_tot, 2);
            memcpy(buf + HEADER_SIZE, data + off, chunk);

            int r = (int)sendto(sock_, (const char*)buf, HEADER_SIZE + (int)chunk, 0,
                                (sockaddr*)&dest_addr_, sizeof(dest_addr_));
            if (r < 0)
                return false;
        }
        seq_++;
        return true;
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_in dest_addr_{};
    uint32_t seq_ = 0;
    static const int MAX_PAYLOAD = 1400;
    static const int HEADER_SIZE = 8; // seq(4) + frag_index(2) + total_frags(2)
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
              << "  " << prog << "                         # RTSP server mode (local)\n"
              << "  " << prog << " push <vps_ip> [port]    # Push to VPS (TCP, default port 8554)\n"
              << "  " << prog << " push <vps_ip> [port] udp # Push to VPS (UDP)\n";
}

int main(int argc, char *argv[])
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    bool push_mode = false;

    if (argc > 1)
    {
        std::string mode = argv[1];
        if (mode == "push" && argc > 2)
        {
            push_mode = true;
            vps_host = argv[2];
            if (argc > 3)
            {
                std::string arg3 = argv[3];
                if (arg3 == "udp")
                {
                    use_udp = true;
                }
                else
                {
                    vps_port = std::atoi(argv[3]);
                    if (argc > 4 && std::string(argv[4]) == "udp")
                        use_udp = true;
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
    bool push_ready = false;
    bool rtsp_ready = false;
    std::unique_ptr<IPushSender> pusher;

    if (push_mode)
    {
        if (use_udp)
        {
            pusher = std::make_unique<UdpPushSender>();
            std::cout << "Push mode: UDP to " << vps_host << ":" << vps_port << std::endl;
        }
        else
        {
            pusher = std::make_unique<TcpPushSender>();
            std::cout << "Push mode: TCP to " << vps_host << ":" << vps_port << std::endl;
        }

        if (!pusher->Connect(vps_host, vps_port))
            std::cerr << "Push connect failed (will retry)" << std::endl;
        else
            push_ready = true;
    }
    else
    {
        std::cout << "RTSP server mode" << std::endl;
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
            if (!pusher->SendNal(nals.data(), nals.size()))
            {
                std::cout << "VPS disconnected, reconnecting..." << std::endl;
                pusher->Disconnect();
                std::this_thread::sleep_for(std::chrono::seconds(1));
                push_ready = pusher->Connect(vps_host, vps_port);
                if (push_ready)
                    pusher->SendNal(nals.data(), nals.size());
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
    if (pusher) pusher->Disconnect();
    encoder->Shutdown();
    capture->Shutdown();
    std::cout << "\nDone. " << frame_count << " frames" << std::endl;
    return 0;
}
