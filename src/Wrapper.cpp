#include "Wrapper.hpp"

#include "RetroAchievements.hpp"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "Libretro.hpp"
#include "LinkCoordinator.hpp"
#include "Debug.hpp"
#include "ScopeExit.hpp"
#include "ThreadCommandInitAudio.hpp"
#include "ThreadCommandReinitAudio.hpp"
#include "ThreadCommandCreateTexture.hpp"
#include "ThreadCommandUpdateTexture.hpp"
#include "ThreadCommandEmitSignal.hpp"

using namespace godot;

namespace Xenu
{
thread_local Wrapper* t_current_wrapper = nullptr;

Wrapper* Wrapper::GetCurrentThreadWrapper()
{
    return t_current_wrapper;
}

void Wrapper::SetCurrentThreadWrapper(Wrapper* wrapper)
{
    t_current_wrapper = wrapper;
}

// Free-function form for callers that must not depend on Wrapper's full
// definition. LinkCoordinator uses this so the link bus can be built and
// tested without dragging in Godot.
Wrapper* CurrentThreadWrapper()
{
    return t_current_wrapper;
}
// Locate a core inside <root>/cores. Android cores are conventionally named
// "<name>_libretro_android.so" but not universally (azahar ships as plain
// "azahar_libretro.so"), so try every convention the platform can use and take
// the first that exists. With none present, return the canonical name so the
// error names that file.
std::string Wrapper::ResolveCorePath(const std::string& root_directory, const std::string& core_name)
{
    const std::filesystem::path cores_dir = std::filesystem::path(root_directory).append("cores");

    static const char* const suffixes[] = {
#ifdef __ANDROID__
        "_libretro_android.so", "_libretro.so",
#elif defined(__linux__)
        "_libretro.so",
#elif defined(__APPLE__)
        "_libretro.dylib",
#else
        "_libretro.dll",
#endif
    };

    for (const char* suffix : suffixes)
    {
        std::filesystem::path candidate = cores_dir / (core_name + suffix);
        if (std::filesystem::is_regular_file(candidate))
            return candidate.string();
    }
    return (cores_dir / (core_name + suffixes[0])).string();
}

void Wrapper::StartContent(const std::string& root_directory, const std::string& core_name, const std::string& game_path)
{
    StartSubsystemContent(root_directory, core_name, game_path, std::string(), {});
}

void Wrapper::StartSubsystemContent(const std::string& root_directory, const std::string& core_name,
                                    const std::string& game_path, const std::string& subsystem_ident,
                                    const std::vector<std::string>& subsystem_paths)
{
    // Nothing is told where to render. The core draws into its own texture and a
    // display samples it, so a machine with nowhere to show its picture is not a
    // case this has to know about.

    StopEmulationThread();

    m_no_content_passes_null = GetNoContentPassesNull();

    m_microphone_handler->ReleaseAll();
    m_microphone_handler->SetSuspended(false);
    m_camera_handler->Reset();
    m_camera_handler->SetSuspended(false);

    // A cartridge run owns an in-memory ROM image. Do not retain or reuse it
    // when this Wrapper is restarted with disc/full-path content.
    std::vector<uint8_t>().swap(m_game_buffer);

    // Same reasoning for a subsystem set: a restart must not inherit the last
    // run's ROM images.
    std::vector<std::vector<unsigned char>>().swap(m_subsystem_buffers);

    auto audio_stream_player = memnew(AudioStreamPlayer3D);
    audio_stream_player->set_name("AudioStreamPlayer3D");
    audio_stream_player->set_attenuation_model(AudioStreamPlayer3D::ATTENUATION_INVERSE_DISTANCE);
    audio_stream_player->set_panning_strength(1.0f);
    audio_stream_player->set_max_db(0.0f);
    Libretro* owner = LiveLibretroNode();
    if (!owner)
    {
        memdelete(audio_stream_player);
        LogError("StartContent: the Libretro node is gone");
        return;
    }
    owner->add_child(audio_stream_player);

    std::filesystem::path core_path = ResolveCorePath(root_directory, core_name);

    m_core = std::make_unique<Core>(core_path.string());
    m_trampolines = std::make_unique<CallbackTrampolines>(this);
    m_environment_handler = std::make_unique<EnvironmentHandler>();
    m_video_handler = std::make_unique<VideoHandler>();
    m_audio_handler = std::make_unique<AudioHandler>();
    m_audio_handler->SetAudioStreamPlayer(audio_stream_player);
    // Where the machines this one gets cabled to will find its sink: a bus
    // paces off the neediest of them. Forgotten again by DropOwner.
    LinkCoordinator::Get().SetSinkClock(this, m_audio_handler->Clock());
    m_input_handler = std::make_unique<InputHandler>();
    m_options_handler = std::make_unique<OptionsHandler>();
    m_message_handler = std::make_unique<MessageHandler>();
    m_log_handler = std::make_unique<LogHandler>();

    m_video_handler->Init();

    m_root_directory = root_directory;
    m_temp_directory = std::filesystem::path(root_directory).append("temp").string();
    m_game_path = game_path;
    // Set before the emulation thread is constructed, like everything else here:
    // the std::thread constructor is the synchronisation edge, and these are read
    // only on that thread.
    m_subsystem_ident = subsystem_ident;
    m_subsystem_paths = subsystem_paths;
    // Per-content, and a restart reuses this Wrapper, so the descriptors belong to
    // the core instance that is about to be torn down and re-created.
    m_memory_descriptors.clear();
    m_memory_addrspaces.clear();
    m_supports_achievements = true;
    std::string system_directory = std::filesystem::path(root_directory).append("system").append(core_name).string();
    std::string save_directory = std::filesystem::path(root_directory).append("save").append(core_name).string();
    std::string core_assets_directory = std::filesystem::path(root_directory).append("core_assets").append(core_name).string();
    m_environment_handler->SetDirectories(system_directory, save_directory, core_assets_directory);

    if (!std::filesystem::is_directory(m_temp_directory))
    {
        std::error_code ec;
        std::filesystem::create_directories(m_temp_directory, ec);
        if (ec)
        {
            LogError("Failed to create temp directory: " + m_temp_directory + " - " + ec.message());
            return;
        }
    }

    ClearCoreIdentity();
    m_controller_screens_offered.store(false, std::memory_order_relaxed);

    m_stop_requested = false;
    m_thread_exited = false;
    // Before the thread exists, so a request made on the very next line of the
    // caller's frame is queued for it rather than refused.
    m_starting = true;
    m_thread = std::thread(&Wrapper::EmulationThreadLoop, this);
}

void Wrapper::StopContent()
{
    // Silence output immediately (main-thread entry point, so touching the
    // AudioStreamPlayer3D is safe here): a core whose teardown hangs can keep
    // firing audio callbacks from its internal threads long after the stop, and
    // the player must not keep voicing them.
    if (m_audio_handler)
        m_audio_handler->SetPlaying(false);
    m_microphone_handler->SetSuspended(true);
    m_camera_handler->SetSuspended(true);
    // Non-blocking: the join + teardown (SRAM flush, retro_unload_game,
    // retro_deinit, DLL unload) all happen off the main thread / deferred to
    // _process, so powering a system off does not hitch the frame.
    StopEmulationThread(false);
}

bool Wrapper::ShutdownForExit(uint32_t budget_ms)
{
    if (m_audio_handler)
        m_audio_handler->SilenceForTeardown();
    m_microphone_handler->SetSuspended(true);
    m_camera_handler->SetSuspended(true);
    return StopEmulationThreadBounded(budget_ms);
}

bool Wrapper::StopEmulationThreadBounded(uint32_t budget_ms)
{
    if (!m_core)
        return true;

    m_stop_requested = true;
    m_running = false;
    m_condition_variable.notify_all();
    m_np_cv.notify_all();
    m_stopping = true;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (!m_thread_exited.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!m_thread_exited.load(std::memory_order_acquire))
        return false;

    if (m_thread.joinable())
        m_thread.join();
    FinishTeardown();
    return true;
}

void Wrapper::AbandonThread()
{
    if (m_audio_handler)
        m_audio_handler->SilenceForTeardown();
    m_microphone_handler->SetSuspended(true);
    m_camera_handler->SetSuspended(true);
    // Deliberately no FinishTeardown: the handlers and the core are still in use
    // by the thread we are walking away from.
    if (m_thread.joinable())
        m_thread.detach();
}

Libretro* Wrapper::LiveLibretroNode() const
{
    if (m_libretro_node_id == 0)
        return nullptr;
    return Object::cast_to<Libretro>(ObjectDB::get_instance(m_libretro_node_id));
}

void Wrapper::NotifyContentLoadFailed(const char* reason) const
{
    if (Libretro* node = LiveLibretroNode())
        node->NotifyContentLoadFailed(godot::String(reason));
}

namespace
{
// Read on the emulation thread as the content loads, written from GDScript
// before StartContent. Defaults to the libretro convention.
std::atomic<bool> g_no_content_passes_null{ true };
}

void Wrapper::SetNoContentPassesNull(bool passes_null)
{
    g_no_content_passes_null.store(passes_null, std::memory_order_relaxed);
}

bool Wrapper::GetNoContentPassesNull()
{
    return g_no_content_passes_null.load(std::memory_order_relaxed);
}

Ref<ImageTexture> Wrapper::GetVideoTexture() const
{
    return m_video_handler ? m_video_handler->GetTexture() : Ref<ImageTexture>();
}

Ref<Image> Wrapper::GetVideoImage() const
{
    return m_video_handler ? m_video_handler->GetImage() : Ref<Image>();
}

VideoGeometry Wrapper::GetVideoGeometry() const
{
    return m_video_handler ? m_video_handler->GetGeometry() : VideoGeometry{};
}

uint32_t Wrapper::GetVideoGeometrySerial() const
{
    return m_video_handler ? m_video_handler->GetGeometrySerial() : 0u;
}

bool Wrapper::HasControllerScreens() const
{
    return m_controller_screens_offered.load(std::memory_order_relaxed);
}

void Wrapper::NoteControllerScreensOffered()
{
    std::lock_guard<std::mutex> lock(m_controller_screen_mutex);
    // Emptied rather than erased: a texture may only be freed on the main thread.
    for (auto& [key, screen] : m_controller_screens)
    {
        screen.pixels.clear();
        screen.stamp = 0;
        screen.seen = 0;
    }
    m_controller_screens_offered.store(true, std::memory_order_relaxed);
}

bool Wrapper::StoreControllerScreen(unsigned port, unsigned index, const uint32_t* pixels,
                                    unsigned width, unsigned height)
{
    if (!pixels || width == 0 || height == 0 || width > 4096 || height > 4096)
        return false;
    const uint64_t key = (static_cast<uint64_t>(port) << 32) | index;
    std::lock_guard<std::mutex> lock(m_controller_screen_mutex);
    ControllerScreen& screen = m_controller_screens[key];
    screen.width = width;
    screen.height = height;
    screen.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height);
    ++screen.stamp;
    return true;
}

