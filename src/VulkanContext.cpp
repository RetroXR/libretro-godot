#include "VulkanContext.hpp"
#include "VideoTransferStats.hpp"
#include "Debug.hpp"
#include "PixelSwizzle.hpp"

#include <cstring>
#include <utility>
#include <vector>

// Must precede <windows.h> below: godot-cpp's heavy template/object headers
// don't play well with windows.h's macro soup (min/max/interface/etc.) once
// windows.h has already been included.
#include <godot_cpp/classes/os.hpp>

#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#include "DynLib.hpp"
#endif

#ifdef __APPLE__
#include <vulkan/vulkan_metal.h>
#include "MacMetalLayer.hpp"
#endif

// Flip this to 1 and rebuild to opt back into Vulkan validation. Off by
// default: the editor process is always debug-tagged, so OS::is_debug_build()
// below cannot tell "editor Play" apart from "real debug testing", and gating
// validation on that check alone leaves it always on in the editor, adding
// per-call CPU overhead on this hot HW-render path.
#define VULKAN_VALIDATION_ENABLED 0

using namespace godot;

namespace Xenu
{

#ifdef __ANDROID__
// AImageReader hands out an ANativeWindow with no JNI or Java Surface
// plumbing. The NDK only declares it at API 24+, so it's resolved out of
// libmediandk.so at runtime instead, which keeps the build's API level from
// deciding whether Vulkan cores can get a surface.
namespace
{
struct AImageReaderOpaque;

using PFN_AImageReader_new =
    int32_t (*)(int32_t width, int32_t height, int32_t format, int32_t max_images,
                AImageReaderOpaque** reader);
using PFN_AImageReader_newWithUsage =
    int32_t (*)(int32_t width, int32_t height, int32_t format, uint64_t usage,
                int32_t max_images, AImageReaderOpaque** reader);
using PFN_AImageReader_getWindow = int32_t (*)(AImageReaderOpaque* reader, ANativeWindow** window);
using PFN_AImageReader_delete    = void (*)(AImageReaderOpaque* reader);

constexpr int32_t  kAImageFormatRGBA8888   = 0x1;
constexpr uint64_t kAHBUsageGpuSampled     = 1ULL << 8;
constexpr uint64_t kAHBUsageGpuFramebuffer = 1ULL << 9;
} // namespace
#endif

// A frame's GPU work is microseconds; anything approaching this means a
// signal was lost. Bounded so that costs a dropped frame instead of a
// permanently frozen app.
static constexpr uint64_t kFenceTimeoutNs = 2'000'000'000ull;

// ---------------------------------------------------------------------------
// Static callback trampolines
// ---------------------------------------------------------------------------

void VulkanContext::s_SetImage(void* handle, const retro_vulkan_image* image,
                               uint32_t n_sems, const VkSemaphore* sems, uint32_t src_family)
{
    static_cast<VulkanContext*>(handle)->SetImage(image, n_sems, sems, src_family);
}

uint32_t VulkanContext::s_GetSyncIndex(void* /*handle*/)
{
    return 0;
}

uint32_t VulkanContext::s_GetSyncIndexMask(void* /*handle*/)
{
    return 0x1;
}

void VulkanContext::s_SetCommandBuffers(void* handle, uint32_t num_cmd, const VkCommandBuffer* cmd)
{
    auto* ctx = static_cast<VulkanContext*>(handle);
    if (num_cmd == 0)
        return;
    if (!cmd)
    {
        LogError("VulkanContext: set_command_buffers received a null command-buffer array.");
        return;
    }

    // Multiple calls before video_refresh are legal. Preserve their order and
    // submit all of them before the frontend's readback command buffer.
    std::lock_guard<std::mutex> lock(ctx->m_state_mutex);
    ctx->m_pending_command_buffers.insert(ctx->m_pending_command_buffers.end(),
                                           cmd, cmd + num_cmd);
}

void VulkanContext::s_WaitSyncIndex(void* handle)
{
    auto* ctx = static_cast<VulkanContext*>(handle);
    // The core calls this before it starts rendering into the image, to
    // make sure nothing is still reading from it. The only GPU work WE ever
    // submit against that image is the ReadbackToPixels copy, already
    // tracked by m_fence. Waiting on just that (instead of a full
    // vkDeviceWaitIdle, which stalls every queue on the entire device) gives
    // the same guarantee without serializing the whole GPU every frame.
    if (ctx->m_device == VK_NULL_HANDLE || ctx->m_fence == VK_NULL_HANDLE)
        return;

    std::lock_guard<std::mutex> lock(ctx->m_fence_mutex);
    // The core is about to draw into the image we read from. If the wait times
    // out the copy is still in flight, but this callback has no way to say so:
    // returning without waiting is what the core takes as a yes. Report it
    // rather than swallow it.
    if (!ctx->WaitReadbackFenceLocked() && ctx->m_readback_pending)
        LogWarning("VulkanContext: core is reusing the image while our copy is "
                   "still running; expect a torn frame.");
}

void VulkanContext::s_LockQueue(void* handle)
{
    static_cast<VulkanContext*>(handle)->m_queue_mutex.lock();
}

void VulkanContext::s_UnlockQueue(void* handle)
{
    static_cast<VulkanContext*>(handle)->m_queue_mutex.unlock();
}

void VulkanContext::s_SetSignalSemaphore(void* handle, VkSemaphore semaphore)
{
    auto* ctx = static_cast<VulkanContext*>(handle);
    std::lock_guard<std::mutex> lock(ctx->m_state_mutex);
    // This is state for the next video_refresh call. A later call replaces it;
    // signalling the replaced semaphore here would tell the core the image is
    // reusable before the frontend has even consumed the frame.
    ctx->m_signal_semaphore = semaphore;
}

// ---------------------------------------------------------------------------
// Instance/device creation wrappers (passed to core's negotiation interface)
// ---------------------------------------------------------------------------

static bool s_NamePresent(const std::vector<const char*>& names, const char* wanted)
{
    for (const char* name : names)
        if (name && std::strcmp(name, wanted) == 0)
            return true;
    return false;
}

static void s_AppendNameUnique(std::vector<const char*>& names, const char* name)
{
    if (!s_NamePresent(names, name))
        names.push_back(name);
}

static bool s_InstanceSupportsExtension(const char* extension)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
        return false;

    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data()) != VK_SUCCESS)
        return false;
    for (const VkExtensionProperties& p : props)
        if (std::strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

static VkInstance s_CreateInstanceWrapper(void* /*opaque*/, const VkInstanceCreateInfo* ci)
{
    VkInstance inst = VK_NULL_HANDLE;
    if (!ci)
        return inst;

#ifdef __APPLE__
    // MoltenVK exposes a portability implementation rather than a conformant
    // native Vulkan device. A v2 core controls VkInstance creation, so patch its
    // request here as well as the frontend-created path below; otherwise the
    // instance succeeds but enumerates no physical devices.
    std::vector<const char*> exts;
    if (ci->ppEnabledExtensionNames)
        exts.assign(ci->ppEnabledExtensionNames,
                    ci->ppEnabledExtensionNames + ci->enabledExtensionCount);

    auto add_if_supported = [&exts](const char* name) {
        if (s_InstanceSupportsExtension(name))
            s_AppendNameUnique(exts, name);
    };
    add_if_supported(VK_KHR_SURFACE_EXTENSION_NAME);
    add_if_supported(VK_EXT_METAL_SURFACE_EXTENSION_NAME);

    VkInstanceCreateInfo patched = *ci;
    if (s_InstanceSupportsExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        s_AppendNameUnique(exts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        patched.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    patched.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    patched.ppEnabledExtensionNames = exts.data();
    vkCreateInstance(&patched, nullptr, &inst);
#else
    vkCreateInstance(ci, nullptr, &inst);
#endif
    return inst;
}

static bool s_DeviceSupportsExtension(VkPhysicalDevice gpu, const char* extension)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    if (count == 0)
        return false;

    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, props.data());
    for (const VkExtensionProperties& p : props)
        if (std::strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

static bool s_DeviceSupportsFault(VkPhysicalDevice gpu)
{
    return s_DeviceSupportsExtension(gpu, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
}

// The core creates the device on v2 negotiation and we only wrap the call, so
// this is the one place an extension can be added to a device we do not own.
//
// VK_EXT_device_fault is added because VK_ERROR_DEVICE_LOST on its own carries no
// information at all: the Adreno GMU reports "GPU hang detected", the device dies,
// and nothing says which submission or why. With the extension enabled,
// vkGetDeviceFaultInfoEXT gives the driver's own description of the fault. It is
// requested unconditionally rather than behind a debug flag: it costs nothing
// until a device is lost, and by then it is the only account of what happened.
static VkDevice s_CreateDeviceWrapper(VkPhysicalDevice gpu, void* /*opaque*/, const VkDeviceCreateInfo* ci)
{
    VkDevice dev = VK_NULL_HANDLE;
    if (!ci)
        return dev;

    std::vector<const char*> exts;
    if (ci->ppEnabledExtensionNames)
        exts.assign(ci->ppEnabledExtensionNames,
                    ci->ppEnabledExtensionNames + ci->enabledExtensionCount);

#ifdef __APPLE__
    // The portability subset must be enabled when the selected device exposes
    // it; omitting an advertised portability subset makes device creation
    // non-conformant.
    constexpr const char* portability_subset = "VK_KHR_portability_subset";
    if (s_DeviceSupportsExtension(gpu, portability_subset))
        s_AppendNameUnique(exts, portability_subset);
#endif

    const bool inject_fault = s_DeviceSupportsFault(gpu)
        && !s_NamePresent(exts, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    if (inject_fault)
    {
        s_AppendNameUnique(exts, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);

        // The extension does nothing unless the feature is enabled too.
        VkPhysicalDeviceFaultFeaturesEXT fault{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
        fault.deviceFault = VK_TRUE;
        fault.pNext       = const_cast<void*>(ci->pNext);

        VkDeviceCreateInfo patched      = *ci;
        patched.pNext                   = &fault;
        patched.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
        patched.ppEnabledExtensionNames = exts.data();

        if (vkCreateDevice(gpu, &patched, nullptr, &dev) == VK_SUCCESS)
            return dev;

        // Never let a diagnostic stop the core from running: fall back to
        // exactly what it asked for.
        LogWarning("VulkanContext: device creation with VK_EXT_device_fault failed; "
                   "retrying without it.");
        dev = VK_NULL_HANDLE;
        for (auto it = exts.begin(); it != exts.end(); ++it)
        {
            if (std::strcmp(*it, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) == 0)
            {
                exts.erase(it);
                break;
            }
        }
    }

    VkDeviceCreateInfo patched = *ci;
    patched.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    patched.ppEnabledExtensionNames = exts.data();
    vkCreateDevice(gpu, &patched, nullptr, &dev);
    return dev;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool VulkanContext::Init(retro_hw_render_context_negotiation_interface_vulkan* neg,
                         int32_t frame_w, int32_t frame_h)
{
    m_negotiation = neg;
    m_frame_w = frame_w > 0 ? frame_w : 640;
    m_frame_h = frame_h > 0 ? frame_h : 480;

    // ---- Create VkInstance ----
    // If the core supplies v2 negotiation with create_instance, let it drive instance creation.
    if (neg && neg->interface_version >= 2 && neg->create_instance)
    {
        const VkApplicationInfo* app_info =
            (neg->get_application_info) ? neg->get_application_info() : nullptr;

        m_negotiation_engaged = true;
        m_instance = neg->create_instance(vkGetInstanceProcAddr, app_info,
                                          s_CreateInstanceWrapper, this);

        if (!m_instance)
            LogWarning("VulkanContext: core's create_instance returned null; using self-created instance");
    }

    if (m_instance == VK_NULL_HANDLE)
    {
        const VkApplicationInfo* app_info_ptr = nullptr;
        VkApplicationInfo app_info{};

        if (neg && neg->get_application_info)
            app_info_ptr = neg->get_application_info();

        // Cores may advertise a low apiVersion (e.g. 1.0) but internally
        // compile shaders targeting a higher SPIR-V version.  Query the
        // driver's maximum supported instance version and use that so the
        // validation environment matches what the core actually needs.
        //
        // The default is 1.0, not 1.2: vkEnumerateInstanceVersion is itself a
        // 1.1 symbol, so its absence means a 1.0 loader, and asking a 1.0
        // loader for 1.2 fails the whole instance with INCOMPATIBLE_DRIVER.
        uint32_t max_api_version = VK_API_VERSION_1_0;
        auto enumVer = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
        if (enumVer)
            enumVer(&max_api_version);

        if (app_info_ptr)
        {
            // Copy the core's app info but override the API version.
            app_info = *app_info_ptr;
            app_info.apiVersion = max_api_version;
            app_info_ptr = &app_info;
        }
        else
        {
            app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app_info.pApplicationName   = "XenuLibretro";
            app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
            app_info.pEngineName        = "XenuLibretro";
            app_info.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
            app_info.apiVersion         = max_api_version;
            app_info_ptr = &app_info;
        }

        VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ici.pApplicationInfo = app_info_ptr;

        // Extensions useful to cores doing Vulkan HW rendering. vkCreateInstance
        // fails outright on a single unsupported name, and
        // VK_KHR_get_surface_capabilities2 is not core-promoted and is absent on
        // some Android drivers, so ask the loader what exists first rather than
        // taking the whole Vulkan path down.
        uint32_t avail_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &avail_count, nullptr);
        std::vector<VkExtensionProperties> avail(avail_count);
        if (avail_count)
            vkEnumerateInstanceExtensionProperties(nullptr, &avail_count, avail.data());

        auto supported = [&avail](const char* name) {
            for (const auto& e : avail)
                if (std::strcmp(e.extensionName, name) == 0)
                    return true;
            return false;
        };

        std::vector<const char*> inst_exts;
        auto want = [&](const char* name, bool required) {
            if (supported(name))
                inst_exts.push_back(name);
            else if (required)
                LogWarning(std::string("VulkanContext: instance extension ") + name
                    + " unavailable; cores needing a surface will fail.");
        };

        want(VK_KHR_SURFACE_EXTENSION_NAME, true);
#ifdef _WIN32
        want(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, true);
#endif
#ifdef __ANDROID__
        want(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, true);
#endif
#ifdef __APPLE__
        want(VK_EXT_METAL_SURFACE_EXTENSION_NAME, true);
        if (supported(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        {
            want(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, true);
            ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif
        want("VK_KHR_get_physical_device_properties2", false);
        want("VK_KHR_get_surface_capabilities2", false);
        want(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false);

        ici.enabledExtensionCount   = static_cast<uint32_t>(inst_exts.size());
        ici.ppEnabledExtensionNames = inst_exts.data();

        // Validation layers, gated on VULKAN_VALIDATION_ENABLED above: validation
        // intercepts and checks every Vulkan call, which is real CPU overhead on
        // this hot per-frame HW-render path.
        const char* validation_layer = "VK_LAYER_KHRONOS_validation";
#if VULKAN_VALIDATION_ENABLED
        if (OS::get_singleton()->is_debug_build())
        {
            ici.enabledLayerCount   = 1;
            ici.ppEnabledLayerNames = &validation_layer;
        }
#endif

        VkResult r = vkCreateInstance(&ici, nullptr, &m_instance);
        if (r != VK_SUCCESS)
        {
            LogError("VulkanContext: vkCreateInstance failed: " + std::to_string(r));
            return false;
        }
    }

    LogOK("VulkanContext: VkInstance created.");

    // ---- Set up validation debug messenger (debug builds only, see above) ----
#if VULKAN_VALIDATION_ENABLED
    if (OS::get_singleton()->is_debug_build())
    {
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger)
        {
            VkDebugUtilsMessengerCreateInfoEXT dbg_ci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            dbg_ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                                   | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            dbg_ci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                   | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbg_ci.pfnUserCallback = [](
                VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                const VkDebugUtilsMessengerCallbackDataEXT* data,
                void* /*user*/) -> VkBool32
            {
                if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                    LogError("VkValidation: " + std::string(data->pMessage));
                else
                    LogWarning("VkValidation: " + std::string(data->pMessage));
                return VK_FALSE;
            };
            createMessenger(m_instance, &dbg_ci, nullptr, &m_debug_messenger);
            LogOK("VulkanContext: Validation layers enabled.");
        }
    }
#endif

    // ---- Select physical device ----
    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(m_instance, &gpu_count, nullptr);
    if (gpu_count == 0)
    {
        LogError("VulkanContext: No GPUs found.");
        return false;
    }

    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(m_instance, &gpu_count, gpus.data());

    m_gpu = VK_NULL_HANDLE;
    for (auto& g : gpus)
    {
        uint32_t qfam_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qfam_count, nullptr);
        std::vector<VkQueueFamilyProperties> qfams(qfam_count);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qfam_count, qfams.data());

        for (uint32_t i = 0; i < qfam_count; ++i)
        {
            constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((qfams[i].queueFlags & required) == required)
            {
                m_gpu          = g;
                m_queue_family = i;
                break;
            }
        }
        if (m_gpu != VK_NULL_HANDLE)
            break;
    }

    if (m_gpu == VK_NULL_HANDLE)
    {
        LogError("VulkanContext: No GPU with graphics+compute queue found.");
        return false;
    }

    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_gpu, &props);
        LogOK("VulkanContext: GPU: " + std::string(props.deviceName));
    }

    // ---- Create a surface for cores that require one (e.g. Dolphin) ----
#ifdef _WIN32
    {
        // Use WS_POPUP so the entire window is client area (no borders/title
        // bar that shrink it at high DPI).
        //
        // Sized to the frame geometry the core declared, not to something
        // comfortably large. This surface is not a viewport, it is the shape of
        // the picture: a core that needs a surface builds its swapchain to fit it
        // and presents a frame that size, whatever its internal resolution
        // (Dolphin renders at its EFB scale and downsamples on the way out).
        // Oversize it and the core presents oversized while still reporting the
        // base size to video_refresh, and since ReadbackToPixels copies the
        // top-left reported-size rectangle the picture becomes a corner of the
        // game. OpenGL is immune because there the core draws into an FBO we
        // size ourselves.
        m_hidden_hwnd = CreateWindowExW(
            0, L"STATIC", L"XenuLibretro_VkSurface", WS_POPUP,
            0, 0, m_frame_w, m_frame_h, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);

        if (m_hidden_hwnd)
        {
            auto createSurface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
                vkGetInstanceProcAddr(m_instance, "vkCreateWin32SurfaceKHR"));
            if (createSurface)
            {
                VkWin32SurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
                sci.hinstance = GetModuleHandleW(nullptr);
                sci.hwnd      = static_cast<HWND>(m_hidden_hwnd);
                VkResult r = createSurface(m_instance, &sci, nullptr, &m_surface);
                if (r != VK_SUCCESS)
                    LogWarning("VulkanContext: vkCreateWin32SurfaceKHR failed: " + std::to_string(r));
                else
                {
                    LogOK("VulkanContext: VkSurfaceKHR created at "
                        + std::to_string(m_frame_w) + "x" + std::to_string(m_frame_h) + ".");
                    // What the core will actually see. Worth printing: if this
                    // disagrees with the frame size reported to video_refresh,
                    // the readback is cropping and the picture will be a corner.
                    VkSurfaceCapabilitiesKHR caps{};
                    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu, m_surface, &caps) == VK_SUCCESS)
                        Log("VulkanContext: surface extent "
                            + std::to_string(caps.currentExtent.width) + "x"
                            + std::to_string(caps.currentExtent.height));
                }
            }
        }
    }
#elif defined(__ANDROID__)
    {
        // Nothing is ever presented to this surface: PPSSPP fakes its whole
        // swapchain and delivers frames through set_image. It still runs its
        // full desktop bring-up on whatever handle create_device is given, and
        // its hooked vkCreate*SurfaceKHR hands ours straight back, so a null
        // surface lets ReinitSurface() "succeed" and ChooseQueue() then aborts
        // the process querying formats for it. An offscreen ImageReader window
        // is all the surface has to be.
        m_mediandk = DynLib_Open("libmediandk.so");
        if (!m_mediandk)
        {
            LogWarning("VulkanContext: libmediandk.so unavailable; no VkSurfaceKHR.");
        }
        else
        {
            auto readerNew = reinterpret_cast<PFN_AImageReader_new>(
                DynLib_Sym(m_mediandk, "AImageReader_new"));
            auto readerNewWithUsage = reinterpret_cast<PFN_AImageReader_newWithUsage>(
                DynLib_Sym(m_mediandk, "AImageReader_newWithUsage"));
            auto readerGetWindow = reinterpret_cast<PFN_AImageReader_getWindow>(
                DynLib_Sym(m_mediandk, "AImageReader_getWindow"));

            // Sized to the declared frame geometry, for the same reason the Windows
            // window is: a core that needs a surface may build its swapchain from
            // it and present a frame that size. Android always reports a concrete
            // currentExtent, so a core that reads it has nothing to choose and
            // takes these dimensions verbatim. A surface smaller than the size
            // reported to video_refresh would have ReadbackToPixels copy the
            // reported rectangle off the end of the image.
            AImageReaderOpaque* reader = nullptr;
            if (readerNewWithUsage)
                readerNewWithUsage(m_frame_w, m_frame_h, kAImageFormatRGBA8888,
                                   kAHBUsageGpuSampled | kAHBUsageGpuFramebuffer,
                                   2, &reader);
            else if (readerNew)
                readerNew(m_frame_w, m_frame_h, kAImageFormatRGBA8888, 2, &reader);

            m_android_reader = reader;

            ANativeWindow* window = nullptr;
            if (reader && readerGetWindow)
                readerGetWindow(reader, &window);

            if (!window)
            {
                LogWarning("VulkanContext: AImageReader gave no ANativeWindow; no VkSurfaceKHR.");
            }
            else
            {
                auto createSurface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
                    vkGetInstanceProcAddr(m_instance, "vkCreateAndroidSurfaceKHR"));
                if (!createSurface)
                {
                    LogWarning("VulkanContext: vkCreateAndroidSurfaceKHR unavailable.");
                }
                else
                {
                    VkAndroidSurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
                    sci.window = window;
                    VkResult r = createSurface(m_instance, &sci, nullptr, &m_surface);
                    if (r != VK_SUCCESS)
                        LogWarning("VulkanContext: vkCreateAndroidSurfaceKHR failed: " + std::to_string(r));
                    else
                    {
                        LogOK("VulkanContext: VkSurfaceKHR created at "
                            + std::to_string(m_frame_w) + "x" + std::to_string(m_frame_h) + ".");
                        // What a core reading the surface will see. Worth printing: if
                        // this disagrees with the size reported to video_refresh, the
                        // readback is copying past the end of the image.
                        VkSurfaceCapabilitiesKHR caps{};
                        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu, m_surface, &caps) == VK_SUCCESS)
                            Log("VulkanContext: surface extent "
                                + std::to_string(caps.currentExtent.width) + "x"
                                + std::to_string(caps.currentExtent.height));
                    }
                }
            }
        }
    }
