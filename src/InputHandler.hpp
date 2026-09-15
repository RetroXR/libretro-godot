#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <bitset>
#include <mutex>

#include <libretro.h>

#include "SensorIndex.hpp"

namespace Xenu
{
class InputHandler
{
public:
    static void PollCallback();
    static int16_t StateCallback(uint32_t port, uint32_t device, uint32_t index, uint32_t id);
    
    struct RetroController
    {
        std::string name;
        uint32_t id;
    };

    // TODO: Handle this better when needed for UI
    struct RetroDevice
    {
        uint32_t port;
        uint32_t device;
        uint32_t index;
        uint32_t id;
        std::string description;
    };

    static constexpr uint32_t MAX_POINTERS = 4;

    struct PointerState
    {
        int16_t x = 0;
        int16_t y = 0;
        int16_t pressed = 0;
    };

    /// Sub-devices addressable per port: 0 is the controller, 1 is whatever is
    /// plugged into it. A Wii Remote with a Nunchuk is one player with two
    /// accelerometers, which is what this exists for.
    static constexpr uint32_t MAX_SENSOR_INDEX = 2;

    /// One sensor set. The defaults are the at-rest pose, so a sub-device
    /// nothing has ever written to reads as a device lying still on a table
    /// rather than as one in freefall.
    struct SensorState
    {
        float accel_x = 0.0f;
        float accel_y = 0.0f;
        float accel_z = 1.0f;   // g, and gravity is still felt at rest
        float gyro_x = 0.0f;
        float gyro_y = 0.0f;
        float gyro_z = 0.0f;    // radians/second, and a still device is not turning
        bool accel_enabled = false;
        bool gyro_enabled = false;
    };

    /// Input state owned by the frontend rather than the core. Core savestates
    /// do not contain this data, so rollback snapshots must preserve it too or
    /// a replay can poll keyboard/mouse/sensor state from the abandoned future.
    struct NetplayState
    {
        std::unordered_map<uint32_t, uint16_t> joypad_buttons;
        std::unordered_map<uint32_t, int16_t> mouse_x;
        std::unordered_map<uint32_t, int16_t> mouse_y;
        std::unordered_map<uint32_t, uint32_t> mouse_buttons;
        std::array<std::bitset<RETROK_LAST>, 4> key_state{};
        std::unordered_map<uint32_t, int16_t> lightgun_x;
        std::unordered_map<uint32_t, int16_t> lightgun_y;
        std::unordered_map<uint32_t, int16_t> lightgun_is_offscreen;
        std::unordered_map<uint32_t, uint32_t> lightgun_buttons;
        std::unordered_map<uint32_t, std::array<PointerState, MAX_POINTERS>> pointers;
        std::unordered_map<uint32_t, int16_t> analog_left_x;
        std::unordered_map<uint32_t, int16_t> analog_left_y;
        std::unordered_map<uint32_t, int16_t> analog_right_x;
        std::unordered_map<uint32_t, int16_t> analog_right_y;
        std::unordered_map<uint32_t, std::array<SensorState, MAX_SENSOR_INDEX>> sensors;
    };

    NetplayState CaptureNetplayState() const;
    void RestoreNetplayState(const NetplayState& state);

    void SetJoypadButtonStates(uint32_t port, uint16_t states);
    uint16_t GetJoypadButtonStates(uint32_t port);
    /// Bits ORed into every joypad read of `port`, for a button that belongs to a
    /// peripheral rather than to the controller writing the port's mask.
    void SetJoypadExtraButtons(uint32_t port, uint16_t buttons);
    void ClearJoypadExtraButtons();

    void SetMousePosition(uint32_t port, int16_t x, int16_t y);
    uint16_t GetMouseX(uint32_t port);
    uint16_t GetMouseY(uint32_t port);
    void SetMouseButtons(uint32_t port, uint32_t buttons);
    uint32_t GetMouseButtons(uint32_t port);

