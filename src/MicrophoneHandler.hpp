#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <libretro.h>

namespace godot
{
class PackedVector2Array;
}

namespace Xenu
{
class Wrapper;

// RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE for one Wrapper.
//
// Handles are slots in a process-wide pool that is never freed, so a call that
// arrives late on a core's own thread finds a slot, never freed memory. Only
// open_mic resolves the Wrapper through the thread-local, which is why it is a
// callback trampoline; every other call is answered from the handle.
class MicrophoneHandler
{
public:
    static constexpr unsigned k_pool_size    = 32;
    static constexpr unsigned k_max_per_core = 4;
    static constexpr unsigned k_default_rate = 48000;
    static constexpr unsigned k_min_rate     = 1000;
    static constexpr unsigned k_max_rate     = 384000;

    explicit MicrophoneHandler(Wrapper& wrapper);
    ~MicrophoneHandler();

    MicrophoneHandler(const MicrophoneHandler&) = delete;
    MicrophoneHandler& operator=(const MicrophoneHandler&) = delete;
    MicrophoneHandler(MicrophoneHandler&&) = delete;
    MicrophoneHandler& operator=(MicrophoneHandler&&) = delete;

    bool GetMicrophoneInterface(retro_microphone_interface* iface);

    static retro_microphone_t* RETRO_CALLCONV OpenMic(const retro_microphone_params_t* params);
    static void RETRO_CALLCONV CloseMic(retro_microphone_t* microphone);
    static bool RETRO_CALLCONV GetParams(const retro_microphone_t* microphone, retro_microphone_params_t* params);
    static bool RETRO_CALLCONV SetMicState(retro_microphone_t* microphone, bool state);
    static bool RETRO_CALLCONV GetMicState(const retro_microphone_t* microphone);
    static int RETRO_CALLCONV ReadMic(retro_microphone_t* microphone, int16_t* samples, size_t num_samples);

    /// Main thread. Fed to every microphone of this core that is switched on.
    void Push(const godot::PackedVector2Array& frames, double source_rate, float gain);
    /// A microphone is open and switched on, and the machine is neither stopped
    /// nor in netplay.
    bool IsActive() const;
    /// Stopped machines read silence. A change empties every stream.
    void SetSuspended(bool suspended);
    void ResetAll();
    /// Frees every slot this core still holds. Only once the core can make no
    /// further calls.
    void ReleaseAll();

private:
    retro_microphone_t* Open(unsigned rate);
    bool Silenced() const;

    Wrapper& m_wrapper;
    std::atomic<uint32_t> m_owned{0};
    std::atomic<uint32_t> m_enabled{0};
    std::atomic<bool> m_suspended{false};
};
}
