#include "InputHandler.hpp"

#include "Wrapper.hpp"
#include "Libretro.hpp"
#include "Debug.hpp"

namespace Xenu
{
InputHandler::NetplayState InputHandler::CaptureNetplayState() const
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    NetplayState state;
    state.joypad_buttons = m_joypad_buttons;
    state.mouse_x = m_mouse_x;
    state.mouse_y = m_mouse_y;
    state.mouse_buttons = m_mouse_buttons;
    state.key_state = m_key_state;
    state.lightgun_x = m_lightgun_x;
    state.lightgun_y = m_lightgun_y;
    state.lightgun_is_offscreen = m_lightgun_is_offscreen;
    state.lightgun_buttons = m_lightgun_buttons;
    state.pointers = m_pointers;
    state.analog_left_x = m_analog_left_x;
    state.analog_left_y = m_analog_left_y;
    state.analog_right_x = m_analog_right_x;
    state.analog_right_y = m_analog_right_y;
    state.sensors = m_sensors;
    return state;
}

void InputHandler::RestoreNetplayState(const NetplayState& state)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_joypad_buttons = state.joypad_buttons;
    m_mouse_x = state.mouse_x;
    m_mouse_y = state.mouse_y;
    m_mouse_buttons = state.mouse_buttons;
    m_key_state = state.key_state;
    m_lightgun_x = state.lightgun_x;
    m_lightgun_y = state.lightgun_y;
    m_lightgun_is_offscreen = state.lightgun_is_offscreen;
    m_lightgun_buttons = state.lightgun_buttons;
    m_pointers = state.pointers;
    m_analog_left_x = state.analog_left_x;
    m_analog_left_y = state.analog_left_y;
    m_analog_right_x = state.analog_right_x;
    m_analog_right_y = state.analog_right_y;
    m_sensors = state.sensors;
}

void InputHandler::PollCallback()
{
    // The frame boundary for mouse deltas. libretro says a delta accumulates
    // until retro_input_poll and then holds still for every read of that frame.
    auto instance = Wrapper::GetCurrentThreadWrapper();
    if (instance && instance->m_input_handler)
        instance->m_input_handler->LatchMouseDeltas(instance->GetFrameCount());
}


void InputHandler::LatchMouseDeltas(int64_t frame)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    if (frame == m_mouse_latch_frame)
        return;
    m_mouse_latch_frame = frame;
    for (auto& [port, dx] : m_mouse_x)
    {
        m_mouse_latched_x[port] = dx;
        dx = 0;
    }
    for (auto& [port, dy] : m_mouse_y)
    {
        m_mouse_latched_y[port] = dy;
        dy = 0;
    }
}

int16_t InputHandler::StateCallback(uint32_t port, uint32_t device, uint32_t index, uint32_t id)
{
    auto instance = Wrapper::GetCurrentThreadWrapper();
    if (!instance)
    {
        LogError("Libretro instance is null.");
        return 0;
    }

    device &= RETRO_DEVICE_MASK;

    switch (device)
    {
    case RETRO_DEVICE_JOYPAD:
        return instance->m_input_handler->ProcessJoypadDevice(port, id);
    case RETRO_DEVICE_MOUSE:
        return instance->m_input_handler->ProcessMouseDevice(port, id);
    case RETRO_DEVICE_KEYBOARD:
        return instance->m_input_handler->ProcessKeyboardDevice(port, id);
    case RETRO_DEVICE_LIGHTGUN:
        return instance->m_input_handler->ProcessLightgunDevice(port, id);
    case RETRO_DEVICE_POINTER:
        return instance->m_input_handler->ProcessPointerDevice(port, index, id);
    case RETRO_DEVICE_ANALOG:
        return instance->m_input_handler->ProcessAnalogDevice(port, index, id);
    case RETRO_DEVICE_NONE:
        // A port with nothing in it, which is an ordinary state and not a
        // fault. It arrives here constantly: a core polls every port it has
        // every frame whether or not anything is plugged into it, and a
        // GameCube lead announces (7 << 8) | RETRO_DEVICE_NONE, so its base
        // type IS none. Logged, this was 45 lines a second out of the
        // EMULATION thread, each one a string build and an android log write,
        // and it was audible: the sound crackled for as long as the port was
        // polled. Silence is the correct answer, and zero is the correct value.
        return 0;
    default:
        LogErrorOnce("Unhandled input device: " + std::to_string(device) + " for port: " + std::to_string(port) + " and id: " + std::to_string(id));
        break;
    }

    return 0;
}

