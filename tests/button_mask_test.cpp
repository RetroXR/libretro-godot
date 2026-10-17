// Standalone harness for how a held button is answered. Build and run with
// tests/run_tests.py.
//
// input_state returns an int16_t. Answering with the button's bit instead of
// 1 loses every button from id 16 up, and the light gun's RELOAD is id 16: no
// core was ever told a reload. Nothing else shows it either, because a press
// that reaches nobody looks the same as a game that ignores it.

#include <cstdio>

#include "ButtonMask.hpp"
#include "libretro.h"

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// ── T1: every light gun button reaches the core as held ─────────────────────
static void TestEveryLightgunButton()
{
    std::printf("T1 a held light gun button reads 1, whatever its id\n");

    const unsigned ids[] = {
        RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, RETRO_DEVICE_ID_LIGHTGUN_AUX_A,
        RETRO_DEVICE_ID_LIGHTGUN_AUX_B, RETRO_DEVICE_ID_LIGHTGUN_START,
        RETRO_DEVICE_ID_LIGHTGUN_SELECT, RETRO_DEVICE_ID_LIGHTGUN_AUX_C,
        RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP, RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN,
        RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT, RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT,
        RETRO_DEVICE_ID_LIGHTGUN_RELOAD,
    };
    bool all = true;
    for (unsigned id : ids)
    {
        if (ButtonFromMask(1u << id, id) != 1)
        {
            std::printf("    id %u held reads %d\n", id, ButtonFromMask(1u << id, id));
            all = false;
        }
    }
    Check(all, "each one held alone reads exactly 1");
    Check(ButtonFromMask(1u << RETRO_DEVICE_ID_LIGHTGUN_RELOAD, RETRO_DEVICE_ID_LIGHTGUN_RELOAD) == 1,
          "RELOAD (id 16) held reads 1 -- the bit itself, 65536, is 0 as an int16_t");
    Check(ButtonFromMask(1u << 15, 15) == 1, "id 15 held reads 1, not -32768");
    Check(ButtonFromMask(1u << RETRO_DEVICE_ID_JOYPAD_R3, RETRO_DEVICE_ID_JOYPAD_R3) == 1,
          "a joypad's R3 (bit 15 of the same answer) held reads 1");
}

// ── T2: nothing else reads as held ──────────────────────────────────────────
static void TestOnlyThatButton()
{
    std::printf("T2 a button reads 0 while only others are held\n");

    const uint32_t all_but_reload = ~(1u << RETRO_DEVICE_ID_LIGHTGUN_RELOAD);
    Check(ButtonFromMask(all_but_reload, RETRO_DEVICE_ID_LIGHTGUN_RELOAD) == 0,
          "every other button held, RELOAD reads 0");
    Check(ButtonFromMask(0u, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) == 0, "nothing held, TRIGGER reads 0");
    Check(ButtonFromMask(0xFFFFFFFFu, 32) == 0 && ButtonFromMask(0xFFFFFFFFu, 200) == 0,
          "an id past the mask is not held (and is no shift by 32 or more)");
}

int main()
{
    TestEveryLightgunButton();
    TestOnlyThatButton();
    if (g_failures)
    {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
