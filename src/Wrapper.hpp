#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_key.hpp>

#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <vector>
#include <queue>
#include <map>
#include <set>
#include <array>
#include "TransferPakInterface.hpp"
#include <deque>
#include <iterator>
#include <unordered_map>

#include <libretro.h>
#include <readerwriterqueue.h>

#include "ThreadCommand.hpp"
#include "EmuThreadCommands.hpp"
#include "Core.hpp"
#include "CallbackTrampolines.hpp"
#include "EnvironmentHandler.hpp"
#include "VideoHandler.hpp"
#include "AudioHandler.hpp"
#include "InputHandler.hpp"
#include "OptionsHandler.hpp"
#include "MessageHandler.hpp"
#include "LogHandler.hpp"
#include "MicrophoneHandler.hpp"

namespace Xenu
{
// Forward declaration to avoid circular include (Libretro.hpp includes Wrapper.hpp indirectly)
class Libretro;

class Wrapper
{
public:
    Wrapper() = default;
    ~Wrapper();

    Wrapper(const Wrapper&) = delete;
    Wrapper& operator=(const Wrapper&) = delete;
    Wrapper(Wrapper&&) = delete;
    Wrapper& operator=(Wrapper&&) = delete;

    /// Returns the Wrapper instance currently running on this thread. Per-wrapper
    /// callback trampolines set this before dispatch, including on core-created
    /// threads, so no process-global fallback is needed or safe.
    static Wrapper* GetCurrentThreadWrapper();

    /// Set or clear the current-thread Wrapper pointer. Called automatically by
    /// the emulation thread; also used by thread commands and cleanup code that
    /// run on the main thread and need access to the owning Wrapper.
    static void SetCurrentThreadWrapper(Wrapper* wrapper);

    /// Locate a core inside <root>/cores, trying every filename convention the
    /// platform can use. Shared with the pre-start option peek so both resolve
    /// the same file for a given core name.
    static std::string ResolveCorePath(const std::string& root_directory, const std::string& core_name);

    void StartContent(const std::string& root_directory, const std::string& core_name, const std::string& game_path);

    /// As StartContent, plus a libretro subsystem load: multi-file content the
    /// core takes as one unit. game_path keeps its usual meaning as the identity
    /// path; subsystem_paths is the ordered list handed to
    /// retro_load_game_special. An empty ident behaves exactly like StartContent.
    void StartSubsystemContent(const std::string& root_directory, const std::string& core_name,
                               const std::string& game_path, const std::string& subsystem_ident,
                               const std::vector<std::string>& subsystem_paths);
    void StopContent();
    /// Silence, then stop the emulation thread, waiting at most `budget_ms` for it
    /// to leave the core. True when it exited and teardown finished. False means
    /// the core never unwound (Dolphin has managed this): the thread is still
    /// running inside this Wrapper, so the caller must Abandon it rather than
    /// destroy it.
    bool ShutdownForExit(uint32_t budget_ms);

    /// Same bounded stop without the silencing, for a restart.
    bool StopEmulationThreadBounded(uint32_t budget_ms);

    /// Give up on a thread that will not stop: silence it and detach it, leaving
    /// every handler it is still inside alive. A Wrapper this has been called on
    /// must never be destroyed.
    void AbandonThread();
    /// The running core's picture. See VideoHandler::GetTexture.
    godot::Ref<godot::ImageTexture> GetVideoTexture() const;
    /// The same frame CPU-side. See VideoHandler::GetImage.
    godot::Ref<godot::Image> GetVideoImage() const;

    /// Whether the core asked for the controller display interface.
    bool HasControllerScreens() const;
    /// The screen of device `index` on controller `port`, or null until the core
    /// has pushed one. MAIN THREAD: it builds a texture.
    godot::Ref<godot::ImageTexture> GetControllerScreenTexture(int port, int index);
    /// retro_controller_display_interface.refresh. Any thread.
    bool StoreControllerScreen(unsigned port, unsigned index, const uint32_t* pixels,
                               unsigned width, unsigned height);
    /// Called when the core takes the interface, at every content start.
    void NoteControllerScreensOffered();
    /// Whether the core's sound is heard at all. Used to be a side effect of
    /// SetScreenMesh — a machine with nowhere to put its picture was muted — which
    /// only worked while the picture went somewhere by being PAINTED there.
    void SetAudioPlaying(bool playing);

    const std::unordered_map<std::string, OptionCategory>& GetOptionCategories() const { return m_options_handler->GetCategories(); }
    const std::unordered_map<std::string, OptionDefinition>& GetOptionDefinitions() const { return m_options_handler->GetDefinitions(); }
    const std::unordered_map<std::string, std::string>& GetOptionValues() const { return m_options_handler->GetValues(); }
    void SetCoreOption(const std::string& key, const std::string& value);

    /// Returns per-port controller info as an Array of Arrays of Dictionaries
    /// [{name, id}], indexed by port number. Consumed by Libretro::GetControllerInfo().
    godot::Array GetControllerInfo() const;

    /// Tell the running core which device type is active on a given port.
    /// Calls retro_set_controller_port_device and updates the local tracking map.
    void SetControllerPortDevice(uint32_t port, uint32_t device);

    /// Light gun input forwarding. Called from the Libretro node on the main thread.
    void SetLightgunPosition(uint32_t port, int16_t x, int16_t y);
    void SetLightgunIsOffscreen(uint32_t port, bool offscreen);
    void SetLightgunButton(uint32_t port, int button_id, bool pressed);

    /// Per-port joypad input. Replaces the hardcoded port-0 path in _process for
    /// physical controller objects that know their own port assignment.
    void SetJoypadState(uint32_t port, uint16_t button_mask, int16_t analog_lx, int16_t analog_ly, int16_t analog_rx, int16_t analog_ry);
    /// Read the frontend's buffered joypad state without consuming it. This is
    /// useful for diagnostics and no-core integration tests; values are
    /// [buttons, left_x, left_y, right_x, right_y].
    godot::PackedInt32Array PeekJoypadState(uint32_t port) const;

    /// Keyboard input: update the RETRO_DEVICE_KEYBOARD poll bitset AND fire
    /// the core's keyboard event callback (modifiers derived from held keys).
    /// Keyboard state is global in practice, so feed port 0.
    void SetKeyState(uint32_t port, uint32_t keycode, bool down, uint32_t character);

