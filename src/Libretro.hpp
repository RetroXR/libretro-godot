#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <memory>
#include <string>
#include <unordered_map>

#include "OptionsHandler.hpp"

namespace Xenu
{
class LibretroOptionCategory : public godot::RefCounted
{
    GDCLASS(LibretroOptionCategory, godot::RefCounted);

    friend class Libretro;

public:
    const godot::String& GetDescription() const { return m_desc; }
    const godot::String& GetInfo() const { return m_info; }

private:
    godot::String m_desc;
    godot::String m_info;

protected:
    static void _bind_methods()
    {
        godot::ClassDB::bind_method(godot::D_METHOD("GetDescription"), &LibretroOptionCategory::GetDescription);
        godot::ClassDB::bind_method(godot::D_METHOD("GetInfo"), &LibretroOptionCategory::GetInfo);
    }
};

class LibretroOptionValue : public godot::RefCounted
{
    GDCLASS(LibretroOptionValue, godot::RefCounted);

    friend class Libretro;

public:
    const godot::String& GetValue() const { return m_value; }
    const godot::String& GetLabel() const { return m_label; }

private:
    godot::String m_value;
    godot::String m_label;

protected:
    static void _bind_methods()
    {
        godot::ClassDB::bind_method(godot::D_METHOD("GetValue"), &LibretroOptionValue::GetValue);
        godot::ClassDB::bind_method(godot::D_METHOD("GetLabel"), &LibretroOptionValue::GetLabel);
    }
};

class LibretroOptionDefinition : public godot::RefCounted
{
    GDCLASS(LibretroOptionDefinition, godot::RefCounted);

    friend class Libretro;

public:
    const godot::String& GetDescription() const { return m_desc; }
    const godot::String& GetDescriptionCategorized() const { return m_desc_categorized; }
    const godot::String& GetInfo() const { return m_info; }
    const godot::String& GetInfoCategorized() const { return m_info_categorized; }
    const godot::String& GetCategoryKey() const { return m_category_key; }
    const godot::Array& GetValues() const { return m_values; }
    const godot::String& GetDefaultValue() const { return m_default_value; }

private:
    godot::String m_desc;
    godot::String m_desc_categorized;
    godot::String m_info;
    godot::String m_info_categorized;
    godot::String m_category_key;
    godot::Array m_values;
    godot::String m_default_value;
    
protected:
    static void _bind_methods()
    {
        godot::ClassDB::bind_method(godot::D_METHOD("GetDescription"), &LibretroOptionDefinition::GetDescription);
        godot::ClassDB::bind_method(godot::D_METHOD("GetDescriptionCategorized"), &LibretroOptionDefinition::GetDescriptionCategorized);
        godot::ClassDB::bind_method(godot::D_METHOD("GetInfo"), &LibretroOptionDefinition::GetInfo);
        godot::ClassDB::bind_method(godot::D_METHOD("GetInfoCategorized"), &LibretroOptionDefinition::GetInfoCategorized);
        godot::ClassDB::bind_method(godot::D_METHOD("GetCategoryKey"), &LibretroOptionDefinition::GetCategoryKey);
        godot::ClassDB::bind_method(godot::D_METHOD("GetValues"), &LibretroOptionDefinition::GetValues);
        godot::ClassDB::bind_method(godot::D_METHOD("GetDefaultValue"), &LibretroOptionDefinition::GetDefaultValue);
    }
};

class Wrapper;

// Node3D, not Node: the AudioStreamPlayer3D created per-instance in
// Wrapper::StartContent is parented here, and a Node3D under a plain Node roots
// its own transform hierarchy, so the emitter would sit at the world origin and
// never follow the cabinet/handheld when it is picked up and carried around.
class Libretro : public godot::Node3D
{
    GDCLASS(Libretro, godot::Node3D);

    friend class Wrapper;
    
public:
    Libretro();
    ~Libretro();

    void StartContent(godot::String root_directory, godot::String core_name, godot::String game_path);

    /// Start a libretro SUBSYSTEM load: multi-file content a core takes as one
    /// unit (an N64 cartridge with its 64DD disk, a linked Game Boy pair, a
    /// Satellaview broadcast with its host cartridge).
    ///
    /// game_path keeps exactly the meaning it has in StartContent: it is the
    /// identity path, and saves, save states, netplay hashing and achievements
    /// all key off it. subsystem_paths is a SEPARATE ordered list, in the order
    /// the core declared its roms, used for nothing but the load call itself.
    ///
    /// The ident and the file count are checked against what the core publishes,
    /// which is only known once the core is already loading -- so a mismatch
    /// arrives as a content load failure, not as a return value here.
    void StartSubsystemContent(godot::String root_directory, godot::String core_name, godot::String game_path,
                               godot::String subsystem_ident, const godot::PackedStringArray& subsystem_paths);
    void StopContent();