Ref<ImageTexture> Wrapper::GetControllerScreenTexture(int port, int index)
{
    if (port < 0 || index < 0)
        return Ref<ImageTexture>();
    const uint64_t key = (static_cast<uint64_t>(port) << 32) | static_cast<uint32_t>(index);

    PackedByteArray bytes;
    unsigned width = 0;
    unsigned height = 0;
    uint64_t stamp = 0;
    {
        std::lock_guard<std::mutex> lock(m_controller_screen_mutex);
        auto it = m_controller_screens.find(key);
        if (it == m_controller_screens.end() || it->second.pixels.empty())
            return Ref<ImageTexture>();
        ControllerScreen& screen = it->second;
        if (screen.texture.is_valid() && screen.stamp == screen.seen)
            return screen.texture;
        width = screen.width;
        height = screen.height;
        stamp = screen.stamp;
        bytes.resize(static_cast<int64_t>(screen.pixels.size()) * 4);
        uint8_t* dst = bytes.ptrw();
        for (uint32_t px : screen.pixels)
        {
            *dst++ = static_cast<uint8_t>(px >> 16);
            *dst++ = static_cast<uint8_t>(px >> 8);
            *dst++ = static_cast<uint8_t>(px);
            *dst++ = 255;
        }
    }

    Ref<Image> image = Image::create_from_data(
        static_cast<int32_t>(width), static_cast<int32_t>(height), false, Image::FORMAT_RGBA8, bytes);
    if (image.is_null())
        return Ref<ImageTexture>();

    std::lock_guard<std::mutex> lock(m_controller_screen_mutex);
    ControllerScreen& screen = m_controller_screens[key];
    // update() keeps a texture's size, so a resized screen gets a new one.
    if (screen.texture.is_valid() && screen.texture->get_width() == static_cast<int32_t>(width)
            && screen.texture->get_height() == static_cast<int32_t>(height))
        screen.texture->update(image);
    else
        screen.texture = ImageTexture::create_from_image(image);
    screen.seen = stamp;
    return screen.texture;
}

