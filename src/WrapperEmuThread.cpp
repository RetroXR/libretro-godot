#include "Wrapper.hpp"

#include "RetroAchievements.hpp"

#include <godot_cpp/classes/os.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
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
void Wrapper::StopEmulationThread(bool blocking)
{
    if (!m_core)
    {
        return;
    }

    m_stop_requested = true;
    m_running = false;
    m_condition_variable.notify_all(); // wake emulation thread if blocked on InitAudio CV wait
    m_np_cv.notify_all();              // wake emulation thread if blocked on the netplay input gate
    // Same reason: an emulation thread parked on the link barrier is waiting
    // on a peer that is never going to publish again, and the join below would
    // block forever behind it.
    LinkCoordinator::Get().DropOwner(this);
    m_stopping = true;

    if (!blocking)
        return;   // _process() joins + finishes the teardown once the thread exits

    if (m_thread.joinable())
        m_thread.join();
    FinishTeardown();
}

void Wrapper::FinishTeardown()
{
    if (!m_core)
        return;   // already torn down
    m_stopping = false;
    m_thread_exited = false;

    // Before the core unloads: the memory regions point into its allocations, and
    // ReadMemory would follow them into freed pages. Hands the session back so the
    // next cabinet powered on can claim it.
    if (RetroAchievements* ra = RetroAchievements::GetSingleton())
        ra->ReleaseSession(this);

    // Set the thread-local pointer on the main thread so that handler DeInit
    // calls (which call GetCurrentThreadWrapper()) can find the right instance.
    SetCurrentThreadWrapper(this);
    m_video_handler->DeInit();
    m_audio_handler->DeInit();
    SetCurrentThreadWrapper(nullptr);

    m_microphone_handler->ReleaseAll();

    ClearCoreIdentity();

    m_core->Unload();

    m_core = nullptr;
    m_trampolines = nullptr;
    m_environment_handler = nullptr;
    m_video_handler = nullptr;
    m_audio_handler = nullptr;
    m_input_handler = nullptr;
    m_options_handler = nullptr;
    m_message_handler = nullptr;
    m_log_handler = nullptr;

    // Discard commands the dying thread left queued; they must not execute
    // against the next content run's handlers.
    std::unique_ptr<ThreadCommand> stale;
    while (m_main_thread_commands_queue.try_dequeue(stale)) {}
    std::unique_ptr<EmuThreadCommand> stale_emu;
    while (m_emu_thread_commands_queue.try_dequeue(stale_emu)) {}
}

