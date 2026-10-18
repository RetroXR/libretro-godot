#pragma once

// The shape of what a core draws, in the pure form the bridge and its tests share.
//
// Header-only and free of Godot, so tests/run_tests.py can compile it alone:
// VideoHandler.cpp reaches Godot and cannot be built there.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Xenu
{

/// What the main thread may ask about the picture without touching a frame.
///
/// `frame_*` are the pixels GDScript actually receives: AFTER the bridge has
/// rotated the frame, so a vertical game is portrait here. `base_*` and
/// `core_aspect` are the core's own words (retro_game_geometry), which are in
/// whatever orientation that core chose -- libretro does not say whether a
/// rotating core describes its shape before or after the rotation, so nothing
/// below depends on it (see DisplayAspect).
struct VideoGeometry
{
    uint32_t base_width = 0;
    uint32_t base_height = 0;
    /// retro_game_geometry::aspect_ratio as reported; <= 0 means "use base_*".
    float core_aspect = 0.0f;
    /// RETRO_ENVIRONMENT_SET_ROTATION, 0..3 quarter turns counter-clockwise.
    uint32_t rotation = 0;
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
};

/// The shape the picture is meant to be SEEN in, as width / height of the glass.
///
/// Arcade pixels are not square (CPS1 draws 384x224 for a 4:3 monitor), so the
/// frame's pixel ratio is the wrong answer and the core's aspect_ratio is the
/// right one. But a rotating core may report that aspect before or after its own
/// rotation, and the pixels the bridge hands over are already rotated. So the
/// MAGNITUDE comes from the core (4:3 or 3:4 is the same shape either way) and
/// the ORIENTATION from the frame we really produced: portrait pixels give a
/// portrait aspect, landscape pixels a landscape one. That is correct whichever
/// way the core reports, and it is what a vertical game on a landscape monitor
/// needs (pillarboxed, as on the real thing).
///
/// Before the first frame there is no orientation to read, so the core's value
/// is returned as it stands; 0 means nothing is known yet.
inline float DisplayAspect(const VideoGeometry& g)
{
    float aspect = g.core_aspect;
    if (!(aspect > 0.0f))
    {
        if (g.base_width > 0 && g.base_height > 0)
            aspect = static_cast<float>(g.base_width) / static_cast<float>(g.base_height);
        else if (g.frame_width > 0 && g.frame_height > 0)
            aspect = static_cast<float>(g.frame_width) / static_cast<float>(g.frame_height);
        else
            return 0.0f;
    }

    if (g.frame_width == 0 || g.frame_height == 0 || g.frame_width == g.frame_height)
        return aspect;

    const float wide = aspect >= 1.0f ? aspect : 1.0f / aspect;
    return g.frame_width > g.frame_height ? wide : 1.0f / wide;
}

/// Size of a frame after `rotation` quarter turns.
inline void RotatedSize(uint32_t width, uint32_t height, uint32_t rotation,
                        uint32_t* out_width, uint32_t* out_height)
{
    const bool odd = (rotation & 1u) != 0;
    *out_width = odd ? height : width;
    *out_height = odd ? width : height;
}

/// Rotate and/or flip a frame of 32-bit pixels, `rotation` quarter turns
/// counter-clockwise (libretro's convention), optionally flipping rows first
/// (a GL framebuffer is bottom-up). `dst` holds RotatedSize() pixels.
///
/// This is QueueFrame's transform lifted out unchanged, so the rotation and its
/// interaction with flip_y can be pinned by a test instead of by a romset.
inline void TransformFrame(const uint32_t* src, uint32_t width, uint32_t height,
                           uint32_t rotation, bool flip_y, uint32_t* dst)
{
    rotation &= 3u;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    RotatedSize(width, height, rotation, &output_width, &output_height);

    // Unrotated, the transform only reorders whole rows, so it is a row-order
    // reversal rather than a per-pixel walk.
    if (rotation == 0)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t sy = flip_y ? height - 1 - y : y;
            std::memcpy(dst + static_cast<size_t>(y) * width,
                        src + static_cast<size_t>(sy) * width,
                        static_cast<size_t>(width) * sizeof(uint32_t));
        }
        return;
    }

    // A rotation genuinely gathers. Per row one source coordinate is fixed and
    // only the other tracks x, so the switch stays outside the inner loop.
    for (uint32_t y = 0; y < output_height; ++y)
    {
        uint32_t* out_row = dst + static_cast<size_t>(y) * output_width;

        switch (rotation)
        {
        case 1: // 90 degrees counter-clockwise in libretro coordinates
        {
            const uint32_t sx = width - 1 - y;
            for (uint32_t x = 0; x < output_width; ++x)
            {
                const uint32_t sy = flip_y ? height - 1 - x : x;
                out_row[x] = src[static_cast<size_t>(sy) * width + sx];
            }
            break;
        }
        case 2:
        {
            const uint32_t ry = height - 1 - y;
            const uint32_t sy = flip_y ? height - 1 - ry : ry;
            const uint32_t* in_row = src + static_cast<size_t>(sy) * width;
            for (uint32_t x = 0; x < output_width; ++x)
                out_row[x] = in_row[width - 1 - x];
            break;
        }
        case 3:
        {
            const uint32_t sx = y;
            for (uint32_t x = 0; x < output_width; ++x)
            {
                const uint32_t ry = height - 1 - x;
                const uint32_t sy = flip_y ? height - 1 - ry : ry;
                out_row[x] = src[static_cast<size_t>(sy) * width + sx];
            }
            break;
        }
        default:
            break;
        }
    }
}

} // namespace Xenu
