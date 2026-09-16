#include "EnvironmentHandler.hpp"

#include <godot_cpp/variant/string.hpp>

#include "LinkCoordinator.hpp"
#include "Wrapper.hpp"
#include "Debug.hpp"
#include "InputHandler.hpp"
#include "VideoHandler.hpp"
#include <libretro_vulkan.h>

#include <filesystem>
#include <mutex>
#include <unordered_set>

#define VFS_FRONTEND
#include <vfs/vfs_implementation.h>
#include <features/features_cpu.h>

using namespace godot;

namespace Xenu
{
#define RETRO_ENVIRONMENT_RETROARCH_START_BLOCK 0x800000
#define RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND (2 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
#define RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB (3 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
#define RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE (4 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)

static retro_time_t retro_perf_get_time_usec()
{
    return cpu_features_get_time_usec();
}

static uint64_t retro_get_cpu_features()
{
    return cpu_features_get();
}

static retro_perf_tick_t retro_perf_get_counter()
{
    return cpu_features_get_perf_counter();
}

static void retro_perf_register(retro_perf_counter* counter)
{
}

static void retro_perf_start(retro_perf_counter* counter)
{
}

static void retro_perf_stop(retro_perf_counter* counter)
{
}

static void retro_perf_log()
{
}

static bool runloop_clear_all_thread_waits(uint32_t clear_threads, void* data)
{
    if (clear_threads > 0)
    {
        // start audio
    }
    else
    {
        // stop audio
    }

    return true;
}

#define ENVIRONMENT_COMMANDS \
    X(RETRO_ENVIRONMENT_EXPERIMENTAL) \
    X(RETRO_ENVIRONMENT_PRIVATE) \
    X(RETRO_ENVIRONMENT_SET_ROTATION) \
    X(RETRO_ENVIRONMENT_GET_OVERSCAN) \
    X(RETRO_ENVIRONMENT_GET_CAN_DUPE) \
    X(RETRO_ENVIRONMENT_SET_MESSAGE) \
    X(RETRO_ENVIRONMENT_SHUTDOWN) \
    X(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL) \
    X(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) \
    X(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) \
    X(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS) \
    X(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK) \
    X(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE) \
    X(RETRO_ENVIRONMENT_SET_HW_RENDER) \
    X(RETRO_ENVIRONMENT_GET_VARIABLE) \
    X(RETRO_ENVIRONMENT_SET_VARIABLES) \
    X(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE) \
    X(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME) \
    X(RETRO_ENVIRONMENT_GET_LIBRETRO_PATH) \
    X(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK) \
    X(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK) \
    X(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES) \
    X(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_LOG_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_PERF_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY) \
    X(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY) \
    X(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO) \
    X(RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK) \
    X(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO) \
    X(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO) \
    X(RETRO_ENVIRONMENT_SET_MEMORY_MAPS) \
    X(RETRO_ENVIRONMENT_SET_GEOMETRY) \
    X(RETRO_ENVIRONMENT_GET_USERNAME) \
    X(RETRO_ENVIRONMENT_GET_LANGUAGE) \
    X(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER) \
    X(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE) \
    X(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS) \
    X(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE) \
    X(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS) \
    X(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT) \
    X(RETRO_ENVIRONMENT_GET_VFS_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_LED_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE) \
    X(RETRO_ENVIRONMENT_GET_MIDI_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_FASTFORWARDING) \
    X(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE) \
    X(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS) \
    X(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION) \
    X(RETRO_ENVIRONMENT_SET_CORE_OPTIONS) \
    X(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL) \
    X(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY) \
    X(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER) \
    X(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION) \
    X(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION) \
    X(RETRO_ENVIRONMENT_SET_MESSAGE_EXT) \
    X(RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS) \
    X(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK) \
    X(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY) \
    X(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE) \
    X(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE) \
    X(RETRO_ENVIRONMENT_GET_GAME_INFO_EXT) \
    X(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2) \
    X(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL) \
    X(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK) \
    X(RETRO_ENVIRONMENT_SET_VARIABLE) \
    X(RETRO_ENVIRONMENT_GET_THROTTLE_STATE) \
    X(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT) \
    X(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT) \
    X(RETRO_ENVIRONMENT_GET_JIT_CAPABLE) \
    X(RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_DEVICE_POWER) \
    X(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_LINK_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_LINK_INTERFACE_FINAL) \
    X(RETRO_ENVIRONMENT_GET_TRANSFER_PAK_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_TRANSFER_PAK_INTERFACE_FINAL) \
    X(RETRO_ENVIRONMENT_GET_CONTROLLER_AUDIO_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_CONTROLLER_AUDIO_INTERFACE_FINAL) \
    X(RETRO_ENVIRONMENT_GET_CONTROLLER_DISPLAY_INTERFACE) \
    X(RETRO_ENVIRONMENT_GET_PLAYLIST_DIRECTORY) \
    X(RETRO_ENVIRONMENT_GET_FILE_BROWSER_START_DIRECTORY) \
    X(RETRO_ENVIRONMENT_RETROARCH_START_BLOCK) \
    X(RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND) \
    X(RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB) \
    X(RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE)

static std::string EnvironmentString(uint32_t cmd)
{
    switch (cmd)
    {
#define X(name) case name: return #name;
        ENVIRONMENT_COMMANDS
#undef X
    default:
        return "Unknown Environment Command: " + std::to_string(cmd);
    }
}

static bool EnvironmentNotImplemented(uint32_t cmd, bool log = true, bool default_returns = false)
{
    if (!log)
        return default_returns;

    if (default_returns)
        LogWarning("Environment not implemented: " + EnvironmentString(cmd));
    else
        LogError("Environment not implemented: " + EnvironmentString(cmd));
    return default_returns;
}

static bool EnvironmentUnknown(uint32_t cmd)
{
    LogError("Environment unknown: " + EnvironmentString(cmd));
    return false;
}

/// Cores that keep their own saves (bsnes, the PS2 cores) reach disk through
/// this call and nowhere else, so it is the only place a frontend can learn
/// where such a save went. Said once per path: bsnes reopens save.ram on a
/// timer and a line per reopen would bury everything else.
static libretro_vfs_implementation_file* VfsFileOpenLogged(const char* path, unsigned mode, unsigned hints)
{
    libretro_vfs_implementation_file* file = retro_vfs_file_open_impl(path, mode, hints);
    if (path && (mode & RETRO_VFS_FILE_ACCESS_WRITE))
    {
        static std::mutex s_mutex;
        static std::unordered_set<std::string> s_seen;
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            first = s_seen.insert(path).second;
        }
        if (first)
        {
            if (file)
                Log(std::string("VFS: core opened for writing ") + path +
                    ((mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) ? " (update)" : " (truncate)"));
            else
                LogWarning(std::string("VFS: core failed to open for writing ") + path);
        }
    }
    else if (!file && path && (hints & RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS) == 0)
    {
        // A read that fails is usually a missing BIOS or disc; mostly harmless
        // (cores probe optional files), so it is a plain line and not an error.
        Log(std::string("VFS: core could not open ") + path);
    }
    return file;
}