void Wrapper::SetAudioPlaying(bool playing)
{
    if (m_audio_handler)
        m_audio_handler->SetPlaying(playing);
}

uint32_t Wrapper::GetAudioBufferOccupancy() const
{
    return m_audio_handler ? m_audio_handler->BufferOccupancy() : 0;
}

double Wrapper::GetAudioBrakeMs() const
{
    return m_audio_handler ? m_audio_handler->LastBrakeMs() : 0.0;
}

Dictionary Wrapper::GetPacingStats()
{
    Dictionary stats;
    stats["run_ms"] = static_cast<double>(m_run_ns.load(std::memory_order_relaxed)) / 1.0e6;
    stats["run_worst_ms"] =
        static_cast<double>(m_run_worst_ns.exchange(0, std::memory_order_relaxed)) / 1.0e6;
    stats["brake_sleep_ms"] =
        static_cast<double>(m_brake_sleep_ns.load(std::memory_order_relaxed)) / 1.0e6;
    stats["audio_frames"] =
        static_cast<int64_t>(m_audio_handler ? m_audio_handler->FramesProduced() : 0);
    return stats;
}

bool Wrapper::AcceptsEmuCommands() const
{
    return m_core && (m_running.load(std::memory_order_acquire) ||
                      m_starting.load(std::memory_order_acquire));
}

