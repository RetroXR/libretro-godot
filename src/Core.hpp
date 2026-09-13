#pragma once

#include <libretro.h>

#include <string>

#include "Debug.hpp"
#include "DynLib.hpp"

namespace Xenu
{
class Wrapper;
class CallbackTrampolines;

class Core
{
public:
    Core(const std::string& path);
    ~Core() = default;

    bool Load(CallbackTrampolines* trampolines);
    void Unload();

    const std::string& GetName() const;
    bool GetSupportsNoGame() const;
    bool GetNeedFullpath() const;

    /// What the core calls itself, read from retro_get_system_info at load.
    /// This, not the file name and not a hash of the binary, is the only build
    /// identity two peers on different platforms can compare: the same core
    /// built for win-x64 and android-arm64 shares a library_version and shares
    /// nothing else.
    const std::string& GetLibraryName() const;
    const std::string& GetLibraryVersion() const;
    uint32_t GetApiVersion() const;

    decltype(&::retro_set_environment) retro_set_environment                       = nullptr;
    decltype(&::retro_set_video_refresh) retro_set_video_refresh                   = nullptr;
    decltype(&::retro_set_audio_sample) retro_set_audio_sample                     = nullptr;
    decltype(&::retro_set_audio_sample_batch) retro_set_audio_sample_batch         = nullptr;
    decltype(&::retro_set_input_poll) retro_set_input_poll                         = nullptr;
    decltype(&::retro_set_input_state) retro_set_input_state                       = nullptr;
    decltype(&::retro_init) retro_init                                             = nullptr;
    decltype(&::retro_deinit) retro_deinit                                         = nullptr;
    decltype(&::retro_api_version) retro_api_version                               = nullptr;
    decltype(&::retro_get_system_info) retro_get_system_info                       = nullptr;
    decltype(&::retro_get_system_av_info) retro_get_system_av_info                 = nullptr;
    decltype(&::retro_set_controller_port_device) retro_set_controller_port_device = nullptr;
    decltype(&::retro_reset) retro_reset                                           = nullptr;
    decltype(&::retro_run) retro_run                                               = nullptr;
    decltype(&::retro_serialize_size) retro_serialize_size                         = nullptr;
    decltype(&::retro_serialize) retro_serialize                                   = nullptr;
    decltype(&::retro_unserialize) retro_unserialize                               = nullptr;
    decltype(&::retro_cheat_reset) retro_cheat_reset                               = nullptr;
    decltype(&::retro_cheat_set) retro_cheat_set                                   = nullptr;
    decltype(&::retro_load_game) retro_load_game                                   = nullptr;
    decltype(&::retro_load_game_special) retro_load_game_special                   = nullptr;
    decltype(&::retro_unload_game) retro_unload_game                               = nullptr;
    decltype(&::retro_get_region) retro_get_region                                 = nullptr;
    decltype(&::retro_get_memory_data) retro_get_memory_data                       = nullptr;
    decltype(&::retro_get_memory_size) retro_get_memory_size                       = nullptr;

    bool SetSupportsNoGame(bool* supports);
    bool GetLibretroPath(const char** path) const;

private:
    std::string m_name;
    std::string m_path;
    // Empty until the installed core has been copied successfully. Unload must
    // never remove m_path while it still names the user's installed core.
    std::string m_temporary_path;
    void* m_handle = nullptr;
    bool m_supports_no_game = false;
    bool m_need_fullpath = false;
    std::string m_library_name;
    std::string m_library_version;
    uint32_t m_api_version = 0;

    bool LoadHandle();

    /// A symbol the core is allowed not to have. Silent when it is missing,
    /// which is the whole difference from LoadFunction_: an absent optional
    /// symbol is a core without that extension, not a core that failed to load.
    template<typename T>
    void LoadOptionalFunction_(T& function_ptr, const std::string& function_name)
    {
        function_ptr = reinterpret_cast<T>(DynLib_Sym(m_handle, function_name.c_str()));
    }

    template<typename T>
    bool LoadFunction_(T& function_ptr, const std::string& function_name)
    {
        function_ptr = reinterpret_cast<T>(DynLib_Sym(m_handle, function_name.c_str()));
        if (!function_ptr)
        {
            LogError("Failed to load function: " + function_name);
            return false;
        }
        return true;
    }
};
}