#elif defined(__APPLE__)
    {
        m_metal_layer = CreateMacMetalLayer(m_frame_w, m_frame_h);
        if (!m_metal_layer)
        {
            LogWarning("VulkanContext: could not create a CAMetalLayer; no VkSurfaceKHR.");
        }
        else
        {
            auto createSurface = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
                vkGetInstanceProcAddr(m_instance, "vkCreateMetalSurfaceEXT"));
            if (!createSurface)
            {
                LogWarning("VulkanContext: vkCreateMetalSurfaceEXT unavailable.");
            }
            else
            {
                VkMetalSurfaceCreateInfoEXT sci{ VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
                sci.pLayer = static_cast<const CAMetalLayer*>(m_metal_layer);
                VkResult r = createSurface(m_instance, &sci, nullptr, &m_surface);
                if (r != VK_SUCCESS)
                    LogWarning("VulkanContext: vkCreateMetalSurfaceEXT failed: "
                               + std::to_string(r));
                else
                {
                    LogOK("VulkanContext: Metal VkSurfaceKHR created at "
                        + std::to_string(m_frame_w) + "x" + std::to_string(m_frame_h) + ".");
                    VkSurfaceCapabilitiesKHR caps{};
                    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu, m_surface, &caps) == VK_SUCCESS)
                        Log("VulkanContext: surface extent "
                            + std::to_string(caps.currentExtent.width) + "x"
                            + std::to_string(caps.currentExtent.height));
                }
            }
        }
    }
