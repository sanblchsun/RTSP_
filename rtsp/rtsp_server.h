#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include <cstdint>

// RTSP server for H.264 streaming
// Supports RTP/AVP/TCP interleaved mode (works through NAT)
// and RTP/AVP/UDP unicast mode
class RtspServer
{
public:
    using LogCallback = std::function<void(const std::string &)>;
    using RtpDataCallback = std::function<void(uint8_t *data, size_t size)>;

    RtspServer();
    ~RtspServer();

    bool Start(uint16_t port = 8554);
    void Stop();

    // Set stream parameters (must be called before Start)
    void SetVideoParams(int width, int height,
                        const std::vector<uint8_t> &sps,
                        const std::vector<uint8_t> &pps,
                        int fps = 30);

    // Set callbacks
    void SetLogCallback(LogCallback cb);

    // Send RTP packet to connected client(s)
    // data must include RTP header (12 bytes) + payload
    void SendRtp(const uint8_t *data, size_t size);

    bool IsClientConnected() const { return client_connected_.load(); }

private:
    void AcceptLoop();
    void HandleClient();

    // RTSP request handling
    std::string HandleOptions(const std::string &req);
    std::string HandleDescribe(const std::string &req);
    std::string HandleSetup(const std::string &req);
    std::string HandlePlay(const std::string &req);
    std::string HandleTeardown(const std::string &req);

    // SDP generation
    std::string GenerateSdp() const;

    uint16_t port_ = 8554;
    int video_width_ = 1920;
    int video_height_ = 1080;
    int fps_ = 30;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;

    // TCP interleaved channels
    int rtp_channel_ = 0;
    int rtcp_channel_ = 1;

    // Server state
    int server_fd_ = -1;
    int client_fd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<bool> client_connected_{false};
    std::thread accept_thread_;
    std::thread client_thread_;
    std::string client_address_;

    // RTSP CSeq tracking
    int cseq_ = 0;

    LogCallback log_;

    void Log(const std::string &msg);
    void SendResponse(const std::string &response);
    void SendInterleavedRtp(const uint8_t *data, size_t size);
};