void Wrapper::AnswerNoSaveState()
{
    if (Libretro* node = LiveLibretroNode())
        node->call_deferred("emit_signal", "savestate_ready",
            godot::PackedByteArray(), static_cast<int64_t>(-1));
}

void Wrapper::AnswerNoLoadState()
{
    if (Libretro* node = LiveLibretroNode())
        node->call_deferred("emit_signal", "savestate_loaded", false);
}

void Wrapper::RequestSaveState()
{
    if (!AcceptsEmuCommands())
    {
        AnswerNoSaveState();
        return;
    }
    m_emu_thread_commands_queue.enqueue(std::make_unique<EmuThreadCommandSaveState>());
}

void Wrapper::RequestLoadState(const godot::PackedByteArray& data, int64_t frame)
{
    if (!AcceptsEmuCommands())
    {
        AnswerNoLoadState();
        return;
    }
    m_emu_thread_commands_queue.enqueue(std::make_unique<EmuThreadCommandLoadState>(data, frame));
}

void Wrapper::EmitSignalOnMainThread(const godot::StringName& signal_name, const godot::Array& args)
{
    m_main_thread_commands_queue.enqueue(
        std::make_unique<ThreadCommandEmitSignal>(this, signal_name, args));
}

/// Emulation thread, once the core has been loaded and has declared its
/// options, and strictly before retro_load_game: a core reads the options that
/// decide what machine it even is - mGBA's gb_model among them - while loading
/// the game, so applying them afterwards would be applying them too late.
void Wrapper::ApplyPendingCoreOptions()
{
    std::vector<std::pair<std::string, std::string>> pending;
    {
        std::lock_guard<std::mutex> lock(m_pending_options_mutex);
        pending.swap(m_pending_core_options);
    }
    if (!m_options_handler)
        return;
    for (const auto& option : pending)
    {
        m_options_handler->SetVariable(option.first, option.second);
        Log("Applied held core option " + option.first + " = " + option.second);
    }
}

void Wrapper::PublishCoreIdentity()
{
    if (!m_core)
        return;
    std::lock_guard<std::mutex> lock(m_core_identity_mutex);
    m_core_library_name = m_core->GetLibraryName();
    m_core_library_version = m_core->GetLibraryVersion();
    m_core_api_version = m_core->GetApiVersion();
    m_core_identity_ready = true;
}

void Wrapper::PublishCoreSerializeSize()
{
    if (m_core_serialize_size_published || !m_core || !m_core->retro_serialize_size)
        return;
    m_core_serialize_size_published = true;
    const int64_t serialize_size = static_cast<int64_t>(m_core->retro_serialize_size());
    std::lock_guard<std::mutex> lock(m_core_identity_mutex);
    m_core_serialize_size = serialize_size;
}

void Wrapper::ClearCoreDebug()
{
    std::lock_guard<std::mutex> lock(m_core_debug_mutex);
    m_core_debug = CoreDebugInfo{};
    m_core_debug_at = {};
    m_core_debug_run_ns = 0;
    m_core_debug_frames = 0;
    m_core_debug_worst_ns.store(0, std::memory_order_relaxed);
}