#endif

    // ---- Create logical device ----
    retro_vulkan_context vk_ctx{};
    bool device_from_negotiation = false;

    if (neg)
    {
        if (neg->interface_version >= 2 && neg->create_device2)
        {
            m_negotiation_engaged   = true;
            device_from_negotiation = neg->create_device2(
                &vk_ctx, m_instance, m_gpu, m_surface,
                vkGetInstanceProcAddr, s_CreateDeviceWrapper, this);

            if (!device_from_negotiation)
                LogWarning("VulkanContext: create_device2 failed; using self-created device");
        }
        else if (neg->create_device)
        {
            // Cores dereference required_features without a null check (e.g.
            // paraLLEl-RDP does `enabled_features = *required_features`), and
            // RetroArch always passes a zeroed struct plus VK_KHR_swapchain, so
            // match that exactly.
            std::vector<const char*> device_extensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };

#ifdef __APPLE__
            constexpr const char* portability_subset = "VK_KHR_portability_subset";
            if (s_DeviceSupportsExtension(m_gpu, portability_subset))
                device_extensions.push_back(portability_subset);
#endif

            // VK_EXT_device_fault, so a lost device can say what killed it. This
            // list is the only way in on the v1 path: the core builds its own
            // VkDeviceCreateInfo and calls vkCreateDevice itself, so
            // s_CreateDeviceWrapper is never reached. A core is required to honor
            // this list (Dolphin merges it into its own via AddNameUnique).
            //
            // The matching VkPhysicalDeviceFaultFeaturesEXT cannot be delivered
            // this way, because v1 carries a flat VkPhysicalDeviceFeatures with no
            // pNext, so the extension is enabled while its feature bit is not.
            // vkGetDeviceFaultInfoEXT resolves either way.
            const bool want_fault = s_DeviceSupportsFault(m_gpu);
            if (want_fault)
                device_extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);

            const VkPhysicalDeviceFeatures required_features{};

            m_negotiation_engaged   = true;
            device_from_negotiation = neg->create_device(
                &vk_ctx, m_instance, m_gpu, m_surface,
                vkGetInstanceProcAddr,
                device_extensions.data(), static_cast<unsigned>(device_extensions.size()),
                nullptr, 0,
                &required_features);

            // A core is entitled to refuse an extension it was not expecting, and a
            // diagnostic must never be why the picture does not come up.
            if (!device_from_negotiation && want_fault)
            {
                LogWarning("VulkanContext: create_device failed with VK_EXT_device_fault; "
                           "retrying without it.");
                device_extensions.pop_back();
                vk_ctx = {};
                device_from_negotiation = neg->create_device(
                    &vk_ctx, m_instance, m_gpu, m_surface,
                    vkGetInstanceProcAddr,
                    device_extensions.data(), static_cast<unsigned>(device_extensions.size()),
                    nullptr, 0,
                    &required_features);
            }

            if (!device_from_negotiation)
                LogWarning("VulkanContext: create_device failed; using self-created device");
        }
    }

    if (device_from_negotiation)
    {
        m_device       = vk_ctx.device;
        m_queue        = vk_ctx.queue;
        m_queue_family = vk_ctx.queue_family_index;
        m_gpu          = vk_ctx.gpu;
    }
    else
    {
        std::vector<const char*> fallback_extensions;

#ifdef __APPLE__
        constexpr const char* portability_subset = "VK_KHR_portability_subset";
        if (s_DeviceSupportsExtension(m_gpu, portability_subset))
            fallback_extensions.push_back(portability_subset);
#endif

        if (m_surface != VK_NULL_HANDLE)
        {
            // The fallback device must be able to drive the surface we handed
            // to the core. The original selection considered graphics/compute
            // only and created a device without VK_KHR_swapchain, making the
            // non-negotiated surface unusable.
            VkBool32 present_supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(m_gpu, m_queue_family,
                                                  m_surface, &present_supported);
            if (!present_supported)
            {
                uint32_t qfam_count = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(m_gpu, &qfam_count, nullptr);
                std::vector<VkQueueFamilyProperties> qfams(qfam_count);
                vkGetPhysicalDeviceQueueFamilyProperties(m_gpu, &qfam_count, qfams.data());

                bool found = false;
                for (uint32_t i = 0; i < qfam_count; ++i)
                {
                    constexpr VkQueueFlags required =
                        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
                    VkBool32 can_present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(m_gpu, i, m_surface,
                                                          &can_present);
                    if ((qfams[i].queueFlags & required) == required && can_present)
                    {
                        m_queue_family = i;
                        found = true;
                        break;
                    }
                }

                if (!found)
                {
                    LogError("VulkanContext: no graphics+compute queue can present to the surface.");
                    return false;
                }
            }

            if (!s_DeviceSupportsExtension(m_gpu, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            {
                LogError("VulkanContext: surface exists but VK_KHR_swapchain is unavailable.");
                return false;
            }
            fallback_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }

        float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qci.queueFamilyIndex = m_queue_family;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &queue_priority;

        VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos    = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(fallback_extensions.size());
        dci.ppEnabledExtensionNames = fallback_extensions.data();

        VkResult r = vkCreateDevice(m_gpu, &dci, nullptr, &m_device);
        if (r != VK_SUCCESS)
        {
            LogError("VulkanContext: vkCreateDevice failed: " + std::to_string(r));
            return false;
        }

        vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);
    }

    LogOK("VulkanContext: VkDevice created.");

    // ---- Command pool + buffer ----
    VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = m_queue_family;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(m_device, &cpci, nullptr, &m_cmd_pool) != VK_SUCCESS)
    {
        LogError("VulkanContext: vkCreateCommandPool failed.");
        return false;
    }

    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = m_cmd_pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(m_device, &cbai, &m_cmd_buf) != VK_SUCCESS)
    {
        LogError("VulkanContext: vkAllocateCommandBuffers failed.");
        return false;
    }

    // ---- Fence ----
    // Pre-signaled: s_WaitSyncIndex waits on this fence, and the core calls
    // it before the FIRST frame too, when nothing has been submitted yet, and
    // an unsignaled fence would hang that first wait forever.
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(m_device, &fci, nullptr, &m_fence) != VK_SUCCESS)
    {
        LogError("VulkanContext: vkCreateFence failed.");
        return false;
    }

    // ---- Fill retro_hw_render_interface_vulkan ----
    m_interface.interface_type       = RETRO_HW_RENDER_INTERFACE_VULKAN;
    m_interface.interface_version    = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    m_interface.handle               = this;
    m_interface.instance             = m_instance;
    m_interface.gpu                  = m_gpu;
    m_interface.device               = m_device;
    m_interface.get_device_proc_addr   = vkGetDeviceProcAddr;
    m_interface.get_instance_proc_addr = vkGetInstanceProcAddr;
    m_interface.queue                = m_queue;
    m_interface.queue_index          = m_queue_family;
    m_interface.set_image            = s_SetImage;
    m_interface.get_sync_index       = s_GetSyncIndex;
    m_interface.get_sync_index_mask  = s_GetSyncIndexMask;
    m_interface.set_command_buffers  = s_SetCommandBuffers;
    m_interface.wait_sync_index      = s_WaitSyncIndex;
    m_interface.lock_queue           = s_LockQueue;
    m_interface.unlock_queue         = s_UnlockQueue;
    m_interface.set_signal_semaphore = s_SetSignalSemaphore;

    m_initialized = true;
    LogOK("VulkanContext: initialized.");
    return true;
}

