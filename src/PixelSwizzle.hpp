#pragma once

#include <cstdint>

namespace Xenu
{

// 32bpp channel swizzles for frames read back from a hardware-rendered core.
//
// These carry TWO bodies for several conversions, and the split is measured,
// not stylistic. A channel swizzle can be written two ways:
//
//   byte-wise   dst[i*4+0] = src[i*4+2], ...
//   uint32      shifts and masks on one 32-bit word
//
// and no single choice is right on both targets:
//
//   * On aarch64 the byte-wise form IS a hardware idiom. ld4 de-interleaves 64
//     bytes into four registers - all B, all G, all R, all A - and st4 writes
//     them back interleaved, so the swizzle costs a register rename over 16
//     pixels. Clang emits exactly that. The uint32 form throws it away and does
//     real arithmetic instead.
//   * On x86 there is no ld4/st4. MSVC cannot vectorize the byte-wise form at
//     all (measured: 0 SIMD instructions) because of the strided single-byte
//     stores, while it auto-vectorizes the uint32 form unaided.
//
// Measured per frame, 1280x960, old byte-wise vs uint32:
//
//                        MSVC x86-64      Quest 3 aarch64
//   Vulkan BGRA->RGBA    3.9x faster      0.82x  (22% SLOWER)
//   A2B10G10R10          5.9x faster      0.89x  (12% slower)
//   0RGB1555             3.9x faster      0.50x  (2x SLOWER)
//
// So each target gets the form that suits it. Both bodies are proven
// equivalent by tests/pixel_swizzle_test.cpp, which runs on either.
//
// Byte order in the names is MEMORY order: a little-endian uint32 read of the
// bytes [B,G,R,A] is B | G<<8 | R<<16 | A<<24, which is where the shifts come
// from.
//
// Source and destination are the mapped staging buffer and a PackedByteArray,
// both 32bpp and at least 4-byte aligned, so the uint32 access is sound.

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
#define XENU_PIXEL_BYTEWISE 1
#else
#define XENU_PIXEL_BYTEWISE 0
#endif

// ---------------------------------------------------------------------------
// Windows-only (D3D11/D3D12 are #ifdef _WIN32), so these need no ARM body. The
// uint32 form measured 3.0x faster than byte-wise even on aarch64, so it would
// be the right choice there too.
// ---------------------------------------------------------------------------

/// BGRA8 -> RGBA8, alpha forced opaque.
inline void SwizzleBgraToRgbaOpaque(uint8_t* dst, const uint8_t* src, uint32_t pixels)
{
    const uint32_t* in  = reinterpret_cast<const uint32_t*>(src);
    uint32_t*       out = reinterpret_cast<uint32_t*>(dst);

    for (uint32_t x = 0; x < pixels; ++x)
    {
        const uint32_t c = in[x];
        out[x] = 0xff000000u
               | (c & 0x0000ff00u)
               | ((c >> 16) & 0xffu)
               | ((c & 0xffu) << 16);
    }
}

/// RGBA8 -> RGBA8, alpha forced opaque. A copy that rewrites one channel.
inline void CopyRgbaOpaque(uint8_t* dst, const uint8_t* src, uint32_t pixels)
{
    const uint32_t* in  = reinterpret_cast<const uint32_t*>(src);
    uint32_t*       out = reinterpret_cast<uint32_t*>(dst);

    for (uint32_t x = 0; x < pixels; ++x)
        out[x] = 0xff000000u | (in[x] & 0x00ffffffu);
}

// ---------------------------------------------------------------------------
// Compiled on every platform, so these carry both bodies.
// ---------------------------------------------------------------------------

/// BGRA8 -> RGBA8, alpha preserved.
inline void SwizzleBgraToRgba(uint8_t* dst, const uint8_t* src, uint32_t pixels)
{
#if XENU_PIXEL_BYTEWISE
    for (uint32_t i = 0; i < pixels; ++i)
    {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }
#else
    const uint32_t* in  = reinterpret_cast<const uint32_t*>(src);
    uint32_t*       out = reinterpret_cast<uint32_t*>(dst);

    for (uint32_t x = 0; x < pixels; ++x)
    {
        const uint32_t c = in[x];
        out[x] = (c & 0xff00ff00u)
               | ((c >> 16) & 0xffu)
               | ((c & 0xffu) << 16);
    }
#endif
}

/// SNORM 32bpp -> UNORM RGBA8. SNORM bytes are signed, so [-128,127] has to
/// shift to [0,255]. `bgra` additionally swaps R and B.
inline void SnormToRgba8(uint8_t* dst, const uint8_t* src, uint32_t pixels, bool bgra)
{
#if XENU_PIXEL_BYTEWISE
    const int8_t* s = reinterpret_cast<const int8_t*>(src);
    const uint32_t r = bgra ? 2u : 0u;
    const uint32_t b = bgra ? 0u : 2u;
    for (uint32_t i = 0; i < pixels; ++i)
    {
        dst[i * 4 + 0] = static_cast<uint8_t>(static_cast<int>(s[i * 4 + r]) + 128);
        dst[i * 4 + 1] = static_cast<uint8_t>(static_cast<int>(s[i * 4 + 1]) + 128);
        dst[i * 4 + 2] = static_cast<uint8_t>(static_cast<int>(s[i * 4 + b]) + 128);
        dst[i * 4 + 3] = static_cast<uint8_t>(static_cast<int>(s[i * 4 + 3]) + 128);
    }
#else
    // Adding 128 to a byte is the same as flipping its top bit, which is why
    // this is one XOR rather than four adds.
    const uint32_t* in  = reinterpret_cast<const uint32_t*>(src);
    uint32_t*       out = reinterpret_cast<uint32_t*>(dst);

    if (bgra)
    {
        for (uint32_t x = 0; x < pixels; ++x)
        {
            const uint32_t c = in[x] ^ 0x80808080u;
            out[x] = (c & 0xff00ff00u)
                   | ((c >> 16) & 0xffu)
                   | ((c & 0xffu) << 16);
        }
    }
    else
    {
        for (uint32_t x = 0; x < pixels; ++x)
            out[x] = in[x] ^ 0x80808080u;
    }
#endif
}

/// A2B10G10R10 packed -> RGBA8. R in the low 10 bits, then G, then B, with 2
/// bits of alpha at the top. Alpha is widened from those 2 bits rather than
/// forced opaque; *85 is exactly (a * 255) / 3 over the four possible values.
inline void A2b10g10r10ToRgba8(uint8_t* dst, const uint8_t* src, uint32_t pixels)
{
    const uint32_t* in = reinterpret_cast<const uint32_t*>(src);
#if XENU_PIXEL_BYTEWISE
    for (uint32_t i = 0; i < pixels; ++i)
    {
        const uint32_t p = in[i];
        dst[i * 4 + 0] = static_cast<uint8_t>(((p >>  0) & 0x3FFu) >> 2);
        dst[i * 4 + 1] = static_cast<uint8_t>(((p >> 10) & 0x3FFu) >> 2);
        dst[i * 4 + 2] = static_cast<uint8_t>(((p >> 20) & 0x3FFu) >> 2);
        dst[i * 4 + 3] = static_cast<uint8_t>(((p >> 30) & 0x3u) * 85u);
    }
#else
    uint32_t* out = reinterpret_cast<uint32_t*>(dst);
    for (uint32_t x = 0; x < pixels; ++x)
    {
        const uint32_t p = in[x];
        out[x] = (((p >>  2) & 0xffu))
               | (((p >> 12) & 0xffu) <<  8)
               | (((p >> 22) & 0xffu) << 16)
               | ((((p >> 30) & 0x3u) * 85u) << 24);
    }
#endif
}

/// A2R10G10B10 packed -> RGBA8. The same layout with red and blue the other way
/// round; the driver chooses between the two, not us.
inline void A2r10g10b10ToRgba8(uint8_t* dst, const uint8_t* src, uint32_t pixels)
{
    const uint32_t* in = reinterpret_cast<const uint32_t*>(src);
#if XENU_PIXEL_BYTEWISE
    for (uint32_t i = 0; i < pixels; ++i)
    {
        const uint32_t p = in[i];
        dst[i * 4 + 0] = static_cast<uint8_t>(((p >> 20) & 0x3FFu) >> 2);
        dst[i * 4 + 1] = static_cast<uint8_t>(((p >> 10) & 0x3FFu) >> 2);
        dst[i * 4 + 2] = static_cast<uint8_t>(((p >>  0) & 0x3FFu) >> 2);
        dst[i * 4 + 3] = static_cast<uint8_t>(((p >> 30) & 0x3u) * 85u);
    }
#else
    uint32_t* out = reinterpret_cast<uint32_t*>(dst);
    for (uint32_t x = 0; x < pixels; ++x)
    {
        const uint32_t p = in[x];
        out[x] = (((p >> 22) & 0xffu))
               | (((p >> 12) & 0xffu) <<  8)
               | (((p >>  2) & 0xffu) << 16)
               | ((((p >> 30) & 0x3u) * 85u) << 24);
    }
#endif
}

/// 0RGB1555 -> RGBA8, alpha forced opaque. Strides are in BYTES, so src_stride
/// takes the pitch libretro hands the video callback directly.
///
/// libretro-common has conv_0rgb1555_argb8888 with an SSE2 body, but no
/// _abgr8888 sibling, and Godot's FORMAT_RGBA8 wants R in the first byte, which
/// is why this is ours.
inline void Xrgb1555ToRgba8(uint8_t* dst, const void* src,
                            uint32_t width, uint32_t height,
                            size_t dst_stride, size_t src_stride)
{
    const uint8_t* in_row = static_cast<const uint8_t*>(src);

    for (uint32_t y = 0; y < height; ++y)
    {
        const uint16_t* in = reinterpret_cast<const uint16_t*>(in_row);
#if XENU_PIXEL_BYTEWISE
        uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint16_t p = in[x];
            const uint8_t r5 = static_cast<uint8_t>((p >> 10) & 0x1f);
            const uint8_t g5 = static_cast<uint8_t>((p >>  5) & 0x1f);
            const uint8_t b5 = static_cast<uint8_t>( p        & 0x1f);
            *out++ = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            *out++ = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
            *out++ = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
            *out++ = 0xff;
        }
#else
        uint32_t* out = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * dst_stride);
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t p = in[x];
            const uint32_t r = (p >> 10) & 0x1fu;
            const uint32_t g = (p >>  5) & 0x1fu;
            const uint32_t b =  p        & 0x1fu;