void Wrapper::RefreshCoreDebug()
{
    if (!m_core)
        return;
    CoreDebugInfo d;
    d.ready = true;
    if (m_video_handler)
    {
        d.pixel_format = static_cast<int>(m_video_handler->GetPixelFormat());
        d.hw_context = static_cast<int>(m_video_handler->GetHwContextType());
        d.hw_major = m_video_handler->GetHwVersionMajor();
        d.hw_minor = m_video_handler->GetHwVersionMinor();
        d.shared_context = m_video_handler->GetSharedContextGranted();
    }
    if (m_core->retro_get_system_av_info)
    {
        retro_system_av_info av = {};
        m_core->retro_get_system_av_info(&av);
        d.max_width = av.geometry.max_width;
        d.max_height = av.geometry.max_height;
        d.av_fps = av.timing.fps;
        d.av_sample_rate = av.timing.sample_rate;
    }
    if (m_core->retro_get_system_info)
    {
        retro_system_info info = {};
        m_core->retro_get_system_info(&info);
        d.valid_extensions = info.valid_extensions ? info.valid_extensions : "";
        d.need_fullpath = info.need_fullpath;
        d.block_extract = info.block_extract;
    }
    if (m_environment_handler)
    {
        d.performance_level = m_environment_handler->GetPerformanceLevel();
        d.disk_kind = m_environment_handler->GetDiskControlKind();
        m_environment_handler->GetDiskImages(d.disk_count, d.disk_index);
        for (const auto& s : m_environment_handler->GetSubsystems())
            d.subsystems.push_back(s.desc.empty() ? s.ident : s.desc);
    }
    if (m_options_handler)
        d.options_api = m_options_handler->GetOptionsApi();
    d.supports_achievements = m_supports_achievements;
    d.memory_descriptors = m_memory_descriptors.size();
    if (m_core->retro_get_memory_size)
    {
        d.save_ram = static_cast<int64_t>(m_core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM));
        d.rtc = static_cast<int64_t>(m_core->retro_get_memory_size(RETRO_MEMORY_RTC));
        d.system_ram = static_cast<int64_t>(m_core->retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM));
        d.video_ram = static_cast<int64_t>(m_core->retro_get_memory_size(RETRO_MEMORY_VIDEO_RAM));
    }
    if (m_input_handler)
    {
        d.input_descriptors = m_input_handler->GetInputDescriptorCount();
        d.controller_info_ports = m_input_handler->GetControllerInfoPorts();
        d.port_devices = m_input_handler->GetPortDevices();
    }
    if (m_audio_handler)
    {
        d.audio_status_callback = m_audio_handler->HasBufferStatusCallback();
        d.min_audio_latency_ms = m_audio_handler->GetMinimumAudioLatency();
    }

    // Frame time over the window since the last refresh, from the running totals.
    const uint64_t run_ns = m_run_ns.load(std::memory_order_relaxed);
    const int64_t frames = m_frame_counter.load(std::memory_order_relaxed);
    if (frames > m_core_debug_frames && run_ns >= m_core_debug_run_ns)
        d.run_avg_ms = static_cast<double>(run_ns - m_core_debug_run_ns) / 1.0e6
            / static_cast<double>(frames - m_core_debug_frames);
    d.run_worst_ms = static_cast<double>(
        m_core_debug_worst_ns.exchange(0, std::memory_order_relaxed)) / 1.0e6;
    m_core_debug_run_ns = run_ns;
    m_core_debug_frames = frames;
    m_core_debug_at = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_core_debug_mutex);
    // Keep the last window's figures until a window with frames replaces them.
    if (d.run_avg_ms <= 0.0)
    {
        d.run_avg_ms = m_core_debug.run_avg_ms;
        d.run_worst_ms = std::max(d.run_worst_ms, m_core_debug.run_worst_ms);
    }
    m_core_debug = std::move(d);
}

