#pragma once
#include <cstdint>

#pragma pack(push, 1)
struct RtpHeader
{
    uint8_t  version : 2;    // = 2
    uint8_t  padding : 1;
    uint8_t  ext : 1;
    uint8_t  cc : 4;         // CSRC count
    uint8_t  marker : 1;
    uint8_t  payload_type : 7;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)

static_assert(sizeof(RtpHeader) == 12, "RtpHeader must be 12 bytes");

enum RtpPayloadType : uint8_t
{
    RTP_PT_H264 = 96,   // dynamic payload type for H.264
};