            // 5 bits widened to 8 by replicating the high bits, so 0x1f -> 0xff.
            out[x] = 0xff000000u
                   | (((b << 3) | (b >> 2)) << 16)
                   | (((g << 3) | (g >> 2)) <<  8)
                   |  ((r << 3) | (r >> 2));
        }
#endif
        in_row += src_stride;
    }
}

/// XRGB8888 -> RGBA8, alpha forced opaque. Strides are in BYTES, so src_stride
/// takes the pitch libretro hands the video callback directly.
///
/// The X byte is padding a core never fills, and libretro-common's
/// conv_argb8888_abgr8888 copies it through as alpha -- so a frame that looks
/// right through a shader sampling .rgb is fully transparent to anything that
/// blends, which is every 2D draw and every saved PNG.
inline void Xrgb8888ToRgba8(uint8_t* dst, const void* src,
                            uint32_t width, uint32_t height,
                            size_t dst_stride, size_t src_stride)
{
    const uint8_t* in_row = static_cast<const uint8_t*>(src);

    for (uint32_t y = 0; y < height; ++y)
    {
#if XENU_PIXEL_BYTEWISE
        uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t* in = in_row + static_cast<size_t>(x) * 4;
            *out++ = in[2];
            *out++ = in[1];
            *out++ = in[0];
            *out++ = 0xff;
        }
#else
        const uint32_t* in = reinterpret_cast<const uint32_t*>(in_row);
        uint32_t* out = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * dst_stride);
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t p = in[x];
            out[x] = 0xff000000u
                   | ((p & 0xffu) << 16)
                   | (p & 0x0000ff00u)
                   | ((p >> 16) & 0xffu);
        }
#endif
        in_row += src_stride;
    }
}

} // namespace Xenu