EnvironmentHandler::EnvironmentHandler()
{
    m_vfs_interface =
    {
        retro_vfs_file_get_path_impl,
        VfsFileOpenLogged,
        retro_vfs_file_close_impl,
        retro_vfs_file_size_impl,
        retro_vfs_file_tell_impl,
        retro_vfs_file_seek_impl,
        retro_vfs_file_read_impl,
        retro_vfs_file_write_impl,
        retro_vfs_file_flush_impl,
        retro_vfs_file_remove_impl,
        retro_vfs_file_rename_impl,
        retro_vfs_file_truncate_impl,
        retro_vfs_stat_impl,
        retro_vfs_mkdir_impl,
        retro_vfs_opendir_impl,
        retro_vfs_readdir_impl,
        retro_vfs_dirent_get_name_impl,
        retro_vfs_dirent_is_dir_impl,
        retro_vfs_closedir_impl
    };    
}

bool EnvironmentHandler::Callback(uint32_t cmd, void* data)
{
    auto instance = Wrapper::GetCurrentThreadWrapper();
    if (!instance)
    {
        LogError("Callback: Null Instance.");
        return false;
    }

    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_ROTATION:                                        return data && instance->m_video_handler->SetRotation(*static_cast<uint32_t*>(data));
    case RETRO_ENVIRONMENT_GET_OVERSCAN:                                        return instance->m_video_handler->GetOverscan(static_cast<int32_t*>(data));
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:                                        return instance->m_video_handler->GetCanDupe(static_cast<bool*>(data));
    case RETRO_ENVIRONMENT_SET_MESSAGE:                                         return instance->m_message_handler->SetMessage(static_cast<const retro_message*>(data));
    case RETRO_ENVIRONMENT_SHUTDOWN:                                            return instance->Shutdown();
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:                               return instance->m_environment_handler->SetPerformanceLevel(static_cast<uint32_t*>(data));
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:                                return instance->m_environment_handler->GetSystemDirectory(static_cast<const char**>(data));
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:                                    return instance->m_video_handler->SetPixelFormat(static_cast<const retro_pixel_format*>(data));
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:                               return instance->m_input_handler->SetInputDescriptors(static_cast<const retro_input_descriptor*>(data));
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:                               return instance->m_input_handler->SetKeyboardEventCallback(static_cast<const retro_keyboard_callback*>(data));
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:                          return instance->m_environment_handler->SetDiskControlInterface(static_cast<const retro_disk_control_callback*>(data));
    case RETRO_ENVIRONMENT_SET_HW_RENDER:                                       return instance->m_video_handler->SetHwRender(static_cast<retro_hw_render_callback*>(data));
    case RETRO_ENVIRONMENT_GET_VARIABLE:                                        return instance->m_options_handler->GetVariable(static_cast<retro_variable*>(data));
    case RETRO_ENVIRONMENT_SET_VARIABLES:                                       return instance->m_options_handler->SetVariables(static_cast<const retro_variable*>(data));
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:                                 return instance->m_options_handler->GetVariableUpdate(static_cast<bool*>(data));
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:                                 return instance->m_core->SetSupportsNoGame(static_cast<bool*>(data));
    case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:                                   return instance->m_core->GetLibretroPath(static_cast<const char**>(data));
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:                             return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:                                  return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:                                return instance->m_input_handler->GetRumbleInterface(static_cast<retro_rumble_interface*>(data));
    case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:                       return instance->m_input_handler->GetInputDeviceCapabilities(static_cast<uint32_t*>(data));
    case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:                                return instance->m_input_handler->GetSensorInterface(static_cast<retro_sensor_interface*>(data));
    case RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE:                                return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:                                   return instance->m_log_handler->GetLogInterface(static_cast<retro_log_callback*>(data));
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:                                  return instance->m_environment_handler->GetPerfInterface(static_cast<retro_perf_callback*>(data));
    case RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE:                              return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:                           return instance->m_environment_handler->GetCoreAssetsDirectory(static_cast<const char**>(data));
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:                                  return instance->m_environment_handler->GetSaveDirectory(static_cast<const char**>(data));
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:                                  return instance->m_environment_handler->SetSystemAvInfo(static_cast<const retro_system_av_info*>(data));
    case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK:                           return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:                                  return instance->m_environment_handler->SetSubsystemInfo(static_cast<const retro_subsystem_info*>(data));
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:                                 return instance->m_input_handler->SetControllerInfo(static_cast<const retro_controller_info*>(data));
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:                                     return instance->m_environment_handler->SetMemoryMaps(static_cast<const retro_memory_map*>(data));
    case RETRO_ENVIRONMENT_SET_GEOMETRY:                                        return instance->m_video_handler->SetGeometry(static_cast<const retro_game_geometry*>(data));
    case RETRO_ENVIRONMENT_GET_USERNAME:                                        return instance->m_environment_handler->GetUsername(static_cast<const char**>(data));
    case RETRO_ENVIRONMENT_GET_LANGUAGE:                                        return instance->m_environment_handler->GetLanguage(static_cast<retro_language*>(data));
    // Optional per-frame optimization (core renders straight into our framebuffer);
    // we don't provide one, so return false WITHOUT logging: the core falls back
    // to its own buffer + video_refresh, and cores poll this every frame (spam).
    case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER:                    return EnvironmentNotImplemented(cmd, false);
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
    {
        auto** out = static_cast<const retro_hw_render_interface**>(data);
        *out = instance->m_video_handler->GetHwRenderInterface();
        return (*out != nullptr);
    }
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:                            return instance->m_environment_handler->SetSupportAchievements(static_cast<bool*>(data));
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
    {
        auto* iface = static_cast<retro_hw_render_context_negotiation_interface_vulkan*>(data);
        instance->m_video_handler->SetNegotiationInterface(iface);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:                            return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:                               return instance->m_video_handler->SetSharedContext();
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:                                   return instance->m_environment_handler->GetVfsInterface(static_cast<retro_vfs_interface_info*>(data));
    case RETRO_ENVIRONMENT_GET_LED_INTERFACE:                                   return instance->m_environment_handler->GetLedInterface(static_cast<retro_led_interface*>(data));
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:                              return instance->m_environment_handler->GetAudioVideoEnable(static_cast<retro_av_enable_flags*>(data));
    case RETRO_ENVIRONMENT_GET_MIDI_INTERFACE:                                  return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:                                  return instance->m_environment_handler->GetFastForwarding(static_cast<bool*>(data));
    case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:                             return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:                                  return instance->m_input_handler->GetInputBitmasks(static_cast<bool*>(data));
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:                            return instance->m_options_handler->GetCoreOptionsVersion(static_cast<uint32_t*>(data));
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:                                    return instance->m_options_handler->SetCoreOptions(static_cast<const retro_core_option_definition*>(data));
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:                               return instance->m_options_handler->SetCoreOptions(static_cast<const retro_core_options_intl*>(data));
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:                            return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:                             return instance->m_video_handler->GetPreferredHwRender(static_cast<retro_hw_context_type*>(data));
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:                  return instance->m_environment_handler->GetDiskControlInterfaceVersion(static_cast<uint32_t*>(data));
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:                      return instance->m_environment_handler->SetDiskControlExtInterface(static_cast<const retro_disk_control_ext_callback*>(data));
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:                       return instance->m_message_handler->GetMessageInterfaceVersion(static_cast<uint32_t*>(data));
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:                                     return instance->m_message_handler->SetMessageExt(static_cast<const retro_message_ext*>(data));
    case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:                                 return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:                    return instance->m_audio_handler->SetAudioBufferStatusCallback(static_cast<const retro_audio_buffer_status_callback*>(data));
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:                           return instance->m_audio_handler->SetMinimumAudioLatency(static_cast<const uint32_t*>(data));
    case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:                         return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:                           return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:                                   return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:                                 return instance->m_options_handler->SetCoreOptionsV2(static_cast<const retro_core_options_v2*>(data));
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:                            return instance->m_options_handler->SetCoreOptionsV2Intl(static_cast<const retro_core_options_v2_intl*>(data));
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:            return instance->m_options_handler->SetCoreOptionsUpdateDisplayCallback(static_cast<const retro_core_options_update_display_callback*>(data));
    case RETRO_ENVIRONMENT_SET_VARIABLE:                                        return instance->m_options_handler->SetVariable(static_cast<const retro_variable*>(data));
    case RETRO_ENVIRONMENT_GET_THROTTLE_STATE:                                  return instance->m_environment_handler->GetThrottleState(static_cast<retro_throttle_state*>(data));
    case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:                               return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
    {
        auto* s = static_cast<retro_hw_render_context_negotiation_interface*>(data);
        s->interface_type    = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
        s->interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:                                     return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE:                            return instance->m_microphone_handler->GetMicrophoneInterface(static_cast<retro_microphone_interface*>(data));
    case RETRO_ENVIRONMENT_GET_DEVICE_POWER:                                    return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:                             return EnvironmentNotImplemented(cmd);
    // Both spellings: cores probe the experimental number today, and will
    // find the plain one if libretro ever assigns it.
    case RETRO_ENVIRONMENT_GET_LINK_INTERFACE:
    case RETRO_ENVIRONMENT_GET_LINK_INTERFACE_FINAL:                            return instance->m_environment_handler->GetLinkInterface(static_cast<retro_link_interface*>(data));
    case RETRO_ENVIRONMENT_GET_TRANSFER_PAK_INTERFACE:
    case RETRO_ENVIRONMENT_GET_TRANSFER_PAK_INTERFACE_FINAL:                     return instance->m_environment_handler->GetTransferPakInterface(static_cast<retro_transfer_pak_interface*>(data), instance);
    case RETRO_ENVIRONMENT_GET_CONTROLLER_AUDIO_INTERFACE:
    case RETRO_ENVIRONMENT_GET_CONTROLLER_AUDIO_INTERFACE_FINAL:                 return instance->m_environment_handler->GetControllerAudioInterface(static_cast<retro_controller_audio_interface*>(data), instance);
    case RETRO_ENVIRONMENT_GET_CONTROLLER_DISPLAY_INTERFACE:                     return instance->m_environment_handler->GetControllerDisplayInterface(static_cast<retro_controller_display_interface*>(data), instance);
    case RETRO_ENVIRONMENT_GET_PLAYLIST_DIRECTORY:                              return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_FILE_BROWSER_START_DIRECTORY:                    return EnvironmentNotImplemented(cmd);
    // custom environment commands
    case RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND:                        return EnvironmentNotImplemented(cmd);
    case RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB:                       return instance->m_environment_handler->GetClearAllThreadWaitsCb(static_cast<retro_environment_t*>(data));
    case RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE:                                  return EnvironmentNotImplemented(cmd);
    default:                                                                    return EnvironmentUnknown(cmd);
    }

    return true;
}

void EnvironmentHandler::SetDirectories(const std::string& system_directory, const std::string& save_directory, const std::string& core_assets_directory)
{
    m_system_directory = system_directory;
    m_save_directory = save_directory;
    m_core_assets_directory = core_assets_directory;
}

bool EnvironmentHandler::SetPerformanceLevel(uint32_t* level)
{
    if (!level)
        return true;

    m_performance_level = *static_cast<uint32_t*>(level);
    return true;
}

bool EnvironmentHandler::GetSystemDirectory(const char** directory)
{
    if (!directory)
    {
        LogError("Passed null directory.");
        return false;
    }

    if (!std::filesystem::is_directory(m_system_directory))
    {
        std::error_code ec;
        std::filesystem::create_directories(m_system_directory, ec);
        if (ec)
        {
            LogError("Failed to create system directory: " + m_system_directory + " - " + ec.message());
            return false;
        }
    }

    if (!m_system_directory_announced)
    {
        m_system_directory_announced = true;
        Log("System directory handed to core: " + m_system_directory);
    }
    *directory = m_system_directory.c_str();
    return true;
}

bool EnvironmentHandler::GetSaveDirectory(const char** directory)
{
    if (!directory)
    {
        LogError("Passed null directory.");
        return false;
    }

    if (!std::filesystem::is_directory(m_save_directory))
    {
        std::error_code ec;
        std::filesystem::create_directories(m_save_directory, ec);
        if (ec)
        {
            LogError("Failed to create save directory: " + m_save_directory + " - " + ec.message());
            return false;
        }
    }

    if (!m_save_directory_announced)
    {
        m_save_directory_announced = true;
        Log("Save directory handed to core: " + m_save_directory);
    }
    *directory = m_save_directory.c_str();
    return true;
}

bool EnvironmentHandler::GetCoreAssetsDirectory(const char** directory)
{
    if (!directory)
    {
        LogError("Passed null directory.");
        return false;
    }

    if (!std::filesystem::is_directory(m_core_assets_directory))
    {
        std::error_code ec;
        std::filesystem::create_directories(m_core_assets_directory, ec);
        if (ec)
        {
            LogError("Failed to create core assets directory: " + m_core_assets_directory + " - " + ec.message());
            return false;
        }
    }

    *directory = m_core_assets_directory.c_str();
    return true;
}

bool EnvironmentHandler::SetDiskControlInterface(const retro_disk_control_callback* callback)
{
    if (!callback)
    {
        m_disk_control_callback = {};
        return true;
    }

    m_disk_control_callback = *callback;
    return true;
}

bool EnvironmentHandler::GetPerfInterface(retro_perf_callback* callback)
{
    if (!callback)
        return true;

    callback->get_time_usec = retro_perf_get_time_usec;
    callback->get_cpu_features = retro_get_cpu_features;
    callback->get_perf_counter = retro_perf_get_counter;
    callback->perf_register = retro_perf_register;
    callback->perf_start = retro_perf_start;
    callback->perf_stop = retro_perf_stop;
    callback->perf_log = retro_perf_log;

    return true;
}

bool EnvironmentHandler::SetSystemAvInfo(const retro_system_av_info* av_info)
{
    Wrapper* wrapper = Wrapper::GetCurrentThreadWrapper();
    return wrapper && wrapper->SetSystemAvInfo(av_info);
}

bool EnvironmentHandler::SetSubsystemInfo(const retro_subsystem_info* subsystem_info)
{
    // A core may legally publish this more than once; last call wins, which is
    // what RetroArch does.
    m_subsystems.clear();

    // Publishing nothing is a valid answer, not a failure.
    if (!subsystem_info)
        return true;

    // The outer array is terminated by a null desc, the same shape
    // SET_CONTROLLER_INFO uses. num_roms and num_memory are counts WITHIN an
    // entry, not the bound of the array itself.
    for (int i = 0; subsystem_info[i].desc; ++i)
    {
        const retro_subsystem_info& src = subsystem_info[i];

        // ident is the only field content is ever looked up by. An entry with
        // none can never be selected, and storing it under an empty key would
        // let an empty ident match it by accident.
        if (!src.ident)
        {
            LogWarning("SET_SUBSYSTEM_INFO: entry " + std::to_string(i) + " has no ident, skipping");
            continue;
        }

        SubsystemInfo entry;
        entry.desc = src.desc;
        entry.ident = src.ident;
        entry.id = src.id;

        if (src.roms)
        {
            entry.roms.reserve(src.num_roms);
            for (unsigned j = 0; j < src.num_roms; ++j)
            {
                const retro_subsystem_rom_info& rom_src = src.roms[j];

                SubsystemRomInfo rom;
                // Null-guarded the way SetControllerInfo guards a null desc:
                // cores do miscount these.
                rom.desc = rom_src.desc ? rom_src.desc : "";
                rom.valid_extensions = rom_src.valid_extensions ? rom_src.valid_extensions : "";
                rom.need_fullpath = rom_src.need_fullpath;
                rom.block_extract = rom_src.block_extract;
                rom.required = rom_src.required;

                if (rom_src.memory)
                {
                    rom.memory.reserve(rom_src.num_memory);
                    for (unsigned k = 0; k < rom_src.num_memory; ++k)
                    {
                        SubsystemMemoryInfo memory;
                        memory.extension = rom_src.memory[k].extension ? rom_src.memory[k].extension : "";
                        memory.type = rom_src.memory[k].type;
                        rom.memory.emplace_back(std::move(memory));
                    }
                }

                entry.roms.emplace_back(std::move(rom));
            }
        }

        Log("Subsystem '" + entry.ident + "' (" + entry.desc + "): "
            + std::to_string(entry.roms.size()) + " rom(s), id=" + std::to_string(entry.id));

        m_subsystems.emplace_back(std::move(entry));
    }

    // This used to return false, which tells a core the frontend cannot do
    // subsystems at all -- and a core told that may then legitimately refuse
    // retro_load_game_special.
    return true;
}

const EnvironmentHandler::SubsystemInfo* EnvironmentHandler::FindSubsystem(const std::string& ident) const
{
    for (const SubsystemInfo& entry : m_subsystems)
    {
        if (entry.ident == ident)
            return &entry;
    }

    return nullptr;
}

bool EnvironmentHandler::SetMemoryMaps(const retro_memory_map* memory_maps)
{
    Wrapper* wrapper = Wrapper::GetCurrentThreadWrapper();
    if (wrapper)
        wrapper->SetMemoryDescriptors(memory_maps);
    return true;
}

bool EnvironmentHandler::GetUsername(const char** username) const
{
    if (username)
        *username = Wrapper::GetCurrentThreadWrapper()->m_username.c_str();
    return true;
}

bool EnvironmentHandler::GetLanguage(retro_language* language) const
{
    if (language)
        *language = RETRO_LANGUAGE_ENGLISH;
    return true;
}

bool EnvironmentHandler::SetSupportAchievements(bool* support)
{
    // Dereference rather than just testing the pointer, or a core calling this to
    // say "no" is recorded as saying nothing at all. The callback exists to opt
    // OUT; a core that never calls it is assumed to support achievements.
    if (support == nullptr)
        return false;

    Wrapper* wrapper = Wrapper::GetCurrentThreadWrapper();
    if (wrapper)
        wrapper->SetSupportsAchievements(*support);

    Log(std::string("Achievements: core reports ") + (*support ? "supported" : "unsupported"));
    return true;
}

bool EnvironmentHandler::GetVfsInterface(retro_vfs_interface_info* vfs_interface_info)
{
    if (!vfs_interface_info)
    {
        LogError("Passed null retro_vfs_interface_info.");
        return true;
    }

    if (vfs_interface_info->required_interface_version > s_supported_vfs_version)
    {
        LogError("Requested VFS interface not implemented: " + std::to_string(vfs_interface_info->required_interface_version));
        return false;
    }

    vfs_interface_info->required_interface_version = s_supported_vfs_version;
    vfs_interface_info->iface                      = &m_vfs_interface;

    return true;
}

bool EnvironmentHandler::GetLedInterface(retro_led_interface* led_interface)
{
    if (led_interface)
        led_interface->set_led_state = Wrapper::LedInterfaceSetLedState;
    return true;
}

bool EnvironmentHandler::GetAudioVideoEnable(retro_av_enable_flags* audio_video_enable)
{
    if (audio_video_enable)
        *audio_video_enable = static_cast<retro_av_enable_flags>(RETRO_AV_ENABLE_AUDIO | RETRO_AV_ENABLE_VIDEO);
    return true;
}

bool EnvironmentHandler::GetFastForwarding(bool* fast_forwarding)
{
    if (fast_forwarding)
        *fast_forwarding = false;
    return true;
}

bool EnvironmentHandler::GetDiskControlInterfaceVersion(uint32_t* version)
{
    if (version)
        *version = 1;
    return true;
}

bool EnvironmentHandler::SetDiskControlExtInterface(const retro_disk_control_ext_callback* callback)
{
    if (!callback)
    {
        m_disk_control_ext_callback = {};
        return true;
    }

    m_disk_control_ext_callback = *callback;
    return true;
}

// ── Disk control (emulation thread only) ────────────────────────────────────
// The ext struct's leading members mirror the v0 struct, so each accessor
// prefers the ext function pointer and falls back to v0.

bool EnvironmentHandler::HasDiskControl() const
{
    return m_disk_control_ext_callback.set_eject_state != nullptr
        || m_disk_control_callback.set_eject_state != nullptr;
}

bool EnvironmentHandler::GetDiskEjected() const
{
    if (m_disk_control_ext_callback.get_eject_state)
        return m_disk_control_ext_callback.get_eject_state();
    if (m_disk_control_callback.get_eject_state)
        return m_disk_control_callback.get_eject_state();
    return false;
}

uint32_t EnvironmentHandler::GetDiskImageIndex() const
{
    if (m_disk_control_ext_callback.get_image_index)
        return m_disk_control_ext_callback.get_image_index();
    if (m_disk_control_callback.get_image_index)
        return m_disk_control_callback.get_image_index();
    return 0;
}

uint32_t EnvironmentHandler::GetDiskImageCount() const
{
    if (m_disk_control_ext_callback.get_num_images)
        return m_disk_control_ext_callback.get_num_images();
    if (m_disk_control_callback.get_num_images)
        return m_disk_control_callback.get_num_images();
    return 0;
}

bool EnvironmentHandler::SetDiskEjected(bool ejected)
{
    bool ok = false;
    if (m_disk_control_ext_callback.set_eject_state)
        ok = m_disk_control_ext_callback.set_eject_state(ejected);
    else if (m_disk_control_callback.set_eject_state)
        ok = m_disk_control_callback.set_eject_state(ejected);
    else
    {
        LogWarning("Disk control: core has no set_eject_state");
        return false;
    }
    if (ok)
        Log(std::string("Disk control: tray ") + (ejected ? "opened" : "closed"));
    else
        LogWarning(std::string("Disk control: core refused to ") + (ejected ? "open" : "close") + " the tray");
    return ok;
}

bool EnvironmentHandler::SetDiskImageIndex(uint32_t index)
{
    bool ok = false;
    if (m_disk_control_ext_callback.set_image_index)
        ok = m_disk_control_ext_callback.set_image_index(index);
    else if (m_disk_control_callback.set_image_index)
        ok = m_disk_control_callback.set_image_index(index);
    else
    {
        LogWarning("Disk control: core has no set_image_index");
        return false;
    }
    if (ok)
        Log("Disk control: image index " + std::to_string(index) + " of " +
            std::to_string(GetDiskImageCount()) + " selected");
    else
        LogWarning("Disk control: core refused image index " + std::to_string(index) +
                   " (" + std::to_string(GetDiskImageCount()) + " images)");
    return ok;
}

bool EnvironmentHandler::ReplaceDiskImage(uint32_t index, const std::string& path)
{
    // Disc cores are need_fullpath, so a path-only game info suffices (this is
    // what RetroArch's "Load New Disc" sends for such cores).
    retro_game_info info = { path.c_str(), nullptr, 0, nullptr };
    bool ok = false;
    if (m_disk_control_ext_callback.replace_image_index)
        ok = m_disk_control_ext_callback.replace_image_index(index, &info);
    else if (m_disk_control_callback.replace_image_index)
        ok = m_disk_control_callback.replace_image_index(index, &info);
    else
    {
        LogWarning("Disk control: core has no replace_image_index");
        return false;
    }
    if (ok)
        Log("Disk control: image " + std::to_string(index) + " replaced with " + path);
    else
        LogWarning("Disk control: core refused to replace image " + std::to_string(index) +
                   " with " + path);
    return ok;
}

bool EnvironmentHandler::GetThrottleState(retro_throttle_state* state)
{
    if (!state)
        return true;

    // RETRO_THROTTLE_NONE carries the rate the frontend is actually calling
    // retro_run at, which here is the core's own declared fps: that figure is
    // the pacing loop's ceiling. A rate of 0 means something else entirely --
    // it is what the unthrottled and frame-stepping modes report -- and a core
    // that sizes its frame budget from this reads it as "unbounded". dosbox_pure
    // divides by it, so a 0 inflated its budget ~70x and drove its cycle
    // governor to the 4,000,000 ceiling at a tenth of real speed.
    state->mode = RETRO_THROTTLE_NONE;
    state->rate = 0.0f;

    if (Wrapper* wrapper = Wrapper::GetCurrentThreadWrapper())
    {
        double fps = wrapper->GetDeclaredFps();
        if (!(fps > 0.0))
            fps = wrapper->m_system_av_info.timing.fps;
        if (fps > 0.0)
            state->rate = static_cast<float>(fps);
    }

    return true;
}

bool EnvironmentHandler::GetClearAllThreadWaitsCb(retro_environment_t* env)
{
    if (env)
        *env = runloop_clear_all_thread_waits;
    return true;
}

bool EnvironmentHandler::GetLinkInterface(retro_link_interface* link_interface)
{
    if (!link_interface)
    {
        return false;
    }

    // Every member is the host's, and the trampolines recover the calling
    // instance from its emulation thread, so one shared table serves every core
    // in the process. The core is answered whether or not anything is cabled to
    // it yet: with no peers the bus grants without bound, which is what lets a
    // core keep a single code path and still run standalone.
    *link_interface = *LinkCoordinator::Interface();
    return true;
}

namespace
{
// The core calls these on the emulation thread while a pak is being populated.
// frontend_data is the Wrapper that answered, so several cores in one process
// each get their own table rather than sharing one set of globals -- which is
// the whole reason this interface exists.
const char* TransferPakRomTrampoline(void* frontend_data, unsigned port)
{
    return static_cast<Wrapper*>(frontend_data)->TransferPakRomFor(port);
}

const char* TransferPakRamTrampoline(void* frontend_data, unsigned port)
{
    return static_cast<Wrapper*>(frontend_data)->TransferPakRamFor(port);
}

unsigned TransferPakGenerationTrampoline(void* frontend_data, unsigned port)
{
    return static_cast<Wrapper*>(frontend_data)->TransferPakGenerationFor(port);
}

const char* TransferPakRtcTrampoline(void* frontend_data, unsigned port)
{
    return static_cast<Wrapper*>(frontend_data)->TransferPakRtcFor(port);
}
}

bool EnvironmentHandler::GetTransferPakInterface(retro_transfer_pak_interface* iface, Wrapper* instance)
{
    if (!iface || !instance)
    {
        return false;
    }

    // Answered whether or not any pak is seated yet: a port with nothing in it
    // returns NULL and the core treats that exactly as an empty pak, so the
    // core keeps one code path and the frontend can seat a pak mid-game.
    iface->frontend_data = instance;
    iface->get_rom       = &TransferPakRomTrampoline;
    iface->get_ram       = &TransferPakRamTrampoline;
    iface->generation    = &TransferPakGenerationTrampoline;
    iface->get_rtc       = &TransferPakRtcTrampoline;
    return true;
}

namespace
{
// Called on whichever thread runs the core's audio batch callback, just before
// the batch it belongs with.
bool ControllerAudioPushTrampoline(void* frontend_data, unsigned port, unsigned index,
                                   const int16_t* data, size_t frames)
{
    Wrapper* instance = static_cast<Wrapper*>(frontend_data);
    // Rollback replay: the main batch is dropped, so this is too.
    if (instance->IsNetplayReplaying())
        return true;
    if (!instance->m_audio_handler)
        return false;
    return instance->m_audio_handler->PushControllerFrames(port, index, data, frames);
}

// Any thread; the wrapper copies under its own lock.
bool ControllerDisplayRefreshTrampoline(void* frontend_data, unsigned port, unsigned index,
                                        const uint32_t* pixels, unsigned width, unsigned height)
{
    return static_cast<Wrapper*>(frontend_data)->StoreControllerScreen(port, index, pixels, width, height);
}
}

bool EnvironmentHandler::GetControllerAudioInterface(retro_controller_audio_interface* iface, Wrapper* instance)
{
    if (!iface || !instance)
        return false;

    iface->interface_version = RETRO_CONTROLLER_AUDIO_INTERFACE_VERSION;
    iface->frontend_data     = instance;
    iface->push              = &ControllerAudioPushTrampoline;
    return true;
}

bool EnvironmentHandler::GetControllerDisplayInterface(retro_controller_display_interface* iface, Wrapper* instance)
{
    if (!iface || !instance)
        return false;

    iface->interface_version = RETRO_CONTROLLER_DISPLAY_INTERFACE_VERSION;
    iface->frontend_data     = instance;
    iface->refresh           = &ControllerDisplayRefreshTrampoline;
    instance->NoteControllerScreensOffered();
    return true;
}
}