    // Keyboard poll state: one bit per RETROK_* keycode (the old uint32 mask
    // only covered keycodes < 32 - letters start at 97). Keyboard state is
    // effectively global; cores poll port 0.
    void SetKeyState(uint32_t port, uint32_t keycode, bool down);
    bool GetKeyState(uint32_t port, uint32_t keycode) const;
    /// RETROKMOD_* mask derived from the currently-held modifier keycodes.
    uint16_t GetKeyModifiers(uint32_t port) const;

    void SetLightgunPosition(uint32_t port, int16_t x, int16_t y);
    int16_t GetLightgunX(uint32_t port);
    int16_t GetLightgunY(uint32_t port);
    void SetLightgunIsOffscreen(uint32_t port, int16_t is_offscreen);
    int16_t GetLightgunIsOffscreen(uint32_t port);
    void SetLightgunButtons(uint32_t port, uint32_t buttons);
    uint32_t GetLightgunButtons(uint32_t port);

    // ── Pointer (multi-touch) ────────────────────────────────────────────────
    // libretro's pointer device carries several simultaneous points, selected by
    // the `index` argument of the state callback. Most cores read index 0 only,
    // which is what the unindexed setters below write; Dolphin's IR passthrough
    // reads all four, one per object the Wiimote camera can see.
    void SetPointerPosition(uint32_t port, int16_t x, int16_t y);
    int16_t GetPointerX(uint32_t port);
    int16_t GetPointerY(uint32_t port);
    void SetPointerPressed(uint32_t port, int16_t pressed);
    int16_t GetPointerPressed(uint32_t port);
    int16_t GetPointerCount(uint32_t port);

    void SetPointerIndexState(uint32_t port, uint32_t index, int16_t x, int16_t y, bool pressed);
    /// Drop every index above `count`. Cheaper and less error-prone than making
    /// callers clear the points they no longer have.
    void ClearPointersFrom(uint32_t port, uint32_t index);

    void SetAnalogLeft(uint32_t port, int16_t x, int16_t y);
    int16_t GetAnalogLeftX(uint32_t port);
    int16_t GetAnalogLeftY(uint32_t port);
    void SetAnalogRight(uint32_t port, int16_t x, int16_t y);
    int16_t GetAnalogRightX(uint32_t port);
    int16_t GetAnalogRightY(uint32_t port);

    bool SetKeyboardEventCallback(const retro_keyboard_callback* keyboard_callback);
    void CallKeyboardEventCallback(bool down, uint32_t keycode, uint32_t character, uint16_t keyModifiers);

    bool SetInputDescriptors(const retro_input_descriptor* input_descriptors);
    bool SetControllerInfo(const retro_controller_info* controller_info);
    bool GetRumbleInterface(retro_rumble_interface* rumble_interface);

    // ── Sensor interface (accelerometer + gyroscope) ─────────────────────────
    // The frontend feeds the motion of a held VR handheld as two sensors on the
    // device's OWN axes: an accelerometer in g (at-rest flat ≈ (0, 0, 1)) and a
    // gyroscope in radians/second. Cores enable each independently via
    // set_sensor_state and poll them with get_sensor_input every frame.
    //
    // The two are enabled separately because a core may want either alone.
    // Dolphin binds tilt and swing off the accelerometer but needs the gyro for
    // MotionPlus, and asks for them in two calls.
    //
    // `index` names the sub-device on that port: 0 is the controller, 1 is
    // whatever is plugged into it. See SensorIndex.hpp for the encoding a core
    // uses to ask for one.
    bool GetSensorInterface(retro_sensor_interface* sensor_interface);
    void SetSensorAccel(uint32_t port, float x, float y, float z, uint32_t index = 0);
    void SetSensorGyro(uint32_t port, float x, float y, float z, uint32_t index = 0);
    bool IsSensorEnabled(uint32_t port) const;
    bool IsGyroEnabled(uint32_t port) const;

    std::vector<std::vector<RetroController>> GetControllers() const;
    /// Track which device type is active on each port (defaults to RETRO_DEVICE_JOYPAD).
    void SetPortDevice(uint32_t port, uint32_t device);
    uint32_t GetPortDevice(uint32_t port) const;
    bool GetInputDeviceCapabilities(uint32_t* capabilities);
    bool GetInputBitmasks(bool* available);

private:
    // Main-thread device updates, emulation-thread polling, rollback restores,
    // and callbacks from core-created threads all share this state. Recursive
    // locking lets Process* helpers reuse the public getters safely.
    mutable std::recursive_mutex m_state_mutex;