bool InputHandler::RumbleInterfaceSetRumbleState(uint32_t port, retro_rumble_effect effect, uint16_t strength)
{
    // Runs on the emulation thread (inside retro_run). Dedup against the last
    // known state per port so cores that spam set_rumble_state every frame
    // don't flood the main thread with redundant signal emissions.
    auto instance = Wrapper::GetCurrentThreadWrapper();
    if (!instance || !instance->m_input_handler)
        return false;

    InputHandler* self = instance->m_input_handler.get();

    bool changed = false;
    uint16_t weak = 0;
    uint16_t strong = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(self->m_state_mutex);
        if (effect == RETRO_RUMBLE_STRONG)
        {
            auto it = self->m_rumble_strong.find(port);
            if (it == self->m_rumble_strong.end() || it->second != strength)
            {
                self->m_rumble_strong[port] = strength;
                changed = true;
            }
        }
        else if (effect == RETRO_RUMBLE_WEAK)
        {
            auto it = self->m_rumble_weak.find(port);
            if (it == self->m_rumble_weak.end() || it->second != strength)
            {
                self->m_rumble_weak[port] = strength;
                changed = true;
            }
        }
        else
        {
            return false;
        }

        weak   = self->m_rumble_weak.count(port)   ? self->m_rumble_weak[port]   : 0;
        strong = self->m_rumble_strong.count(port) ? self->m_rumble_strong[port] : 0;
    }

    Libretro* node = instance->LiveLibretroNode();
    if (!changed || node == nullptr)
        return true;

    // Always forward the combined state (both weak and strong) so GDScript
    // sees a single coherent view per port regardless of which effect changed.
    node->call_deferred(
        "emit_signal",
        "rumble_state_changed",
        static_cast<int>(port),
        static_cast<float>(weak)   / 65535.0f,
        static_cast<float>(strong) / 65535.0f);

    return true;
}

void InputHandler::SetJoypadButtonStates(uint32_t port, uint16_t states)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_joypad_buttons[port] = states;
}
uint16_t InputHandler::GetJoypadButtonStates(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_joypad_buttons[port];
}

void InputHandler::SetJoypadExtraButtons(uint32_t port, uint16_t buttons)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_joypad_extra[port] = buttons;
}

void InputHandler::ClearJoypadExtraButtons()
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_joypad_extra.clear();
}

void InputHandler::SetMousePosition(uint32_t port, int16_t x, int16_t y)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_mouse_x[port] += x;
    m_mouse_y[port] += y;
}
void InputHandler::SetMouseButtons(uint32_t port, uint32_t buttons)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_mouse_buttons[port] = buttons;
}
uint16_t InputHandler::GetMouseX(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_mouse_x[port];
}
uint16_t InputHandler::GetMouseY(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_mouse_y[port];
}
uint32_t InputHandler::GetMouseButtons(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_mouse_buttons[port];
}

void InputHandler::SetKeyState(uint32_t port, uint32_t keycode, bool down)
{
    if (keycode >= RETROK_LAST)
        return;
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_key_state[port & 3].set(keycode, down);
}

bool InputHandler::GetKeyState(uint32_t port, uint32_t keycode) const
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return keycode < RETROK_LAST && m_key_state[port & 3].test(keycode);
}

uint16_t InputHandler::GetKeyModifiers(uint32_t port) const
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    const auto& ks = m_key_state[port & 3];
    uint16_t mods = 0;
    if (ks.test(RETROK_LSHIFT) || ks.test(RETROK_RSHIFT)) mods |= RETROKMOD_SHIFT;
    if (ks.test(RETROK_LCTRL)  || ks.test(RETROK_RCTRL))  mods |= RETROKMOD_CTRL;
    if (ks.test(RETROK_LALT)   || ks.test(RETROK_RALT))   mods |= RETROKMOD_ALT;
    if (ks.test(RETROK_LSUPER) || ks.test(RETROK_RSUPER)) mods |= RETROKMOD_META;
    if (ks.test(RETROK_CAPSLOCK)) mods |= RETROKMOD_CAPSLOCK;
    if (ks.test(RETROK_NUMLOCK))  mods |= RETROKMOD_NUMLOCK;
    return mods;
}

void InputHandler::SetLightgunPosition(uint32_t port, int16_t x, int16_t y)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_lightgun_x[port] = x;
    m_lightgun_y[port] = y;
}
void InputHandler::SetLightgunIsOffscreen(uint32_t port, int16_t is_offscreen)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_lightgun_is_offscreen[port] = is_offscreen;
}
void InputHandler::SetLightgunButtons(uint32_t port, uint32_t buttons)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_lightgun_buttons[port] = buttons;
}
int16_t InputHandler::GetLightgunX(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_lightgun_x[port];
}
int16_t InputHandler::GetLightgunY(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_lightgun_y[port];
}
int16_t InputHandler::GetLightgunIsOffscreen(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_lightgun_is_offscreen[port];
}
uint32_t InputHandler::GetLightgunButtons(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_lightgun_buttons[port];
}