    // Link cable.
    // Seating a cable is what joins two machines; a core attaches its serial
    // hardware to the bus on its own, but what it is wired to is the room's
    // decision, not the core's.

    /// Cable `port` on this machine to `port` on `other`.
    ///
    /// Neither machine has to be running. A cable seated into a console that is
    /// switched off is an ordinary thing to do, and the link simply comes alive
    /// when both cores attach their serial hardware to the bus.
    ///
    /// False for a missing or self target, or when the two ends have already
    /// attached and named different link protocols.
    bool LinkConnect(Libretro* other, uint32_t port, uint32_t other_port);
    /// Pull the cable out of `port`. Peers go unbounded immediately, which for
    /// a guest mid-transfer looks like the cable being yanked, because it is.
    void LinkDisconnect(uint32_t port);
    /// Put this machine and every other on `others` on one wire.
    ///
    /// `others` is an Array of Libretro nodes, and `ports` the matching link
    /// port on each, this machine's own port first. The general form of
    /// LinkConnect, for when a chain of cables and junctions has joined three
    /// or four machines rather than two.
    bool LinkConnectGroup(const godot::Array& others, const godot::PackedInt32Array& ports);
    /// Capture/restore the deterministic state outside the linked cores'
    /// savestates. Used when a netplay peer joins a running linked session.
    godot::Array LinkCaptureGroup(const godot::Array& others,
                                  const godot::PackedInt32Array& ports);
    bool LinkRestoreGroup(const godot::Array& others,
                          const godot::PackedInt32Array& ports,
                          const godot::Array& states);
    /// How many machines are attached to `port`'s bus, this one included.
    ///
    /// 0 when nothing is cabled to it, or when the core has not attached its
    /// serial hardware yet. Live rather than cached: the room decides what a
    /// port is wired to, and a cable can be pulled while the machine runs.
    uint32_t LinkPeerCount(uint32_t port);
    /// Messages this machine has taken off the link. A peer count proves the
    /// cable is there; this proves the guests are actually talking over it.
    uint64_t LinkTraffic(uint32_t port);
    /// Messages this machine has put ON the link, counted where they are
    /// accepted rather than where they arrive. Separating the two is what tells
    /// a core that never ran from a message that never got delivered.
    uint64_t LinkSent(uint32_t port);

    /// Ask to track RetroAchievements for the content about to be started.
    /// `console_id` is an RC_CONSOLE_* value (see RaConsoles.for_systemid); 0 means
    /// the system has no RetroAchievements equivalent. Call BEFORE StartContent,
    /// since identification happens as the core comes up. False when another machine
    /// already holds the session, nobody is signed in, or achievements are off.
    bool RaClaimSession(int console_id);
    /// True while this node's core is the one being tracked.
    bool RaHoldsSession() const;

    /// The running core's picture, for a display that samples it rather than being
    /// painted into — the television reads this to put the machine on its glass.
    /// Null before the first frame, and a NEW object whenever the core changes
    /// resolution, so read it per frame instead of caching it.
    godot::Ref<godot::ImageTexture> GetVideoTexture() const;

    /// Whether the core asked for the controller display interface, which it
    /// uses to hand device screens such as a VMU's LCD over separately.
    bool HasControllerScreens() const;
    /// The screen of device `index` on controller `port` -- for a Dreamcast, the
    /// card in expansion slot `index` -- or null until the core has pushed one.
    godot::Ref<godot::ImageTexture> GetControllerScreenTexture(int port, int index);

    /// The same frame as a CPU-side Image — the buffer the texture was uploaded
    /// FROM, so reading it costs no GPU readback. A savestate thumbnail wants
    /// this and never get_image() on the texture, which syncs the GPU.
    ///
    /// duplicate() it before handing it anywhere that outlives this frame: the
    /// core writes the next frame straight into these same pixels.
    godot::Ref<godot::Image> GetVideoImage() const;

    /// Whether this machine's sound is heard. A console wired to nothing is
    /// silent; it used to be silenced as a side effect of having no screen mesh.
    void SetAudioPlaying(bool playing);
    void SetCoreOption(const godot::String& key, const godot::String& value);
    void SetInputEnabled(bool enabled);

