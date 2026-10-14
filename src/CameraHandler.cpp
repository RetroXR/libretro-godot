#include "CameraHandler.hpp"

#include <godot_cpp/variant/packed_byte_array.hpp>

#include <string>

#include "Debug.hpp"
#include "Wrapper.hpp"

namespace Xenu
{
CameraHandler::CameraHandler(Wrapper& wrapper)
    : m_wrapper(wrapper)
{
}

bool CameraHandler::GetCameraInterface(retro_camera_callback* callback)
{
    if (!callback)
        return false;

    // A picture is handed over as memory. A core that can only take a GL texture
    // gets a refusal it can act on, rather than a camera that never delivers.
    const uint64_t raw = UINT64_C(1) << RETRO_CAMERA_BUFFER_RAW_FRAMEBUFFER;
    if (!(callback->caps & raw) || !callback->frame_raw_framebuffer)
    {
        LogWarning("Camera refused: the core does not take raw frames, which is the only way one is delivered");
        return false;
    }

    const unsigned width  = callback->width ? callback->width : k_default_width;
    const unsigned height = callback->height ? callback->height : k_default_height;
    if (width > k_max_side || height > k_max_side)
    {
        LogWarning("Camera refused: the core asked for " + std::to_string(width) + "x" + std::to_string(height));
        return false;
    }

    m_frame         = callback->frame_raw_framebuffer;
    m_initialized   = callback->initialized;
    m_deinitialized = callback->deinitialized;
    m_announced     = false;
    m_stream.Configure(width, height);

    callback->start = &Start;
    callback->stop  = &Stop;
    Log("Camera offered at " + std::to_string(width) + "x" + std::to_string(height));
    return true;
}

bool CameraHandler::Start()
{
    Wrapper* wrapper = Wrapper::GetCurrentThreadWrapper();
    if (!wrapper)
    {
        LogErrorOnce("camera start arrived on a thread with no Wrapper");
        return false;
    }
    // Said once a run: mGBA starts it again for every photograph it is asked for
    // before a picture has arrived.
    if (wrapper->m_camera_handler->m_stream.Start())
        LogOK("Camera started");
    return true;
}

void CameraHandler::Stop()
{
    // Not an error with nothing started: mGBA stops the camera as EVERY Game Boy
    // game unloads, whether or not it was a Game Boy Camera.
    if (Wrapper* wrapper = Wrapper::GetCurrentThreadWrapper())
        wrapper->m_camera_handler->m_stream.Stop();
}

void CameraHandler::NotifyInitialized()
{
    if (!m_frame || m_announced)
        return;
    m_announced = true;
    if (m_initialized)
        m_initialized();
}

void CameraHandler::NotifyDeinitialized()
{
    if (m_announced && m_deinitialized)
        m_deinitialized();
    m_announced = false;
}

void CameraHandler::DeliverPending()
{
    if (m_frame)
        m_stream.Deliver(Silenced(), m_frame);
}

void CameraHandler::Detach()
{
    m_frame         = nullptr;
    m_initialized   = nullptr;
    m_deinitialized = nullptr;
    m_announced     = false;
    m_stream.Stop();
}

bool CameraHandler::Push(const godot::PackedByteArray& rgba, unsigned width, unsigned height)
{
    if (Silenced())
        return false;
    return m_stream.Push(rgba.ptr(), static_cast<size_t>(rgba.size()), width, height);
}

bool CameraHandler::IsActive() const
{
    return m_stream.Started() && !Silenced();
}

void CameraHandler::FrameSize(unsigned& width, unsigned& height) const
{
    m_stream.Size(width, height);
}

CameraStream::Stats CameraHandler::Stats() const
{
    return m_stream.GetStats();
}

void CameraHandler::SetSuspended(bool suspended)
{
    m_suspended.store(suspended, std::memory_order_release);
}

void CameraHandler::Reset()
{
    Detach();
    m_stream.Reset();
}

bool CameraHandler::Silenced() const
{
    return m_suspended.load(std::memory_order_acquire)
        || m_wrapper.m_netplay_enabled.load(std::memory_order_acquire)
        || m_wrapper.m_np_rollback.load(std::memory_order_acquire);
}
}