    /// Translate a Godot key event to its RETROK_* code. Static and side-effect
    /// free: the application decides when a key should reach a core (see
    /// retro_keyboard.gd), this only does the lookup. Takes the whole event
    /// because get_location() selects the left/right modifier variant.
    static retro_key GodotKeyToRetroKey(const godot::Ref<godot::InputEventKey>& keyEvent);

    /// Per-port mouse input (RETRO_DEVICE_MOUSE) for physical mouse objects.
    /// dx/dy are relative deltas ACCUMULATED until the core's next read;
    /// buttons is a bitmask of (1 << RETRO_DEVICE_ID_MOUSE_*): LEFT bit 2,
    /// RIGHT bit 3, MIDDLE bit 6.
    void SetMouseState(uint32_t port, int32_t dx, int32_t dy, uint32_t buttons);

    /// Accelerometer feed (g units, at-rest flat ≈ (0,0,1)) for the libretro
    /// sensor interface, so a held handheld's physical tilt drives tilt carts.
    ///
    /// `index` names the sub-device on that port: 0 is the controller itself,
    /// 1 is whatever is plugged into it, such as a Wii Nunchuk.
    void SetSensorAccel(uint32_t port, float x, float y, float z, uint32_t index = 0);

    /// Gyroscope feed (radians/second about the device's own axes) for the same
    /// interface. Rotation rate, not orientation: a still device reads (0,0,0).
    void SetSensorGyro(uint32_t port, float x, float y, float z, uint32_t index = 0);

    /// Bits ORed into `port`'s joypad reads, for a button on a peripheral.
    void SetJoypadExtraButtons(uint32_t port, uint16_t buttons);

    /// Host microphone frames for every microphone this core has switched on.
    void PushMicrophoneFrames(const godot::PackedVector2Array& frames, double source_rate, float gain);
    bool IsMicrophoneActive() const;

    /// Touch/pointer feed (RETRO_DEVICE_POINTER): x/y normalized to
    /// [-0x7FFF, 0x7FFF] across the WHOLE video output (the composite
    /// framebuffer for dual-screen cores; melonDS maps the bottom-screen
    /// region of it to DS touch).
    void SetPointerState(uint32_t port, int16_t x, int16_t y, bool pressed);
    void SetPointerIndexState(uint32_t port, uint32_t index, int16_t x, int16_t y, bool pressed);

    // ── Netplay (deterministic lockstep) ─────────────────────────────────────
    // In netplay mode the emulation thread runs frame N only once the inputs
    // for frame N have been posted (all masked ports at once), making every
    // peer's core execute an identical input timeline.

    /// Enable/disable lockstep gating. port_mask selects which of ports 0-3
    /// participate; start_frame (>= 0) resets the frame counter (use 0 for a
    /// cold start, the savestate frame for a late join). Safe to call before
    /// StartContent.
    void SetNetplayMode(bool enabled, uint32_t port_mask, int64_t start_frame);

    /// Post the agreed inputs for one frame: flat array of 4 ports × 5 values
    /// {button_mask, analog_lx, analog_ly, analog_rx, analog_ry}, followed by
    /// the optional sensor, pointer, and keyboard auxiliary block documented
    /// below. Legacy 20-value frames remain valid.
    /// Lockstep: releases the gate for `frame`. Rollback: confirms `frame`, and
    /// a mismatch against what was executed triggers rewind+replay.
    void PostNetplayInputs(int64_t frame, const godot::PackedInt32Array& flat);

    // ── Rollback (GGPO-style, layered on netplay mode) ───────────────────────
    // Locally-owned ports apply live with zero added delay; remote ports are
    // predicted (hold-last-input). Each frame is savestated into a ring before
    // it runs. When confirmed inputs contradict a prediction the emulation
    // thread reloads the state at the mispredicted frame and silently replays
    // (audio dropped, video skipped except the final frame), so the local
    // player never feels the network.

    /// Enable rollback within netplay mode. local_mask ⊆ port_mask marks the
    /// ports whose input is sampled live on this peer; max_ahead caps how far
    /// past the last confirmed frame the emulation may speculate before
    /// stalling. Call after SetNetplayMode, before StartContent.
    void SetNetplayRollback(bool enabled, uint32_t local_mask, int max_ahead);

    /// Change which ports are locally sampled at an agreed future frame. This
    /// is how a fixed controller receiver can be unplugged or reassigned while
    /// rollback remains active without changing ownership at different times
    /// on different peers.
    bool ScheduleNetplayLocalMask(int64_t frame, uint32_t local_mask);

    /// Drain the per-frame local-input records the emulation thread produced:
    /// flat groups of 7 ints {frame, port, buttons, alx, aly, arx, ary}. These
    /// are the authoritative "what this peer pressed on frame N" values that
    /// the session ships to the host for assembly.
    godot::PackedInt32Array TakeNetplayLocalRecords();

    /// Core-specific ids for a machine's SECOND save region. libretro.h has one
    /// save-RAM id, so a core that carries two publishes the other itself and
    /// each picks its own number.
    static constexpr unsigned SRAM_B_SUFAMI_TURBO = (4 << 8) | RETRO_MEMORY_SAVE_RAM;
    static constexpr unsigned SRAM_B_PCSX_MEMCARD2 = (1 << 8) | RETRO_MEMORY_SAVE_RAM;

    // ── Battery saves (SRAM / RETRO_MEMORY_SAVE_RAM) ─────────────────────────
    // The frontend owns persistence: SRAM is loaded from m_sram_path right
    // after retro_load_game and flushed (dirty-checked) every ~10 s and at
    // shutdown. An empty path disables persistence entirely (e.g. a PSX with
    // no memory card seated).

    /// Set the .srm file backing this run. Call before StartContent; calling
    /// while running performs a hot-swap on the emulation thread (flush the
    /// old file, load the new one into SAVE_RAM): a real memory-card swap.
    void SetSramPath(const godot::String& path);

    /// Netplay: inject SRAM content directly (applied at load instead of the
    /// file, so every peer boots with identical SRAM). Never flushed.
    void SetSramData(const godot::PackedByteArray& data);