    /// Which hardware API GET_PREFERRED_HW_RENDER advertises, as a
    /// retro_hw_context_type (6 = Vulkan, 7 = D3D11, 9 = D3D12). Applies to
    /// every core started afterwards, so set it before StartContent.
    static void SetPreferredHwRender(int context_type);

    /// Which convention a no-content start hands retro_load_game: true for a
    /// null pointer (libretro's own, and RetroArch's), false for a zeroed
    /// retro_game_info. Measured per core -- see Wrapper::SetNoContentPassesNull.
    /// Must be set before StartContent; it is read as the core loads.
    static void SetNoContentPassesNull(bool passes_null);


    /// Returns per-port controller info as Array[Dictionary{port, controllers: Array[{name,id}], current_id}].
    godot::Array GetControllerInfo();
    /// Meta XR Audio voice ids this core's sound is spatialized through, or
    /// empty when it is running on the fallback AudioStreamPlayer3D.
    godot::PackedInt32Array GetAudioVoiceIds();
    /// The Meta XR Audio voice a controller's own sound plays on -- device `index`
    /// on `port`, index 0 being a Wii Remote's speaker -- or -1 until that device
    /// has made a sound, and always -1 on the fallback backend.
    int GetControllerAudioVoiceId(int port, int index);

    /// True once the audio sink is up and a backend has actually been chosen.
    ///
    /// Until then GetAudioVoiceIds() answers empty for a reason that cannot be
    /// told apart from the fallback backend's permanent empty, because
    /// StartContent only ENQUEUES the audio init -- Wrapper posts a
    /// ThreadCommandInitAudio that the node drains in a later _process. A caller
    /// binding on that empty is guessing, and how long it has to wait is the
    /// core's load time: a real Dolphin took EIGHT SECONDS to answer.
    bool IsAudioReady() const;

    /// 0 stereo, 1 the left channel to both speakers, 2 the right to both.
    void SetAudioChannelMode(int mode);
    /// Decode the stereo pair into six placed channels. Returns what is
    /// actually engaged, which is false wherever the decoder or the voices are
    /// unavailable — the stereo path then goes on working unchanged.
    bool SetSurroundEnabled(bool on);
    /// Send the decoded six to the output device's own speakers as well, through
    /// a 6 x 8 gain matrix; empty stops it. See AudioHandler::SetSurroundDiscrete.
    bool SetSurroundDiscrete(const godot::PackedFloat32Array& matrix);

    /// Tell the running core which device type is active on a given port.
    void SetControllerPortDevice(int port, int device);

    /// Light gun input. Called from GDScript each frame when the gun is plugged in.
    void SetLightgunPosition(int port, int x, int y);
    void SetLightgunIsOffscreen(int port, bool offscreen);
    void SetLightgunButton(int port, int button_id, bool pressed);

    /// Per-port joypad input. Called from GDScript by physical retro controller objects.
    void SetJoypadState(int port, int button_mask, int analog_lx, int analog_ly, int analog_rx, int analog_ry);
    /// Non-consuming view of the buffered joypad state:
    /// [buttons, left_x, left_y, right_x, right_y].
    godot::PackedInt32Array PeekJoypadState(int port) const;
    /// Relative mouse deltas (accumulated until the core's next read) + button
    /// bitmask of (1 << RETRO_DEVICE_ID_MOUSE_*) for a RETRO_DEVICE_MOUSE port.
    void SetMouseState(int port, int dx, int dy, int buttons);
    /// Keyboard: RETROK_* keycode down/up + unicode character. Updates the
    /// RETRO_DEVICE_KEYBOARD poll bitset and fires the core's keyboard event
    /// callback (modifiers derived from held keys). Feed port 0.
    void SetKeyState(int port, int keycode, bool down, int character);
    /// True while a physical RetroKeyboard object owns the OS-keyboard feed.
    /// Godot key event -> RETROK_* code. The application decides when to send a
    /// key (retro_keyboard.gd); this is only the lookup table.
    int GodotKeyToRetroKey(const godot::Ref<godot::InputEventKey>& event) const;

    /// Accelerometer feed for tilt-sensor games (g units, at-rest ≈ (0,0,1)).
    /// Fed each frame from a held handheld's physical orientation.
    ///
    /// `index` names the sub-device on that port and defaults to 0, the
    /// controller itself. A Wii Remote sends its Nunchuk's motion on 1, which
    /// is how one player on one port carries two accelerometers.
    void SetSensorAccel(int port, float x, float y, float z, int index = 0);