// ---------------------------------------------------------------------------
// Destroy
// ---------------------------------------------------------------------------

VulkanContext::~VulkanContext()
{
    Destroy();
}

// Deliberately NOT gated on m_initialized. Init() can return false after the
// instance, the surface and the platform window already exist, and VideoHandler
// drops the context on that path, so gating here would leak all three (plus the
// device, pool and fences on the later failures).
void VulkanContext::Destroy()
{
    if (m_device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);

    DestroyStagingBuffer();

    for (VkFence fence : m_completion_fences)
        vkDestroyFence(m_device, fence, nullptr);
    m_completion_fences.clear();

    if (m_fence != VK_NULL_HANDLE)
    {
        vkDestroyFence(m_device, m_fence, nullptr);
        m_fence = VK_NULL_HANDLE;
    }

    if (m_cmd_pool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device, m_cmd_pool, nullptr);
        m_cmd_pool = VK_NULL_HANDLE;
        m_cmd_buf  = VK_NULL_HANDLE;
    }

    // Let the negotiation interface clean up any auxiliary resources it owns.
    // Gated on having actually called one of its create entry points: the spec
    // requires this even when creation failed, but calling it for a core we
    // never asked to create anything hands it a teardown for state it does not
    // have.
    if (m_negotiation_engaged && m_negotiation && m_negotiation->destroy_device)
        m_negotiation->destroy_device();
    m_negotiation_engaged = false;

    if (m_device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    if (m_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }

#ifdef _WIN32
    if (m_hidden_hwnd)
    {
        DestroyWindow(static_cast<HWND>(m_hidden_hwnd));
        m_hidden_hwnd = nullptr;
    }
#endif

#ifdef __ANDROID__
    // Strictly after vkDestroySurfaceKHR above: the surface holds a reference
    // on the reader's ANativeWindow.
    if (m_android_reader && m_mediandk)
    {
        auto readerDelete = reinterpret_cast<PFN_AImageReader_delete>(
            DynLib_Sym(m_mediandk, "AImageReader_delete"));
        if (readerDelete)
            readerDelete(static_cast<AImageReaderOpaque*>(m_android_reader));
    }
    m_android_reader = nullptr;

    if (m_mediandk)
    {
        DynLib_Close(m_mediandk);
        m_mediandk = nullptr;
    }
#endif

#ifdef __APPLE__
    // Strictly after vkDestroySurfaceKHR: MoltenVK retains the layer through
    // the surface and may still refer to it while destroying the surface.
    if (m_metal_layer)
    {
        DestroyMacMetalLayer(m_metal_layer);
        m_metal_layer = nullptr;
    }
#endif

    if (m_debug_messenger != VK_NULL_HANDLE)
    {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger)
            destroyMessenger(m_instance, m_debug_messenger, nullptr);
        m_debug_messenger = VK_NULL_HANDLE;
    }

    if (m_instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    // Reset the rest so a second Init() on this object starts clean rather than
    // inheriting a pending readback or a stale image handle.
    m_gpu              = VK_NULL_HANDLE;
    m_queue            = VK_NULL_HANDLE;
    m_readback_pending = false;
    m_current_vk_image = VK_NULL_HANDLE;
    m_wait_semaphores.clear();
    m_pending_command_buffers.clear();
    m_new_image_pending = false;
    m_signal_semaphore = VK_NULL_HANDLE;
    m_src_queue_family = VK_QUEUE_FAMILY_IGNORED;
    m_interface        = {};
    m_initialized      = false;
}