    /// Declare that this machine's battery save lives on REMOVABLE media, so
    /// an unbacked run means "nothing is plugged in" rather than "don't save".
    /// With this set and no path or injected bytes, SAVE_RAM is blanked at load
    /// instead of being left as the core initialized it. pcsx_rearmed hands back
    /// a fully FORMATTED card when the frontend supplies nothing, so without
    /// blanking a PSX with no card seated accepts a save and loses it at
    /// power-off; blanking makes the game report an unformatted card instead.
    ///
    /// Opt-in on purpose: a fixed-storage core may initialize SAVE_RAM to 0xFF
    /// (flash/EEPROM) and zeroing that would fake corrupt save data, and a
    /// netplay client legitimately runs with an empty path and empty bytes.
    void SetRemovableStorage(bool removable);

    /// Force a dirty-check flush now (emu-thread command).
    void RequestSramFlush();

    // Emulation-thread internals (SRAM).
    void LoadSramFromSource();
    /// `final_flush` marks the flush at core shutdown; it only labels the
    /// signal, the write itself is identical.
    void FlushSramIfDirty(bool final_flush = false);
    void ApplySramSwap(const std::string& new_path);

    /// The file a writable CONTENT medium is written back to, when the medium
    /// itself is what the core mutates. A BS-X 8M Memory Pack is the case: the
    /// .bs handed to the core IS the flash, so there is no separate save file
    /// and no load step -- the content load already put the right bytes there.
    void SetPackPath(const godot::String& path);
    /// Emu thread: snapshot the pack as loaded, so the first dirty check has
    /// something to compare against.
    void SnapshotPack();
    /// Emu thread: write the pack back over its own file iff it changed.
    void FlushPackIfDirty(bool final_flush = false);

    /// The file the SECOND cartridge's battery is kept in, on a machine that
    /// holds two at once. Only the Sufami Turbo does.
    ///
    /// It needs a path of its own because the two saves are two REGIONS, not one
    /// longer one: snes9x answers RETRO_MEMORY_SAVE_RAM with slot A's SRAM alone
    /// and puts slot B's under a memory id of its own, 0x10000 further into the
    /// same block and past the end of what SAVE_RAM reports. A frontend reading
    /// only SAVE_RAM keeps half a linked pair's progress and loses the other
    /// half without saying so -- which is what SD Ultra Battle means when it
    /// reports that the B cassette's backup is not initialised, every launch.
    ///
    /// Ordinary save semantics, unlike SetPackPath: this file is read back into
    /// the core at content load and written out again when it changes.
    ///
    /// `memory_id` says which region, because the id is the core's own and each
    /// core picks a different number. Calling while running hot-swaps, as
    /// SetSramPath does -- a PlayStation's second card can be pulled mid-game.
    void SetSramBPath(const godot::String& path, unsigned memory_id = SRAM_B_SUFAMI_TURBO);
    /// Emu thread: fill slot B's SRAM from its file, and snapshot it.
    void LoadSramBFromSource();
    /// Emu thread: write slot B's SRAM out iff it changed.
    void FlushSramBIfDirty(bool final_flush = false);
    /// Emu thread: flush the outgoing second region, then adopt a new one.
    void ApplySramBSwap(const std::string& new_path, unsigned memory_id);

    /// The file that holds the cartridge's real-time clock, RETRO_MEMORY_RTC.
    /// Read at the next content load and written when the clock changes; a path
    /// set while content runs waits for that load, because a cartridge cannot
    /// change under a running machine. The bytes are the core's own layout, so
    /// the file belongs to one core. Not announced through sram_flushed: it is
    /// not a save anything backs up.
    void SetRtcPath(const godot::String& path);
    /// Emu thread: adopt the path SetRtcPath left, fill the clock from its file
    /// before the first frame, and snapshot it.
    void LoadRtcFromSource();
    /// Emu thread: write the clock out iff it changed.
    void FlushRtcIfDirty(bool final_flush = false);

    /// One Controller Pak's 32 KiB, bound to a file of its own inside the ONE
    /// SAVE_RAM block both N64 cores publish. The paks are not separate memory
    /// ids the way the Sufami Turbo's second cartridge is -- all four live in
    /// the cartridge .srm at 0x800 + port * 0x8000 -- so a pak that travels
    /// between controllers needs its bytes moved in and out of that slab by the
    /// frontend at every seat and unseat. The core cannot: nx backs mempaks
    /// with a read-only storage whose save is a no-op, so core-side writes never
    /// persist and unplug_mempak moves nothing.
    ///
    /// `index` is the libretro port (0-3). An empty path unbinds, same as
    /// ClearSramRegion.
    void SetSramRegionPath(int index, const godot::String& path, int64_t offset, int64_t length);
    void ClearSramRegion(int index);
    /// Emu thread: overlay every bound region on top of SAVE_RAM. MUST run after
    /// LoadSramFromSource, which fills the whole blob from the cartridge .srm
    /// and would otherwise put stale pak bytes back over these.
    void LoadSramRegionsFromSource();
    /// Emu thread: write each bound region back to its own file iff it changed.
    void FlushSramRegionsIfDirty(bool final_flush = false);
    /// Emu thread: adopt any binding staged by SetSramRegionPath -- flush what
    /// the port held, then fill SAVE_RAM from the new pak's file.
    void ApplySramRegionSwaps();
    void LoadSramRegion(int index);
    void FlushSramRegionIfDirty(int index, bool final_flush = false);
    /// The live SAVE_RAM window for one region, or nullptr when the core
    /// has no such block or the region will not fit inside it.
    uint8_t* SramRegionWindow(int index, size_t& out_len);

    /// The Game Boy cartridge in the Transfer Pak on one controller's port, and
    /// where that cartridge's battery lives. Answered to the core through
    /// RETRO_ENVIRONMENT_GET_TRANSFER_PAK_INTERFACE, which is the only route
    /// that is per PORT: the `gb` subsystem and the <rom>.gb sidecar both set
    /// one pair of globals shared by all four paks.
    ///
    /// Setting a port bumps its generation, which is what makes the core re-read
    /// a cartridge swapped while the pak stayed seated.
    void SetTransferPak(int port, const godot::String& rom_path, const godot::String& ram_path);
    void ClearTransferPak(int port);
    /// Where that cartridge's real-time clock is kept, served through
    /// RETRO_ENVIRONMENT_GET_TRANSFER_PAK_CLOCK_INTERFACE. Set before
    /// SetTransferPak: the core asks when the cartridge is next read, which the
    /// generation SetTransferPak bumps is what triggers.
    void SetTransferPakClock(int port, const godot::String& rtc_path);

