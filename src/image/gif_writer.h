#pragma once

#include <windows.h>
#include <objidl.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/output_size.h"

namespace srl {

// Streaming, opaque animated GIF encoder. COM must be initialized by the caller.
// The caller supplies a newly reserved empty stream and owns the partial file.
// The writer retains the stream until Finish/Close; it never renames or deletes it.
class GifWriter {
public:
    GifWriter();
    ~GifWriter();
    GifWriter(const GifWriter&) = delete;
    GifWriter& operator=(const GifWriter&) = delete;

    bool Open(IStream* stream, Size sourceSize, const RECT& sourceRect,
              const OutputSize& output, std::wstring* error);
    bool WriteFrame(const std::vector<std::uint8_t>& bgra, int width, int height,
                    std::int64_t timestamp100ns, std::wstring* error);
    bool Finish(std::int64_t endTimestamp100ns, std::wstring* error);
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace srl