// ---------------------------------------------------------------------------
// SetImage: called by the core before retro_video_refresh
// ---------------------------------------------------------------------------

void VulkanContext::SetImage(const retro_vulkan_image* image,
                             uint32_t n_sems, const VkSemaphore* sems,
                             uint32_t src_family)
{
    std::lock_guard<std::mutex> state_lock(m_state_mutex);

    if (!image)
    {
        m_current_vk_image = VK_NULL_HANDLE;
        m_wait_semaphores.clear();
        m_new_image_pending = true;
        m_image_retracted   = true;
        return;
    }

    if (n_sems > 0 && !sems)
    {
        LogError("VulkanContext: set_image received a null semaphore array.");
        n_sems = 0;
    }

    // Keep the semaphore handles until video_refresh; the readback submission
    // performs the one permitted wait directly at the transfer stage. Copy only
    // the fields we need, never pNext chains.
    m_current_vk_image          = image->create_info.image;
    m_current_format            = image->create_info.format;
    m_current_layout            = image->image_layout;
    m_current_subresource_range = image->create_info.subresourceRange;
    m_src_queue_family          = src_family;
    m_wait_semaphores.clear();
    if (n_sems > 0)
        m_wait_semaphores.assign(sems, sems + n_sems);
    m_new_image_pending         = true;
    m_image_retracted           = false;
}

VulkanContext::FrameWork VulkanContext::TakeFrameWork(
    VkImage& image, VkFormat& format, VkImageLayout& layout,
    VkImageSubresourceRange& range, uint32_t& src_queue_family)
{
    std::lock_guard<std::mutex> state_lock(m_state_mutex);
    image            = m_current_vk_image;
    format           = m_current_format;
    layout           = m_current_layout;
    range            = m_current_subresource_range;
    src_queue_family = m_src_queue_family;

    FrameWork work;
    work.wait_semaphores.swap(m_wait_semaphores);
    work.command_buffers.swap(m_pending_command_buffers);
    work.signal_semaphore = m_signal_semaphore;
    work.newly_published  = m_new_image_pending;
    work.image_retracted  = m_image_retracted;
    m_signal_semaphore    = VK_NULL_HANDLE;
    m_new_image_pending   = false;
    return work;
}