    /// Emu thread, called from the interface trampolines. The returned pointer
    /// stays valid until the next call for the same port, which is the contract
    /// the core copies under.
    const char* TransferPakRomFor(unsigned port);
    const char* TransferPakRamFor(unsigned port);
    const char* TransferPakRtcFor(unsigned port);
    unsigned TransferPakGenerationFor(unsigned port);

    /// Front-panel reset: retro_reset on the emulation thread, between frames.
    /// Nothing is unloaded and no thread is joined, so this cannot block the
    /// caller however the core manages its own threads.
    void RequestReset();

    /// Netplay: apply retro_reset strictly before running confirmed `frame` on
    /// every peer. Unlike RequestReset this is part of the shared timeline.
    void ScheduleReset(int64_t frame);

    /// Will a command enqueued right now ever be drained? True while the
    /// emulation thread is running AND while it is still starting up: the queue
    /// is drained at the top of every loop iteration, which by construction is
    /// after the core is loaded, so enqueueing during startup is correct.
    /// Testing m_running alone loses every request made in the window between
    /// StartContent returning and the emulation thread reaching its loop.
    bool AcceptsEmuCommands() const;

    /// The negative answers, emitted straight through the node's deferred-call
    /// queue rather than the main-thread command queue: _process drains that
    /// queue only while running, and discards it outright while a stop is
    /// pending, so a reply posted there when the core is gone is never seen.
    /// Shared by the request guards and by EmuThreadCommand::Abandon.
    void AnswerNoSaveState();
    void AnswerNoLoadState();
    void AnswerNoDiskInfo();

    /// Serialize the core on the emulation thread; result arrives via the
    /// savestate_ready(data, frame) signal (empty data on failure).
    void RequestSaveState();

    /// Unserialize a savestate on the emulation thread and reset the netplay
    /// schedule to `frame`; result arrives via savestate_loaded(ok).
    void RequestLoadState(const godot::PackedByteArray& data, int64_t frame);

    // Disk control (multi-disc games). All three enqueue emu-thread commands;
    // state comes back via the disk_control_ready(has_control, count,
    // current_index, ejected) signal. Safe no-ops when the core has no
    // disk-control interface (or nothing is running).

    /// Query the core's disk-control state.
    void RequestDiskInfo();

    /// Open (true) / close (false) the core's virtual disc tray.
    void SetDiskEjectState(bool ejected);

    /// Hand the core a new disc file at image `index` (tray must be open).
    void ReplaceDiskImage(uint32_t index, const godot::String& path);

    /// Netplay: schedule a disc op to apply deterministically right before
    /// running confirmed `frame`. op 0 = eject (open tray),
    /// op 1 = replace at `index` with `path` then close the tray.
    void ScheduleDiscOp(int64_t frame, int32_t op, uint32_t index, const godot::String& path);

    /// Netplay: apply a link-cable change strictly before running `frame`, so
    /// every peer joins or drops the same wire on the same emulated frame.
    /// op 1 connects `group` as one bus, head first; op 0 drops group[0].
    ///
    /// The Wrapper pointers are captured here, on the main thread, and held
    /// until the frame lands. That is the same lifetime LinkCoordinator already
    /// gives them for the life of a connection, so it is no weaker than an
    /// immediate ConnectGroup - but a machine freed inside the LINK_LEAD window
    /// would still be a dangling entry, which is why the frontend tears the
    /// schedule down when a session stops.
    void ScheduleLinkOp(int64_t frame, int32_t op,
                        const std::vector<std::pair<Wrapper*, unsigned>>& group);

    // Emulation-thread internals (disk control).
    void EmitDiskInfo();
    void ApplyScheduledDiscOps(int64_t frame);
    void ApplyScheduledLinkOps(int64_t frame);
    void ApplyScheduledResets(int64_t frame);

    struct DiscOp
    {
        int32_t op = 0;
        uint32_t index = 0;
        std::string path;
    };

    int64_t GetFrameCount() const { return m_frame_counter.load(std::memory_order_relaxed); }

    /// How many rewind+replay corrections have happened (diagnostics/HUD).
    int64_t GetNetplayRollbackCount() const { return m_np_rollback_count.load(std::memory_order_relaxed); }

    /// What rollback actually costs on this machine, for a probe to read.
    /// Rollback pays a full retro_serialize per frame whether or not a rewind
    /// ever happens, so the per-frame cost and the per-rewind cost are
    /// separate questions and are counted separately.
    godot::Dictionary GetNetplayRollbackStats() const;

    /// How many core states to keep. Tracks the speculation window, because
    /// that is exactly how far back a rollback anchor can be: the throttle
    /// refuses to run past watermark + max_ahead, so a deeper ring holds
    /// states no rewind can reach. Raise max_ahead and this follows.
    int StateRingDepth() const
    {
        return std::clamp(m_np_max_ahead + NP_STATE_RING_MARGIN,
                          12, static_cast<int>(NP_ROLLBACK_HISTORY));
    }

    /// Who the running core says it is: library_name, library_version, the
    /// libretro API version it was built against, and the size of one savestate.
    /// EMPTY until content has finished loading, and empty again once it stops,
    /// which is what makes it usable as a readiness test as well as an
    /// identity. Published from the emulation thread.
    ///
    /// `serialize_size` is 0 until the core has produced its first frame, and
    /// the other three arrive at load. The split is not tidiness: the size is
    /// not a property a core can always answer for before it has run, because
    /// Dolphin measures one by marshalling onto its CPU thread and walking
    /// every subsystem, and asking early segfaults it on a machine that does
    /// not exist yet. But the identity cannot WAIT for a frame either, because
    /// netplay's gate does not run frame 0 until every peer has reported ready,
    /// and readiness is this dictionary being non-empty. Requiring a frame
    /// deadlocks every cold start; asking at load kills Dolphin. So the two
    /// halves are published at the two different moments each one is safe at.
    ///
    /// A reader comparing sizes across peers must therefore treat 0 as "not
    /// measured yet" rather than as a difference.
    godot::Dictionary GetCoreIdentity() const;

    /// What the core currently declares its machine runs at. Updated when a
    /// running core successfully changes its AV info. 0 until content is running.
    double GetDeclaredFps() const { return m_declared_fps.load(std::memory_order_relaxed); }
    double GetDeclaredSampleRate() const { return m_declared_sample_rate.load(std::memory_order_relaxed); }

    /// Frame uploads discarded because a newer one superseded them before the main
    /// thread drained the queue — i.e. emulated frames that were never displayed.
    /// Reset with each content run.
    int64_t GetDroppedFrameCount() const { return m_dropped_frames.load(std::memory_order_relaxed); }