    /// Gyroscope feed in radians/second about the device's own axes, fed each
    /// frame from how fast a held handheld is turning. This is what drives the
    /// Wii MotionPlus; a device sitting still reads (0, 0, 0).
    void SetSensorGyro(int port, float x, float y, float z, int index = 0);

    /// Touch/pointer feed (RETRO_DEVICE_POINTER): x/y in [-32767, 32767]
    /// across the whole video output. Drives the DS/3DS touch screen.
    /// Writes touch index 0 and drops any other index, so a single-point caller
    /// never inherits leftovers from SetPointerIndexState.
    void SetPointerState(int port, int x, int y, bool pressed);

    /// One touch index of the pointer device (0-3), for cores that read more than
    /// the first. Dolphin's Wiimote IR passthrough is the reason this exists: it
    /// takes one camera object per index, so the frontend can hand it the real
    /// view of the sensor bar instead of a cursor position. Same [-32767, 32767]
    /// range; that core wants the positive half, 0 = 0.0 and 32767 = 1.0.
    /// Unlike SetPointerState this leaves the other indices alone, so send every
    /// index you own each frame, visible or not.
    void SetPointerIndexState(int port, int index, int x, int y, bool pressed);

    /// Button bits ORed into `port`'s joypad reads on top of whatever its
    /// controller writes, for a button that belongs to a peripheral. 0 releases.
    void SetJoypadExtraButtons(int port, int buttons);

    /// Captured host-microphone frames (stereo, [-1, 1]) at `source_rate`, fed to
    /// every microphone this core has switched on and resampled to each one's
    /// rate. `gain` scales them first.
    void PushMicrophoneFrames(const godot::PackedVector2Array& frames, double source_rate, double gain = 1.0);
    /// True while the core has a microphone open and switched on, and the machine
    /// is running and not in netplay.
    bool IsMicrophoneActive() const;
    /// (rms, peak) of the same frames, high-passed and mono-mixed, scaled by
    /// `gain`. Static on purpose: a machine wanting a level need not have a
    /// microphone open in its core -- a Famicom's Controller II is a bit on a
    /// pad, not a capture device -- and the one reader measures once for all of
    /// them.
    static godot::Vector2 MeasureMicrophoneLevel(const godot::PackedVector2Array& frames,
                                                 double source_rate, double gain = 1.0);

    /// Install the Wii System Menu into a Dolphin NAND through the core's own
    /// online system update (the fork's `retroxr_wii_system_update` export), so
    /// the Wii common key the install decrypts with stays inside Dolphin.
    /// BLOCKING for minutes: call it from a worker thread. `progress` is called
    /// on that thread as (processed, total, title_id_hex) and returns false to
    /// cancel once the current title is in. Returns Dolphin's
    /// WiiUtils::UpdateResult (0 installed, 1 already up to date), or -1 when
    /// the core cannot be opened and -2 when it is too old to have the export.
    /// Opens the core in place, like PeekCoreOptions.
    static int32_t RunWiiSystemUpdate(const godot::String& root_directory, const godot::String& core_name,
                                      const godot::String& user_dir, const godot::String& sys_dir,
                                      const godot::String& region, const godot::Callable& progress);

    // ── Netplay (deterministic lockstep) ─────────────────────────────────────
    /// Gate the emulation loop: frame N runs only once PostNetplayInputs(N,…)
    /// arrived. port_mask selects participating ports; start_frame resets the
    /// frame counter. Call before StartContent for a cold start.
    void SetNetplayMode(bool enabled, int port_mask, int64_t start_frame);
    /// Agreed inputs for one frame: 4 device-aware five-int port blocks,
    /// followed by per-port accelerometer/gyro/pointer state and keyboard
    /// events. In rollback mode these are CONFIRMATIONS: a mismatch with what
    /// already ran triggers rewind+replay.
    void PostNetplayInputs(int64_t frame, const godot::PackedInt32Array& inputs);

    /// Enable GGPO-style rollback within netplay mode: locally-owned ports
    /// (local_mask) apply live with zero delay, remote ports are predicted and
    /// corrected via invisible rewind+replay. max_ahead caps speculation.
    void SetNetplayRollback(bool enabled, int local_mask, int max_ahead);
    bool ScheduleNetplayLocalMask(int64_t frame, int local_mask);

