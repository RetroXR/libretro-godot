#pragma once

// RETRO_ENVIRONMENT_SET_VIDEO_VIEWS (95): a core packs several pictures into one
// frame and says where each is. Cemu packs the TV and the Wii U GamePad.
//
// Header-only and free of Godot and libretro.h, so tests/run_tests.py can compile
// it alone. The environment handler copies retro_video_view into VideoView.

#include <cstdint>
#include <vector>

namespace Xenu
{

constexpr unsigned kVideoViewsMax = 8;   // RETRO_VIDEO_VIEWS_MAX
constexpr unsigned kViewEyeNone = 0;     // RETRO_VIDEO_VIEW_EYE_NONE
constexpr unsigned kViewEyeLeft = 1;
constexpr unsigned kViewEyeRight = 2;

struct VideoView
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t screen = 0;
    uint32_t eye = kViewEyeNone;
    float aspect = 0.0f;
};

/// The header's rules for a map: 0 to 8 views, non-zero sizes, a valid eye, an
/// aspect at or below zero or within 0.01..100, screens numbered from 0 with no
/// gaps, and each screen one NONE view or one LEFT plus one RIGHT of one size.
/// An empty map is valid (it clears).
inline bool ValidateVideoViews(const std::vector<VideoView>& views)
{
    if (views.size() > kVideoViewsMax)
        return false;

    unsigned none[kVideoViewsMax] = {};
    unsigned left[kVideoViewsMax] = {};
    unsigned right[kVideoViewsMax] = {};
    const VideoView* first_left[kVideoViewsMax] = {};
    const VideoView* first_right[kVideoViewsMax] = {};
    uint32_t screens = 0;

    for (const VideoView& v : views)
    {
        if (v.width == 0 || v.height == 0)
            return false;
        if (v.aspect > 0.0f && (v.aspect < 0.01f || v.aspect > 100.0f))
            return false;
        if (v.screen >= kVideoViewsMax)
            return false;
        switch (v.eye)
        {
        case kViewEyeNone: ++none[v.screen]; break;
        case kViewEyeLeft: ++left[v.screen]; first_left[v.screen] = &v; break;
        case kViewEyeRight: ++right[v.screen]; first_right[v.screen] = &v; break;
        default: return false;
        }
        if (v.screen + 1 > screens)
            screens = v.screen + 1;
    }

    for (uint32_t s = 0; s < screens; ++s)
    {
        const bool mono = none[s] == 1 && left[s] == 0 && right[s] == 0;
        const bool pair = none[s] == 0 && left[s] == 1 && right[s] == 1 &&
                          first_left[s]->width == first_right[s]->width &&
                          first_left[s]->height == first_right[s]->height;
        if (!mono && !pair)
            return false;   // a gap in the numbering lands here too
    }
    return true;
}

/// Whether every view fits inside a frame of this size. A frame some view does
/// not fit is shown as the packed frame (the map applies again once they fit).
inline bool VideoViewsFit(const std::vector<VideoView>& views, uint32_t frame_width, uint32_t frame_height)
{
    if (frame_width == 0 || frame_height == 0)
        return false;
    for (const VideoView& v : views)
    {
        if (static_cast<uint64_t>(v.x) + v.width > frame_width ||
            static_cast<uint64_t>(v.y) + v.height > frame_height)
            return false;
    }
    return true;
}

/// A view's display aspect: its own when given, else its pixel shape.
inline float VideoViewAspect(const VideoView& v)
{
    if (v.aspect > 0.0f)
        return v.aspect;
    return v.height ? static_cast<float>(v.width) / static_cast<float>(v.height) : 0.0f;
}

} // namespace Xenu