bool VulkanContext::SubmitFrameCompletionLocked(
    FrameWork&& work, VkImage image, VkImageLayout layout,
    const VkImageSubresourceRange& range, uint32_t src_queue_family,
    bool detached)
{
    const bool has_core_commands = !work.command_buffers.empty();
    const bool need_qfot = work.newly_published && !has_core_commands &&
                           !work.wait_semaphores.empty() &&
                           image != VK_NULL_HANDLE &&
                           layout != VK_IMAGE_LAYOUT_UNDEFINED &&
                           layout != VK_IMAGE_LAYOUT_PREINITIALIZED &&
                           src_queue_family != VK_QUEUE_FAMILY_IGNORED &&
                           src_queue_family != m_queue_family;

    if (need_qfot)
    {
        VkCommandBuffer completion_cmd = m_cmd_buf;
        if (detached)
        {
            // m_cmd_buf still belongs to the timed-out submission. Allocate a
            // fresh command buffer and leave it owned by the pool until context
            // teardown; freeing it before this queued recovery runs would be the
            // same lifetime bug we are avoiding here.
            VkCommandBufferAllocateInfo allocate{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocate.commandPool        = m_cmd_pool;
            allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(m_device, &allocate, &completion_cmd) != VK_SUCCESS)
            {
                LogError("VulkanContext: failed to allocate detached ownership-return command buffer.");
                return false;
            }
        }

        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ((!detached && vkResetCommandBuffer(completion_cmd, 0) != VK_SUCCESS) ||
            vkBeginCommandBuffer(completion_cmd, &begin) != VK_SUCCESS)
        {
            LogError("VulkanContext: failed to begin ownership-return command buffer.");
            return false;
        }

        VkImageMemoryBarrier acquire{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        acquire.oldLayout           = layout;
        acquire.newLayout           = layout;
        acquire.srcQueueFamilyIndex = src_queue_family;
        acquire.dstQueueFamilyIndex = m_queue_family;
        acquire.image               = image;
        acquire.subresourceRange    = range;
        vkCmdPipelineBarrier(completion_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &acquire);

        VkImageMemoryBarrier release{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        release.oldLayout           = layout;
        release.newLayout           = layout;
        release.srcQueueFamilyIndex = m_queue_family;
        release.dstQueueFamilyIndex = src_queue_family;
        release.image               = image;
        release.subresourceRange    = range;
        vkCmdPipelineBarrier(completion_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &release);

        if (vkEndCommandBuffer(completion_cmd) != VK_SUCCESS)
        {
            LogError("VulkanContext: failed to end ownership-return command buffer.");
            return false;
        }
        work.command_buffers.push_back(completion_cmd);
    }

    const bool wait_on_image = !has_core_commands && !work.wait_semaphores.empty();
    std::vector<VkPipelineStageFlags> wait_stages(
        wait_on_image ? work.wait_semaphores.size() : 0,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    if (wait_on_image)
    {
        submit.waitSemaphoreCount = static_cast<uint32_t>(work.wait_semaphores.size());
        submit.pWaitSemaphores    = work.wait_semaphores.data();
        submit.pWaitDstStageMask  = wait_stages.data();
    }
    submit.commandBufferCount = static_cast<uint32_t>(work.command_buffers.size());
    submit.pCommandBuffers    = work.command_buffers.data();
    if (work.signal_semaphore != VK_NULL_HANDLE)
    {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &work.signal_semaphore;
    }

    if (submit.waitSemaphoreCount == 0 && submit.commandBufferCount == 0 &&
        submit.signalSemaphoreCount == 0)
        return true;

    VkFence submit_fence = m_fence;
    if (detached)
    {
        VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(m_device, &fence_info, nullptr, &submit_fence) != VK_SUCCESS)
        {
            LogError("VulkanContext: failed to create detached completion fence.");
            return false;
        }
    }
    else if (vkResetFences(m_device, 1, &submit_fence) != VK_SUCCESS)
    {
        LogError("VulkanContext: failed to reset completion fence.");
        return false;
    }

    VkResult result;
    {
        std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
        result = vkQueueSubmit(m_queue, 1, &submit, submit_fence);
    }
    if (result != VK_SUCCESS)
    {
        LogError("VulkanContext: frame-completion vkQueueSubmit failed: "
            + std::to_string(result));
        if (result == VK_ERROR_DEVICE_LOST)
            DumpDeviceFault();
        if (detached)
            vkDestroyFence(m_device, submit_fence, nullptr);
        return false;
    }

    if (detached)
        m_completion_fences.push_back(submit_fence);
    else
        m_readback_pending = true;
    return true;
}

void VulkanContext::CompleteFrameWithoutReadback()
{
    std::unique_lock<std::mutex> fence_lock(m_fence_mutex);
    VkImage image;
    VkFormat format;
    VkImageLayout layout;
    VkImageSubresourceRange range;
    uint32_t src_queue_family;
    const bool previous_complete = WaitReadbackFenceLocked();
    FrameWork work = TakeFrameWork(image, format, layout, range, src_queue_family);

    SubmitFrameCompletionLocked(std::move(work), image, layout, range,
                                src_queue_family, !previous_complete);
}

// ---------------------------------------------------------------------------
// ReadbackToPixels: blits the Vulkan image to a staging buffer and copies
// to a PackedByteArray.  Replaces glReadPixels for Vulkan cores.
// ---------------------------------------------------------------------------

bool VulkanContext::ReadbackToPixels(uint32_t width, uint32_t height, PackedByteArray& out, VideoTransferStats* stats)
{
    VideoTransferTimer total_timer(stats, VideoTransferStats::READBACK_TOTAL);
    std::unique_lock<std::mutex> fence_lock(m_fence_mutex);

    // Nothing that owns the command buffer, fence or staging allocation may be
    // touched until the previous submission has completed. In particular this
    // must precede the resize below: destroying a too-small staging buffer first
    // would free memory that a timed-out copy could still be writing.
    VkImage                 image      = VK_NULL_HANDLE;
    VkFormat                format     = VK_FORMAT_UNDEFINED;
    VkImageLayout           layout     = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageSubresourceRange range{};
    uint32_t                src_family = VK_QUEUE_FAMILY_IGNORED;
    bool previous_complete;
    {
        VideoTransferTimer wait_timer(stats, VideoTransferStats::READBACK_WAIT);
        previous_complete = WaitReadbackFenceLocked();
    }
    FrameWork work = TakeFrameWork(image, format, layout, range, src_family);
    if (!previous_complete)
    {
        // The normal command buffer and fence still belong to the previous
        // submission. Queue this frame's synchronization behind it using fresh
        // recovery resources so a transient timeout cannot strand the core's
        // input or output semaphores.
        SubmitFrameCompletionLocked(std::move(work), image, layout, range,
                                    src_family, true);
        return false;
    }

    // A second refresh without set_image sees no wait semaphores and performs
    // no second queue-family ownership transfer.

    if (image == VK_NULL_HANDLE)
    {
        // A core that RETRACTED its image is blanking the field on purpose, and
        // says so by calling set_image(nullptr) before the refresh. LRPS2 does
        // it whenever the PS2's PCRTC has nothing to merge - around every video
        // mode change, which for Ace Combat 04 is 7% of frames - so logging it
        // per frame buries the run. Only a refresh with no image the core never
        // published at all is a protocol error, and that is worth one line.
        const bool retracted = work.image_retracted;
        if (!retracted && !m_logged_no_image)
        {
            m_logged_no_image = true;
            LogWarning("VulkanContext::ReadbackToPixels: video_refresh with no "
                       "image set and none retracted; keeping the last frame. "
                       "Further occurrences are not logged.");
        }
        SubmitFrameCompletionLocked(std::move(work), image, layout, range, src_family);
        return false;
    }

    if (!work.newly_published)
    {
        // A second video_refresh without set_image is a duplicated frame. Keep
        // the last uploaded texture rather than reading the image again: after
        // the first readback a cross-family image has already been released
        // back to the core and cannot legally be acquired a second time.
        SubmitFrameCompletionLocked(std::move(work), image, layout, range, src_family);
        return false;
    }

    const bool supported_format =
        format == VK_FORMAT_B8G8R8A8_UNORM ||
        format == VK_FORMAT_B8G8R8A8_SRGB  ||
        format == VK_FORMAT_B8G8R8A8_SNORM ||
        format == VK_FORMAT_R8G8B8A8_UNORM ||
        format == VK_FORMAT_R8G8B8A8_SRGB  ||
        format == VK_FORMAT_R8G8B8A8_SNORM ||
        format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
        format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    if (!supported_format)
    {
        if (m_warned_format != static_cast<int64_t>(format))
        {
            m_warned_format = static_cast<int64_t>(format);
            LogError("VulkanContext: refusing unsupported VkFormat "
                + std::to_string(static_cast<int>(format))
                + "; its texel size/conversion is not implemented.");
        }
        SubmitFrameCompletionLocked(std::move(work), image, layout, range, src_family);
        return false;
    }

    const VkDeviceSize needed = (VkDeviceSize)width * height * 4u;
    if (m_staging_size < needed)
    {
        DestroyStagingBuffer();
        if (!CreateStagingBuffer(needed))
        {
            SubmitFrameCompletionLocked(std::move(work), image, layout, range, src_family);
            return false;
        }
    }

    // ---- Record command buffer ----
    VkCommandBufferBeginInfo cbbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkResetCommandBuffer(m_cmd_buf, 0) != VK_SUCCESS ||
        vkBeginCommandBuffer(m_cmd_buf, &cbbi) != VK_SUCCESS)
    {
        LogError("VulkanContext: failed to begin readback command buffer.");
        SubmitFrameCompletionLocked(std::move(work), image, layout, range, src_family);
        return false;
    }

    // GENERAL is a contract, not just another layout. libretro_vulkan.h: "if
    // GENERAL layout is used for the image ... the frontend is not allowed to
    // perform any layout transitions, so concurrent reads from core and frontend
    // are allowed." vkCmdCopyImageToBuffer accepts GENERAL as a source layout
    // directly, so the copy needs a dependency but no transition.
    const bool          image_is_general = (layout == VK_IMAGE_LAYOUT_GENERAL);
    const VkImageLayout copy_layout      = image_is_general
                                             ? VK_IMAGE_LAYOUT_GENERAL
                                             : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const bool need_layout_transition = (layout != copy_layout);

    // UNDEFINED and PREINITIALIZED are illegal as a barrier's newLayout, so a
    // core that published one gets no restore rather than an invalid barrier.
    const bool can_restore_layout = layout != VK_IMAGE_LAYOUT_UNDEFINED &&
                                    layout != VK_IMAGE_LAYOUT_PREINITIALIZED;

    // Queue family ownership. libretro_vulkan.h spells out the round trip: the
    // core releases the image to us, we acquire it, we release it back, the core
    // re-acquires. Skipping either half leaves a core that released the image
    // without a matching acquire, and it never gets the image back.
    const bool has_core_commands = !work.command_buffers.empty();
    const bool need_qfot = work.newly_published && !has_core_commands &&
                           !work.wait_semaphores.empty() &&
                           src_family != VK_QUEUE_FAMILY_IGNORED &&
                           src_family != m_queue_family;

    if (need_qfot && !m_logged_src_queue_family)
    {
        m_logged_src_queue_family = true;
        Log("VulkanContext: core owns the image on queue family "
            + std::to_string(src_family) + ", ours is "
            + std::to_string(m_queue_family) + ", doing ownership transfers.");
    }

    // Acquire half, plus the layout transition if one is due.
    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout           = layout;
        barrier.newLayout           = copy_layout;
        barrier.srcQueueFamilyIndex = need_qfot ? src_family     : VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = need_qfot ? m_queue_family : VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = image;
        barrier.subresourceRange    = range;

        vkCmdPipelineBarrier(m_cmd_buf,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Copy image → staging buffer
    VkBufferImageCopy copy{};
    copy.bufferOffset      = 0;
    copy.bufferRowLength   = 0; // tightly packed
    copy.bufferImageHeight = 0; // tightly packed
    copy.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    // Both taken from the view the core published; pinning mipLevel to 0 would
    // read the wrong mip for a view onto anything but the first.
    copy.imageSubresource.mipLevel       = range.baseMipLevel;
    copy.imageSubresource.baseArrayLayer = range.baseArrayLayer;
    copy.imageSubresource.layerCount     = 1;
    copy.imageOffset = { 0, 0, 0 };
    copy.imageExtent = { width, height, 1 };

    vkCmdCopyImageToBuffer(m_cmd_buf, image, copy_layout, m_staging_buf, 1, &copy);

    // Release half, plus the restore transition if one is due.
    if ((need_layout_transition && can_restore_layout) || need_qfot)
    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT |
                                      VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout           = copy_layout;
        barrier.newLayout           = (need_layout_transition && can_restore_layout)
                                        ? layout : copy_layout;
        barrier.srcQueueFamilyIndex = need_qfot ? m_queue_family : VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = need_qfot ? src_family     : VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = image;
        barrier.subresourceRange    = range;

        vkCmdPipelineBarrier(m_cmd_buf,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Staging buffer host-read barrier
    VkBufferMemoryBarrier buf_barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    buf_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    buf_barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
    buf_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buf_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buf_barrier.buffer              = m_staging_buf;
    buf_barrier.offset              = 0;
    buf_barrier.size                = needed;

    vkCmdPipelineBarrier(m_cmd_buf,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, nullptr, 1, &buf_barrier, 0, nullptr);

    if (vkEndCommandBuffer(m_cmd_buf) != VK_SUCCESS)
    {
        LogError("VulkanContext: failed to end readback command buffer.");
        SubmitFrameCompletionLocked(std::move(work), image, layout, range, src_family);
        return false;
    }

    // ---- Submit + wait ----
    std::vector<VkCommandBuffer> submit_commands = work.command_buffers;
    submit_commands.push_back(m_cmd_buf);
    const bool wait_on_image = !has_core_commands && !work.wait_semaphores.empty();
    std::vector<VkPipelineStageFlags> wait_stages(
        wait_on_image ? work.wait_semaphores.size() : 0,
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    if (wait_on_image)
    {
        si.waitSemaphoreCount = static_cast<uint32_t>(work.wait_semaphores.size());
        si.pWaitSemaphores    = work.wait_semaphores.data();
        si.pWaitDstStageMask  = wait_stages.data();
    }
    si.commandBufferCount = static_cast<uint32_t>(submit_commands.size());
    si.pCommandBuffers    = submit_commands.data();

    // Signal the semaphore requested by the core (if any)
    if (work.signal_semaphore != VK_NULL_HANDLE)
    {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &work.signal_semaphore;
    }

    if (vkResetFences(m_device, 1, &m_fence) != VK_SUCCESS)
    {
        LogError("VulkanContext: failed to reset readback fence.");
        return false;
    }
    VkResult submit_result = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
        submit_result = vkQueueSubmit(m_queue, 1, &si, m_fence);
    }

    // Only claim a pending readback if one is actually in flight. Setting the
    // flag for a submit that never landed left a fence nothing would ever
    // signal, and every subsequent frame then burned the full 2 s timeout
    // waiting on it.
    if (submit_result != VK_SUCCESS)
    {
        LogError("VulkanContext: readback vkQueueSubmit failed: "
            + std::to_string(submit_result));
        if (submit_result == VK_ERROR_DEVICE_LOST)
            DumpDeviceFault();
        // The readback command may be what failed validation. Still try to
        // consume the core work and complete its synchronization without it.
        SubmitFrameCompletionLocked(std::move(work), image, layout, range, src_family);
        return false;
    }
    m_readback_pending = true;

    // Nothing below may touch the staging buffer until the copy into it has
    // finished. On a timeout the submit is still running, so abandon the frame
    // rather than read a buffer that is still being written.
    {
        VideoTransferTimer wait_timer(stats, VideoTransferStats::READBACK_WAIT);
        if (!WaitReadbackFenceLocked())
            return false;
    }

    VideoTransferTimer copy_timer(stats, VideoTransferStats::READBACK_COPY);
    // ---- Map and copy to output ----
    void* mapped = nullptr;
    VkResult map_result = vkMapMemory(m_device, m_staging_mem, 0, needed, 0, &mapped);
    if (map_result != VK_SUCCESS || !mapped)
    {
        LogError("VulkanContext: vkMapMemory (staging) failed: "
            + std::to_string(map_result));
        return false;
    }

    out.resize((int64_t)needed);
    uint8_t*       dst = out.ptrw();
    const uint8_t* src = static_cast<const uint8_t*>(mapped);

    // Normalize BGRA formats to RGBA (Godot Image::FORMAT_RGBA8 expects R first)
    if (format == VK_FORMAT_B8G8R8A8_UNORM ||
        format == VK_FORMAT_B8G8R8A8_SRGB)
    {
        SwizzleBgraToRgba(dst, src, width * height);
    }
    else if (format == VK_FORMAT_R8G8B8A8_UNORM ||
             format == VK_FORMAT_R8G8B8A8_SRGB)
    {
        memcpy(dst, src, (size_t)needed);
    }
    else if (format == VK_FORMAT_B8G8R8A8_SNORM ||
             format == VK_FORMAT_R8G8B8A8_SNORM)
    {
        // SNORM bytes are signed values, not display-ready UNORM bytes. Shift
        // [-128, 127] to [0, 255], swapping R/B for the BGRA variant.
        SnormToRgba8(dst, src, width * height,
                     format == VK_FORMAT_B8G8R8A8_SNORM);
    }
    // 10-bit packed, which is what a modern surface hands back when it can
    // (Dolphin's swapchain on this GPU is A2B10G10R10). Still 32 bits a pixel,
    // so the copy sizes hold, but the channels are bit-fields rather than bytes,
    // and read as RGBA8 it comes out in the wrong colors entirely.
    //
    // One uint32 per pixel: R in the low 10 bits, then G, then B, and 2 bits of
    // alpha at the top. Shift each down to 8 bits. Alpha is widened from its
    // 2 bits rather than forced opaque, and 0b11 maps to 255 exactly.
    else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
    {
        A2b10g10r10ToRgba8(dst, src, width * height);
    }
    // The same layout with red and blue the other way round; the driver chooses
    // between the two, not us.
    else if (format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
    {
        A2r10g10b10ToRgba8(dst, src, width * height);
    }
    vkUnmapMemory(m_device, m_staging_mem);
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// What the driver says killed the device. Only meaningful after something has
// already returned VK_ERROR_DEVICE_LOST.
void VulkanContext::DumpDeviceFault()
{
    if (m_fault_reported || m_device == VK_NULL_HANDLE)
        return;
    m_fault_reported = true;

    auto getFaultInfo = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
        vkGetDeviceProcAddr(m_device, "vkGetDeviceFaultInfoEXT"));
    if (!getFaultInfo)
    {
        // Resolves only when the extension was actually enabled on THIS device,
        // which is the honest test: the core owns device creation and may have
        // fallen back to a create-info without it.
        LogError("VulkanContext: device lost, and VK_EXT_device_fault is not "
                 "enabled on this device; no fault detail available.");
        return;
    }

    // Two-call idiom: counts first, then the arrays sized from them.
    VkDeviceFaultCountsEXT counts{ VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
    if (getFaultInfo(m_device, &counts, nullptr) != VK_SUCCESS)
    {
        LogError("VulkanContext: device lost, and vkGetDeviceFaultInfoEXT gave no counts.");
        return;
    }

    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);

    VkDeviceFaultInfoEXT info{ VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos  = vendors.empty()   ? nullptr : vendors.data();
    counts.vendorBinarySize = 0;   // not requesting the vendor crash dump

    if (getFaultInfo(m_device, &counts, &info) != VK_SUCCESS)
    {
        LogError("VulkanContext: device lost, and vkGetDeviceFaultInfoEXT failed.");
        return;
    }

    LogError(std::string("VulkanContext: DEVICE FAULT: ") + info.description);

    for (uint32_t i = 0; i < counts.addressInfoCount; ++i)
    {
        const VkDeviceFaultAddressInfoEXT& a = addresses[i];
        // reportedAddress is only accurate to +/- addressPrecision, which is a
        // power-of-two mask, so quoting it unqualified would overstate the answer.
        LogError("VulkanContext:   address type " + std::to_string((int)a.addressType)
            + " at 0x" + std::to_string(a.reportedAddress)
            + " (precision 0x" + std::to_string(a.addressPrecision) + ")");
    }

    for (uint32_t i = 0; i < counts.vendorInfoCount; ++i)
    {
        const VkDeviceFaultVendorInfoEXT& v = vendors[i];
        LogError(std::string("VulkanContext:   vendor: ") + v.description
            + " code " + std::to_string(v.vendorFaultCode)
            + " data " + std::to_string(v.vendorFaultData));
    }

    if (counts.addressInfoCount == 0 && counts.vendorInfoCount == 0)
        LogError("VulkanContext:   (driver reported no address or vendor detail)");
}

// Returns false when the submission is STILL RUNNING, in which case the caller
// must not touch anything it owns.
//
// A timeout means the fence has not signaled precisely BECAUSE the GPU is still
// executing that submit, so m_readback_pending must survive it. Clearing the
// flag would let the next frame do all three of these to live objects:
//
//   * vkResetFences on a fence in use by a pending submission
//   * vkResetCommandBuffer + re-record of a command buffer still executing
//   * vkMapMemory and a read of the staging buffer still being written into
//
// The first two are undefined behavior; on Adreno they corrupt the ring buffer
// and the kernel driver kills the context. The third is a plain data race that
// shows up as corrupt scanlines rather than as an error.
bool VulkanContext::WaitReadbackFenceLocked()
{
    if (m_readback_pending)
    {
        VkResult r = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, kFenceTimeoutNs);
        if (r == VK_SUCCESS)
        {
            m_readback_pending = false;
        }
        else if (r == VK_TIMEOUT)
        {
            // Stays pending on purpose. Every path that would reuse the fence,
            // command buffer or staging buffer checks this first.
            LogWarning("VulkanContext: readback fence still pending after "
                + std::to_string(kFenceTimeoutNs / 1000000ull)
                + " ms; dropping this frame and leaving the submit in flight.");
            return false;
        }
        else
        {
            LogError("VulkanContext: vkWaitForFences failed ("
                + std::to_string(static_cast<int>(r))
                + "); the device is gone, not just slow.");
            if (r == VK_ERROR_DEVICE_LOST)
                DumpDeviceFault();
            m_readback_pending = false;
            return false;
        }
    }

    // A timeout-recovery submission uses a fresh fence because m_fence still
    // belongs to the timed-out readback. Reap those fences before reporting the
    // sync index complete; otherwise wait_sync_index could return while core
    // command buffers or an ownership-return barrier were still queued.
    for (auto it = m_completion_fences.begin(); it != m_completion_fences.end();)
    {
        VkResult r = vkWaitForFences(m_device, 1, &*it, VK_TRUE, kFenceTimeoutNs);
        if (r == VK_SUCCESS)
        {
            vkDestroyFence(m_device, *it, nullptr);
            it = m_completion_fences.erase(it);
            continue;
        }
        if (r == VK_TIMEOUT)
        {
            LogWarning("VulkanContext: detached frame-completion fence still pending after "
                + std::to_string(kFenceTimeoutNs / 1000000ull) + " ms.");
            return false;
        }

        LogError("VulkanContext: detached completion vkWaitForFences failed ("
            + std::to_string(static_cast<int>(r)) + ").");
        if (r == VK_ERROR_DEVICE_LOST)
            DumpDeviceFault();
        vkDestroyFence(m_device, *it, nullptr);
        m_completion_fences.erase(it);
        return false;
    }

    return true;
}

bool VulkanContext::CreateStagingBuffer(VkDeviceSize size)
{
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size        = size;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bci, nullptr, &m_staging_buf) != VK_SUCCESS)
    {
        LogError("VulkanContext: vkCreateBuffer (staging) failed.");
        return false;
    }

    VkMemoryRequirements mem_req{};
    vkGetBufferMemoryRequirements(m_device, m_staging_buf, &mem_req);

    // This buffer exists purely for the CPU to READ BACK from (GPU → CPU) every
    // frame. HOST_VISIBLE | HOST_COHERENT alone does not guarantee HOST_CACHED,
    // and on most drivers that means write-combined (uncached) memory: fine for
    // CPU→GPU uploads, but CPU reads from it are drastically slower than normal
    // RAM (~45 ms to read back a 480x272 frame). Prefer a cached memory type when
    // one exists for this buffer, and fall back to the plain combo otherwise.
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(m_gpu, &mem_props);
    const VkMemoryPropertyFlags cached_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    int32_t cached_type = -1;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
    {
        if ((mem_req.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & cached_flags) == cached_flags)
        {
            cached_type = (int32_t)i;
            break;
        }
    }

    uint32_t memory_type = 0;
    if (cached_type >= 0)
    {
        memory_type = static_cast<uint32_t>(cached_type);
    }
    else if (!FindMemoryType(mem_req.memoryTypeBits,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
             memory_type))
    {
        vkDestroyBuffer(m_device, m_staging_buf, nullptr);
        m_staging_buf = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = mem_req.size;
    mai.memoryTypeIndex = memory_type;

    if (vkAllocateMemory(m_device, &mai, nullptr, &m_staging_mem) != VK_SUCCESS)
    {
        LogError("VulkanContext: vkAllocateMemory (staging) failed.");
        vkDestroyBuffer(m_device, m_staging_buf, nullptr);
        m_staging_buf = VK_NULL_HANDLE;
        return false;
    }

    VkResult bind_result = vkBindBufferMemory(m_device, m_staging_buf, m_staging_mem, 0);
    if (bind_result != VK_SUCCESS)
    {
        LogError("VulkanContext: vkBindBufferMemory (staging) failed: "
            + std::to_string(bind_result));
        vkFreeMemory(m_device, m_staging_mem, nullptr);
        vkDestroyBuffer(m_device, m_staging_buf, nullptr);
        m_staging_mem = VK_NULL_HANDLE;
        m_staging_buf = VK_NULL_HANDLE;
        return false;
    }
    m_staging_size = size;
    return true;
}

void VulkanContext::DestroyStagingBuffer()
{
    if (m_device == VK_NULL_HANDLE)
        return;

    if (m_staging_buf != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_staging_buf, nullptr);
        m_staging_buf = VK_NULL_HANDLE;
    }
    if (m_staging_mem != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_staging_mem, nullptr);
        m_staging_mem = VK_NULL_HANDLE;
    }
    m_staging_size = 0;
}

bool VulkanContext::FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags props,
                                   uint32_t& memory_type)
{
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(m_gpu, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
    {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
        {
            memory_type = i;
            return true;
        }
    }

    LogError("VulkanContext: FindMemoryType: no suitable memory type found");
    return false;
}

} // namespace Xenu
