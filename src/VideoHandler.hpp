#pragma once

#include <godot_cpp/classes/image_texture.hpp>

#include <cstdint>
#include <memory>

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
#include <SDL3/SDL_video.h>
#elif defined(__ANDROID__)
#include <EGL/egl.h>
#endif

#include <libretro.h>
#include "VulkanContext.hpp"

namespace Xenu
{
class Wrapper;
// Held by pointer only, so <d3d11.h>/<d3d12.h> stay out of this header, along
// with the windows.h macros they drag in (one of which renames GetSystemDirectory).
class D3D11Context;
class D3D12Context;

class VideoHandler
{
public:
    VideoHandler();
    ~VideoHandler();

    static void RefreshCallback(const void* data, uint32_t width, uint32_t height, size_t pitch);
    static uintptr_t HwRenderGetCurrentFramebuffer();
    static retro_proc_address_t HwRenderGetProcAddress(const char* sym);

    void Init();
    void DeInit();

    bool InitHwRenderContext(int32_t width, int32_t height);
    /// Replace an active hardware context for SET_SYSTEM_AV_INFO. The caller
    /// owns transaction rollback because it must stage the matching AV-info
    /// object before each context_reset callback.
    bool ReinitHwRenderContext(int32_t width, int32_t height);
    bool UsesHardwareRendering() const { return m_context_reset != nullptr; }
    /// The core's picture, for a display that SAMPLES it instead of being painted
    /// into. Null until the first frame has produced a texture.
    ///
    /// Main-thread only, and safe without a lock for that reason: CreateTexture and
    /// UpdateTexture both run on the main thread as queued ThreadCommands, so a
    /// reader on that thread cannot see a half-swapped texture. The identity changes
    /// whenever the core changes resolution — CreateTexture makes a NEW ImageTexture
    /// — so a caller must re-read it rather than cache it.
    godot::Ref<godot::ImageTexture> GetTexture() const { return m_texture; }

    /// The same frame, CPU-side: the buffer the texture was uploaded FROM, so
    /// reading it costs no GPU readback and stalls no pipeline. Main-thread only
    /// for the same reason GetTexture() is.
    ///
    /// The emulation thread keeps writing into this Image, so anything that
    /// outlives the current frame — an encode, a worker task — must take a
    /// duplicate() rather than hold the reference.
    godot::Ref<godot::Image> GetImage() const { return m_image; }

    void SetImageFormat(godot::Image::Format format);
    void CreateTexture(int32_t width, int32_t height, godot::Image::Format image_format, godot::PackedByteArray pixel_data, bool flip_y);
    void UpdateTexture(godot::PackedByteArray pixel_data, int32_t width, int32_t height, bool flip_y);

    bool SetRotation(uint32_t rotation);
    bool GetOverscan(bool* overscan);
    bool GetCanDupe(bool* can_dupe);
    bool SetPixelFormat(const retro_pixel_format* pixel_format);
    bool SetGeometry(const retro_game_geometry* geometry);
    bool SetHwRender(retro_hw_render_callback* hw_render_callback);
    bool GetPreferredHwRender(retro_hw_context_type* hw_context_type) const;

    /// The core wants to create further contexts that share objects with ours —
    /// Dolphin's async shader compiler does, and refusing costs it the compiler
    /// worker thread entirely. Only answered for the GL context types, where a
    /// context is a legal share parent; a shared Vulkan context means something
    /// else and is not offered. Asked after SetHwRender, so the type is known.
    bool SetSharedContext() const;

    /// Invoke the core's context_destroy callback. Must run on the emulation
    /// thread before retro_unload_game (RetroArch's ordering), since Vulkan cores
    /// like paraLLEl-RDP free all their VkDevice objects only in this callback.
    void NotifyContextDestroy(bool preserve_callback = false);

    void SetNegotiationInterface(retro_hw_render_context_negotiation_interface_vulkan* iface)
    {
        m_negotiation_iface = iface;
    }

    retro_hw_render_interface_vulkan* GetVulkanInterface()
    {
        return m_vulkan_ctx ? m_vulkan_ctx->GetInterface() : nullptr;
    }

    /// Whether a hardware-rendered frame arrives bottom-up and has to be
    /// flipped for Godot. True only for GL, whose framebuffers are bottom-left
    /// origin; Vulkan and both D3D APIs are already top-down.
    ///
    /// This is only correct as long as the readback reads the framebuffer the
    /// frontend handed the core -- see the bind in RefreshCallback. A picture
    /// that comes out upside down on GL is that binding, not this rule.
    bool HwFrameNeedsFlip() const;

    /// The render interface for whichever hardware context this core got, as
    /// the opaque type RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE hands back.
    /// Every retro_hw_render_interface_* begins with type + version, so the
    /// core validates it before touching anything API-specific.
    const retro_hw_render_interface* GetHwRenderInterface() const;

    /// What GET_PREFERRED_HW_RENDER answers. Cores that support several APIs
    /// pick this one first, so it decides whether a core takes its Vulkan,
    /// D3D11 or D3D12 path.
    static void SetPreferredHwRender(retro_hw_context_type type);
    static retro_hw_context_type GetPreferredHwRenderType();

private:
    // Size of the frame the emulation thread produced last. Emulation-thread
    // state: it decides there whether a frame needs a new texture or fits the
    // existing one. The main thread must not read it to size a queued frame:
    // by the time a queued frame executes, this has already moved on.
    uint32_t m_last_width = 0;
    uint32_t m_last_height = 0;
    godot::Image::Format m_image_format;
    godot::Ref<godot::Image> m_image = nullptr;
    godot::Ref<godot::ImageTexture> m_texture = nullptr;
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    SDL_Window* m_sdl_window = nullptr;
    SDL_GLContext m_sdl_gl_context = nullptr;
#elif defined(__ANDROID__)
    EGLDisplay m_egl_display = EGL_NO_DISPLAY;
    EGLContext m_egl_context = EGL_NO_CONTEXT;
    EGLSurface m_egl_surface = EGL_NO_SURFACE;
#endif

    std::unique_ptr<VulkanContext> m_vulkan_ctx;
#ifdef _WIN32
    std::unique_ptr<D3D11Context> m_d3d11_ctx;
    std::unique_ptr<D3D12Context> m_d3d12_ctx;
#endif
    retro_hw_render_context_negotiation_interface_vulkan* m_negotiation_iface = nullptr;

    uint32_t m_rotation = 0;
    retro_hw_context_reset_t m_context_reset = nullptr;
    // Libretro specifies 0RGB1555 until a core explicitly negotiates another
    // format. Cores are not required to call SET_PIXEL_FORMAT.
    retro_pixel_format m_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_hw_context_reset_t m_context_destroy = nullptr;
    retro_hw_context_type m_hw_context_type = RETRO_HW_CONTEXT_NONE;
    bool m_context_active = false;

    // Last geometry seen by SetGeometry, so a core that re-reports the SAME
    // geometry (azahar does this on some inputs) doesn't spam an identical log.
    unsigned m_last_geom_w = 0;
    unsigned m_last_geom_h = 0;
    float    m_last_geom_aspect = 0.0f;

    void QueueFrame(Wrapper* wrapper, godot::PackedByteArray pixel_data,
                    uint32_t width, uint32_t height, bool flip_y);
    void DestroyHwRenderContext();
};
}
