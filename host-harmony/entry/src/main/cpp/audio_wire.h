#ifndef HRD_AUDIO_WIRE_H
#define HRD_AUDIO_WIRE_H
#include <cstddef>
#include <cstdint>
#include <array>
namespace audio_wire {
constexpr uint32_t RATE = 48000;
constexpr size_t FRAME_BYTES = 4, CHUNK_BYTES = 3840, QUEUE_BYTES = CHUNK_BYTES * 6, MAX_CALLBACK_BYTES = RATE * FRAME_BYTES;
constexpr size_t HEADER_BYTES = 40;
inline void Put(uint8_t* out, uint64_t n, size_t count) { for (size_t i = count; i > 0; --i) { out[i - 1] = uint8_t(n); n >>= 8; } }
inline std::array<uint8_t, HEADER_BYTES> Header(uint8_t kind, size_t size, uint64_t pts, uint64_t sequence, uint64_t stream)
{
    std::array<uint8_t, HEADER_BYTES> h {};
    h[0] = 'H'; h[1] = 'R'; h[2] = 'D'; h[3] = 'A'; h[4] = 1; h[5] = kind; h[6] = 2; h[7] = 1;
    Put(h.data() + 8, size, 4); Put(h.data() + 12, RATE, 4); Put(h.data() + 16, pts, 8);
    Put(h.data() + 24, sequence, 8); Put(h.data() + 32, stream, 8); return h;
}
}
#endif