void InputHandler::SetPointerPosition(uint32_t port, int16_t x, int16_t y)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    auto& p = m_pointers[port][0];
    p.x = x;
    p.y = y;
}
void InputHandler::SetPointerPressed(uint32_t port, int16_t pressed)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_pointers[port][0].pressed = pressed;
}
void InputHandler::SetPointerIndexState(uint32_t port, uint32_t index, int16_t x, int16_t y, bool pressed)
{
    if (index >= MAX_POINTERS)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    auto& p = m_pointers[port][index];
    p.x = x;
    p.y = y;
    p.pressed = pressed ? 1 : 0;
}
void InputHandler::ClearPointersFrom(uint32_t port, uint32_t index)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    auto it = m_pointers.find(port);
    if (it == m_pointers.end())
        return;

    for (uint32_t i = index; i < MAX_POINTERS; ++i)
        it->second[i] = {};
}
int16_t InputHandler::GetPointerX(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_pointers[port][0].x;
}
int16_t InputHandler::GetPointerY(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_pointers[port][0].y;
}
int16_t InputHandler::GetPointerPressed(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_pointers[port][0].pressed;
}
int16_t InputHandler::GetPointerCount(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    int16_t count = 0;
    for (const auto& p : m_pointers[port])
        count += (p.pressed != 0) ? 1 : 0;
    return count;
}

void InputHandler::SetAnalogLeft(uint32_t port, int16_t x, int16_t y)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_analog_left_x[port] = x;
    m_analog_left_y[port] = y;
}
void InputHandler::SetAnalogRight(uint32_t port, int16_t x, int16_t y)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_analog_right_x[port] = x;
    m_analog_right_y[port] = y;
}
int16_t InputHandler::GetAnalogLeftX(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_analog_left_x[port];
}
int16_t InputHandler::GetAnalogLeftY(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_analog_left_y[port];
}
int16_t InputHandler::GetAnalogRightX(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_analog_right_x[port];
}
int16_t InputHandler::GetAnalogRightY(uint32_t port)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_analog_right_y[port];
}

void InputHandler::CallKeyboardEventCallback(bool down, uint32_t keycode, uint32_t character, uint16_t keyModifiers)
{
    retro_keyboard_event_t callback = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
        callback = m_keyboard_event;
    }
    if (callback)
        callback(down, keycode, character, keyModifiers);
}

bool InputHandler::SetInputDescriptors(const retro_input_descriptor* input_descriptors)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_devices.clear();

    if (!input_descriptors)
    {
        return false;
    }

    for (int i = 0; input_descriptors[i].description; ++i)
    {
        InputHandler::RetroDevice device;
        device.port        = input_descriptors[i].port;
        device.device      = input_descriptors[i].device;
        device.index       = input_descriptors[i].index;
        device.id          = input_descriptors[i].id;
        device.description = input_descriptors[i].description;
        m_devices.emplace_back(std::move(device));
    }

    return true;
}

bool InputHandler::SetControllerInfo(const retro_controller_info* controller_info)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_controllers.clear();

    if (!controller_info)
        return true;

    for (int i = 0; controller_info[i].types; ++i)
    {
        std::vector<InputHandler::RetroController> controllers;
        for (int j = 0; j < controller_info[i].num_types; ++j)
        {
            // Looks like some cores don't set num_types to the correct value...
            if (!controller_info[i].types[j].desc) 
                continue;
            
            InputHandler::RetroController controller;
            controller.name = controller_info[i].types[j].desc;
            controller.id   = controller_info[i].types[j].id;
            controllers.emplace_back(std::move(controller));
        }
        m_controllers.emplace_back(std::move(controllers));
    }

    return true;
}

bool InputHandler::GetRumbleInterface(retro_rumble_interface* rumble_interface)
{
    if (!rumble_interface)
        return true;

    rumble_interface->set_rumble_state = RumbleInterfaceSetRumbleState;
    return true;
}

// ── Sensor interface (accelerometer + gyroscope) ─────────────────────────────

bool InputHandler::GetSensorInterface(retro_sensor_interface* sensor_interface)
{
    if (!sensor_interface)
        return true;

    sensor_interface->set_sensor_state = SensorSetStateCallback;
    sensor_interface->get_sensor_input = SensorGetInputCallback;
    return true;
}

