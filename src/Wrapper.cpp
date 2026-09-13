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
    // Non-blocking: the join + teardown (SRAM flush, retro_unload_game,
    // retro_deinit, DLL unload) all happen off the main thread / deferred to
    // _process, so powering a system off does not hitch the frame.
    StopEmulationThread(false);
}

bool Wrapper::ShutdownForExit(uint32_t budget_ms)
{
    if (m_audio_handler)
        m_audio_handler->SilenceForTeardown();
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

bool Wrapper::HasVmuScreens() const
{
    return m_core != nullptr && m_core->flycast_get_vmu_screen != nullptr;
}

Ref<ImageTexture> Wrapper::GetVmuScreenTexture(int index)
{
    if (index < 0 || index >= VMU_SCREEN_COUNT)
        return Ref<ImageTexture>();

    PackedByteArray bytes;
    uint64_t stamp = 0;
    {
        std::lock_guard<std::mutex> lock(m_vmu_screen_mutex);
        // Never drawn. Not the same as an all-off screen, which is a card that
        // is present and dark, so there is nothing to hand back rather than a
        // black texture that would read as one.
        if (m_vmu_screen_pixels[index].empty())
            return Ref<ImageTexture>();
        stamp = m_vmu_screen_stamp[index];
        if (m_vmu_screen_tex[index].is_valid() && stamp == m_vmu_screen_seen[index])
            return m_vmu_screen_tex[index];
        const size_t size = m_vmu_screen_pixels[index].size() * sizeof(uint32_t);
        bytes.resize(static_cast<int64_t>(size));
        std::memcpy(bytes.ptrw(), m_vmu_screen_pixels[index].data(), size);
    }

    // The core packs R | G << 8 | B << 16 | A << 24, which on a little-endian
    // machine is R, G, B, A in memory order -- so RGBA8 and no swizzle.
    Ref<Image> image = Image::create_from_data(
        VMU_SCREEN_W, VMU_SCREEN_H, false, Image::FORMAT_RGBA8, bytes);
    if (image.is_null())
        return Ref<ImageTexture>();

    if (m_vmu_screen_tex[index].is_valid())
        m_vmu_screen_tex[index]->update(image);
    else
        m_vmu_screen_tex[index] = ImageTexture::create_from_image(image);
    m_vmu_screen_seen[index] = stamp;
    return m_vmu_screen_tex[index];
}

void Wrapper::PublishVmuScreens()
{
    if (m_core == nullptr || m_core->flycast_get_vmu_screen == nullptr)
        return;

    for (int i = 0; i < VMU_SCREEN_COUNT; ++i)
    {
        // Stamp first. Copying eight panels every frame would be 48 KB of the
        // same pixels sixty times a second; the stamp says which of them moved.
        uint64_t changed = 0;
        if (!m_core->flycast_get_vmu_screen(
                static_cast<unsigned>(i), nullptr, 0, &changed))
            continue;
        {
            std::lock_guard<std::mutex> lock(m_vmu_screen_mutex);
            if (!m_vmu_screen_pixels[i].empty() && changed == m_vmu_screen_stamp[i])
                continue;
        }

        uint32_t pixels[VMU_SCREEN_W * VMU_SCREEN_H] = {};
        if (!m_core->flycast_get_vmu_screen(
                static_cast<unsigned>(i), pixels, std::size(pixels), &changed))
            continue;
        std::lock_guard<std::mutex> lock(m_vmu_screen_mutex);
        m_vmu_screen_stamp[i] = changed;
        m_vmu_screen_pixels[i].assign(pixels, pixels + std::size(pixels));
    }
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
}

godot::Dictionary Wrapper::GetCoreIdentity() const
{
    godot::Dictionary out;
    std::lock_guard<std::mutex> lock(m_core_identity_mutex);
    if (!m_core_identity_ready)
        return out;
    out["library_name"] = godot::String(m_core_library_name.c_str());
    out["library_version"] = godot::String(m_core_library_version.c_str());
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