    /// Audio sink fill 0-100, and the pacing brake in ms. Both forward to the audio
    /// handler, which may not exist yet; defined out of line because it is only
    /// forward declared here.
    uint32_t GetAudioBufferOccupancy() const;
    double GetAudioBrakeMs() const;

    /// Queue a signal emission on the owning Libretro node (main thread).
    /// Callable from the emulation thread.
    void EmitSignalOnMainThread(const godot::StringName& signal_name, const godot::Array& args);

    /// CRC32 over every writable region of the core's published memory map, for
    /// cores that do not answer RETRO_MEMORY_SYSTEM_RAM at all. Sets `ok` false
    /// when the core published no usable map either.
    uint32_t MappedRamCrc(bool& ok) const;

    /// The exact bytes MappedRamCrc hashes, with the region boundaries, so a
    /// divergence can be located instead of merely detected. A CRC says two
    /// runs differ; this says WHERE, which for a savestate fault is the whole
    /// question. Diagnostic only - it reads core memory from the calling
    /// thread, so take it while the core is parked at a netplay gate.
    godot::Dictionary SnapshotMappedRam() const;
    /// CRC32 of the core's system RAM, emitted as netplay_crc(frame, crc)
    /// every m_np_crc_interval frames while in netplay mode (desync detection).
    void EmitNetplayCrc(int64_t frame);
    /// Whichever oracle this core is set to. See m_np_crc_from_state.
    uint32_t ComputeNetplayCrc(bool& ok);
    uint32_t ComputeRamCrc(bool& ok) const;
    /// CRC32 of a savestate taken right now, for cores whose RAM cannot be read
    /// coherently from this thread. See m_np_crc_from_state.
    uint32_t StateCrc(bool& ok);

    // Emulation-thread internals (rollback engine).
    void NetplayRollbackIteration(double frame_duration_ms, double& accumulator);
    bool NetplayRollbackReplay(int64_t to_frame, uint32_t mask);
    uint32_t NetplayLocalMaskForFrameLocked(int64_t frame) const;
    uint32_t ApplyScheduledNetplayLocalMask(int64_t frame);
    bool SaveRollbackState(int64_t frame);
    void FailNetplayRollback(const std::string& reason);
    bool IsNetplayPortManaged(uint32_t port) const;
    // Netplay frame payload: 4 ports × 5 ints. Joypads use btn/analogs, mice
    // use buttons/dx/dy, and lightguns use buttons/x/y/offscreen. Each port is
    // followed by an aux block: flags, accel[2]*xyz (milli-g), gyro[2]*xyz
    // (centi-rad/s), pointer[4]*{x,y,pressed}. The tail has 4 keyboard events.
    // Legacy shorter frames are accepted (the remainder is zeroed).
    static constexpr int NP_PORTS = 4;
    static constexpr int NP_INPUT_INTS_PER_PORT = 5;
    static constexpr int NP_INPUT_INTS = NP_PORTS * NP_INPUT_INTS_PER_PORT;
    static constexpr int NP_SENSOR_COUNT = 2;
    static constexpr int NP_POINTER_COUNT = 4;
    static constexpr int NP_AUX_INTS_PER_PORT = 25;
    static constexpr int NP_AUX_INTS = NP_PORTS * NP_AUX_INTS_PER_PORT;
    static constexpr int NP_KEY_SLOTS = 4;
    static constexpr int NP_AUX_OFFSET = NP_INPUT_INTS;
    static constexpr int NP_KEY_OFFSET = NP_AUX_OFFSET + NP_AUX_INTS;
    static constexpr int NP_FRAME_INTS = NP_KEY_OFFSET + NP_KEY_SLOTS * 2;
    /// How far back the INPUT schedule is kept. Not the state ring: see
    /// StateRingDepth().
    static constexpr int64_t NP_ROLLBACK_HISTORY = 40;
    /// The furthest a peer may run past the last confirmed frame. This is the
    /// same quantity RetroArch calls NETPLAY_MAX_STALL_FRAMES (60 there, for
    /// internet play; this is a LAN session, where 24 frames is 400 ms).
    static constexpr int NP_MAX_AHEAD_LIMIT = 24;
    /// Slack above max_ahead for the anchor scan, which starts one frame past
    /// the last VERIFIED frame rather than at the current one.
    static constexpr int NP_STATE_RING_MARGIN = 8;
    static_assert(NP_MAX_AHEAD_LIMIT + NP_STATE_RING_MARGIN <= NP_ROLLBACK_HISTORY,
        "the state ring must fit inside the input window: raising max_ahead "
        "past it would let a rollback anchor fall off the ring, which stops "
        "the session rather than degrading it");
    static constexpr int64_t NP_MAX_FUTURE_INPUTS = 600;
    using NpFrame = std::array<int32_t, NP_FRAME_INTS>;

    struct RollbackState
    {
        int64_t frame = 0;
        std::vector<uint8_t> core;
        InputHandler::NetplayState input;
    };

    void ApplyNetplayInputs(const NpFrame& inputs, uint32_t mask);
    void ApplyNetplayAux(const NpFrame& inputs);
    void FlushNetplayCrcs();

    void _process(double delta);
    void PollDesktopInput();


    const std::string& GetRootDirectory() const { return m_root_directory; }
    const std::string& GetTempDirectory() const { return m_temp_directory; }

    /// Descriptors handed over by RETRO_ENVIRONMENT_SET_MEMORY_MAPS, deep-copied.
    /// The core only guarantees the array and the addrspace strings for the
    /// duration of that one callback; the `ptr` values inside stay valid for the
    /// session, which is what makes copying the rest worthwhile. rcheevos needs
    /// this to translate a RetroAchievements address into a host pointer.
    void SetMemoryDescriptors(const retro_memory_map* memory_maps);
    /// A retro_memory_map view over the copies. Empty descriptors when the core
    /// never sent any, which rc_libretro_memory_init handles by falling back to
    /// retro_get_memory_data.
    retro_memory_map GetMemoryMap() const;
    /// Whether the core declared achievement support via
    /// RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS. Cores that never call it are
    /// assumed to support them; the callback exists to opt OUT.
    bool GetSupportsAchievements() const { return m_supports_achievements; }
    void SetSupportsAchievements(bool support) { m_supports_achievements = support; }
    /// retro_get_memory_data/size for a RETRO_MEMORY_* id, for callers that need
    /// the raw region rather than the CRC ComputeRamCrc returns. Emulation thread.
    void GetCoreMemory(uint32_t id, uint8_t*& out_data, size_t& out_size) const;