void InputHandler::SetSensorAccel(uint32_t port, float x, float y, float z, uint32_t index)
{
    if (index >= MAX_SENSOR_INDEX)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    SensorState& state = m_sensors[port][index];
    state.accel_x = x;
    state.accel_y = y;
    state.accel_z = z;
}

/// Angular velocity in radians/second about the device's own axes.
void InputHandler::SetSensorGyro(uint32_t port, float x, float y, float z, uint32_t index)
{
    if (index >= MAX_SENSOR_INDEX)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    SensorState& state = m_sensors[port][index];
    state.gyro_x = x;
    state.gyro_y = y;
    state.gyro_z = z;
}

bool InputHandler::IsSensorEnabled(uint32_t port) const
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    auto it = m_sensors.find(port);
    return it != m_sensors.end() && it->second[0].accel_enabled;
}

bool InputHandler::IsGyroEnabled(uint32_t port) const
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    auto it = m_sensors.find(port);
    return it != m_sensors.end() && it->second[0].gyro_enabled;
}

std::vector<std::vector<InputHandler::RetroController>> InputHandler::GetControllers() const
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_controllers;
}

void InputHandler::SetPortDevice(uint32_t port, uint32_t device)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_port_devices[port] = device;
}

uint32_t InputHandler::GetPortDevice(uint32_t port) const
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    auto it = m_port_devices.find(port);
    return it != m_port_devices.end() ? it->second : RETRO_DEVICE_JOYPAD;
}

/// Emu thread (called by the core). Accelerometer and gyroscope are tracked
/// independently; illuminance reports unsupported so cores fall back gracefully.
bool InputHandler::SensorSetStateCallback(unsigned port, retro_sensor_action action, unsigned rate)
{
    auto instance = Wrapper::GetCurrentThreadWrapper();
    if (!instance || !instance->m_input_handler)
        return false;

    // Saying no to a sub-device this build does not carry IS the compatibility
    // contract, not an error path: it is what a core probing for one is meant
    // to be told, so that it falls back to whatever it does without one.
    unsigned index = 0;
    unsigned base = 0;
    if (!RetroSensorSplit(static_cast<unsigned>(action), MAX_SENSOR_INDEX, index, base))
        return false;

    InputHandler& handler = *instance->m_input_handler;
    std::lock_guard<std::recursive_mutex> lock(handler.m_state_mutex);
    SensorState& state = handler.m_sensors[port][index];
    switch (base)
    {
    case RETRO_SENSOR_ACCELEROMETER_ENABLE:
        state.accel_enabled = true;
        Log("Sensor: accelerometer enabled on port " + std::to_string(port) + " sub-device " +
            std::to_string(index) + " rate=" + std::to_string(rate));
        return true;
    case RETRO_SENSOR_ACCELEROMETER_DISABLE:
        state.accel_enabled = false;
        return true;
    case RETRO_SENSOR_GYROSCOPE_ENABLE:
        state.gyro_enabled = true;
        Log("Sensor: gyroscope enabled on port " + std::to_string(port) + " sub-device " +
            std::to_string(index) + " rate=" + std::to_string(rate));
        return true;
    case RETRO_SENSOR_GYROSCOPE_DISABLE:
        state.gyro_enabled = false;
        return true;
    default:
        return false;
    }
}

/// Emu thread (called by the core each frame while enabled).
float InputHandler::SensorGetInputCallback(unsigned port, unsigned id)
{
    auto instance = Wrapper::GetCurrentThreadWrapper();
    if (!instance || !instance->m_input_handler)
        return 0.0f;

    // Same contract as the enable call: a sub-device this build does not carry
    // reads as zero, which is what the header documents for an invalid argument.
    unsigned index = 0;
    unsigned base = 0;
    if (!RetroSensorSplit(id, MAX_SENSOR_INDEX, index, base))
        return 0.0f;

    auto& handler = *instance->m_input_handler;
    std::lock_guard<std::recursive_mutex> lock(handler.m_state_mutex);
    // A sub-device nothing has written to answers with SensorState's defaults,
    // which are the at-rest pose: still, and feeling gravity on Z.
    auto it = handler.m_sensors.find(port);
    const SensorState state = it != handler.m_sensors.end() ? it->second[index] : SensorState{};
    switch (base)
    {
    case RETRO_SENSOR_ACCELEROMETER_X: return state.accel_x;
    case RETRO_SENSOR_ACCELEROMETER_Y: return state.accel_y;
    case RETRO_SENSOR_ACCELEROMETER_Z: return state.accel_z;
    case RETRO_SENSOR_GYROSCOPE_X:     return state.gyro_x;
    case RETRO_SENSOR_GYROSCOPE_Y:     return state.gyro_y;
    case RETRO_SENSOR_GYROSCOPE_Z:     return state.gyro_z;
    default:                           return 0.0f;
    }
}