    /// Drain per-frame local-input records: flat groups of 7 ints
    /// {frame, port, buttons, alx, aly, arx, ary}: what this peer actually
    /// pressed each frame, to be shipped to the host for assembly.
    godot::PackedInt32Array TakeNetplayLocalRecords();
    /// Async savestate → savestate_ready(data: PackedByteArray, frame: int).
    void RequestSaveState();
    /// Async state load + schedule reset → savestate_loaded(ok: bool).
    void RequestLoadState(const godot::PackedByteArray& data, int64_t frame);
    // ── Battery saves (SRAM) ─────────────────────────────────────────────────
    /// Backing .srm file for this run. Call before StartContent; while running
    /// it hot-swaps (flush old, load new): a physical memory-card swap.
    /// Empty path = no persistence (PSX with no card seated).
    void SetSramPath(const godot::String& path);
    /// The file a writable CONTENT medium is written back to -- a BS-X memory
    /// pack, whose .bs IS the flash the core mutates. Unlike SetSramPath there
    /// is no matching load: the content load already supplied the bytes.
    void SetPackPath(const godot::String& path);
    /// The second save region's file, on a machine that carries two: a Sufami
    /// Turbo's B cartridge or a PlayStation's slot-2 memory card. `memory_id` is
    /// the core's own id for that region -- SRAM_B_SUFAMI_TURBO or
    /// SRAM_B_PCSX_MEMCARD2. See Wrapper::SetSramBPath for why it is a separate
    /// file.
    void SetSramBPath(const godot::String& path, int64_t memory_id);
    /// The cartridge's real-time clock file, read at the next content load. ""
    /// keeps no clock. See Wrapper::SetRtcPath.
    void SetRtcPath(const godot::String& path);

    /// Bind one Controller Pak's 32 KiB slice of the single SAVE_RAM block both
    /// N64 cores publish, to a file of its own. `index` is the libretro port.
    void SetSramRegionPath(int index, const godot::String& path, int64_t offset, int64_t length);
    void ClearSramRegion(int index);

    /// The Game Boy cartridge in the Transfer Pak on one controller's port, and
    /// where that cartridge's battery lives. Per PORT, unlike the `gb` subsystem
    /// and the sidecar, which share one cartridge across all four paks.
    void SetTransferPak(int port, const godot::String& rom_path, const godot::String& ram_path);
    /// Where that cartridge's real-time clock is kept. Set before SetTransferPak.
    void SetTransferPakClock(int port, const godot::String& rtc_path);
    void ClearTransferPak(int port);
    /// Netplay: inject exact SRAM bytes applied at load instead of the file.
    void SetSramData(const godot::PackedByteArray& data);
    /// This machine saves to REMOVABLE media (a PSX memory card). With nothing
    /// seated, SAVE_RAM is blanked so the game reports unformatted media rather
    /// than accepting a save the core invented and losing it at power-off.
    void SetRemovableStorage(bool removable);
    /// Force a dirty-check flush of SRAM to its backing file now.
    void RequestSramFlush();
    /// Front-panel reset (retro_reset), applied between frames. The core stays
    /// loaded, so the screen, audio voices and port bindings all survive.
    void RequestReset();
    /// Netplay reset applied before one agreed emulated frame on every peer.
    void ScheduleReset(int64_t frame);
    // ── Disk control (multi-disc games) ──────────────────────────────────────
    /// Query the core's disk-control state → disk_control_ready(has_control,
    /// count, current_index, ejected). Emits has_control=false when the core
    /// lacks the interface or nothing is running.
    void RequestDiskInfo();
    /// Open (true) / close (false) the core's virtual disc tray.
    void SetDiskEjectState(bool ejected);
    /// Hand the core a new disc file at image `index` (tray must be open).
    void ReplaceDiskImage(int64_t index, const godot::String& path);
    /// Netplay: apply a link-cable change right before running `frame` on every
    /// peer, so a plug seated mid-game joins the bus on one agreed frame rather
    /// than whenever each peer's hands happened to move. Same shape as
    /// LinkConnectGroup - one port per machine, this one first - with op 1 to
    /// join and op 0 to drop this machine's port.
    void ScheduleLinkOp(int64_t frame, int64_t op, const godot::Array& others,
                        const godot::PackedInt32Array& ports);

    /// Netplay: apply a disc op right before running confirmed `frame` on every peer.
    /// op 0 = eject; op 1 = replace at `index` with `path` + close tray.
    void ScheduleDiscOp(int64_t frame, int64_t op, int64_t index, const godot::String& path);