    /// Apply RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO synchronously while the core
    /// is inside retro_run. Reinitializes affected drivers before returning.
    bool SetSystemAvInfo(const retro_system_av_info* av_info);

    /// The core is handed this struct's address in retro_get_system_av_info and may write
    /// through that pointer long afterwards (ScummVM revises timing.fps from inside
    /// context_reset), so it must outlive the call and cannot be a stack local.
    /// Read it live at the point of use; do not copy the fields out and cache them, or a
    /// core that legitimately retimes is pinned to its opening declaration.
    retro_system_av_info m_system_av_info = {};

    std::unique_ptr<Core> m_core = nullptr;
    std::unique_ptr<CallbackTrampolines> m_trampolines = nullptr;
    std::unique_ptr<EnvironmentHandler> m_environment_handler = nullptr;
    std::unique_ptr<VideoHandler> m_video_handler = nullptr;
    std::unique_ptr<AudioHandler> m_audio_handler = nullptr;
    // Exists before a core starts so physical peripherals can already feed (and
    // diagnostics can inspect) the frontend boundary. StartContent replaces it
    // with a clean handler for the new core.
    std::unique_ptr<InputHandler> m_input_handler = std::make_unique<InputHandler>();
    std::unique_ptr<OptionsHandler> m_options_handler = nullptr;
    std::unique_ptr<MessageHandler> m_message_handler = nullptr;
    std::unique_ptr<LogHandler> m_log_handler = nullptr;
    // Lives as long as the Wrapper: a microphone handle can outlive a core run on
    // the core's own threads, and resolves through this.
    std::unique_ptr<MicrophoneHandler> m_microphone_handler = std::make_unique<MicrophoneHandler>(*this);

    std::thread m_thread;
    moodycamel::ReaderWriterQueue<std::unique_ptr<ThreadCommand>> m_main_thread_commands_queue;
    std::mutex m_mutex;
    bool m_mutex_done = false;
    bool m_audio_reinit_success = false;
    bool m_audio_reinit_restore_failed = false;
    std::condition_variable m_condition_variable;
    std::atomic<bool> m_running = false;
    // Raised by StartContent before the thread launches, dropped once the loop
    // owns the core (or the thread gives up). Between those two points there is
    // a thread that WILL drain the command queue, which m_running cannot say.
    std::atomic<bool> m_starting = false;
    std::atomic<bool> m_stop_requested = false; // set by main thread; never written by emulation thread
    // Deferred-stop bookkeeping: m_stopping = a stop was signalled and teardown
    // is pending; m_thread_exited = the emulation thread has fully exited (set
    // by an RAII guard covering every exit path, so a deferred join can't hang).
    std::atomic<bool> m_stopping = false;
    std::atomic<bool> m_thread_exited = false;
    bool m_input_enabled = false;   // only true for the actively-controlled instance

    // Desired device per port, surviving across content runs. A controller (or
    // mouse) plugged in while the system is OFF records its device type here;
    // the emulation thread applies the map right after retro_load_game so the
    // core polls the right device from frame one. Guarded by m_port_device_mutex
    // (main thread writes, emulation thread reads once at startup).
    std::mutex m_port_device_mutex;
    std::unordered_map<uint32_t, uint32_t> m_pending_port_devices;

    // Netplay state. The input schedule maps frame → 4 ports × 5 int32s and is
    // written by the main thread (PostNetplayInputs) and consumed by the
    // emulation thread under m_np_mutex. In netplay mode the emulation thread
    // is the ONLY InputHandler writer for the masked ports.
    moodycamel::ReaderWriterQueue<std::unique_ptr<EmuThreadCommand>> m_emu_thread_commands_queue;
    std::atomic<bool> m_netplay_enabled = false;
    std::atomic<int64_t> m_frame_counter = 0;
    // Diagnostics published to the main thread (see the getters above). Written on
    // the emulation thread, read from _process; relaxed is enough for all three
    // because nothing else is ordered against them.
    // Core identity, published once per content run (see GetCoreIdentity). A
    // plain mutex rather than atomics: the strings are read a handful of times
    // per session, and a release flag cannot stop a reader mid-string when the
    // emulation thread clears them at teardown.
    // Core options set before the core existed. SetCoreOption is a main-thread
    // call and callers configure a machine BEFORE starting it, so dropping
    // these made a pre-start setter a silent no-op: the log said "core is not
    // running, skipping" and the caller believed it had set something.
    // Buffered here and applied once the core has declared its options.
    std::mutex m_pending_options_mutex;
    std::vector<std::pair<std::string, std::string>> m_pending_core_options;
    void ApplyPendingCoreOptions();

    mutable std::mutex m_core_identity_mutex;
    std::string m_core_library_name;
    std::string m_core_library_version;
    uint32_t m_core_api_version = 0;
    int64_t m_core_serialize_size = 0;
    bool m_core_identity_ready = false;
    /// Emulation thread only, so a plain bool: the measurement is one-shot.
    bool m_core_serialize_size_published = false;
    /// Has THIS content run a frame? Not the same question as a non-zero
    /// m_frame_counter, which is what used to gate the measurement: the counter
    /// is reset nowhere, so a second StartContent on a reused Wrapper inherited
    /// the previous run's thousands and let the measurement fire on the first
    /// loop pass, before the new core had run anything. Dolphin answers
    /// retro_serialize_size by walking every subsystem, so on a machine that has
    /// not booted it dereferences a null SI device and takes the process with it.
    /// Set beside every retro_run, cleared by ClearCoreIdentity.
    bool m_core_ran_frame = false;
    void PublishCoreIdentity();
    void PublishCoreSerializeSize();
    void ClearCoreIdentity();

