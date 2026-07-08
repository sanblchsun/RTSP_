#pragma once
#include <vector>
#include <cstdint>
#include <string>

class IEncoder
{
public:
    virtual ~IEncoder() = default;

    virtual bool Initialize(int width, int height, int fps, int qp) = 0;
    virtual bool EncodeFrame(const std::vector<uint8_t> &bgra,
                             std::vector<uint8_t> &out_nal) = 0;
    virtual void Flush(std::vector<uint8_t> &out_nal) = 0;
    virtual void Shutdown() = 0;

    virtual int GetWidth() const = 0;
    virtual int GetHeight() const = 0;
    virtual bool IsInitialized() const = 0;
    virtual std::string GetName() const = 0;
};