godot::Dictionary Wrapper::GetCoreDebugInfo() const
{
    godot::Dictionary out;
    std::lock_guard<std::mutex> lock(m_core_debug_mutex);
    const CoreDebugInfo& d = m_core_debug;
    if (!d.ready)
        return out;
    out["pixel_format"] = d.pixel_format;
    out["hw_context"] = d.hw_context;
    out["hw_version_major"] = static_cast<int64_t>(d.hw_major);
    out["hw_version_minor"] = static_cast<int64_t>(d.hw_minor);
    out["shared_context"] = d.shared_context;
    out["max_width"] = static_cast<int64_t>(d.max_width);
    out["max_height"] = static_cast<int64_t>(d.max_height);
    out["av_fps"] = d.av_fps;
    out["av_sample_rate"] = d.av_sample_rate;
    out["valid_extensions"] = godot::String::utf8(d.valid_extensions.c_str());
    out["need_fullpath"] = d.need_fullpath;
    out["block_extract"] = d.block_extract;
    out["performance_level"] = static_cast<int64_t>(d.performance_level);
    out["options_api"] = d.options_api;
    out["supports_achievements"] = d.supports_achievements;
    out["memory_descriptors"] = static_cast<int64_t>(d.memory_descriptors);
    out["save_ram"] = d.save_ram;
    out["rtc"] = d.rtc;
    out["system_ram"] = d.system_ram;
    out["video_ram"] = d.video_ram;
    out["disk_control"] = d.disk_kind;
    out["disk_count"] = static_cast<int64_t>(d.disk_count);
    out["disk_index"] = static_cast<int64_t>(d.disk_index);
    godot::PackedStringArray subsystems;
    for (const auto& s : d.subsystems)
        subsystems.push_back(godot::String::utf8(s.c_str()));
    out["subsystems"] = subsystems;
    out["input_descriptors"] = static_cast<int64_t>(d.input_descriptors);
    out["controller_info_ports"] = static_cast<int64_t>(d.controller_info_ports);
    godot::Dictionary ports;
    for (const auto& [port, device] : d.port_devices)
        ports[static_cast<int64_t>(port)] = static_cast<int64_t>(device);
    out["port_devices"] = ports;
    out["audio_status_callback"] = d.audio_status_callback;
    out["min_audio_latency_ms"] = static_cast<int64_t>(d.min_audio_latency_ms);
    out["run_avg_ms"] = d.run_avg_ms;
    out["run_worst_ms"] = d.run_worst_ms;
    return out;
}

void Wrapper::ClearCoreIdentity()
{
    std::lock_guard<std::mutex> lock(m_core_identity_mutex);
    m_core_library_name.clear();
    m_core_library_version.clear();
    m_core_api_version = 0;
    m_core_serialize_size = 0;
    m_core_identity_ready = false;
    m_core_serialize_size_published = false;
    m_core_ran_frame = false;
    ClearCoreDebug();
}

godot::Dictionary Wrapper::GetCoreIdentity() const
{
    godot::Dictionary out;
    std::lock_guard<std::mutex> lock(m_core_identity_mutex);
    if (!m_core_identity_ready)
        return out;
    out["library_name"] = godot::String::utf8(m_core_library_name.c_str());
    out["library_version"] = godot::String::utf8(m_core_library_version.c_str());
    out["api_version"] = static_cast<int64_t>(m_core_api_version);
    out["serialize_size"] = m_core_serialize_size;
    return out;
}

void Wrapper::SetCoreOption(const std::string& key, const std::string& value)
{
    Log("SetCoreOption: key=" + key + " value=" + value);

    if (!m_options_handler || !m_running.load(std::memory_order_acquire)
        || m_stopping.load(std::memory_order_acquire))
    {
        // Not "skipping". A caller sets options and THEN starts content, so
        // this is the ordinary path, not an error one. Hold them until the
        // core has declared what it understands.
        std::lock_guard<std::mutex> lock(m_pending_options_mutex);
        for (auto& pending : m_pending_core_options)
        {
            if (pending.first == key)
            {
                pending.second = value;
                return;
            }
        }
        m_pending_core_options.emplace_back(key, value);
        Log("SetCoreOption: core not up yet; held for start");
        return;
    }

    m_emu_thread_commands_queue.enqueue(
        std::make_unique<EmuThreadCommandSetCoreOption>(key, value));
}

