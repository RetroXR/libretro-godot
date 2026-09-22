#include "VideoHandler.hpp"

#include <godot_cpp/classes/mesh_instance3d.hpp>

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_video.h>
#elif defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#endif

#include "Wrapper.hpp"
#include "Debug.hpp"
#ifdef _WIN32
#include "D3D11Context.hpp"
#include "D3D12Context.hpp"
#endif

#include "PixelSwizzle.hpp"

#include <gfx/scaler/pixconv.h>

#include <atomic>
#include <chrono>
#include <utility>

using namespace godot;

namespace Xenu
{
// Out of line: the unique_ptr members hold types this header only forward
// declares, so the destructor has to be emitted where they are complete.
VideoHandler::VideoHandler() = default;
VideoHandler::~VideoHandler() = default;

void VideoHandler::RefreshCallback(const void* data, uint32_t width, uint32_t height, size_t pitch)
{
    auto instance = Wrapper::GetCurrentThreadWrapper();
    if (!instance)
    {
        LogError("RefreshCallback: Null Instance.");
        return;
    }

    auto complete_vulkan_frame = [&instance]() {
        if (instance->m_video_handler->m_vulkan_ctx)
            instance->m_video_handler->m_vulkan_ctx->CompleteFrameWithoutReadback();
    };

    // set_signal_semaphore is consumed by the next video_refresh and must be
    // signalled even for a duplicated/empty frame. Returning here without a
    // queue submission can leave the core waiting forever before its next run.
    if (!data || width == 0 || height == 0)
    {
        complete_vulkan_frame();
        return;
    }

    // Rollback replay: skip texture uploads for intermediate replayed frames
    // (only the final frame of the replay refreshes the screen).
    if (instance->IsNetplayReplayVideoMuted())
    {
        complete_vulkan_frame();
        return;
    }

    PackedByteArray pixel_data;

#if defined(_WIN32) || defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__)
    if (data == RETRO_HW_FRAME_BUFFER_VALID)
    {
        pixel_data.resize((int64_t)width * height * 4);

        if (instance->m_video_handler->m_vulkan_ctx)
        {
            // Vulkan path: readback from Vulkan image via staging buffer.
            // False is a dropped frame, and pixel_data is then still the buffer
            // resized above, which Godot zero-fills - so uploading it would
            // black the screen for a frame. Keep the last good frame instead.
            if (!instance->m_video_handler->m_vulkan_ctx->ReadbackToPixels(width, height, pixel_data))
                return;
        }
#ifdef _WIN32
        else if (instance->m_video_handler->m_d3d11_ctx)
        {
            instance->m_video_handler->m_d3d11_ctx->ReadbackToPixels(width, height, pixel_data);
        }
        else if (instance->m_video_handler->m_d3d12_ctx)
        {
            instance->m_video_handler->m_d3d12_ctx->ReadbackToPixels(width, height, pixel_data);
        }
#endif
        else
        {
#ifdef __APPLE__
            LogError("Hardware frame arrived without a Vulkan context on macOS.");
            return;
#else
            // OpenGL path: read pixels from the current framebuffer.
            //
            // Deliberately NOT followed by a buffer swap. get_current_framebuffer()
            // hands the core FBO 0, the back buffer of a hidden, never-presented
            // window that exists only to own the GL context. Swapping it would
            // rotate front/back under the core, so it would draw each frame into
            // the buffer holding the frame before last. A core that repaints
            // every pixel would not notice; ScummVM updates only dirty
            // rectangles, so its picture would alternate between two half-stale
            // images.
            // Read the framebuffer we HANDED the core, not whichever one it
            // happened to leave bound. A core is under no obligation to restore
            // the binding, and GLideN64 (ParaLLEl N64) returns from retro_run
            // with one of its own FBOs still current. Reading that gave a
            // top-down image, which the bottom-up flip below then turned upside
            // down -- the same game was correct on that core's software and
            // Vulkan renderers, which take neither this path nor the flip.
            //
            // Desktop GL: glBindFramebuffer is not in the GL 1.1 headers
            // SDL_opengl.h exposes, so it is resolved once at runtime. A failed
            // lookup reads whatever is bound -- the old behaviour, not a black
            // screen. GLES3 on Android declares it outright.
#if defined(__ANDROID__)
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
#else
            using BindFbo = void(APIENTRY*)(GLenum, GLuint);
            static BindFbo bind_fbo =
                reinterpret_cast<BindFbo>(SDL_GL_GetProcAddress("glBindFramebuffer"));
            if (bind_fbo)
                bind_fbo(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
#endif
            glReadPixels(0, 0, (int)width, (int)height, GL_RGBA, GL_UNSIGNED_BYTE, pixel_data.ptrw());
#endif
        }

        // Vulkan/D3D images are top-down; GL framebuffers are bottom-up.
        instance->m_video_handler->QueueFrame(instance, pixel_data, width, height,
            instance->m_video_handler->HwFrameNeedsFlip());

        return;
    }
#endif

    switch (instance->m_video_handler->m_pixel_format)
    {
    case RETRO_PIXEL_FORMAT_XRGB8888:
    {
        pixel_data.resize(width * height * 4);

        uint8_t* dst = pixel_data.ptrw();

        Xrgb8888ToRgba8(dst, data, width, height,
                        static_cast<size_t>(width) * 4, pitch);

        instance->m_video_handler->QueueFrame(instance, pixel_data, width, height, false);
    }
    break;
    case RETRO_PIXEL_FORMAT_RGB565:
    {
        pixel_data.resize(width * height * 4);

        const uint16_t* src = static_cast<const uint16_t*>(data);
        uint8_t* dst = pixel_data.ptrw();

        conv_rgb565_abgr8888(dst, src, width, height, width * 4, pitch);

        instance->m_video_handler->QueueFrame(instance, pixel_data, width, height, false);
    }
    break;
    case RETRO_PIXEL_FORMAT_0RGB1555:
    {
        pixel_data.resize(width * height * 4);

        uint8_t* dst = pixel_data.ptrw();

        Xrgb1555ToRgba8(dst, data, width, height,
                        static_cast<size_t>(width) * 4, pitch);

        instance->m_video_handler->QueueFrame(instance, pixel_data, width, height, false);
    }
    break;
    case RETRO_PIXEL_FORMAT_UNKNOWN:
    default:
        LogError("Unhandled pixel format: " + std::to_string(instance->m_video_handler->m_pixel_format));
        return;
    }
}

bool VideoHandler::HwFrameNeedsFlip() const
{
    if (m_vulkan_ctx)
        return false;
#ifdef _WIN32
    if (m_d3d11_ctx || m_d3d12_ctx)
        return false;
#endif
    return true;
}

void VideoHandler::QueueFrame(Wrapper* wrapper, PackedByteArray pixel_data,
                              uint32_t width, uint32_t height, bool flip_y)
{
    const uint32_t rotation = m_rotation & 3u;
    uint32_t output_width = (rotation & 1u) ? height : width;
    uint32_t output_height = (rotation & 1u) ? width : height;

    if (flip_y || rotation != 0)
    {
        PackedByteArray transformed;
        transformed.resize(static_cast<int64_t>(output_width) * output_height * 4);
        const uint8_t* src = pixel_data.ptr();
        uint8_t* dst = transformed.ptrw();

        // Unrotated, the transform only reorders whole rows - it never moves a
        // pixel within its row - so it is a row-order reversal rather than a
        // per-pixel walk. Worth separating: HwFrameNeedsFlip() is true for the
        // GL and EGL paths, so this is the case every OpenGL hardware frame
        // takes, and a 1280x720 frame is 720 memcpys instead of 921,600 trips
        // through the loop below.
        if (rotation == 0)
        {
            const size_t row_bytes = static_cast<size_t>(width) * 4;
            for (uint32_t y = 0; y < height; ++y)
                memcpy(dst + static_cast<size_t>(y) * row_bytes,
                       src + static_cast<size_t>(height - 1 - y) * row_bytes,
                       row_bytes);
        }
        else
        {
            // A rotation genuinely gathers, so this stays a per-pixel walk. Two
            // things it no longer does: switch on the rotation in its innermost
            // body, where the branch was loop-invariant and blocked any widening
            // of the copy, and move the pixel a byte at a time when it is 32 bits
            // that are already 4-byte aligned on both sides.
            const uint32_t* s32 = reinterpret_cast<const uint32_t*>(src);
            uint32_t*       d32 = reinterpret_cast<uint32_t*>(dst);

            for (uint32_t y = 0; y < output_height; ++y)
            {
                uint32_t* out_row = d32 + static_cast<size_t>(y) * output_width;

                // Per row, one of the two source coordinates is fixed; only the
                // other tracks x. Hoisted so the inner loop is a plain stride.
                switch (rotation)
                {
                case 1: // 90 degrees counter-clockwise in libretro coordinates
                {
                    const uint32_t sx = width - 1 - y;
                    for (uint32_t x = 0; x < output_width; ++x)
                    {
                        const uint32_t sy = flip_y ? height - 1 - x : x;
                        out_row[x] = s32[static_cast<size_t>(sy) * width + sx];
                    }
                    break;
                }
                case 2:
                {
                    const uint32_t ry = height - 1 - y;
                    const uint32_t sy = flip_y ? height - 1 - ry : ry;
                    const uint32_t* in_row = s32 + static_cast<size_t>(sy) * width;
                    for (uint32_t x = 0; x < output_width; ++x)
                        out_row[x] = in_row[width - 1 - x];
                    break;
                }
                case 3:
                {
                    const uint32_t sx = y;
                    for (uint32_t x = 0; x < output_width; ++x)
                    {
                        const uint32_t ry = height - 1 - x;
                        const uint32_t sy = flip_y ? height - 1 - ry : ry;
                        out_row[x] = s32[static_cast<size_t>(sy) * width + sx];
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
        pixel_data = std::move(transformed);
    }

    // These dimensions are emulation-thread-owned. Avoid consulting m_image or
    // m_image_format here: both Godot Refs are created on the main thread.
    if (output_width != m_last_width || output_height != m_last_height)
    {
        m_last_width = output_width;
        m_last_height = output_height;
        wrapper->CreateTexture(Image::FORMAT_RGBA8, pixel_data,
            static_cast<int32_t>(output_width), static_cast<int32_t>(output_height), false);
    }
    else
    {
        wrapper->UpdateTexture(pixel_data,
            static_cast<int32_t>(output_width), static_cast<int32_t>(output_height), false);
    }
}

const retro_hw_render_interface* VideoHandler::GetHwRenderInterface() const
{
#ifdef _WIN32
    if (m_d3d11_ctx)
        return reinterpret_cast<const retro_hw_render_interface*>(m_d3d11_ctx->GetInterface());
    if (m_d3d12_ctx)
        return reinterpret_cast<const retro_hw_render_interface*>(m_d3d12_ctx->GetInterface());
#endif
    if (m_vulkan_ctx)
        return reinterpret_cast<const retro_hw_render_interface*>(m_vulkan_ctx->GetInterface());
    return nullptr;
}

namespace
{
// Read by GetPreferredHwRender on the emulation thread, written from GDScript
// before StartContent spins that thread up.
std::atomic<retro_hw_context_type> g_preferred_hw_render{ RETRO_HW_CONTEXT_VULKAN };
}

void VideoHandler::SetPreferredHwRender(retro_hw_context_type type)
{
    g_preferred_hw_render.store(type, std::memory_order_relaxed);
}

retro_hw_context_type VideoHandler::GetPreferredHwRenderType()
{
    return g_preferred_hw_render.load(std::memory_order_relaxed);
}

uintptr_t VideoHandler::HwRenderGetCurrentFramebuffer()
{
    return 0;
}

retro_proc_address_t VideoHandler::HwRenderGetProcAddress(const char* sym)
{
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    return reinterpret_cast<retro_proc_address_t>(SDL_GL_GetProcAddress(sym));
#elif defined(__ANDROID__)
    return reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(sym));
#else
    return nullptr;
#endif
}

void VideoHandler::Init()
{
    // Nothing to set up for the picture any more. This used to build an emissive
    // material and install it on a mesh the frontend handed over — and, because a
    // material installed once is the last word, everything that later wanted to
    // show or hide a machine had to be sequenced around that single write. The
    // frame goes into m_texture; whoever displays it asks for it.
}

void VideoHandler::DeInit()
{
    if (m_texture.is_valid())
        m_texture.unref();

    if (m_image.is_valid())
        m_image.unref();

    DestroyHwRenderContext();
}

void VideoHandler::DestroyHwRenderContext()
{
    // Destroy Vulkan context before platform-specific GL teardown.
    if (m_vulkan_ctx)
    {
        m_vulkan_ctx->Destroy();
        m_vulkan_ctx.reset();
    }

#ifdef _WIN32
    if (m_d3d11_ctx)
    {
        m_d3d11_ctx->Destroy();
        m_d3d11_ctx.reset();
    }
    if (m_d3d12_ctx)
    {
        m_d3d12_ctx->Destroy();
        m_d3d12_ctx.reset();
    }
#endif

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    if (m_sdl_gl_context)
    {
        SDL_GL_DestroyContext(m_sdl_gl_context);
        m_sdl_gl_context = nullptr;
    }

    if (m_sdl_window)
    {
        SDL_DestroyWindow(m_sdl_window);
        m_sdl_window = nullptr;
    }
#elif defined(__ANDROID__)
    if (m_egl_display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (m_egl_context != EGL_NO_CONTEXT)
        {
            eglDestroyContext(m_egl_display, m_egl_context);
            m_egl_context = EGL_NO_CONTEXT;
        }

        if (m_egl_surface != EGL_NO_SURFACE)
        {
            eglDestroySurface(m_egl_display, m_egl_surface);
            m_egl_surface = EGL_NO_SURFACE;
        }

        eglTerminate(m_egl_display);
        m_egl_display = EGL_NO_DISPLAY;
    }
#endif
}

void VideoHandler::NotifyContextDestroy(bool preserve_callback)
{
    // Runs on the emulation thread, before retro_unload_game/retro_deinit,
    // while the GL/Vulkan context is still current on this thread. Matches
    // RetroArch's teardown order so cores can free API objects they own.
    if (m_context_active && m_context_destroy)
        m_context_destroy();
    m_context_active = false;
    if (!preserve_callback)
        m_context_destroy = nullptr;
}

bool VideoHandler::ReinitHwRenderContext(int32_t width, int32_t height)
{
    // Software-rendered cores have no frontend context to rebuild.
    if (!m_context_reset)
        return true;

    NotifyContextDestroy(true);
    DestroyHwRenderContext();
    return InitHwRenderContext(width, height);
}

bool VideoHandler::InitHwRenderContext(int32_t width, int32_t height)
{
    if (!m_context_reset)
    {
        Log("Software rendering: core requested no hardware context (" +
            std::to_string(width) + "x" + std::to_string(height) + ")");
        return true;
    }

#ifdef _WIN32
    if (m_hw_context_type == RETRO_HW_CONTEXT_D3D11)
    {
        if (m_d3d11_ctx)
            return true;
        Log("Creating D3D11 context...");
        m_d3d11_ctx = std::make_unique<D3D11Context>();
        if (!m_d3d11_ctx->Init())
        {
            LogError("D3D11Context::Init failed.");
            m_d3d11_ctx.reset();
            return false;
        }
        m_context_reset();
        m_context_active = true;
        return true;
    }

    if (m_hw_context_type == RETRO_HW_CONTEXT_D3D12)
    {
        if (m_d3d12_ctx)
            return true;
        Log("Creating D3D12 context...");
        m_d3d12_ctx = std::make_unique<D3D12Context>();
        if (!m_d3d12_ctx->Init())
        {
            LogError("D3D12Context::Init failed.");
            m_d3d12_ctx.reset();
            return false;
        }
        m_context_reset();
        m_context_active = true;
        return true;
    }
#endif

    // ---- Vulkan path (both platforms) ----
    if (m_hw_context_type == RETRO_HW_CONTEXT_VULKAN)
    {
        if (m_vulkan_ctx)
        {
            Log("Vulkan context already initialized, skipping.");
            return true;
        }
        Log("Creating Vulkan context...");
        m_vulkan_ctx = std::make_unique<VulkanContext>();
        // The frame size reaches the context now: it sizes the surface a core
        // like Dolphin builds its swapchain from, and therefore the size of the
        // frame it presents back. It used to be dropped here.
        if (!m_vulkan_ctx->Init(m_negotiation_iface, width, height))
        {
            LogError("VulkanContext::Init failed.");
            m_vulkan_ctx.reset();
            return false;
        }
        LogOK("Vulkan context created successfully.");
        m_context_reset();
        m_context_active = true;
        return true;
    }

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    Log("Creating OpenGL context...");

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        LogError("Failed to initialize SDL video subsystem: " + std::string(SDL_GetError()));
        return false;
    }

    m_sdl_window = SDL_CreateWindow("Libretro OpenGL Window", width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!m_sdl_window)
    {
        LogError("Failed to create SDL window: " + std::string(SDL_GetError()));
        return false;
    }

    m_sdl_gl_context = SDL_GL_CreateContext(m_sdl_window);
    if (!m_sdl_gl_context)
    {
        LogError("Failed to create SDL OpenGL context: " + std::string(SDL_GetError()));
        SDL_DestroyWindow(m_sdl_window);
        m_sdl_window = nullptr;
        return false;
    }

    LogOK("OpenGL context created successfully.");

    if (!SDL_GL_MakeCurrent(m_sdl_window, m_sdl_gl_context))
    {
        LogError("Failed to make OpenGL context current: " + std::string(SDL_GetError()));
        return false;
    }

    m_context_reset();
    m_context_active = true;

    return true;
#elif defined(__ANDROID__)
    Log("Creating OpenGL ES context...");

    m_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_egl_display == EGL_NO_DISPLAY)
    {
        LogError("Failed to get EGL display.");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(m_egl_display, &major, &minor))
    {
        LogError("Failed to initialize EGL.");
        m_egl_display = EGL_NO_DISPLAY;
        return false;
    }

    Log("EGL version: " + std::to_string(major) + "." + std::to_string(minor));

    bool use_gles2 = (m_hw_context_type == RETRO_HW_CONTEXT_OPENGLES2);
    EGLint renderable_type = use_gles2 ? EGL_OPENGL_ES2_BIT : 0x00000040; // EGL_OPENGL_ES3_BIT_KHR
    EGLint context_version = use_gles2 ? 2 : 3;

    Log("Requesting GLES " + std::to_string(context_version) + " context");

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, renderable_type,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(m_egl_display, config_attribs, &config, 1, &num_configs) || num_configs == 0)
    {
        LogError("Failed to choose EGL config.");
        eglTerminate(m_egl_display);
        m_egl_display = EGL_NO_DISPLAY;
        return false;
    }

    const EGLint pbuffer_attribs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_NONE
    };

    m_egl_surface = eglCreatePbufferSurface(m_egl_display, config, pbuffer_attribs);
    if (m_egl_surface == EGL_NO_SURFACE)
    {
        LogError("Failed to create EGL pbuffer surface.");
        eglTerminate(m_egl_display);
        m_egl_display = EGL_NO_DISPLAY;
        return false;
    }

    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, context_version,
        EGL_NONE
    };

    m_egl_context = eglCreateContext(m_egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (m_egl_context == EGL_NO_CONTEXT)
    {
        LogError("Failed to create EGL context.");
        eglDestroySurface(m_egl_display, m_egl_surface);
        m_egl_surface = EGL_NO_SURFACE;
        eglTerminate(m_egl_display);
        m_egl_display = EGL_NO_DISPLAY;
        return false;
    }

    if (!eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context))
    {
        LogError("Failed to make EGL context current.");
        eglDestroyContext(m_egl_display, m_egl_context);
        m_egl_context = EGL_NO_CONTEXT;
        eglDestroySurface(m_egl_display, m_egl_surface);
        m_egl_surface = EGL_NO_SURFACE;
        eglTerminate(m_egl_display);
        m_egl_display = EGL_NO_DISPLAY;
        return false;
    }

    LogOK("OpenGL ES context created successfully.");

    {
        const char* gl_version  = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* gl_renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* gl_glsl     = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        Log(std::string("GL_VERSION: ")  + (gl_version  ? gl_version  : "(null)"));
        Log(std::string("GL_RENDERER: ") + (gl_renderer ? gl_renderer : "(null)"));
        Log(std::string("GL_SHADING_LANGUAGE_VERSION: ") + (gl_glsl ? gl_glsl : "(null)"));

        EGLint surf_w = 0, surf_h = 0;
        eglQuerySurface(m_egl_display, m_egl_surface, EGL_WIDTH, &surf_w);
        eglQuerySurface(m_egl_display, m_egl_surface, EGL_HEIGHT, &surf_h);
        Log("Pbuffer surface: " + std::to_string(surf_w) + "x" + std::to_string(surf_h)
            + " (requested " + std::to_string(width) + "x" + std::to_string(height) + ")");
    }

    m_context_reset();
    m_context_active = true;

    return true;
#else
    LogWarning("Hardware rendering is not supported on this platform.");
    return false;
#endif
}

void VideoHandler::SetImageFormat(Image::Format format)
{
    m_image_format = format;
}

bool VideoHandler::SetRotation(uint32_t rotation)
{
    if (rotation > 3)
    {
        LogError("Invalid libretro rotation: " + std::to_string(rotation));
        return false;
    }
    m_rotation = rotation;
    return true;
}

bool VideoHandler::GetOverscan(bool* overscan)
{
    if (!overscan)
    {
        LogError("Passed null pointer for overscan.");
        return false;
    }

    // bool, as libretro.h declares it: writing an int32 here zeroed the three
    // bytes after a core's flag -- in Beetle Lynx the low byte of its
    // input_state_cb, which became the poll callback and read every button as 0.
    *overscan = false;
    return true;
}

bool VideoHandler::GetCanDupe(bool* can_dupe)
{
    if (!can_dupe)
    {
        LogError("Passed null pointer for can_dupe.");
        return false;
    }

    *can_dupe = true;
    return true;
}

bool VideoHandler::SetPixelFormat(const retro_pixel_format* pixel_format)
{
    if (!pixel_format)
        return false;
        
    auto pf = *pixel_format;

    switch (pf)
    {
    case RETRO_PIXEL_FORMAT_0RGB1555:
        Log("Using RETRO_PIXEL_FORMAT_0RGB1555");
        break;
    case RETRO_PIXEL_FORMAT_XRGB8888:
        Log("Using RETRO_PIXEL_FORMAT_XRGB8888");
        break;
    case RETRO_PIXEL_FORMAT_RGB565:
        Log("Using RETRO_PIXEL_FORMAT_RGB565");
        break;
    case RETRO_PIXEL_FORMAT_UNKNOWN:
    default:
        LogError("Unhandled pixel format: " + std::to_string(pf));
        return false;
    }

    m_pixel_format = pf;
    return true;
}

bool VideoHandler::SetGeometry(const retro_game_geometry* geometry)
{
    // TODO: Handle geometry changes when not applied to a 3D model (ie. Sprite2D)

    if (!geometry)
        return true;

    // A core may re-report unchanged geometry repeatedly (azahar does so when the
    // R button toggles its layout), which produced an identical log each time.
    // Only log when the geometry actually changes.
    if (geometry->base_width != m_last_geom_w ||
        geometry->base_height != m_last_geom_h ||
        geometry->aspect_ratio != m_last_geom_aspect)
    {
        m_last_geom_w = geometry->base_width;
        m_last_geom_h = geometry->base_height;
        m_last_geom_aspect = geometry->aspect_ratio;

        Log(std::to_string(geometry->base_width) + "x" + std::to_string(geometry->base_height) +
            " @ " + std::to_string(geometry->max_width) + "x" + std::to_string(geometry->max_height) +
            " (aspect ratio: " + std::to_string(geometry->aspect_ratio) + ")");
    }

    return true;
}

bool VideoHandler::SetHwRender(retro_hw_render_callback* hw_render_callback)
{
    if (!hw_render_callback)
    {
        LogError("Passed null retro_hw_render_callback");
        return false;
    }

    Log("Setting hardware render callback...");

    Log("Context type: " + std::to_string(hw_render_callback->context_type));

    // Logged, not acted on. Every GL core here declares bottom-left, which is
    // the GL default and what the readback assumes; it is worth printing
    // because a core that ever declares otherwise would make an upside-down
    // picture explainable in one line instead of a rebuild.
    Log(std::string("Declared frame origin: ")
        + (hw_render_callback->bottom_left_origin ? "bottom-left" : "top-left"));

    bool supported = false;
    switch (hw_render_callback->context_type)
    {
#if defined(__ANDROID__)
    case RETRO_HW_CONTEXT_OPENGLES2:
    case RETRO_HW_CONTEXT_OPENGLES3:
    case RETRO_HW_CONTEXT_OPENGLES_VERSION:
#elif defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    case RETRO_HW_CONTEXT_OPENGL:
    case RETRO_HW_CONTEXT_OPENGL_CORE:
#endif
#ifdef _WIN32
    case RETRO_HW_CONTEXT_D3D11:
    case RETRO_HW_CONTEXT_D3D12:
#endif
    case RETRO_HW_CONTEXT_VULKAN:
        supported = true;
        break;
    default:
        break;
    }

    if (!supported)
    {
        LogError("Unsupported context type: " + std::to_string(hw_render_callback->context_type));
        return false;
    }

    m_context_reset = hw_render_callback->context_reset;
    m_context_destroy = hw_render_callback->context_destroy;
    m_hw_context_type = hw_render_callback->context_type;

    hw_render_callback->get_current_framebuffer = VideoHandler::HwRenderGetCurrentFramebuffer;
    hw_render_callback->get_proc_address = VideoHandler::HwRenderGetProcAddress;

    // Vulkan context creation is deferred to InitHwRenderContext (called from
    // EmulationThreadLoop after retro_load_game returns).  Cores like Dolphin
    // call SET_HW_RENDER *before* SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
    // so the negotiation interface is not yet available here.

    return true;
}

bool VideoHandler::SetSharedContext() const
{
    switch (m_hw_context_type)
    {
    case RETRO_HW_CONTEXT_OPENGL:
    case RETRO_HW_CONTEXT_OPENGL_CORE:
    case RETRO_HW_CONTEXT_OPENGLES2:
    case RETRO_HW_CONTEXT_OPENGLES3:
    case RETRO_HW_CONTEXT_OPENGLES_VERSION:
        Log("Shared context granted for context type: " + std::to_string(m_hw_context_type));
        return true;
    default:
        return false;
    }
}

bool VideoHandler::GetPreferredHwRender(retro_hw_context_type* hw_context_type) const
{
    if (!hw_context_type)
        return false;

#ifdef __APPLE__
    // Metal is not a libretro context type. MoltenVK is the native macOS
    // hardware path, so never advertise a preference that this build cannot
    // create even if another platform left a process-wide override behind.
    *hw_context_type = RETRO_HW_CONTEXT_VULKAN;
    return true;
#endif

    // Vulkan unless GDScript asked for something else. Cores that support the
    // answer select it; the rest fall back to their own order via SET_HW_RENDER.
    *hw_context_type = GetPreferredHwRenderType();
    return true;
}

void VideoHandler::CreateTexture(int32_t width, int32_t height, Image::Format image_format, PackedByteArray pixel_data, bool flip_y)
{
    m_image.instantiate();
    m_image->create(width, height, false, image_format);
    m_image->set_data(width, height, false, image_format, pixel_data);
    if (flip_y)
        m_image->flip_y();

    // A NEW texture object, which is why a display samples GetTexture() per frame
    // rather than holding on to what it got.
    m_texture = ImageTexture::create_from_image(m_image);
}

void VideoHandler::UpdateTexture(PackedByteArray pixel_data, int32_t width, int32_t height, bool flip_y)
{
    if (m_image.is_null())
        return;

    // Judged against the image, using the size these pixels were captured at.
    // A frame queued before a resolution change no longer describes the image
    // it lands on; the CreateTexture behind it in the queue is the one that
    // resizes, so drop this rather than push 640x480 bytes at a 256x240 image.
    if (width != m_image->get_width() || height != m_image->get_height())
        return;

    m_image->set_data(width, height, false, m_image->get_format(), pixel_data);
    if (flip_y)
        m_image->flip_y();

    if (!m_image->is_empty() && m_texture.is_valid())
        m_texture->update(m_image);
}
}
