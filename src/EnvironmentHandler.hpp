#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <libretro.h>

#include "ControllerAudioInterface.hpp"
#include "ControllerDisplayInterface.hpp"
#include "LinkInterface.hpp"
#include "TransferPakInterface.hpp"

namespace Xenu
{
class Wrapper;

class EnvironmentHandler
{
public:
    EnvironmentHandler();
    ~EnvironmentHandler() = default;

    static bool Callback(uint32_t cmd, void* data);

    void SetDirectories(const std::string& system_directory, const std::string& save_directory, const std::string& core_assets_directory);

    // Disk control (physical disc eject/swap). EMULATION THREAD ONLY, since these
    // call straight into the core's registered callbacks. Prefer the ext
    // callback, fall back to v0; every call is null-guarded so cores without
    // the interface are safe no-ops.
    bool HasDiskControl() const;
    bool GetDiskEjected() const;
    uint32_t GetDiskImageIndex() const;
    uint32_t GetDiskImageCount() const;
    bool SetDiskEjected(bool ejected);
    bool SetDiskImageIndex(uint32_t index);
    bool ReplaceDiskImage(uint32_t index, const std::string& path);

    // Subsystems: multi-file content a core loads as one unit -- an N64
    // cartridge together with its 64DD disk, a pair of linked Game Boys, a
    // Satellaview broadcast alongside its host cartridge.
    //
    // A core publishes these during retro_set_environment, so this table is
    // filled while the core is loading and read on that same thread by the
    // content-load path. It stays empty for the great majority of cores.
    //
    // Owned copies rather than the core's own pointers. The strings here are
    // only ever read back by us and never handed to anything else, so plain
    // std::string is safe -- unlike the memory descriptors, which must keep a
    // char* alive for rcheevos and therefore need a parallel arena.
    struct SubsystemMemoryInfo
    {
        std::string extension;
        uint32_t type = 0;
    };

    // need_fullpath here is PER ROM, and is not the same thing as
    // Core::GetNeedFullpath(), which is a single flag for the whole core. A
    // 64DD run is exactly the case that differs: the disk is opened from its
    // path while the cartridge is handed over as bytes.
    struct SubsystemRomInfo
    {
        std::string desc;
        std::string valid_extensions;
        bool need_fullpath = false;
        bool block_extract = false;
        bool required = false;
        std::vector<SubsystemMemoryInfo> memory;
    };

    struct SubsystemInfo
    {
        std::string desc;
        std::string ident;
        std::vector<SubsystemRomInfo> roms;
        uint32_t id = 0;
    };

    const std::vector<SubsystemInfo>& GetSubsystems() const { return m_subsystems; }

    // Resolve a published ident ("ndd", "gb_link", ...). Null when the core has
    // no such subsystem, which is how a stale or mistyped catalog entry surfaces.
    const SubsystemInfo* FindSubsystem(const std::string& ident) const;

private:
    static const uint32_t s_supported_vfs_version = 3;

    std::vector<SubsystemInfo> m_subsystems;

    uint32_t m_performance_level = 0;
    std::string m_system_directory;
    bool m_system_directory_announced = false;
    bool m_save_directory_announced = false;
    std::string m_save_directory;
    std::string m_core_assets_directory;
    retro_vfs_interface m_vfs_interface;
    retro_disk_control_callback m_disk_control_callback = {};
    retro_disk_control_ext_callback m_disk_control_ext_callback = {};

    bool SetPerformanceLevel(uint32_t* level);
    bool GetSystemDirectory(const char** directory);
    bool GetSaveDirectory(const char** directory);
    bool GetCoreAssetsDirectory(const char** directory);
    bool SetDiskControlInterface(const retro_disk_control_callback* callback);
    bool GetPerfInterface(retro_perf_callback* callback);
    bool SetSystemAvInfo(const retro_system_av_info* av_info);
    bool SetSubsystemInfo(const retro_subsystem_info* subsystem_info);
    bool SetMemoryMaps(const retro_memory_map* memory_maps);
    bool GetUsername(const char** username) const;
    bool GetLanguage(retro_language* language) const;
    bool SetSupportAchievements(bool* support);
    bool GetVfsInterface(retro_vfs_interface_info* vfs_interface_info);
    bool GetLedInterface(retro_led_interface* led_interface);
    bool GetAudioVideoEnable(retro_av_enable_flags* flags);
    bool GetFastForwarding(bool* fast_forwarding);
    bool GetDiskControlInterfaceVersion(uint32_t* version);
    bool SetDiskControlExtInterface(const retro_disk_control_ext_callback* callback);
    bool GetThrottleState(retro_throttle_state* state);
    bool GetClearAllThreadWaitsCb(retro_environment_t* env);
    bool GetLinkInterface(retro_link_interface* link_interface);
    bool GetTransferPakInterface(retro_transfer_pak_interface* iface, Wrapper* instance);
    bool GetTransferPakClockInterface(retro_transfer_pak_clock_interface* iface, Wrapper* instance);
    bool GetControllerAudioInterface(retro_controller_audio_interface* iface, Wrapper* instance);
    bool GetControllerDisplayInterface(retro_controller_display_interface* iface, Wrapper* instance);
};
}
