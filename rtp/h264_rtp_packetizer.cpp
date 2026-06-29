#include "h264_rtp_packetizer.h"
#include "rtp_header.h"
#include <cstring>
#include <algorithm>

#ifdef _MSC_VER
#include <intrin.h>
#define bswap16(x) _byteswap_ushort(x)
#define bswap32(x) _byteswap_ulong(x)
#else
#define bswap16(x) __builtin_bswap16(x)
#define bswap32(x) __builtin_bswap32(x)
#endif

H264RtpPacketizer::H264RtpPacketizer() = default;

void H264RtpPacketizer::SetSsrc(uint32_t ssrc) { ssrc_ = ssrc; }
void H264RtpPacketizer::SetPayloadType(uint8_t pt) { payload_type_ = pt; }
void H264RtpPacketizer::ResetSequence() { sequence_ = 0; }
void H264RtpPacketizer::SetMaxPayloadSize(size_t bytes) { max_payload_ = bytes; }

void H264RtpPacketizer::WriteHeader(uint8_t *buf, bool marker, uint32_t timestamp)
{
    auto *hdr = reinterpret_cast<RtpHeader *>(buf);
    hdr->version = 2;
    hdr->padding = 0;
    hdr->ext = 0;
    hdr->cc = 0;
    hdr->marker = marker ? 1 : 0;
    hdr->payload_type = payload_type_;
    hdr->sequence = bswap16(sequence_++);
    hdr->timestamp = bswap32(timestamp);
    hdr->ssrc = bswap32(ssrc_);
}

static inline uint8_t nal_type(const uint8_t *nal)
{
    return nal[0] & 0x1F;
}

static inline bool is_start_code(const uint8_t *data, size_t size, size_t pos)
{
    if (pos + 3 >= size) return false;
    if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1)
        return true;
    if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1)
        return true;
    return false;
}

void H264RtpPacketizer::Packetize(
    const uint8_t *nal_data, size_t nal_size,
    uint32_t timestamp, std::vector<std::vector<uint8_t>> &out_packets)
{
    out_packets.clear();

    size_t offset = 0;
    while (offset < nal_size)
    {
        size_t start_code_len = 0;
        if (offset + 3 < nal_size)
        {
            if (nal_data[offset] == 0 && nal_data[offset + 1] == 0 &&
                nal_data[offset + 2] == 0 && nal_data[offset + 3] == 1)
                start_code_len = 4;
            else if (nal_data[offset] == 0 && nal_data[offset + 1] == 0 &&
                     nal_data[offset + 2] == 1)
                start_code_len = 3;
        }
        offset += start_code_len;
        if (offset >= nal_size)
            break;

        size_t nal_start = offset;
        while (offset < nal_size && !is_start_code(nal_data, nal_size, offset))
            offset++;

        const uint8_t *nal = nal_data + nal_start;
        size_t nalu_size = offset - nal_start;
        if (nalu_size == 0)
            continue;

        if (nalu_size <= max_payload_)
            PacketizeSingleNal(nal, nalu_size, timestamp, out_packets);
        else
            PacketizeFuA(nal, nalu_size, timestamp, out_packets);
    }
}

void H264RtpPacketizer::PacketizeSingleNal(
    const uint8_t *nal, size_t size, uint32_t ts,
    std::vector<std::vector<uint8_t>> &out)
{
    std::vector<uint8_t> pkt(sizeof(RtpHeader) + size);
    WriteHeader(pkt.data(), true, ts);
    std::memcpy(pkt.data() + sizeof(RtpHeader), nal, size);
    out.push_back(std::move(pkt));
}

void H264RtpPacketizer::PacketizeFuA(
    const uint8_t *nal, size_t size, uint32_t ts,
    std::vector<std::vector<uint8_t>> &out)
{
    uint8_t nal_header = nal[0];
    uint8_t nal_type = nal_header & 0x1F;
    const uint8_t *payload = nal + 1;
    size_t payload_size = size - 1;

    size_t num_packets = (payload_size + max_payload_ - 1) / max_payload_;
    size_t offset = 0;

    for (size_t i = 0; i < num_packets; i++)
    {
        size_t chunk_size = std::min(max_payload_, payload_size - offset);
        bool first = (i == 0);
        bool last = (i == num_packets - 1);

        uint8_t fu_indicator = (nal_header & 0xE0) | H264NalType::kFuA;
        uint8_t fu_header = (first ? 0x80 : 0) | (last ? 0x40 : 0) | nal_type;

        std::vector<uint8_t> pkt(sizeof(RtpHeader) + 2 + chunk_size);
        WriteHeader(pkt.data(), last, ts);
        pkt[sizeof(RtpHeader) + 0] = fu_indicator;
        pkt[sizeof(RtpHeader) + 1] = fu_header;
        std::memcpy(pkt.data() + sizeof(RtpHeader) + 2, payload + offset, chunk_size);
        out.push_back(std::move(pkt));

        offset += chunk_size;
    }
}
