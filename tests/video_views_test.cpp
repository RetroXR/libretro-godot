// Standalone harness for VideoViews.hpp. Build and run with tests/run_tests.py.
//
// The map of RETRO_ENVIRONMENT_SET_VIDEO_VIEWS: which maps are refused, and
// when a frame is shown whole instead of split.

#include <cmath>
#include <cstdio>
#include <vector>

#include "VideoViews.hpp"

using Xenu::VideoView;
using Xenu::ValidateVideoViews;
using Xenu::VideoViewsFit;
using Xenu::VideoViewAspect;

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static VideoView View(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t screen, uint32_t eye = 0, float aspect = 0.0f)
{
    VideoView v;
    v.x = x; v.y = y; v.width = w; v.height = h; v.screen = screen; v.eye = eye; v.aspect = aspect;
    return v;
}

int main()
{
    std::printf("validate/\n");
    // Cemu's TV + GamePad map: 1280x720 beside 854x480.
    const std::vector<VideoView> wiiu = {View(0, 0, 1280, 720, 0, 0, 16.0f / 9.0f), View(1280, 0, 854, 480, 1, 0, 16.0f / 9.0f)};
    Check(ValidateVideoViews(wiiu), "TV + GamePad is a valid map");
    Check(ValidateVideoViews({}), "an empty map is valid (it clears)");
    Check(!ValidateVideoViews({View(0, 0, 0, 720, 0)}), "a zero width is refused");
    Check(!ValidateVideoViews({View(0, 0, 10, 10, 1)}), "a map with no screen 0 is refused (a gap)");
    Check(!ValidateVideoViews({View(0, 0, 10, 10, 0), View(0, 0, 10, 10, 2)}), "screens 0 and 2 with no 1 are refused");
    Check(!ValidateVideoViews({View(0, 0, 10, 10, 0), View(0, 0, 10, 10, 0)}), "two NONE views of one screen are refused");
    Check(ValidateVideoViews({View(0, 0, 10, 10, 0, 1), View(10, 0, 10, 10, 0, 2)}), "a LEFT/RIGHT pair is valid");
    Check(!ValidateVideoViews({View(0, 0, 10, 10, 0, 1), View(10, 0, 12, 10, 0, 2)}), "a pair of two sizes is refused");
    Check(!ValidateVideoViews({View(0, 0, 10, 10, 0, 1)}), "a LEFT with no RIGHT is refused");
    Check(!ValidateVideoViews({View(0, 0, 10, 10, 0, 3)}), "an unknown eye is refused");
    Check(!ValidateVideoViews({View(0, 0, 10, 10, 0, 0, 200.0f)}), "an aspect over 100 is refused");
    Check(ValidateVideoViews({View(0, 0, 10, 10, 0, 0, -1.0f)}), "a negative aspect means width/height");
    std::vector<VideoView> nine;
    for (uint32_t i = 0; i < 9; ++i)
        nine.push_back(View(i, 0, 1, 1, i));
    Check(!ValidateVideoViews(nine), "nine views are refused");

    std::printf("fit/\n");
    Check(VideoViewsFit(wiiu, 2134, 720), "the map fits its packed frame");
    Check(!VideoViewsFit(wiiu, 2133, 720), "one pixel short and the frame is shown whole");
    Check(!VideoViewsFit(wiiu, 1280, 720), "the old TV-only frame is shown whole");
    Check(!VideoViewsFit(wiiu, 0, 0), "no frame yet: nothing fits");

    std::printf("aspect/\n");
    Check(std::fabs(VideoViewAspect(View(0, 0, 854, 480, 0, 0, 0.0f)) - 854.0f / 480.0f) < 1e-5f, "no aspect: the pixel shape");
    Check(std::fabs(VideoViewAspect(View(0, 0, 100, 100, 0, 0, 1.5f)) - 1.5f) < 1e-6f, "an aspect given wins");

    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