    std::unordered_map<uint32_t, uint16_t> m_joypad_buttons;
    // Not in NetplayState: cleared when netplay starts, refused on its ports.
    std::unordered_map<uint32_t, uint16_t> m_joypad_extra;

    std::unordered_map<uint32_t, int16_t> m_mouse_x;
    std::unordered_map<uint32_t, int16_t> m_mouse_y;
    // What the CORE sees this frame, held still for the whole of it. Separate
    // from the accumulators above because a delta is consumed per emulated
    // frame, not per read -- see LatchMouseDeltas.
    std::unordered_map<uint32_t, int16_t> m_mouse_latched_x;
    std::unordered_map<uint32_t, int16_t> m_mouse_latched_y;
    int64_t m_mouse_latch_frame = -1;
    std::unordered_map<uint32_t, uint32_t> m_mouse_buttons;

    std::array<std::bitset<RETROK_LAST>, 4> m_key_state{};

    std::unordered_map<uint32_t, int16_t> m_lightgun_x;
    std::unordered_map<uint32_t, int16_t> m_lightgun_y;
    std::unordered_map<uint32_t, int16_t> m_lightgun_is_offscreen;
    std::unordered_map<uint32_t, uint32_t> m_lightgun_buttons;

    // One entry per touch index. RETRO_DEVICE_ID_POINTER_COUNT is derived from
    // the pressed flags rather than stored, so a caller can never leave a count
    // that disagrees with the points behind it.
    std::unordered_map<uint32_t, std::array<PointerState, MAX_POINTERS>> m_pointers;

    std::unordered_map<uint32_t, int16_t> m_analog_left_x;
    std::unordered_map<uint32_t, int16_t> m_analog_left_y;
    std::unordered_map<uint32_t, int16_t> m_analog_right_x;
    std::unordered_map<uint32_t, int16_t> m_analog_right_y;

    // Last rumble state per port. Used to dedup the set_rumble_state callback:
    // cores call it every frame even when values haven't changed, and we only
    // want to notify the Godot main thread on actual transitions.
    std::unordered_map<uint32_t, uint16_t> m_rumble_weak;
    std::unordered_map<uint32_t, uint16_t> m_rumble_strong;

    // Sensor state per port, one entry per addressable sub-device. Accelerometer
    // in g, gyroscope in radians/second. Written by the main thread (benign
    // race, same as the analog maps) and read by the emu-thread sensor callback.
    std::unordered_map<uint32_t, std::array<SensorState, MAX_SENSOR_INDEX>> m_sensors;

    std::vector<std::vector<RetroController>> m_controllers;
    std::unordered_map<uint32_t, uint32_t> m_port_devices;
    std::vector<RetroDevice> m_devices;
    retro_keyboard_event_t m_keyboard_event = nullptr;

    int16_t ProcessJoypadDevice(uint32_t port, uint32_t id);
    int16_t ProcessMouseDevice(uint32_t port, uint32_t id);

    /// Move the accumulated mouse deltas into the per-frame latch, once per
    /// emulated frame. Idempotent within a frame, so it is safe to call from
    /// both retro_input_poll and the state reads themselves.
    void LatchMouseDeltas(int64_t frame);
    int16_t ProcessKeyboardDevice(uint32_t port, uint32_t id);
    int16_t ProcessLightgunDevice(uint32_t port, uint32_t id);
    int16_t ProcessPointerDevice(uint32_t port, uint32_t index, uint32_t id);
    int16_t ProcessAnalogDevice(uint32_t port, uint32_t index, uint32_t id);

    static bool RumbleInterfaceSetRumbleState(uint32_t port, retro_rumble_effect effect, uint16_t strength);
    static bool SensorSetStateCallback(unsigned port, retro_sensor_action action, unsigned rate);
    static float SensorGetInputCallback(unsigned port, unsigned id);
};
}