bool InputHandler::GetInputDeviceCapabilities(uint32_t* capabilities)
{
    if (!capabilities)
        return true;

    *capabilities = (1 << RETRO_DEVICE_JOYPAD)
                  | (1 << RETRO_DEVICE_MOUSE)
                  | (1 << RETRO_DEVICE_KEYBOARD)
                  | (1 << RETRO_DEVICE_LIGHTGUN)
                  | (1 << RETRO_DEVICE_ANALOG)
                  | (1 << RETRO_DEVICE_POINTER);
    return true;
}

bool InputHandler::GetInputBitmasks(bool* available)
{
    // data should be ignored but just in case a core misuses this environment
    if (available)
        *available = true;
    return true;
}

bool InputHandler::SetKeyboardEventCallback(const retro_keyboard_callback* keyboard_callback)
{
    if (!keyboard_callback)
        return false;

    if (!keyboard_callback->callback)
    {
        LogError("Invalid callback provided.");
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_keyboard_event = keyboard_callback->callback;
    return true;
}

int16_t InputHandler::ProcessJoypadDevice(uint32_t port, uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    uint16_t buttons = m_joypad_buttons[port];
    const auto extra = m_joypad_extra.find(port);
    if (extra != m_joypad_extra.end())
        buttons |= extra->second;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return static_cast<int16_t>(buttons);
    return buttons & (1 << id);
}

int16_t InputHandler::ProcessMouseDevice(uint32_t port, uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    // Belt and braces: a core is supposed to call retro_input_poll once a frame
    // and most do, but the latch must not depend on it. Keyed on the frame
    // number, so whichever of the two gets here first is the one that latches.
    if (auto* instance = Wrapper::GetCurrentThreadWrapper())
        LatchMouseDeltas(instance->GetFrameCount());
    switch (id)
    {
    // Returned WITHOUT clearing. Zero-on-read looks equivalent and is not: a
    // core may read X and Y several times while building one frame, and the
    // read the game actually consumes is then 0. That is why the cursor never
    // moved in Mario Artist under ParaLLEl N64, and why bsnes2014 could not
    // move Mario Paint's while snes9x could.
    case RETRO_DEVICE_ID_MOUSE_X:
        return m_mouse_latched_x[port];
    case RETRO_DEVICE_ID_MOUSE_Y:
        return m_mouse_latched_y[port];
    default:
        return m_mouse_buttons[port] & (1 << id);
    }
}

int16_t InputHandler::ProcessKeyboardDevice(uint32_t port, uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return GetKeyState(port, id) ? 1 : 0;
}

int16_t InputHandler::ProcessLightgunDevice(uint32_t port, uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    switch (id)
    {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X:
            return m_lightgun_x[port];
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y:
            return m_lightgun_y[port];
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN:
            return m_lightgun_is_offscreen[port];
        default:
            return m_lightgun_buttons[port] & (1 << id);
    }
}

int16_t InputHandler::ProcessPointerDevice(uint32_t port, uint32_t index, uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    // COUNT is a property of the whole device, so it ignores the index: cores
    // read it at index 0 and would otherwise see nothing for the others.
    if (id == RETRO_DEVICE_ID_POINTER_COUNT)
        return GetPointerCount(port);

    if (index >= MAX_POINTERS)
        return 0;

    const auto& p = m_pointers[port][index];
    switch (id)
    {
        case RETRO_DEVICE_ID_POINTER_X:
            return p.x;
        case RETRO_DEVICE_ID_POINTER_Y:
            return p.y;
        case RETRO_DEVICE_ID_POINTER_PRESSED:
            return p.pressed;
        default:
            return 0;
    }
}

int16_t InputHandler::ProcessAnalogDevice(uint32_t port, uint32_t index, uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    switch (index)
    {
        case RETRO_DEVICE_INDEX_ANALOG_LEFT:
            switch (id)
            {
                case RETRO_DEVICE_ID_ANALOG_X:
                    return m_analog_left_x[port];
                case RETRO_DEVICE_ID_ANALOG_Y:
                    return m_analog_left_y[port];
                default:
                    return 0;
            }
        case RETRO_DEVICE_INDEX_ANALOG_RIGHT:
            switch (id)
            {
                case RETRO_DEVICE_ID_ANALOG_X:
                    return m_analog_right_x[port];
                case RETRO_DEVICE_ID_ANALOG_Y:
                    return m_analog_right_y[port];
                default:
                    return 0;
            }
        default:
            return 0;
    }
}
}