bool Wrapper::SetSystemAvInfo(const retro_system_av_info* av_info)
{
    if (!av_info || !m_running.load(std::memory_order_acquire) ||
        m_stopping.load(std::memory_order_acquire) || m_handling_av_info)
    {
        LogError("SET_SYSTEM_AV_INFO is unavailable outside a running frame");
        return false;
    }

    // The core owns the callback argument. Copy it before context_destroy or a
    // main-thread wait can invalidate the storage behind that pointer.
    const retro_system_av_info requested = *av_info;
    const auto valid_av_info = [](const retro_system_av_info& info)
    {
        const retro_game_geometry& geometry = info.geometry;
        const retro_system_timing& timing = info.timing;
        const unsigned max_dimension =
            static_cast<unsigned>(std::numeric_limits<int32_t>::max());
        return geometry.base_width > 0 && geometry.base_height > 0 &&
            geometry.max_width >= geometry.base_width &&
            geometry.max_height >= geometry.base_height &&
            geometry.base_width <= max_dimension &&
            geometry.base_height <= max_dimension &&
            geometry.max_width <= max_dimension &&
            geometry.max_height <= max_dimension &&
            std::isfinite(geometry.aspect_ratio) && geometry.aspect_ratio >= 0.0f &&
            std::isfinite(timing.fps) && timing.fps > 0.0 &&
            std::isfinite(timing.sample_rate) && timing.sample_rate >= 0.0;
    };
    if (!valid_av_info(requested))
    {
        LogError("Core supplied invalid system AV info");
        return false;
    }

    m_handling_av_info = true;
    ScopeExit finish([this]() { m_handling_av_info = false; });

    const retro_system_av_info previous = m_system_av_info;
    const auto hw_max_dimensions_changed = [&](const retro_system_av_info& info)
    {
        return m_video_handler->UsesHardwareRendering() &&
            (info.geometry.max_width != previous.geometry.max_width ||
             info.geometry.max_height != previous.geometry.max_height);
    };
    if (hw_max_dimensions_changed(requested))
    {
        // These backends expose a surface at the base dimensions. Accepting a
        // larger maximum would promise SET_GEOMETRY can grow into capacity that
        // does not exist, while sizing the surface to max distorts base frames.
        LogError("Changing maximum video dimensions is unsupported for hardware rendering");
        return false;
    }

    bool video_reinitialized = false;
    const auto restore_video = [&]()
    {
        m_system_av_info = previous;
        if (video_reinitialized && !m_stop_requested.load() &&
            !m_video_handler->ReinitHwRenderContext(
                static_cast<int32_t>(previous.geometry.base_width),
                static_cast<int32_t>(previous.geometry.base_height)))
        {
            LogError("Failed to restore the previous hardware render context");
            StopEmulationThread(false);
        }
        // A context_reset may write through the persistent pointer. A failed
        // SET_SYSTEM_AV_INFO is transactional, so retain the pre-call values.
        m_system_av_info = previous;
    };

    // Some cores retain the retro_get_system_av_info destination and revise it
    // from context_reset. Stage the request there before invoking that callback,
    // then treat its post-reset values as the final declaration.
    m_system_av_info = requested;
    if (requested.geometry.base_width != previous.geometry.base_width ||
        requested.geometry.base_height != previous.geometry.base_height)
    {
        video_reinitialized = true;
        if (!m_video_handler->ReinitHwRenderContext(
                static_cast<int32_t>(requested.geometry.base_width),
                static_cast<int32_t>(requested.geometry.base_height)))
        {
            restore_video();
            return false;
        }

        if (!valid_av_info(m_system_av_info))
        {
            LogError("Core supplied invalid system AV info from context_reset");
            restore_video();
            return false;
        }

        if (hw_max_dimensions_changed(m_system_av_info))
        {
            LogError("context_reset changed unsupported maximum video dimensions");
            restore_video();
            return false;
        }

        // If context_reset revised the base geometry, rebuild once more at the
        // dimensions it actually settled on. Reject an unstable second change.
        const unsigned reset_width = m_system_av_info.geometry.base_width;
        const unsigned reset_height = m_system_av_info.geometry.base_height;
        if (reset_width != requested.geometry.base_width ||
            reset_height != requested.geometry.base_height)
        {
            if (!m_video_handler->ReinitHwRenderContext(
                    static_cast<int32_t>(reset_width),
                    static_cast<int32_t>(reset_height)) ||
                !valid_av_info(m_system_av_info) ||
                hw_max_dimensions_changed(m_system_av_info) ||
                m_system_av_info.geometry.base_width != reset_width ||
                m_system_av_info.geometry.base_height != reset_height)
            {
                LogError("Core supplied unstable geometry from context_reset");
                restore_video();
                return false;
            }
        }
    }

    if (!valid_av_info(m_system_av_info))
    {
        restore_video();
        return false;
    }

    if (m_system_av_info.timing.sample_rate != previous.timing.sample_rate)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_mutex_done = false;
        m_audio_reinit_success = false;
        m_audio_reinit_restore_failed = false;
        m_main_thread_commands_queue.enqueue(std::make_unique<ThreadCommandReinitAudio>(
            this, m_system_av_info.timing.sample_rate));
        m_condition_variable.wait(lock, [&]
            { return m_mutex_done || m_stop_requested.load(); });

        if (!m_mutex_done || !m_audio_reinit_success || m_stop_requested.load())
        {
            const bool restore_failed = m_audio_reinit_restore_failed;
            lock.unlock();
            restore_video();
            if (restore_failed)
            {
                LogError("Previous audio backend could not be restored; stopping the core");
                StopEmulationThread(false);
            }
            LogError("Failed to apply the core's new audio sample rate");
            return false;
        }
    }

    m_video_handler->SetGeometry(&m_system_av_info.geometry);
    const retro_game_geometry& applied_geometry = m_system_av_info.geometry;
    const retro_system_timing& applied_timing = m_system_av_info.timing;
    m_declared_fps.store(applied_timing.fps, std::memory_order_relaxed);
    m_declared_sample_rate.store(applied_timing.sample_rate, std::memory_order_relaxed);
    if (applied_timing.fps != previous.timing.fps)
        m_timing_revision.fetch_add(1, std::memory_order_release);

    Log("System AV Info: " + std::to_string(applied_geometry.base_width) + "x" +
        std::to_string(applied_geometry.base_height) + " @ " +
        std::to_string(applied_geometry.max_width) + "x" +
        std::to_string(applied_geometry.max_height) + " (aspect ratio: " +
        std::to_string(applied_geometry.aspect_ratio) + ") FPS: " +
        std::to_string(applied_timing.fps) + " Sample Rate: " +
        std::to_string(applied_timing.sample_rate));
    return true;
}