    /// Diagnostic: the bytes the netplay CRC hashes, plus region boundaries.
    godot::Dictionary SnapshotMappedRam() const;

    /// How often a netplay RAM CRC is emitted, in frames. See Wrapper.
    void SetNetplayCrcInterval(int64_t frames);
    /// Take the desync CRC from a savestate rather than live RAM, for a core
    /// whose RAM cannot be read coherently between frames.
    void SetNetplayCrcFromState(bool from_state);

    /// Frames executed since content start (or since the last state load).
    int64_t GetFrameCount() const;

    /// Who the running core says it is: library_name, library_version,
    /// api_version, serialize_size. EMPTY until the core has finished loading
    /// content, and empty again after it stops. Netplay compares this across
    /// peers, because it is the one build identity that means the same thing on
    /// Windows, Linux, macOS and a Quest. The core FILE never matches there.
    godot::Dictionary GetCoreIdentity() const;

    /// Rewind+replay corrections performed so far (rollback diagnostics).
    int64_t GetNetplayRollbackCount() const;
    godot::Dictionary GetNetplayRollbackStats() const;

    /// Performance HUD readings. Every one is a relaxed atomic load, so they are
    /// safe to poll from _process on any number of instances at once — each node
    /// answers only for its own core.
    ///
    /// The core's declared timing, and how far behind the picture is:
    double GetDeclaredFps() const;
    double GetDeclaredSampleRate() const;
    /// Frames produced but never shown, because a newer one replaced them before
    /// the main thread drained the queue. Resets with each content run.
    int64_t GetDroppedFrameCount() const;
    /// Audio sink fill 0-100 (<= 10 is what the core is told is an underrun), and
    /// how long the pacing loop is holding the core back. A brake pinned at 0 with
    /// a low fill is a core that cannot keep up.
    int64_t GetAudioBufferOccupancy() const;
    double GetAudioBrakeMs() const;

    void ConnectOptionsReady(const godot::Callable& callable, uint32_t flags = 0u);

    void _exit_tree() override;
    void _process(double delta) override;

    /// Called from the emulation thread (via Wrapper::LiveLibretroNode) when options are ready.
    void NotifyOptionsReady();

    /// Called from the emulation thread after SAVE_RAM was actually written to
    /// disk, i.e. only when the dirty check found a change. `final` marks the
    /// flush at core shutdown, the last one for this run.
    void NotifySramFlushed(const godot::String& path, int64_t size, bool final_flush);
    /// A lamp on the emulated machine changed. Cores report these through
    /// RETRO_ENVIRONMENT_GET_LED_INTERFACE; the Satellaview's ACCESS lamp is the
    /// one this project draws. `led` is the core's own index.
    void NotifyLedState(int32_t led, bool on);

    /// Called from the emulation thread when the run could not start: the core
    /// would not load, the file was unreadable, or retro_load_game refused.
    /// Without this a refused load raises nothing at all and the machine sits
    /// powered on and black, indistinguishable from a broken core or a dead TV.
    void NotifyContentLoadFailed(const godot::String& reason);

    /// Read a core's option set without starting it, for menus that let the
    /// player set options before launch. Returns a dictionary shaped like the
    /// options_ready signal ("categories", "definitions", "values"), with each
    /// option sitting at its core-declared default. Empty if the core could not
    /// be read. Blocking, but only as long as a dlopen plus the core's static
    /// initializers; no emulation is started.
    godot::Dictionary PeekCoreOptions(const godot::String& root_directory, const godot::String& core_name);

private:
    /// Hand a Wrapper whose core will not stop to a store that outlives this node,
    /// and take a fresh one. The abandoned thread is still running inside it, so it
    /// can be neither joined nor destroyed; leaking it is the only way out that is
    /// not a hang or a use-after-free.
    void AbandonWrapper();

    std::unique_ptr<Wrapper> m_wrapper;

    /// Seconds since the last rc_client_idle. Every node runs this, so the tick
    /// costs one mutexed no-op per node per second when nothing is signed in.
    double m_ra_idle_accumulator = 0.0;

    static godot::Dictionary ConvertOptionCategories(const std::unordered_map<std::string, OptionCategory>& categories);
    static godot::Dictionary ConvertOptionDefinitions(const std::unordered_map<std::string, OptionDefinition>& definitions);
    static godot::Dictionary ConvertOptionValues(const std::unordered_map<std::string, std::string>& values);

    static void _bind_methods();
};
}