    std::atomic<double> m_declared_fps{0.0};
    std::atomic<double> m_declared_sample_rate{0.0};
    std::atomic<uint64_t> m_timing_revision{0};
    std::atomic<int64_t> m_dropped_frames{0};
    std::atomic<uint32_t> m_np_port_mask = 0x1;
    std::mutex m_np_mutex;
    std::condition_variable m_np_cv;
    std::map<int64_t, NpFrame> m_np_inputs;
    int64_t m_np_crc_interval = 60;
    /// Take the desync CRC from a SAVESTATE rather than from live RAM.
    ///
    /// Reading RAM straight off the pointer assumes the core is quiescent when
    /// retro_run returns, and a core that emulates its CPU on a thread of its
    /// own is not: Dolphin's retro_run comes back on a GPU field boundary while
    /// its CPU thread keeps writing, so hashing 24 MB races that thread and two
    /// instances disagree even when the emulation is identical. The hash is
    /// then not an oracle, it is a coin toss with a good reputation.
    ///
    /// A savestate is the one snapshot libretro offers that such a core has to
    /// make coherent -- Dolphin marshals retro_serialize onto its CPU thread and
    /// waits for it -- so two of them can be compared. It costs far more than a
    /// RAM hash, which is what m_np_crc_interval is for.
    bool m_np_crc_from_state = false;
    /// Reused between checkpoints so a 92 MB state is not reallocated each time.
    std::vector<uint8_t> m_np_crc_state_buffer;

    /// How often a netplay RAM CRC is emitted, in frames. 60 for a session:
    /// often enough to catch a desync, rare enough that hashing the whole of
    /// RAM is not a per-frame cost. A vetting run sets it to 1 to find the
    /// FIRST frame that diverges, which is a different question from whether
    /// it diverged.
    void SetNetplayCrcInterval(int64_t frames) {
        m_np_crc_interval = frames > 0 ? frames : 1;
    }

    /// Choose which snapshot the desync CRC is taken from. See
    /// m_np_crc_from_state for why a core can need the expensive one.
    void SetNetplayCrcFromState(bool from_state) {
        m_np_crc_from_state = from_state;
    }

    // Netplay-scheduled disc ops (eject / replace), applied on the emulation
    // thread right before running their frame. Guarded by m_np_mutex.
    // A tray can be opened and closed before the same future boundary. Keep
    // both operations, in scheduling order, instead of replacing the eject
    // with the swap merely because their frame key matches.
    std::multimap<int64_t, DiscOp> m_disc_schedule;

    /// Netplay-scheduled front-panel resets. Multiple presses at one boundary
    /// are retained, matching the disc/link schedules' no-loss rule.
    std::multiset<int64_t> m_reset_schedule;

    struct LinkOp
    {
        int32_t op = 0;
        std::vector<std::pair<Wrapper*, unsigned>> group;
    };
    /// Netplay-scheduled link changes, applied on the emulation thread right
    /// before their frame. Guarded by m_np_mutex, like the disc schedule.
    // More than one cable may move on the same agreed frame.  A multimap keeps
    // every operation instead of silently replacing the first one.
    std::multimap<int64_t, LinkOp> m_link_schedule;

    // Rollback state. Everything below m_np_mutex-guarded unless noted.
    std::atomic<bool> m_np_rollback = false;
    std::atomic<uint32_t> m_np_local_mask = 0;
    uint32_t m_np_initial_local_mask = 0;
    /// Frame -> complete local ownership mask from that frame onward. Kept for
    /// the rollback history so a replay crossing a handoff uses the owner that
    /// was authoritative on each original frame.
    std::map<int64_t, uint32_t> m_np_local_mask_schedule;
    std::atomic<int64_t> m_np_rollback_count = 0;   // rewind+replay corrections
    std::atomic<int64_t> m_np_serialize_us = 0;     // time in retro_serialize for the ring
    std::atomic<int64_t> m_np_serialize_n = 0;      // states taken
    std::atomic<int64_t> m_np_replay_us = 0;        // time rewinding and replaying
    std::atomic<int64_t> m_np_replay_frames = 0;    // frames re-run across all rewinds
    std::atomic<int64_t> m_np_max_depth = 0;        // deepest single rewind, in frames
    std::atomic<int64_t> m_np_state_bytes = 0;      // bytes resident in the ring
    std::atomic<int64_t> m_np_state_slots = 0;      // states resident in the ring
    int m_np_max_ahead = 8;
    NpFrame m_np_live_local{};              // live local inputs (main thread writes)
    std::vector<int32_t> m_np_local_records;                // flat {frame,port,5 vals} drained by main thread
    // Emulation-thread-only rollback bookkeeping (no lock needed):
    std::map<int64_t, NpFrame> m_np_used;   // inputs each executed frame actually ran with
    std::deque<RollbackState> m_np_states;            // core + frontend input state before frame N
    std::map<int64_t, uint32_t> m_np_crc_pending;           // captured CRCs awaiting confirmation
    int64_t m_np_watermark = -1;                            // highest contiguous confirmed frame
    int64_t m_np_verified = -1;                             // highest frame verified/corrected against confirmations
    bool m_np_replaying = false;                            // true while re-running frames after a rewind
    bool m_np_replay_mute_video = false;                    // skip video uploads for this replayed frame
    bool m_handling_av_info = false;                        // reject recursive driver reinitialization

    /// True while the emulation thread is silently replaying frames after a
    /// rollback; audio/video callbacks drop their output. Emu thread only.
    bool IsNetplayReplaying() const { return m_np_replaying; }
    bool IsNetplayReplayVideoMuted() const { return m_np_replaying && m_np_replay_mute_video; }

    // SRAM persistence. m_sram_path/m_sram_pending are set from the main
    // thread BEFORE StartContent (or swapped via EmuThreadCommandSetSram);
    // the shadow copy is emulation-thread-only.
    std::string m_sram_path;
    godot::PackedByteArray m_sram_pending;
    std::vector<uint8_t> m_sram_shadow;
    int64_t m_sram_flush_counter = 0;
    bool m_removable_storage = false;

    // Writable content (the BS-X memory pack). Set from the main thread before
    // StartContent; the shadow is emulation-thread-only, as SRAM's is.
    std::string m_pack_path;
    std::vector<uint8_t> m_pack_shadow;

    // The second cartridge's battery, on a two-cartridge adapter. Same
    // ownership rules as SRAM above: path from the main thread before
    // StartContent, shadow touched only on the emulation thread.
    std::string m_sram_b_path;
    std::vector<uint8_t> m_sram_b_shadow;
    unsigned m_sram_b_id = SRAM_B_SUFAMI_TURBO;

    // The cartridge's clock. m_rtc_next_path is the main thread's, under the
    // mutex; m_rtc_path and the shadow are the emulation thread's, copied from it
    // at content load.
    std::mutex m_rtc_mutex;
    std::string m_rtc_next_path;
    std::string m_rtc_path;
    std::vector<uint8_t> m_rtc_shadow;
    bool m_rtc_on_disk = false;