void Wrapper::_process(double delta)
{
    // Deferred stop: once the emulation thread has fully exited on its own,
    // the join is instant, so finish the teardown here without ever blocking.
    if (m_stopping.load(std::memory_order_acquire))
    {
        // While the stop is pending, keep DISCARDING queued main-thread
        // commands: a core hung in teardown (mupen gles3) can keep streaming
        // texture-update commands from its internal threads, and an undrained
        // queue grows a full frame's pixels per callback until OOM.
        std::unique_ptr<ThreadCommand> stale;
        while (m_main_thread_commands_queue.try_dequeue(stale))
            stale.reset();

        if (m_thread_exited.load(std::memory_order_acquire))
        {
            if (m_thread.joinable())
                m_thread.join();
            FinishTeardown();
        }
        return;
    }

    if (!m_running)
        return;

    // Drain to empty, but collapse each run of frame uploads to its last. The
    // emulation thread produces at the core's frame rate whatever the main
    // thread manages, so a main thread that falls behind has to chase a queue
    // that keeps refilling; once one upload costs more than a frame, executing
    // every one of them means _process stops returning at all. Ordering is
    // preserved: only uploads that a newer upload supersedes are dropped.
    std::unique_ptr<ThreadCommand> command;
    std::unique_ptr<ThreadCommand> newest_upload;
    while (m_main_thread_commands_queue.try_dequeue(command))
    {
        if (command->IsFrameUpload())
        {
            // Count what this supersedes. These are frames the core produced and
            // nobody ever saw, which is the one number that separates "the core is
            // slow" from "the room is too busy to show what the core produced".
            if (newest_upload)
                m_dropped_frames.fetch_add(1, std::memory_order_relaxed);
            newest_upload = std::move(command);
            continue;
        }
        if (newest_upload)
        {
            newest_upload->Execute();
            newest_upload.reset();
        }
        command->Execute();
    }
    if (newest_upload)
        newest_upload->Execute();

    PollDesktopInput();
}

Wrapper::~Wrapper()
{
    // Destroying a joinable std::thread calls std::terminate, so always block here.
    StopEmulationThread(true);
}

void Wrapper::CreateTexture(Image::Format image_format, PackedByteArray pixel_data, int32_t width, int32_t height, bool flip_y)
{
    m_video_handler->SetImageFormat(image_format);
    m_main_thread_commands_queue.enqueue(std::make_unique<ThreadCommandCreateTexture>(this, image_format, pixel_data, width, height, flip_y));
}

void Wrapper::UpdateTexture(PackedByteArray pixel_data, int32_t width, int32_t height, bool flip_y)
{
    m_main_thread_commands_queue.enqueue(std::make_unique<ThreadCommandUpdateTexture>(this, pixel_data, width, height, flip_y));
}

bool Wrapper::Shutdown()
{
    // RETRO_ENVIRONMENT_SHUTDOWN arrives ON the emulation thread (inside
    // retro_run), and a blocking stop would join the thread from itself. Signal
    // only; _process() finishes the teardown after this thread exits.
    Log("Shutting down from core...");
    StopEmulationThread(false);
    return true;
}

void Wrapper::LedInterfaceSetLedState(int32_t led, int32_t state)
{
    // Reported by the core from the emulated machine's own register, not
    // inferred: the Satellaview's ACCESS lamp is $2194 bit 2. Edges only, so
    // this is not a per-frame signal.
    // Static libretro callback: the owning Wrapper comes from the thread-local,
    // never from a global -- two Satellaviews in one room are two instances.
    Wrapper* w = GetCurrentThreadWrapper();
    if (w == nullptr)
        return;
    if (Libretro* node = w->LiveLibretroNode())
        node->NotifyLedState(led, state != 0);
}
}
