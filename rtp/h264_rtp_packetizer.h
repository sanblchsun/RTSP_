#pragma once
#include <cstdint>
#include <vector>

// RFC 3984 - H.264 over RTP (namespaced to avoid conflict with x264.h)
struct H264NalType
{
    enum : uint8_t
    {
        kUnspecified  = 0,
        kSliceNonIdr  = 1,
        kSliceA       = 2,
        kSliceB       = 3,
        kSliceC       = 4,
        kSliceIdr     = 5,
        kSei          = 6,
        kSps          = 7,
        kPps          = 8,
        kAud          = 9,
        kFiller       = 12,
        kStapA        = 24,
        kStapB        = 25,
        kMtap16       = 26,
        kMtap24       = 27,
        kFuA          = 28,
        kFuB          = 29,
    };
};

class H264RtpPacketizer
{
public:
    H264RtpPacketizer();

    void SetSsrc(uint32_t ssrc);
    void SetPayloadType(uint8_t pt);

    void Packetize(const uint8_t *nal_data, size_t nal_size,
                   uint32_t timestamp, std::vector<std::vector<uint8_t>> &out_packets);

    void ResetSequence();
    void SetMaxPayloadSize(size_t bytes);

private:
    uint32_t ssrc_ = 0xDEADBEEF;
    uint8_t  payload_type_ = 96;
    uint16_t sequence_ = 0;
    size_t   max_payload_ = 1400;

    void WriteHeader(uint8_t *buf, bool marker, uint32_t timestamp);
    void PacketizeSingleNal(const uint8_t *nal, size_t size, uint32_t ts,
                            std::vector<std::vector<uint8_t>> &out);
    void PacketizeFuA(const uint8_t *nal, size_t size, uint32_t ts,
                      std::vector<std::vector<uint8_t>> &out);
};
