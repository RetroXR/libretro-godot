#pragma once

#include <atomic>
#include <cstdint>

#include <libretro.h>

#include "CameraStream.hpp"

namespace godot
{
class PackedByteArray;
}

namespace Xenu
{
class Wrapper;

// RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE for one Wrapper.
//
// The reverse of the microphone: there the core calls in for samples, here the
// frontend calls the core's frame_raw_framebuffer, on the emulation thread and
// immediately before retro_run. So the main thread only leaves a picture in the
// CameraStream, and DeliverPending is what the core ever sees.
//
// start and stop carry no handle and no user data, so they resolve the Wrapper
// through the thread-local. That is sound because every core that takes a camera
// here calls them on the emulation thread -- but NOT only inside retro_run, which
// is what libretro.h asks for: mGBA starts the camera inside retro_load_game and
// retro_reset and stops it inside retro_unload_game. They therefore do nothing
// but set a flag.
class CameraHandler
{
public:
    /// For a core that leaves the size to the frontend (a hint of 0).
    static constexpr unsigned k_default_width  = 320;
    static constexpr unsigned k_default_height = 240;
    static constexpr unsigned k_max_side       = 2048;

    explicit CameraHandler(Wrapper& wrapper);

    CameraHandler(const CameraHandler&) = delete;
    CameraHandler& operator=(const CameraHandler&) = delete;
    CameraHandler(CameraHandler&&) = delete;
    CameraHandler& operator=(CameraHandler&&) = delete;

    bool GetCameraInterface(retro_camera_callback* callback);

    static bool RETRO_CALLCONV Start();
    static void RETRO_CALLCONV Stop();

    /// Emulation thread, once the game has loaded and before it is unloaded.
    void NotifyInitialized();
    void NotifyDeinitialized();
    /// Emulation thread, immediately before every retro_run.
    void DeliverPending();
    /// Emulation thread, once the core can make no further calls: forgets the
    /// core's callbacks, so nothing here can reach into an unloaded core.
    void Detach();

    /// Main thread. RGBA8, top row first, exactly FrameSize(). False when the
    /// size is not the core's, and while stopped or in netplay.
    bool Push(const godot::PackedByteArray& rgba, unsigned width, unsigned height);
    /// The core has started its camera, and the machine is neither stopped nor
    /// in netplay. A core that merely ASKED for the interface is not active:
    /// mGBA asks on every Game Boy load and starts only for a Game Boy Camera.
    bool IsActive() const;
    /// (0, 0) until a core has asked for the interface.
    void FrameSize(unsigned& width, unsigned& height) const;
    CameraStream::Stats Stats() const;
    /// A stopped machine's core holds the blank.
    void SetSuspended(bool suspended);
    /// Main thread, with no emulation thread running.
    void Reset();

private:
    bool Silenced() const;

    Wrapper& m_wrapper;
    CameraStream m_stream;
    std::atomic<bool> m_suspended{false};

    // Emulation thread only (Reset aside). The core's, so they die with it.
    retro_camera_frame_raw_framebuffer_t m_frame = nullptr;
    retro_camera_lifetime_status_t m_initialized = nullptr;
    retro_camera_lifetime_status_t m_deinitialized = nullptr;
    bool m_announced = false;
};
}
