#include "Wrapper.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>

#include <algorithm>
#include <cstring>

#include "Debug.hpp"

using namespace godot;

namespace Xenu
{
static int16_t ToShort(float floatValue, int mul = 1)
{
    return static_cast<int16_t>(Math::clamp(Math::round(floatValue), static_cast<float>(INT16_MIN), static_cast<float>(INT16_MAX)) * mul);
}

// Godot reports both Shift/Ctrl/Alt/Super keys under one keycode, so the whole
// event is needed: get_location() picks the left or right RETROK_* variant.
// Exposed to GDScript as Libretro::GodotKeyToRetroKey so there is only one copy
// of this table; retro_keyboard.gd decides *when* to send a key.
retro_key Wrapper::GodotKeyToRetroKey(const Ref<InputEventKey>& keyEvent)
{
    switch (keyEvent->get_keycode())
    {
        case KEY_NONE: return RETROK_UNKNOWN;
        case KEY_BACKSPACE: return RETROK_BACKSPACE;
        case KEY_TAB: return RETROK_TAB;
        case KEY_CLEAR: return RETROK_CLEAR;
        case KEY_ENTER: return RETROK_RETURN;
        case KEY_PAUSE: return RETROK_PAUSE;
        case KEY_ESCAPE: return RETROK_ESCAPE;
        case KEY_SPACE: return RETROK_SPACE;
        case KEY_EXCLAM: return RETROK_EXCLAIM;
        case KEY_QUOTEDBL: return RETROK_QUOTEDBL;
        case KEY_NUMBERSIGN: return RETROK_HASH;
        case KEY_DOLLAR: return RETROK_DOLLAR;
        case KEY_AMPERSAND: return RETROK_AMPERSAND;
        case KEY_APOSTROPHE: return RETROK_QUOTE;
        case KEY_PARENLEFT: return RETROK_LEFTPAREN;
        case KEY_PARENRIGHT: return RETROK_RIGHTPAREN;
        case KEY_ASTERISK: return RETROK_ASTERISK;
        case KEY_PLUS: return RETROK_PLUS;
        case KEY_COMMA: return RETROK_COMMA;
        case KEY_MINUS: return RETROK_MINUS;
        case KEY_PERIOD: return RETROK_PERIOD;
        case KEY_SLASH: return RETROK_SLASH;
        case KEY_0: return RETROK_0;
        case KEY_1: return RETROK_1;
        case KEY_2: return RETROK_2;
        case KEY_3: return RETROK_3;
        case KEY_4: return RETROK_4;
        case KEY_5: return RETROK_5;
        case KEY_6: return RETROK_6;
        case KEY_7: return RETROK_7;
        case KEY_8: return RETROK_8;
        case KEY_9: return RETROK_9;
        case KEY_COLON: return RETROK_COLON;
        case KEY_SEMICOLON: return RETROK_SEMICOLON;
        case KEY_LESS: return RETROK_LESS;
        case KEY_EQUAL: return RETROK_EQUALS;
        case KEY_GREATER: return RETROK_GREATER;
        case KEY_QUESTION: return RETROK_QUESTION;
        case KEY_AT: return RETROK_AT;
        case KEY_BRACKETLEFT: return RETROK_LEFTBRACKET;
        case KEY_BACKSLASH: return RETROK_BACKSLASH;
        case KEY_BRACKETRIGHT: return RETROK_RIGHTBRACKET;
        case KEY_ASCIICIRCUM: return RETROK_CARET;
        case KEY_UNDERSCORE: return RETROK_UNDERSCORE;
        case KEY_QUOTELEFT: return RETROK_BACKQUOTE;
        case KEY_A: return RETROK_a;
        case KEY_B: return RETROK_b;
        case KEY_C: return RETROK_c;
        case KEY_D: return RETROK_d;
        case KEY_E: return RETROK_e;
        case KEY_F: return RETROK_f;
        case KEY_G: return RETROK_g;
        case KEY_H: return RETROK_h;
        case KEY_I: return RETROK_i;
        case KEY_J: return RETROK_j;
        case KEY_K: return RETROK_k;
        case KEY_L: return RETROK_l;
        case KEY_M: return RETROK_m;
        case KEY_N: return RETROK_n;
        case KEY_O: return RETROK_o;
        case KEY_P: return RETROK_p;
        case KEY_Q: return RETROK_q;
        case KEY_R: return RETROK_r;
        case KEY_S: return RETROK_s;
        case KEY_T: return RETROK_t;
        case KEY_U: return RETROK_u;
        case KEY_V: return RETROK_v;
        case KEY_W: return RETROK_w;
        case KEY_X: return RETROK_x;
        case KEY_Y: return RETROK_y;
        case KEY_Z: return RETROK_z;
        case KEY_BRACELEFT: return RETROK_LEFTBRACE;
        case KEY_BAR: return RETROK_BAR;
        case KEY_BRACERIGHT: return RETROK_RIGHTBRACE;
        case KEY_ASCIITILDE: return RETROK_TILDE;
        case KEY_DELETE: return RETROK_DELETE;
        case KEY_KP_0: return RETROK_KP0;
        case KEY_KP_1: return RETROK_KP1;
        case KEY_KP_2: return RETROK_KP2;
        case KEY_KP_3: return RETROK_KP3;
        case KEY_KP_4: return RETROK_KP4;
        case KEY_KP_5: return RETROK_KP5;
        case KEY_KP_6: return RETROK_KP6;
        case KEY_KP_7: return RETROK_KP7;
        case KEY_KP_8: return RETROK_KP8;
        case KEY_KP_9: return RETROK_KP9;
        case KEY_KP_PERIOD: return RETROK_KP_PERIOD;
        case KEY_KP_DIVIDE: return RETROK_KP_DIVIDE;
        case KEY_KP_MULTIPLY: return RETROK_KP_MULTIPLY;
        case KEY_KP_SUBTRACT: return RETROK_KP_MINUS;
        case KEY_KP_ADD: return RETROK_KP_PLUS;
        case KEY_KP_ENTER: return RETROK_KP_ENTER;
        // case KEY_KP_EQUALS: return RETROK_KP_EQUALS;
        case KEY_UP: return RETROK_UP;
        case KEY_DOWN: return RETROK_DOWN;
        case KEY_RIGHT: return RETROK_RIGHT;
        case KEY_LEFT: return RETROK_LEFT;
        case KEY_INSERT: return RETROK_INSERT;
        case KEY_HOME: return RETROK_HOME;
        case KEY_END: return RETROK_END;
        case KEY_PAGEUP: return RETROK_PAGEUP;
        case KEY_PAGEDOWN: return RETROK_PAGEDOWN;
        case KEY_F1: return RETROK_F1;
        case KEY_F2: return RETROK_F2;
        case KEY_F3: return RETROK_F3;
        case KEY_F4: return RETROK_F4;
        case KEY_F5: return RETROK_F5;
        case KEY_F6: return RETROK_F6;
        case KEY_F7: return RETROK_F7;
        case KEY_F8: return RETROK_F8;
        case KEY_F9: return RETROK_F9;
        case KEY_F10: return RETROK_F10;
        case KEY_F11: return RETROK_F11;
        case KEY_F12: return RETROK_F12;
        case KEY_F13: return RETROK_F13;
        case KEY_F14: return RETROK_F14;
        case KEY_F15: return RETROK_F15;
        case KEY_NUMLOCK: return RETROK_NUMLOCK;
        case KEY_CAPSLOCK: return RETROK_CAPSLOCK;
        case KEY_SCROLLLOCK: return RETROK_SCROLLOCK;
        case KEY_SHIFT:
        {
            if (keyEvent->get_location() == KeyLocation::KEY_LOCATION_LEFT)
                return RETROK_LSHIFT;
            if (keyEvent->get_location() == KeyLocation::KEY_LOCATION_RIGHT)
                return RETROK_RSHIFT;
        }
        break;
        case KEY_CTRL:
        {
            if (keyEvent->get_location() == KeyLocation::KEY_LOCATION_LEFT)
                return RETROK_LCTRL;
            if (keyEvent->get_location() == KeyLocation::KEY_LOCATION_RIGHT)
                return RETROK_RCTRL;
        }
        break;
        case KEY_ALT:
        {
            if (keyEvent->get_location() == KeyLocation::KEY_LOCATION_LEFT)
                return RETROK_LALT;
            if (keyEvent->get_location() == KeyLocation::KEY_LOCATION_RIGHT)
                return RETROK_RALT;
        }
        break;
        case KEY_META:
        {
            // NOTE: may need to return RETROK_LSUPER/RETK_RSUPER instead for some platforms
            if (keyEvent->get_location() == KeyLocation::KEY_LOCATION_LEFT)
                return RETROK_LMETA;
            if (keyEvent->get_location() == KeyLocation::KEY_LOCATION_RIGHT)
                return RETROK_RMETA;
        }
        break;
        // case KEY_MODE: return RETROK_MODE;
        // case KEY_COMPOSE: return RETROK_COMPOSE;
        case KEY_HELP: return RETROK_HELP;
        case KEY_PRINT: return RETROK_PRINT;
        case KEY_SYSREQ: return RETROK_SYSREQ;
        // case KEY_BREAK: return RETROK_BREAK;
        case KEY_MENU: return RETROK_MENU;
        // case KEY_POWER: return RETROK_POWER;
        // case KEY_EURO: return RETROK_EURO;
        // case KEY_UNDO: return RETROK_UNDO;
        // case KEY_OEM_102: return RETROK_OEM_102;
        case KEY_BACK: return RETROK_BROWSER_BACK;
        case KEY_FORWARD: return RETROK_BROWSER_FORWARD;
        case KEY_REFRESH: return RETROK_BROWSER_REFRESH;
        case KEY_STOP: return RETROK_BROWSER_STOP;
        case KEY_SEARCH: return RETROK_BROWSER_SEARCH;
        case KEY_FAVORITES: return RETROK_BROWSER_FAVORITES;
        case KEY_HOMEPAGE: return RETROK_BROWSER_HOME;
        case KEY_VOLUMEMUTE: return RETROK_VOLUME_MUTE;
        case KEY_VOLUMEDOWN: return RETROK_VOLUME_DOWN;
        case KEY_VOLUMEUP: return RETROK_VOLUME_UP;
        case KEY_MEDIANEXT: return RETROK_MEDIA_NEXT;
        case KEY_MEDIAPREVIOUS: return RETROK_MEDIA_PREV;
        case KEY_MEDIASTOP: return RETROK_MEDIA_STOP;
        case KEY_MEDIAPLAY: return RETROK_MEDIA_PLAY_PAUSE;
        case KEY_LAUNCHMAIL: return RETROK_LAUNCH_MAIL;
        case KEY_LAUNCHMEDIA: return RETROK_LAUNCH_MEDIA;
        // case KEY_LAUNCH_APP1: return RETROK_LAUNCH_APP1;
        // case KEY_LAUNCH_APP2: return RETROK_LAUNCH_APP2;

        // Unhandled godot keys
        case KEY_SPECIAL: return RETROK_UNKNOWN;
        case KEY_BACKTAB: return RETROK_UNKNOWN;
        case KEY_F16: return RETROK_UNKNOWN;
        case KEY_F17: return RETROK_UNKNOWN;
        case KEY_F18: return RETROK_UNKNOWN;
        case KEY_F19: return RETROK_UNKNOWN;
        case KEY_F20: return RETROK_UNKNOWN;
        case KEY_F21: return RETROK_UNKNOWN;
        case KEY_F22: return RETROK_UNKNOWN;
        case KEY_F23: return RETROK_UNKNOWN;
        case KEY_F24: return RETROK_UNKNOWN;
        case KEY_F25: return RETROK_UNKNOWN;
        case KEY_F26: return RETROK_UNKNOWN;
        case KEY_F27: return RETROK_UNKNOWN;
        case KEY_F28: return RETROK_UNKNOWN;
        case KEY_F29: return RETROK_UNKNOWN;
        case KEY_F30: return RETROK_UNKNOWN;
        case KEY_F31: return RETROK_UNKNOWN;
        case KEY_F32: return RETROK_UNKNOWN;
        case KEY_F33: return RETROK_UNKNOWN;
        case KEY_F34: return RETROK_UNKNOWN;
        case KEY_F35: return RETROK_UNKNOWN;
        case KEY_HYPER: return RETROK_UNKNOWN;
        case KEY_MEDIARECORD: return RETROK_UNKNOWN;
        case KEY_STANDBY: return RETROK_UNKNOWN;
        case KEY_OPENURL: return RETROK_UNKNOWN;
        case KEY_LAUNCH0: return RETROK_UNKNOWN;
        case KEY_LAUNCH1: return RETROK_UNKNOWN;
        case KEY_LAUNCH2: return RETROK_UNKNOWN;
        case KEY_LAUNCH3: return RETROK_UNKNOWN;
        case KEY_LAUNCH4: return RETROK_UNKNOWN;
        case KEY_LAUNCH5: return RETROK_UNKNOWN;
        case KEY_LAUNCH6: return RETROK_UNKNOWN;
        case KEY_LAUNCH7: return RETROK_UNKNOWN;
        case KEY_LAUNCH8: return RETROK_UNKNOWN;
        case KEY_LAUNCH9: return RETROK_UNKNOWN;
        case KEY_LAUNCHA: return RETROK_UNKNOWN;
        case KEY_LAUNCHB: return RETROK_UNKNOWN;
        case KEY_LAUNCHC: return RETROK_UNKNOWN;
        case KEY_LAUNCHD: return RETROK_UNKNOWN;
        case KEY_LAUNCHE: return RETROK_UNKNOWN;
        case KEY_LAUNCHF: return RETROK_UNKNOWN;
        case KEY_GLOBE: return RETROK_UNKNOWN;
        case KEY_KEYBOARD: return RETROK_UNKNOWN;
        case KEY_JIS_EISU: return RETROK_UNKNOWN;
        case KEY_JIS_KANA: return RETROK_UNKNOWN;
        case KEY_UNKNOWN: return RETROK_UNKNOWN;
        case KEY_PERCENT: return RETROK_UNKNOWN;
        case KEY_YEN: return RETROK_UNKNOWN;
        case KEY_SECTION: return RETROK_UNKNOWN;
        default:
            LogError("Unmapped key code from Godot to Libretro: " + std::to_string(keyEvent->get_keycode()));
            break;
    };

    return RETROK_UNKNOWN;
}

godot::Array Wrapper::GetControllerInfo() const
{
    Array result;
    if (!m_input_handler)
        return result;

    const auto controllers = m_input_handler->GetControllers();
    for (uint32_t port = 0; port < controllers.size(); ++port)
    {
        Array port_controllers;
        for (const auto& ctrl : controllers[port])
        {
            Dictionary entry;
            entry["name"] = String(ctrl.name.c_str());
            entry["id"]   = static_cast<int>(ctrl.id);
            port_controllers.append(entry);
        }
        // Also record which device is currently selected for this port
        Dictionary port_entry;
        port_entry["port"]        = static_cast<int>(port);
        port_entry["controllers"] = port_controllers;
        port_entry["current_id"]  = static_cast<int>(m_input_handler->GetPortDevice(port));
        result.append(port_entry);
    }
    return result;
}

void Wrapper::SetControllerPortDevice(uint32_t port, uint32_t device)
{
    // Always remember the selection: a controller/mouse plugged in while the
    // system is OFF has no core yet; the emulation thread applies this map
    // right after retro_load_game so the core polls the right device.
    {
        std::lock_guard<std::mutex> lock(m_port_device_mutex);
        m_pending_port_devices[port] = device;
    }
    if (!m_core || !m_input_handler)
    {
        Log("SetControllerPortDevice: no core running, port=" + std::to_string(port)
            + " device=" + std::to_string(device) + " recorded for next start");
        return;
    }
    // Live change: route through the emulation thread so the core call lands
    // strictly between retro_run() frames (never mid-frame from this thread).
    m_emu_thread_commands_queue.enqueue(
        std::make_unique<EmuThreadCommandSetPortDevice>(port, device));
}

void Wrapper::SetLightgunPosition(uint32_t port, int16_t x, int16_t y)
{
    if (!m_input_handler)
        return;
    if (IsNetplayPortManaged(port))
    {
        if (m_np_rollback.load(std::memory_order_acquire) &&
            (m_np_local_mask.load(std::memory_order_relaxed) & (1u << port)))
        {
            std::lock_guard<std::mutex> lock(m_np_mutex);
            m_np_live_local[port * 5 + 1] = x;
            m_np_live_local[port * 5 + 2] = y;
        }
        return;
    }
    m_input_handler->SetLightgunPosition(port, x, y);
}

void Wrapper::SetLightgunIsOffscreen(uint32_t port, bool offscreen)
{
    if (!m_input_handler)
        return;
    if (IsNetplayPortManaged(port))
    {
        if (m_np_rollback.load(std::memory_order_acquire) &&
            (m_np_local_mask.load(std::memory_order_relaxed) & (1u << port)))
        {
            std::lock_guard<std::mutex> lock(m_np_mutex);
            m_np_live_local[port * 5 + 3] = offscreen ? 1 : 0;
        }
        return;
    }
    m_input_handler->SetLightgunIsOffscreen(port, offscreen ? 1 : 0);
}

void Wrapper::SetLightgunButton(uint32_t port, int button_id, bool pressed)
{
    if (!m_input_handler)
        return;
    uint32_t buttons = 0;
    if (IsNetplayPortManaged(port))
    {
        if (!(m_np_rollback.load(std::memory_order_acquire) &&
              (m_np_local_mask.load(std::memory_order_relaxed) & (1u << port))))
            return;
        std::lock_guard<std::mutex> lock(m_np_mutex);
        buttons = static_cast<uint32_t>(m_np_live_local[port * 5]);
        if (pressed)
            buttons |= (1u << button_id);
        else
            buttons &= ~(1u << button_id);
        m_np_live_local[port * 5] = static_cast<int32_t>(buttons);
        return;
    }
    buttons = m_input_handler->GetLightgunButtons(port);
    if (pressed)
        buttons |= (1u << button_id);
    else
        buttons &= ~(1u << button_id);
    m_input_handler->SetLightgunButtons(port, buttons);
}

void Wrapper::SetJoypadState(uint32_t port, uint16_t button_mask, int16_t analog_lx, int16_t analog_ly, int16_t analog_rx, int16_t analog_ry)
{
    if (!m_input_handler)
        return;
    // In netplay mode the emulation thread applies the agreed per-frame inputs
    // for the masked ports; live main-thread writes would desync peers.
    if (IsNetplayPortManaged(port))
    {
        // Rollback: locally-owned masked ports feed the live-input slot that
        // the emulation thread samples each frame (zero added delay). Remote
        // masked ports stay blocked (prediction + confirmations own them).
        if (m_np_rollback.load(std::memory_order_acquire) &&
            (m_np_local_mask.load(std::memory_order_relaxed) & (1u << port)))
        {
            std::lock_guard<std::mutex> lock(m_np_mutex);
            int32_t* v = m_np_live_local.data() + port * 5;
            v[0] = button_mask;
            v[1] = analog_lx; v[2] = analog_ly;
            v[3] = analog_rx; v[4] = analog_ry;
        }
        return;
    }
    m_input_handler->SetJoypadButtonStates(port, button_mask);
    m_input_handler->SetAnalogLeft(port, analog_lx, analog_ly);
    m_input_handler->SetAnalogRight(port, analog_rx, analog_ry);
}

godot::PackedInt32Array Wrapper::PeekJoypadState(uint32_t port) const
{
    godot::PackedInt32Array out;
    out.resize(5);
    if (!m_input_handler)
        return out;

    const InputHandler::NetplayState state = m_input_handler->CaptureNetplayState();
    const auto value_or_zero = [port](const auto& values) -> int32_t
    {
        const auto it = values.find(port);
        return it == values.end() ? 0 : static_cast<int32_t>(it->second);
    };
    out[0] = value_or_zero(state.joypad_buttons);
    out[1] = value_or_zero(state.analog_left_x);
    out[2] = value_or_zero(state.analog_left_y);
    out[3] = value_or_zero(state.analog_right_x);
    out[4] = value_or_zero(state.analog_right_y);
    return out;
}

void Wrapper::SetMouseState(uint32_t port, int32_t dx, int32_t dy, uint32_t buttons)
{
    if (!m_input_handler)
        return;
    auto clamp16 = [](int64_t v) -> int32_t
    {
        return static_cast<int32_t>(v < -0x7fff ? -0x7fff : (v > 0x7fff ? 0x7fff : v));
    };
    if (IsNetplayPortManaged(port))
    {
        // Match InputHandler's relative-delta semantics: collect every main-
        // thread update until the emulation thread samples this local frame.
        if (m_np_rollback.load(std::memory_order_acquire) &&
            (m_np_local_mask.load(std::memory_order_relaxed) & (1u << port)))
        {
            std::lock_guard<std::mutex> lock(m_np_mutex);
            int32_t* v = m_np_live_local.data() + port * 5;
            v[0] = static_cast<int32_t>(buttons);
            v[1] = clamp16(static_cast<int64_t>(v[1]) + dx);
            v[2] = clamp16(static_cast<int64_t>(v[2]) + dy);
        }
        return;
    }
    m_input_handler->SetMousePosition(port, static_cast<int16_t>(clamp16(dx)), static_cast<int16_t>(clamp16(dy)));
    m_input_handler->SetMouseButtons(port, buttons);
}

void Wrapper::SetKeyState(uint32_t port, uint32_t keycode, bool down, uint32_t character)
{
    if (!m_input_handler)
        return;
    // During netplay, keyboard input rides the deterministic frame schedule
    // (key-event slots); live writes would desync peers. Keyboard state is
    // global (port 0), so gate on port 0 being masked.
    if (m_netplay_enabled.load(std::memory_order_acquire) &&
        (m_np_port_mask.load(std::memory_order_relaxed) & 1u))
        return;
    if (!m_running.load(std::memory_order_acquire)
        || m_stopping.load(std::memory_order_acquire))
        return;
    m_emu_thread_commands_queue.enqueue(
        std::make_unique<EmuThreadCommandKeyboardEvent>(port, keycode, down, character));
}

void Wrapper::SetSensorAccel(uint32_t port, float x, float y, float z, uint32_t index)
{
    if (m_input_handler && !IsNetplayPortManaged(port))
        m_input_handler->SetSensorAccel(port, x, y, z, index);
}

void Wrapper::SetSensorGyro(uint32_t port, float x, float y, float z, uint32_t index)
{
    if (m_input_handler && !IsNetplayPortManaged(port))
        m_input_handler->SetSensorGyro(port, x, y, z, index);
}

void Wrapper::PushMicrophoneFrames(const godot::PackedVector2Array& frames, double source_rate, float gain)
{
    m_microphone_handler->Push(frames, source_rate, gain);
}

bool Wrapper::IsMicrophoneActive() const
{
    return m_microphone_handler->IsActive();
}

void Wrapper::SetPointerState(uint32_t port, int16_t x, int16_t y, bool pressed)
{
    if (m_input_handler && !IsNetplayPortManaged(port))
    {
        m_input_handler->SetPointerPosition(port, x, y);
        m_input_handler->SetPointerPressed(port, pressed ? 1 : 0);
        // A single-point caller owns the whole device: anything a multi-point
        // caller left on the other indices would otherwise keep being reported.
        m_input_handler->ClearPointersFrom(port, 1);
    }
}

void Wrapper::SetPointerIndexState(uint32_t port, uint32_t index, int16_t x, int16_t y, bool pressed)
{
    if (m_input_handler && !IsNetplayPortManaged(port))
        m_input_handler->SetPointerIndexState(port, index, x, y, pressed);
}

/// Main thread: the desktop fallback, polling the global InputMap into port 0.
/// Skipped entirely unless this instance is the one the player is driving.
void Wrapper::PollDesktopInput()
{
    auto input = godot::Input::get_singleton();

    // Only the instance the player is controlling should consume global input actions.
    if (!m_input_enabled)
        return;

    uint32_t joypad_buttons = 0;

    if (input->is_action_pressed("RETRO_JOYPAD_B"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_B);
    if (input->is_action_pressed("RETRO_JOYPAD_Y"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_Y);
    if (input->is_action_pressed("RETRO_JOYPAD_SELECT"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
    if (input->is_action_pressed("RETRO_JOYPAD_START"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_START);
    if (input->is_action_pressed("RETRO_JOYPAD_UP"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
    if (input->is_action_pressed("RETRO_JOYPAD_DOWN"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (input->is_action_pressed("RETRO_JOYPAD_LEFT"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (input->is_action_pressed("RETRO_JOYPAD_RIGHT"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (input->is_action_pressed("RETRO_JOYPAD_A"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_A);
    if (input->is_action_pressed("RETRO_JOYPAD_X"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_X);
    if (input->is_action_pressed("RETRO_JOYPAD_L"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_L);
    if (input->is_action_pressed("RETRO_JOYPAD_R"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_R);
    if (input->is_action_pressed("RETRO_JOYPAD_L2"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_L2);
    if (input->is_action_pressed("RETRO_JOYPAD_R2"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_R2);
    if (input->is_action_pressed("RETRO_JOYPAD_L3"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_L3);
    if (input->is_action_pressed("RETRO_JOYPAD_R3"))
        joypad_buttons |= (1 << RETRO_DEVICE_ID_JOYPAD_R3);

    m_input_handler->SetJoypadButtonStates(0, joypad_buttons);

    Vector2 analog_left = {};

    if (input->get_action_strength("RETRO_ANALOG_LEFT_X_NEGATIVE"))
        analog_left.x -= 1.0f;
    if (input->get_action_strength("RETRO_ANALOG_LEFT_X_POSITIVE"))
        analog_left.x += 1.0f;
    if (input->get_action_strength("RETRO_ANALOG_LEFT_Y_POSITIVE"))
        analog_left.y -= 1.0f;
    if (input->get_action_strength("RETRO_ANALOG_LEFT_Y_NEGATIVE"))
        analog_left.y += 1.0f;

    if (analog_left.length_squared() > 1.0f)
        analog_left = analog_left.normalized();

    m_input_handler->SetAnalogLeft(0, ToShort(analog_left.x) * 0x7fff, ToShort(analog_left.y) * 0x7fff);

    Vector2 analog_right = {};

    if (input->get_action_strength("RETRO_ANALOG_RIGHT_X_NEGATIVE"))
        analog_right.x -= 1.0f;
    if (input->get_action_strength("RETRO_ANALOG_RIGHT_X_POSITIVE"))
        analog_right.x += 1.0f;
    if (input->get_action_strength("RETRO_ANALOG_RIGHT_Y_POSITIVE"))
        analog_right.y -= 1.0f;
    if (input->get_action_strength("RETRO_ANALOG_RIGHT_Y_NEGATIVE"))
        analog_right.y += 1.0f;

    if (analog_right.length_squared() > 1.0f)
        analog_right = analog_right.normalized();

    m_input_handler->SetAnalogRight(0, ToShort(analog_right.x) * 0x7fff, ToShort(analog_right.y) * 0x7fff);
}
}