void Wrapper::EmulationThreadLoop()
{
    // Mark thread exit on EVERY path out of this function (including early
    // load-failure returns) so a deferred stop's join can never hang.
    struct ExitFlag
    {
        std::atomic<bool>& flag;
        ~ExitFlag() { flag.store(true, std::memory_order_release); }
    } exit_flag{m_thread_exited};

    SetCurrentThreadWrapper(this);
    Log("Libretro Thread starting...");

    bool core_initialized = false;
    bool game_loaded = false;
    bool context_initialized = false;
    bool sram_active = false;
    ScopeExit lifecycle_cleanup([&]()
    {
        // Answer whatever is still queued before tearing anything down. Callers
        // of RequestSaveState/RequestLoadState/RequestDiskInfo are waiting on a
        // signal, and a core that failed to load — or one being stopped — leaves
        // no other thread that will ever reply to them.
        //
        // Clear the flags FIRST so nothing new is accepted, then drain. The gap
        // between a guard passing and the enqueue landing is not closed by this
        // and cannot be without a lock on the hot path; a caller still needs its
        // own timeout for that window.
        m_running = false;
        m_starting = false;
        {
            std::unique_ptr<EmuThreadCommand> orphan;
            while (m_emu_thread_commands_queue.try_dequeue(orphan))
                orphan->Abandon(*this);
        }

        // Every successful libretro lifecycle call has a matching teardown,
        // including failures before the frame loop begins.
        if (game_loaded)
        {
            // Braced to say what it already does. FlushPackIfDirty was never
            // inside this `if` -- there were no braces -- and it is left outside
            // rather than quietly brought in: it guards itself on an empty pack
            // path, so the behaviour is unchanged and this is only the
            // indentation catching up with the code.
            if (sram_active)
            {
                FlushSramIfDirty(true);
                FlushSramBIfDirty(true);
                FlushSramRegionsIfDirty(true);
            }
            FlushPackIfDirty(true);
            m_sram_pending = godot::PackedByteArray();
            if (context_initialized)
                m_video_handler->NotifyContextDestroy();
            // Each call announced BEFORE it is made: a core that will not unwind
            // (Dolphin is one) hangs inside it, and the last line says which.
            Log("Teardown: retro_unload_game...");
            m_core->retro_unload_game();
            Log("Teardown: retro_unload_game returned");
        }
        if (core_initialized)
        {
            Log("Teardown: retro_deinit...");
            m_core->retro_deinit();
            Log("Teardown: retro_deinit returned");
        }

        SetCurrentThreadWrapper(nullptr);
        Log("Libretro thread stopped.");
    });

    if (!m_core->Load(m_trampolines.get()))
    {
        NotifyContentLoadFailed("This core could not be loaded.");
        return;
    }
    core_initialized = true;

    // Before retro_load_game: options set while the core did not yet exist are
    // exactly the ones that decide how it loads.
    ApplyPendingCoreOptions();

    if (!m_subsystem_ident.empty())
    {
        // SET_SUBSYSTEM_INFO arrives during retro_set_environment, inside
        // Core::Load above, so the published table is already in hand here.
        const EnvironmentHandler::SubsystemInfo* subsystem = m_environment_handler->FindSubsystem(m_subsystem_ident);
        if (!subsystem)
        {
            LogError("Core does not publish subsystem '" + m_subsystem_ident + "'.");
            NotifyContentLoadFailed("This system cannot be started this way.");
            return;
        }

        // Resolved by name, so a core can publish a subsystem it did not export
        // an entry point for. Calling through the null pointer would be a crash
        // rather than a load failure.
        if (!m_core->retro_load_game_special)
        {
            LogError("Core publishes subsystem '" + m_subsystem_ident + "' but exports no retro_load_game_special.");
            NotifyContentLoadFailed("This system cannot be started this way.");
            return;
        }

        // A short list would leave a retro_game_info zeroed, and a core reading
        // it would fault. Hard stop rather than a best effort.
        if (m_subsystem_paths.size() != subsystem->roms.size())
        {
            LogError("Subsystem '" + m_subsystem_ident + "' expects " + std::to_string(subsystem->roms.size())
                     + " file(s) but " + std::to_string(m_subsystem_paths.size()) + " were given.");
            NotifyContentLoadFailed("The wrong number of files was given for this system.");
            return;
        }

        const size_t rom_count = m_subsystem_paths.size();

        // Sized ONCE, before a single data pointer is taken out of it. Growing
        // this later would invite a per-element resize after the game infos
        // already point into them.
        m_subsystem_buffers.clear();
        m_subsystem_buffers.resize(rom_count);

        std::vector<retro_game_info> game_infos(rom_count);

        // Shared across all the files rather than per file: the point of the cap
        // is the process memory budget, and N cartridges cost N times as much.
        constexpr size_t MAX_GAME_BUFFER_BYTES = 512ull * 1024ull * 1024ull;
        size_t total_buffered = 0;

        for (size_t i = 0; i < rom_count; ++i)
        {
            const std::string& path = m_subsystem_paths[i];
            const EnvironmentHandler::SubsystemRomInfo& rom = subsystem->roms[i];

            game_infos[i] = {};

            if (path.empty())
            {
                // An optional slot the player left empty is legal -- a console
                // with nothing in one of its bays. A required one is not.
                if (rom.required)
                {
                    LogError("Subsystem '" + m_subsystem_ident + "' rom " + std::to_string(i)
                             + " (" + rom.desc + ") is required but no file was given.");
                    NotifyContentLoadFailed("A required file for this system is missing.");
                    return;
                }
                continue;
            }

            if (!std::filesystem::is_regular_file(path))
            {
                LogError("Subsystem file not found (rom " + std::to_string(i) + ", " + rom.desc + "): " + path);
                NotifyContentLoadFailed("One of the files for this system is missing.");
                return;
            }

            // m_subsystem_paths is not touched again for the life of the run, so
            // this pointer stays valid as long as the core needs it.
            game_infos[i].path = path.c_str();
            game_infos[i].meta = nullptr;

            // PER ROM, not Core::GetNeedFullpath(). A 64DD run is exactly the
            // case that differs between the two.
            if (rom.need_fullpath)
            {
                Log("Subsystem rom " + std::to_string(i) + " (" + rom.desc + ") needs fullpath: " + path);
                continue;
            }

            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
            {
                LogError("Failed to open subsystem file: " + path);
                NotifyContentLoadFailed("One of the files for this system could not be opened.");
                return;
            }

            const size_t game_size = static_cast<size_t>(file.tellg());
            file.seekg(0, std::ios::beg);

            if (game_size > MAX_GAME_BUFFER_BYTES - total_buffered)
            {
                LogError("Subsystem content needs " + std::to_string((total_buffered + game_size) / (1024 * 1024))
                         + " MB in memory (limit " + std::to_string(MAX_GAME_BUFFER_BYTES / (1024 * 1024))
                         + " MB), at " + path);
                NotifyContentLoadFailed("These files are too large for this core to load.");
                return;
            }

            m_subsystem_buffers[i].resize(game_size);
            if (game_size != 0 && !file.read(reinterpret_cast<char*>(m_subsystem_buffers[i].data()), game_size))
            {
                LogError("Failed to read subsystem file: " + path);
                NotifyContentLoadFailed("One of the files for this system could not be read.");
                return;
            }

            total_buffered += game_size;
            // Taken after the only resize this element will ever see.
            game_infos[i].data = reinterpret_cast<const void*>(m_subsystem_buffers[i].data());
            game_infos[i].size = game_size;
        }

        Log("Loading subsystem '" + m_subsystem_ident + "' (id=" + std::to_string(subsystem->id) + ") with "
            + std::to_string(rom_count) + " file(s).");

        if (!m_core->retro_load_game_special(subsystem->id, game_infos.data(), rom_count))
        {
            LogError("retro_load_game_special failed for subsystem '" + m_subsystem_ident + "'.");
            NotifyContentLoadFailed("This core refused these files.");
            return;
        }
        game_loaded = true;
    }
    else if (m_game_path.empty())
    {
        // Kept as a floor. Measured 2026-08-20 over sixteen cores: not one stock
        // core starts without content, and six take the process down when asked
        // (mgba and parallel_n64 dereference a null game info; mednafen_saturn,
        // neocd, dolphin and same_cdi die on a zeroed one). A core reaching here
        // has to have said SET_SUPPORT_NO_GAME out loud, so a core that never
        // claimed it cannot be attempted by an over-eager BiosBoot row.
        if (!m_core->GetSupportsNoGame())
        {
            LogError("Game not set and this core does not support no game mode.");
            NotifyContentLoadFailed("This system cannot start without a game.");
            return;
        }

        // Which of the two conventions a core wants is not something it says out
        // loud, so it is measured per core and carried in the BiosBoot table. A
        // null pointer is libretro's own convention and what RetroArch passes,
        // which is what a core detecting no-content with `if (!info)` expects; a
        // zeroed struct is the one that does not fault on a core that reads the
        // argument without checking.
        const bool pass_null = GetNoContentPassesNull();
        Log(std::string("Starting with no content, passing ") +
            (pass_null ? "a null game info" : "a zeroed game info") + ".");

        retro_game_info game_info = {};

        if (!m_core->retro_load_game(pass_null ? nullptr : &game_info))
        {
            LogError("Failed to load game");
            NotifyContentLoadFailed("This system cannot start without a game.");
            return;
        }
        game_loaded = true;
    }
    else
    {
        if (!std::filesystem::is_regular_file(m_game_path))
        {
            LogError("Game not found: " + m_game_path);
            NotifyContentLoadFailed("The game file is missing.");
            return;
        }

        retro_game_info game_info = {};
        game_info.path = m_game_path.c_str();
        game_info.meta = nullptr;

        if (m_core->GetNeedFullpath())
        {
            // Disc cores (PS1/PS2/Saturn/Dreamcast/GC/PSP, MAME, ...) open the
            // image themselves from the path so they can seek, read tracks lazily
            // and hot-swap discs; they never touch game_info.data. Reading a 4 GB
            // ISO into a buffer nobody reads is an instant OOM kill on Quest.
            Log("Core needs fullpath, passing path only: " + m_game_path);
        }
        else
        {
            std::ifstream file(m_game_path, std::ios::binary | std::ios::ate);
            if (!file)
            {
                LogError("Failed to open game file: " + m_game_path);
                NotifyContentLoadFailed("The game file could not be opened.");
                return;
            }

            size_t game_size = static_cast<size_t>(file.tellg());
            file.seekg(0, std::ios::beg);

            // A core that wants the bytes in memory is a cartridge core; those top
            // out well under this. Anything larger is a mismatch (or a corrupt
            // download) and would blow the app's memory budget, so fail loudly
            // instead of attempting the allocation.
            constexpr size_t MAX_GAME_BUFFER_BYTES = 512ull * 1024ull * 1024ull;
            if (game_size > MAX_GAME_BUFFER_BYTES)
            {
                LogError("Game is " + std::to_string(game_size / (1024 * 1024)) +
                         " MB but this core requires the whole file in memory (limit " +
                         std::to_string(MAX_GAME_BUFFER_BYTES / (1024 * 1024)) + " MB): " + m_game_path);
                NotifyContentLoadFailed("This game is too large for this core to load.");
                return;
            }

            m_game_buffer.resize(game_size);
            if (!file.read(reinterpret_cast<char*>(m_game_buffer.data()), game_size))
            {
                LogError("Failed to read game file: " + m_game_path);
                NotifyContentLoadFailed("The game file could not be read.");
                return;
            }

            game_info.data = reinterpret_cast<const void*>(m_game_buffer.data());
            game_info.size = game_size;
        }

        if (!m_core->retro_load_game(&game_info))
        {
            LogError("Failed to load game");
            NotifyContentLoadFailed("This core refused the game.");
            return;
        }
        game_loaded = true;
    }

    // Apply device selections made before the core started (controller/mouse
    // plugged in while the system was off). retro_set_controller_port_device is
    // canonical right after retro_load_game.
    {
        std::lock_guard<std::mutex> lock(m_port_device_mutex);
        for (const auto& [port, device] : m_pending_port_devices)
        {
            // NONE is skipped: it is recorded by an unplug while the machine was
            // off, and announcing it would kill the port's fallback global-input
            // path.
            //
            // JOYPAD used to be skipped too, on the reasoning that a plain
            // joypad is what a core assumes. That holds for nearly all of them
            // and is wrong for at least one. flycast takes its main maple device
            // FROM this call (JOYPAD -> MDT_SegaController), so a port the
            // frontend never announced stayed empty: a Dreamcast came up with no
            // controller at all, and therefore no expansion socket and no VMU,
            // because a VMU lives in the controller rather than the console.
            // Crazy Taxi 2 says so on its own memory-card screen — "The
            // controller has been removed", with both sockets empty under port A.
            //
            // Announcing it is what the API asks for and costs nothing on a core
            // that already assumed it. This loop only carries ports the frontend
            // explicitly set, so it is not a blanket announce over every port.
            if (device == RETRO_DEVICE_NONE)
                continue;
            Log("Applying pre-start port device: port=" + std::to_string(port)
                + " device=" + std::to_string(device));
            m_input_handler->SetPortDevice(port, device);
            m_core->retro_set_controller_port_device(port, device);
        }
    }

    m_system_av_info = {};
    m_core->retro_get_system_av_info(&m_system_av_info);

    Log("FPS: " + std::to_string(m_system_av_info.timing.fps) + " Sample Rate: " + std::to_string(m_system_av_info.timing.sample_rate));

    // Size the hardware render target by the core's BASE geometry, not its max.
    //
    // For a core that renders through a surface (Dolphin), the surface IS the
    // swapchain, and Vulkan requires a swapchain's extent to equal the surface's
    // currentExtent. An oversized surface therefore does not leave the core room
    // to grow; it forces the core to render bigger and present scaled into it
    // while video_refresh still reports the base size, so the readback takes
    // base rows off the top of a taller picture.
    //
    // SET_GEOMETRY cannot repair this: it is specified to complete in constant
    // time and to perform no video reinitialization, so it may not rebuild a
    // surface. A core that genuinely needs to grow past base has to say so
    // through SET_SYSTEM_AV_INFO, which is the call that does permit
    // reinitialization.
    const int32_t hw_width = static_cast<int32_t>(m_system_av_info.geometry.base_width);
    const int32_t hw_height = static_cast<int32_t>(m_system_av_info.geometry.base_height);

    if (!m_video_handler->InitHwRenderContext(hw_width, hw_height))
    {
        LogError("Failed to initialize video");
        return;
    }
    context_initialized = true;

    if (m_stop_requested)
        return;

    // Running before not-starting, never the other way round: a request landing
    // between the two must see one of them true or it is refused.
    m_running = true;
    m_starting = false;

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_mutex_done = false;
        m_main_thread_commands_queue.enqueue(std::make_unique<ThreadCommandInitAudio>(this, 0.1f, m_system_av_info.timing.sample_rate));
        m_condition_variable.wait(lock, [&]{ return m_mutex_done || m_stop_requested.load(); });
        if (m_stop_requested)
            return;
    }

    // Achievements. Deferred to here rather than straight after retro_load_game so
    // the core is fully initialized: the memory map arrives during load, and a
    // core may still be registering descriptors while av_info is being read.
    // Cartridge cores hand over the bytes already resident in m_game_buffer;
    // need_fullpath cores never read the file, so rc_hash opens the path itself.
    if (RetroAchievements* ra = RetroAchievements::GetSingleton())
    {
        // rc_hash takes one file. A subsystem run has several, and the identity
        // of the run -- what saves, states and netplay already key off -- is
        // m_game_path, so that is the one to hash. The bytes are deliberately
        // not passed: on a subsystem run m_game_buffer is empty by construction
        // (the images live in m_subsystem_buffers), and rc_hash opens the path
        // itself, which is the same arrangement disc cores already use.
        const bool subsystem_run = !m_subsystem_ident.empty();
        if (ra->HoldsSession(this))
            ra->BeginLoadGame(this, m_game_path,
                !subsystem_run && !m_core->GetNeedFullpath() && !m_game_buffer.empty() ? m_game_buffer.data() : nullptr,
                !subsystem_run && !m_core->GetNeedFullpath() ? m_game_buffer.size() : 0);
    }

    double frame_duration_ms = 1000.0 / m_system_av_info.timing.fps;
    m_timing_revision.store(0, std::memory_order_relaxed);
    uint64_t timing_revision = 0;

    // Publish what the core declared, from the one read the pacing loop itself
    // trusts. A later read is not the same value: a core may revise timing behind
    // the frontend's back, and the HUD compares measured against DECLARED, so it
    // has to be the figure this loop is actually pacing to.
    m_declared_fps.store(m_system_av_info.timing.fps, std::memory_order_relaxed);
    m_declared_sample_rate.store(m_system_av_info.timing.sample_rate, std::memory_order_relaxed);
    m_dropped_frames.store(0, std::memory_order_relaxed);

    // Name, version and API only. All three were read at load from
    // retro_get_system_info and cost no call into the core here, so this is
    // safe for every core AND available before a single frame has run - which
    // netplay requires, because its gate will not run frame 0 until every peer
    // has reported ready. The savestate size is the dangerous one and follows
    // the first frame instead.
    PublishCoreIdentity();

    auto last_time = std::chrono::steady_clock::now();
    double accumulator = 0.0;

    // Slack left before the next frame is due. sleep_for rounds up to the OS
    // timer granularity (coarse on Windows without timeBeginPeriod), so waking
    // early and taking the last stretch on the clock beats oversleeping frames.
    constexpr double SLEEP_MARGIN_MS = 1.5;

    // Pacing has two parts, and the declared fps is only one of them.
    //
    // The brake is the audio sink. Its mixer drains at the hardware's rate, so how
    // full it is measures real time, the one quantity in this loop a core cannot
    // misreport. Run a frame while the sink still wants audio; wait when it does
    // not. This needs neither a correct fps nor an honest sample count, and cores
    // falsify both: azahar returns when the *game* presents, so its declared 60
    // describes neither the call rate nor the time a call covers, and ScummVM can
    // bill 2.5 million ms of audio for a single retro_run.
    //
    // The declared fps is a ceiling on top of that, never a target: it caps how
    // fast the loop may run, and is the only brake left for a core that emits no
    // audio for the sink to measure.

    // When the next call may go out under the ceiling. Advanced by one frame per
    // call rather than reset to now, so a call that overruns is not paid for twice.
    auto next_call_due = std::chrono::steady_clock::now();

    // One wait is bounded: past this the frame runs anyway and samples drop.
    constexpr double MAX_BRAKE_WAIT_MS = 250.0;

    // Bounding one wait does not bound the loop of them: a sink nothing is
    // draining reports "still full" on every pass, and the loop would brake
    // forever without ever calling the core. Bound the silence instead. A sink
    // that has not asked for audio for longer than it could possibly hold (the
    // voice ring is 32768 frames, ~0.68 s at 48 kHz) is not draining and is
    // therefore measuring nothing, so ignore it and let the fps ceiling pace.
    // The brake resumes the moment the sink asks for a frame again.
    constexpr double SINK_SILENCE_LIMIT_MS = 1000.0;
    auto last_sink_want = std::chrono::steady_clock::now();

    // Battery save: fill SAVE_RAM from the cartridge/memory-card .srm (or the
    // netplay-injected bytes) before the first frame runs.
    LoadSramFromSource();
    // The second cartridge, on the one adapter that holds two.
    LoadSramBFromSource();
    // The Controller Paks last: LoadSramFromSource has just put the cartridge
    // .srm's own copy of these bytes in place, and the pak files are the truth.
    LoadSramRegionsFromSource();
    sram_active = true;
    m_sram_flush_counter = m_frame_counter.load(std::memory_order_relaxed);

    // Writable content (a BS-X memory pack): the core already holds the bytes
    // the .bs was loaded with, so this only records them for the dirty check.
    SnapshotPack();

    // Fresh rollback bookkeeping for this content run (these members are
    // emulation-thread-only, so reset them here rather than in SetNetplay*).
    m_np_states.clear();
    m_np_used.clear();
    m_np_crc_pending.clear();
    m_np_watermark = m_frame_counter.load(std::memory_order_relaxed) - 1;
    m_np_verified = m_np_watermark;
    m_np_replaying = false;
    m_np_replay_mute_video = false;
    m_np_rollback_count.store(0, std::memory_order_relaxed);

    if (Libretro* node = LiveLibretroNode())
        node->NotifyOptionsReady();

    while (m_running)
    {
        if (!m_running)
            break;

        // Drain main→emu commands (savestates, etc.) strictly between frames
        // so they never re-enter the core mid-retro_run.
        {
            std::unique_ptr<EmuThreadCommand> emu_command;
            while (m_emu_thread_commands_queue.try_dequeue(emu_command))
                emu_command->Execute(*this);
        }

        const uint64_t current_timing_revision =
            m_timing_revision.load(std::memory_order_acquire);
        if (current_timing_revision != timing_revision)
        {
            timing_revision = current_timing_revision;
            frame_duration_ms = 1000.0 /
                m_declared_fps.load(std::memory_order_relaxed);
            accumulator = std::min(accumulator, frame_duration_ms * 4.0);
            const auto timing_now = std::chrono::steady_clock::now();
            last_time = timing_now;
            next_call_due = timing_now +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double, std::milli>(frame_duration_ms));
        }

        // The savestate size, once the core has actually produced a frame.
        // Here rather than beside a retro_run because the rollback path
        // continues past both of those, and it would never be measured at all
        // in the one mode that takes a state every single frame.
        //
        // Gated on THIS content having run a frame, not on a non-zero frame
        // counter: nothing resets that counter, so a restart on a reused Wrapper
        // carried the previous run's value in and fired the measurement before
        // the new core had run anything -- which is a segfault in Dolphin, not a
        // wrong number. See m_core_ran_frame.
        if (!m_core_serialize_size_published && m_core_ran_frame)
            PublishCoreSerializeSize();

        // A pak pushed into a controller mid-game binds on the next frame, not
        // at the next flush tick: the check is one atomic read when nothing
        // moved, and seating a pak is a thing done by hand.
        ApplySramRegionSwaps();

        // Battery save: dirty-check flush roughly every 10 seconds of frames.
        //
        // The counter goes BACKWARDS: loading a savestate stores the state's own
        // frame, and netplay rollback rewinds on every correction. A plain
        // (fc - last >= 600) then reads hundreds of thousands of frames negative
        // and never fires again for the rest of the run — the battery save
        // silently stops being written. Restart the window at the rewound frame
        // instead of flushing, so a rollback that corrects every frame does not
        // pay for a dirty check every frame.
        {
            int64_t fc = m_frame_counter.load(std::memory_order_relaxed);
            if (fc < m_sram_flush_counter)
            {
                m_sram_flush_counter = fc;
            }
            else if (fc - m_sram_flush_counter >= 600)
            {
                m_sram_flush_counter = fc;
                FlushSramIfDirty();
                FlushSramBIfDirty();
                FlushSramRegionsIfDirty();
                FlushPackIfDirty();
            }
        }

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(now - last_time).count();
        last_time = now;
        accumulator += elapsed;

        if (!m_netplay_enabled.load(std::memory_order_acquire))
        {
            const auto frame_dur = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(frame_duration_ms));

            // Ceiling. Never call the core faster than it asked to be called.
            const double until_due =
                std::chrono::duration<double, std::milli>(next_call_due - now).count();
            if (until_due > SLEEP_MARGIN_MS)
            {
                std::this_thread::sleep_for(
                    std::chrono::duration<double, std::milli>(until_due - SLEEP_MARGIN_MS));
                continue;
            }

            // Brake. The sink drains at the mixer's rate, so waiting for it to want
            // audio is waiting on real time, whatever the core claims about either
            // its frame rate or its sample count.
            double brake_ms = m_audio_handler->MsUntilSinkWantsFrames();
            if (brake_ms > MAX_BRAKE_WAIT_MS)
                brake_ms = MAX_BRAKE_WAIT_MS;
            m_audio_handler->SetLastBrakeMs(brake_ms);
            if (brake_ms <= SLEEP_MARGIN_MS)
            {
                // The sink wants audio: it is draining, so it is still a clock.
                last_sink_want = now;
            }
            else if (std::chrono::duration<double, std::milli>(now - last_sink_want).count()
                     < SINK_SILENCE_LIMIT_MS)
            {
                std::this_thread::sleep_for(
                    std::chrono::duration<double, std::milli>(brake_ms - SLEEP_MARGIN_MS));
                continue;
            }

            m_audio_handler->CallAudioBufferStatusCallback();

            m_core->retro_run();
            m_core_ran_frame = true;
            m_frame_counter.fetch_add(1, std::memory_order_relaxed);

            // Achievements. Emulation thread, strictly after the frame the core
            // just produced, and only ever on a frame that is final; the rollback
            // paths deliberately do not call this (see NetplayRollbackIteration).
            if (RetroAchievements* ra = RetroAchievements::GetSingleton())
                ra->DoFrame(this);

            // Charge the ceiling one frame. A call that overran its slot must not
            // earn catch-up calls, so the arrears are floored a few frames back
            // rather than replayed as fast-forward.
            next_call_due += frame_dur;
            const auto arrears_floor = std::chrono::steady_clock::now() - frame_dur * 4;
            if (next_call_due < arrears_floor)
                next_call_due = arrears_floor;
            continue;
        }

        // ── Netplay rollback: run ahead on prediction, rewind on correction ──
        if (m_np_rollback.load(std::memory_order_acquire))
        {
            NetplayRollbackIteration(frame_duration_ms, accumulator);
            continue;
        }

        // ── Netplay lockstep: run frame N only once its inputs arrived ──────
        if (accumulator < frame_duration_ms)
            continue;
        // Cap catch-up debt after stalls to a few frames.
        if (accumulator > frame_duration_ms * 4.0)
            accumulator = frame_duration_ms * 4.0;

        NpFrame frame_inputs{};
        {
            std::unique_lock<std::mutex> lock(m_np_mutex);
            int64_t frame = m_frame_counter.load(std::memory_order_relaxed);
            bool ready = m_np_cv.wait_for(lock, std::chrono::milliseconds(4), [&]
                { return m_stop_requested.load() || m_np_inputs.count(frame) > 0; });
            if (m_stop_requested)
                break;
            if (!ready)
                continue;   // stall; loop again (drains commands, re-checks stop)
            frame_inputs = m_np_inputs[frame];
            m_np_inputs.erase(m_np_inputs.begin(), m_np_inputs.lower_bound(frame - 30));
        }

        // Netplay-scheduled disc ops, resets and link changes land strictly
        // before their frame runs, so every peer changes deterministic state on
        // the identical boundary.
        ApplyScheduledDiscOps(m_frame_counter.load(std::memory_order_relaxed));
        ApplyScheduledResets(m_frame_counter.load(std::memory_order_relaxed));
        ApplyScheduledLinkOps(m_frame_counter.load(std::memory_order_relaxed));

        // Apply the agreed inputs: in netplay mode the emulation thread is
        // the sole InputHandler writer for the masked ports. Device-aware
        // (mouse ports get deltas), plus the aux block (sensor/touch).
        ApplyNetplayInputs(frame_inputs, m_np_port_mask.load(std::memory_order_relaxed));
        ApplyNetplayAux(frame_inputs);

        m_audio_handler->CallAudioBufferStatusCallback();
        m_core->retro_run();
        m_core_ran_frame = true;
        int64_t frame_done = m_frame_counter.fetch_add(1, std::memory_order_relaxed) + 1;
        accumulator -= frame_duration_ms;

        // Lockstep netplay frames are final (every peer has confirmed the inputs
        // before the frame runs), so they count, unlike rollback speculation.
        if (RetroAchievements* ra = RetroAchievements::GetSingleton())
            ra->DoFrame(this);

        if (m_np_crc_interval > 0 && frame_done % m_np_crc_interval == 0)
            EmitNetplayCrc(frame_done);
    }
}
}
