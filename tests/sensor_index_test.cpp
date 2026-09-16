// Standalone harness for the sensor sub-device index. Build and run with
// tests/run_tests.py.
//
// Needs nothing from src/ but the header itself: the encoding is the whole
// unit, and it has to be right in a way no running core will tell you about.
// A core built against this asks a frontend that has never heard of it for
// sub-device 1, and the ONLY thing standing between that and a Wii Remote
// reading its Nunchuk's acceleration as its own is that index 0 still encodes
// to exactly the values everybody already uses.
//
// SensorIndex.hpp asserts that identity at compile time as well. These are the
// runtime half: that a decode inverts an encode, and that an index this build
// does not carry is refused rather than folded into one it does.

#include <cstdio>

#include "SensorIndex.hpp"

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// How many sub-devices a frontend carries is the frontend's business, not the
// encoding's. Two is what InputHandler carries; the cases use it as a stand-in
// for "some limit", and one of them checks that the limit is what decides.
static const unsigned CARRIED = 2;

// ── T1: index 0 is today, unchanged ─────────────────────────────────────────
static void TestIndexZeroIsIdentity()
{
    std::printf("T1 sub-device 0 encodes to the values already in use\n");

    Check(RETRO_SENSOR_SUBDEVICE(0, RETRO_SENSOR_ACCELEROMETER_X) == 0u, "accelerometer X stays 0");
    Check(RETRO_SENSOR_SUBDEVICE(0, RETRO_SENSOR_ACCELEROMETER_Y) == 1u, "accelerometer Y stays 1");
    Check(RETRO_SENSOR_SUBDEVICE(0, RETRO_SENSOR_ACCELEROMETER_Z) == 2u, "accelerometer Z stays 2");
    Check(RETRO_SENSOR_SUBDEVICE(0, RETRO_SENSOR_GYROSCOPE_X) == 3u, "gyroscope X stays 3");
    Check(RETRO_SENSOR_SUBDEVICE(0, RETRO_SENSOR_GYROSCOPE_Y) == 4u, "gyroscope Y stays 4");
    Check(RETRO_SENSOR_SUBDEVICE(0, RETRO_SENSOR_GYROSCOPE_Z) == 5u, "gyroscope Z stays 5");
    Check(RETRO_SENSOR_SUBDEVICE(0, RETRO_SENSOR_ILLUMINANCE) == 6u, "illuminance stays 6");

    // The action enum too, since the enable call carries the index there.
    Check(RETRO_SENSOR_SUBDEVICE(0, RETRO_SENSOR_ACCELEROMETER_ENABLE) == 0u, "accel enable stays 0");
    Check(RETRO_SENSOR_SUBDEVICE(0, RETRO_SENSOR_ILLUMINANCE_DISABLE) == 5u, "illuminance disable stays 5");
}

// ── T2: a plain value from a core that has never heard of this ──────────────
static void TestPlainValueReadsAsTheController()
{
    std::printf("T2 an unencoded id is the controller's own sensor\n");

    unsigned index = 99;
    unsigned base = 99;
    Check(RetroSensorSplit(RETRO_SENSOR_ACCELEROMETER_Z, CARRIED, index, base),
          "a plain id is accepted");
    Check(index == 0u, "and names sub-device 0");
    Check(base == (unsigned)RETRO_SENSOR_ACCELEROMETER_Z, "with the id it arrived as");

    Check(RetroSensorSplit(RETRO_SENSOR_GYROSCOPE_ENABLE, CARRIED, index, base),
          "a plain action is accepted");
    Check(index == 0u, "and names sub-device 0 too");
    Check(base == (unsigned)RETRO_SENSOR_GYROSCOPE_ENABLE, "with the action it arrived as");
}

// ── T3: sub-device 1 round-trips ────────────────────────────────────────────
static void TestSubDeviceRoundTrips()
{
    std::printf("T3 sub-device 1 encodes and decodes back\n");

    Check(RETRO_SENSOR_SUBDEVICE(1, RETRO_SENSOR_ACCELEROMETER_X) == 256u, "accelerometer X lands on 256");
    Check(RETRO_SENSOR_SUBDEVICE(1, RETRO_SENSOR_ILLUMINANCE) == 262u, "illuminance lands on 262");

    for (unsigned id = RETRO_SENSOR_ACCELEROMETER_X; id <= RETRO_SENSOR_ILLUMINANCE; ++id)
    {
        unsigned index = 99;
        unsigned base = 99;
        const bool ok = RetroSensorSplit(RETRO_SENSOR_SUBDEVICE(1, id), CARRIED, index, base);
        if (!(ok && index == 1u && base == id))
        {
            Check(false, "every id round-trips through sub-device 1");
            return;
        }
    }
    Check(true, "every id round-trips through sub-device 1");
}

// ── T4: an index this build does not carry is refused ───────────────────────
static void TestUncarriedIndexIsRefused()
{
    std::printf("T4 an index beyond what this build carries is refused\n");

    unsigned index = 99;
    unsigned base = 99;
    // A false return is the answer a core is meant to get: it means "no such
    // sensor here", which is exactly what an unaware frontend would say by
    // never having implemented any of this.
    Check(!RetroSensorSplit(RETRO_SENSOR_SUBDEVICE(2, RETRO_SENSOR_ACCELEROMETER_X), CARRIED, index, base),
          "sub-device 2 is refused when only 2 are carried");
    Check(!RetroSensorSplit(RETRO_SENSOR_SUBDEVICE(7, RETRO_SENSOR_ACCELEROMETER_X), CARRIED, index, base),
          "and so is a far-fetched one");

    // Decoded anyway, so a caller that logs the refusal can say what was asked
    // for rather than reporting whatever the variables held before.
    Check(index == 7u, "the index it asked for is still reported");
    Check(base == (unsigned)RETRO_SENSOR_ACCELEROMETER_X, "and so is the id underneath");

    // The limit is what decides, not the number 1: a frontend carrying only the
    // controller itself refuses the very index this one accepts.
    Check(!RetroSensorSplit(RETRO_SENSOR_SUBDEVICE(1, RETRO_SENSOR_ACCELEROMETER_X), 1u, index, base),
          "carrying only the controller refuses sub-device 1");
}

// ── T5: the two halves cannot collide ───────────────────────────────────────
static void TestNoCollision()
{
    std::printf("T5 a sub-device cannot be mistaken for a plain id\n");

    // The whole scheme rests on every real value fitting under the shift. If an
    // id ever grew past 255, sub-device 1's accelerometer would arrive looking
    // like somebody's plain id and be answered with the wrong device's motion.
    Check(RETRO_SENSOR_ILLUMINANCE < (1 << RETRO_SENSOR_INDEX_SHIFT),
          "the largest id fits under the shift");
    Check(RETRO_SENSOR_ILLUMINANCE_DISABLE < (1 << RETRO_SENSOR_INDEX_SHIFT),
          "the largest action fits under the shift");
    Check(RETRO_SENSOR_SUBDEVICE(1, RETRO_SENSOR_ACCELEROMETER_X) > RETRO_SENSOR_ILLUMINANCE,
          "so sub-device 1 starts past every plain value");
}

int main()
{
    // Unbuffered, so a crash cannot take the lines already printed with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestIndexZeroIsIdentity();
    TestPlainValueReadsAsTheController();
    TestSubDeviceRoundTrips();
    TestUncarriedIndexIsRefused();
    TestNoCollision();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