    // One Controller Pak per libretro port. path/offset/length are written from
    // the main thread as paks are seated; the shadow is emulation-thread-only,
    // exactly as SRAM's is.
    struct SramRegion
    {
        std::string path;
        size_t offset = 0;
        size_t length = 0;
        std::vector<uint8_t> shadow;
    };
    std::array<SramRegion, RETRO_TRANSFER_PAK_PORTS> m_sram_regions;
    // --- Controller screens, pushed through the controller display interface --
    struct ControllerScreen
    {
        unsigned width = 0;
        unsigned height = 0;
        std::vector<uint32_t> pixels;
        uint64_t stamp = 0;
        /// Main thread only: the stamp the texture was last built from.
        uint64_t seen = 0;
        godot::Ref<godot::ImageTexture> texture;
    };
    std::mutex m_controller_screen_mutex;
    std::map<uint64_t, ControllerScreen> m_controller_screens;
    std::atomic<bool> m_controller_screens_offered{false};

    std::mutex m_sram_region_mutex;
    // A pak seated by hand must take effect now, not at the next 600-frame
    // flush tick, so the binding is staged here and adopted by the emulation
    // thread at the top of the frame -- the same shape as the memory-card
    // hot-swap, which cannot run on the caller's thread either.
    std::array<SramRegion, RETRO_TRANSFER_PAK_PORTS> m_sram_region_pending;
    std::array<bool, RETRO_TRANSFER_PAK_PORTS> m_sram_region_has_pending{};
    std::atomic<bool> m_sram_region_dirty{false};

    // Transfer Pak media, per port. Written from the main thread, read from the
    // emulation thread by the interface trampolines, so the table is guarded and
    // the returned strings are copied into views the emu thread alone owns.
    std::array<std::string, RETRO_TRANSFER_PAK_PORTS> m_transfer_pak_rom;
    std::array<std::string, RETRO_TRANSFER_PAK_PORTS> m_transfer_pak_ram;
    std::array<unsigned, RETRO_TRANSFER_PAK_PORTS> m_transfer_pak_generation{};
    std::array<std::string, RETRO_TRANSFER_PAK_PORTS> m_transfer_pak_rom_view;
    std::array<std::string, RETRO_TRANSFER_PAK_PORTS> m_transfer_pak_ram_view;
    std::array<std::string, RETRO_TRANSFER_PAK_PORTS> m_transfer_pak_rtc;
    std::array<std::string, RETRO_TRANSFER_PAK_PORTS> m_transfer_pak_rtc_view;
    std::mutex m_transfer_pak_mutex;

    std::string m_root_directory;
    std::string m_temp_directory;
    std::string m_username = "DefaultUser";
    retro_log_level m_log_level = RETRO_LOG_WARN;

    std::string m_game_path;

    std::vector<unsigned char> m_game_buffer;

    /// Non-empty only for a subsystem run: the ident the core published via
    /// SET_SUBSYSTEM_INFO ("ndd", ...), which selects retro_load_game_special
    /// over retro_load_game. Written by StartSubsystemContent before the
    /// emulation thread exists, and read only on that thread.
    std::string m_subsystem_ident;

    /// Ordered content paths for a subsystem run, one per rom the core declared,
    /// in the core's order. m_game_path remains the single identity path that
    /// saves, save states, achievements and netplay hashing key off; these are
    /// used for nothing but the load call itself.
    std::vector<std::string> m_subsystem_paths;

    /// One buffer per subsystem rom that is not need_fullpath. Sized once to the
    /// rom count before any element is filled, so the retro_game_info data
    /// pointers taken from each element cannot move under the core -- the same
    /// discipline the memory descriptors below need, for the same reason.
    std::vector<std::vector<unsigned char>> m_subsystem_buffers;

    /// Backing store for SetMemoryDescriptors. The strings are held separately so
    /// the char* inside each descriptor keeps pointing at storage we own; a
    /// vector<string> would reallocate its elements' buffers on growth, so this is
    /// only ever filled in one pass and never appended to afterwards.
    std::vector<retro_memory_descriptor> m_memory_descriptors;
    std::vector<std::string> m_memory_addrspaces;
    bool m_supports_achievements = true;

    /// The Libretro node that owns this Wrapper (set by Libretro constructor).
    /// Held by id, not by pointer: the emulation thread signals back through this
    /// node, and it can be freed while that thread is still running (a scene change,
    /// or a core that will not unwind). Resolved per use, so a dead node is a
    /// no-op instead of a dangling call.
    Libretro* LiveLibretroNode() const;

    /// Emu thread: tell the node the run could not start. Safe when the node has
    /// already gone -- LiveLibretroNode answers null and this does nothing.
    void NotifyContentLoadFailed(const char* reason) const;

    /// Which no-content convention retro_load_game is called with. libretro's is
    /// a null pointer, and that is what RetroArch passes, but it is not safe
    /// everywhere: stock mgba dereferences the argument without checking. A
    /// zeroed struct survives that, at the cost of looking to the core like
    /// content with an empty path. Neither works for every core, so the right
    /// one is measured per core by Tools/bios_boot_probe and carried in the
    /// BiosBoot table. Written from GDScript before StartContent spins the
    /// emulation thread up, exactly like SetPreferredHwRender.
    static void SetNoContentPassesNull(bool passes_null);
    static bool GetNoContentPassesNull();

    uint64_t m_libretro_node_id = 0;

    /// Signal the emulation thread to stop. blocking=true joins and tears down
    /// synchronously (StartContent restart, destructor). blocking=false returns
    /// immediately; _process() joins and finishes the teardown once the thread
    /// has exited on its own, so stopping a core never hitches the main thread.
    /// Also the only safe form callable FROM the emulation thread (core-initiated
    /// RETRO_ENVIRONMENT_SHUTDOWN must not join itself).
    void StopEmulationThread(bool blocking = true);
    /// Post-join teardown: handler DeInit, core unload, member reset. Main thread.
    void FinishTeardown();
    void EmulationThreadLoop();
    void CreateTexture(godot::Image::Format image_format, godot::PackedByteArray pixel_data, int32_t width, int32_t height, bool flip_y);
    void UpdateTexture(godot::PackedByteArray pixel_data, int32_t width, int32_t height, bool flip_y);

    bool Shutdown();

    static void LedInterfaceSetLedState(int32_t led, int32_t state);
};
}
