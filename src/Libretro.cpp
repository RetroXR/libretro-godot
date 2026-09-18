#include "Libretro.hpp"

#include "LinkCoordinator.hpp"
#include "Wrapper.hpp"
#include "AudioHandler.hpp"
#include "CoreOptionsPeek.hpp"
#include "RetroAchievements.hpp"
#include "MicrophoneLevel.hpp"

#include <filesystem>
#include <utility>
#include <vector>

using namespace godot;

namespace Xenu
{
namespace
{
/// How long a restart or a teardown waits for a core to leave retro_run before
/// giving up on it. Long enough for any core that unwinds at all; short enough
/// that a wedged one costs a hitch rather than the application.
constexpr uint32_t kStopBudgetMs = 2000;

/// Never freed, deliberately: each entry still has a live emulation thread
/// inside it.
std::vector<std::unique_ptr<Wrapper>>& AbandonedWrappers()
{
    static auto* graveyard = new std::vector<std::unique_ptr<Wrapper>>();
    return *graveyard;
}
}

Libretro::Libretro()
{
    m_wrapper = std::make_unique<Wrapper>();
    m_wrapper->m_libretro_node_id = static_cast<uint64_t>(get_instance_id());
}

Libretro::~Libretro() = default;

void Libretro::AbandonWrapper()
{
    LogError("Core did not stop within " + std::to_string(kStopBudgetMs) +
             " ms; abandoning it. Its memory, its RetroAchievements session and "
             "its Meta XR voices stay claimed until the application exits.");
    m_wrapper->AbandonThread();
    AbandonedWrappers().push_back(std::move(m_wrapper));
    m_wrapper = std::make_unique<Wrapper>();
    m_wrapper->m_libretro_node_id = static_cast<uint64_t>(get_instance_id());
}

void Libretro::ConnectOptionsReady(const godot::Callable& callable, uint32_t flags)
{
    connect("options_ready", callable, flags);
}

void Libretro::StartContent(String root_directory, String core_name, String game_path)
{
    // The previous run has to be gone before this one starts: it owns the handlers
    // the new core would reuse. Bounded, because a core that will not unwind would
    // otherwise hang the caller (a reset, a netplay restart) forever.
    if (!m_wrapper->StopEmulationThreadBounded(kStopBudgetMs))
        AbandonWrapper();
    m_wrapper->StartContent(root_directory.utf8().get_data(), core_name.utf8().get_data(), game_path.utf8().get_data());
}

void Libretro::StartSubsystemContent(String root_directory, String core_name, String game_path,
                                     String subsystem_ident, const PackedStringArray& subsystem_paths)
{
    // Same teardown contract as StartContent: the previous run owns the handlers
    // this one would reuse.
    if (!m_wrapper->StopEmulationThreadBounded(kStopBudgetMs))
        AbandonWrapper();

    std::vector<std::string> paths;
    paths.reserve(static_cast<size_t>(subsystem_paths.size()));
    for (int i = 0; i < subsystem_paths.size(); ++i)
        paths.emplace_back(subsystem_paths[i].utf8().get_data());

    m_wrapper->StartSubsystemContent(root_directory.utf8().get_data(), core_name.utf8().get_data(),
                                     game_path.utf8().get_data(), subsystem_ident.utf8().get_data(), paths);
}

void Libretro::StopContent()
{
    m_wrapper->StopContent();
}

Ref<ImageTexture> Libretro::GetVideoTexture() const
{
    return m_wrapper ? m_wrapper->GetVideoTexture() : Ref<ImageTexture>();
}

bool Libretro::HasControllerScreens() const
{
    return m_wrapper ? m_wrapper->HasControllerScreens() : false;
}

Ref<ImageTexture> Libretro::GetControllerScreenTexture(int port, int index)
{
    return m_wrapper ? m_wrapper->GetControllerScreenTexture(port, index) : Ref<ImageTexture>();
}

Ref<Image> Libretro::GetVideoImage() const
{
    return m_wrapper ? m_wrapper->GetVideoImage() : Ref<Image>();
}

void Libretro::SetAudioPlaying(bool playing)
{
    if (m_wrapper)
        m_wrapper->SetAudioPlaying(playing);
}

void Libretro::SetCoreOption(const godot::String& key, const godot::String& value)
{
    m_wrapper->SetCoreOption(key.utf8().get_data(), value.utf8().get_data());
}

void Libretro::SetInputEnabled(bool enabled)
{
    m_wrapper->m_input_enabled = enabled;
}

void Libretro::SetNoContentPassesNull(bool passes_null)
{
    Wrapper::SetNoContentPassesNull(passes_null);
}

void Libretro::SetPreferredHwRender(int context_type)
{
    VideoHandler::SetPreferredHwRender(static_cast<retro_hw_context_type>(context_type));
}

godot::PackedInt32Array Libretro::GetAudioVoiceIds()
{
    if (!m_wrapper || !m_wrapper->m_audio_handler)
        return godot::PackedInt32Array();
    return m_wrapper->m_audio_handler->GetVoiceIds();
}

int Libretro::GetControllerAudioVoiceId(int port, int index)
{
    if (!m_wrapper || !m_wrapper->m_audio_handler || port < 0 || index < 0)
        return -1;
    return m_wrapper->m_audio_handler->GetControllerVoiceId(static_cast<unsigned>(port), static_cast<unsigned>(index));
}

bool Libretro::IsAudioReady() const
{
    if (!m_wrapper || !m_wrapper->m_audio_handler)
        return false;
    return m_wrapper->m_audio_handler->IsReady();
}

void Libretro::SetAudioChannelMode(int mode)
{
    if (!m_wrapper || !m_wrapper->m_audio_handler)
        return;
    m_wrapper->m_audio_handler->SetChannelMode(mode);
}

bool Libretro::SetSurroundEnabled(bool on)
{
    if (!m_wrapper || !m_wrapper->m_audio_handler)
        return false;
    return m_wrapper->m_audio_handler->SetSurroundEnabled(on);
}

godot::Array Libretro::GetControllerInfo()
{
    return m_wrapper->GetControllerInfo();
}

void Libretro::SetControllerPortDevice(int port, int device)
{
    m_wrapper->SetControllerPortDevice(static_cast<uint32_t>(port), static_cast<uint32_t>(device));
}

void Libretro::SetLightgunPosition(int port, int x, int y)
{
    m_wrapper->SetLightgunPosition(static_cast<uint32_t>(port), static_cast<int16_t>(x), static_cast<int16_t>(y));
}

void Libretro::SetLightgunIsOffscreen(int port, bool offscreen)
{
    m_wrapper->SetLightgunIsOffscreen(static_cast<uint32_t>(port), offscreen);
}

void Libretro::SetLightgunButton(int port, int button_id, bool pressed)
{
    m_wrapper->SetLightgunButton(static_cast<uint32_t>(port), button_id, pressed);
}

void Libretro::SetJoypadState(int port, int button_mask, int analog_lx, int analog_ly, int analog_rx, int analog_ry)
{
    m_wrapper->SetJoypadState(
        static_cast<uint32_t>(port),
        static_cast<uint16_t>(button_mask),
        static_cast<int16_t>(analog_lx),
        static_cast<int16_t>(analog_ly),
        static_cast<int16_t>(analog_rx),
        static_cast<int16_t>(analog_ry));
}

godot::PackedInt32Array Libretro::PeekJoypadState(int port) const
{
    return m_wrapper->PeekJoypadState(static_cast<uint32_t>(port));
}

void Libretro::SetMouseState(int port, int dx, int dy, int buttons)
{
    m_wrapper->SetMouseState(static_cast<uint32_t>(port),
        static_cast<int32_t>(dx), static_cast<int32_t>(dy),
        static_cast<uint32_t>(buttons));
}

void Libretro::SetKeyState(int port, int keycode, bool down, int character)
{
    m_wrapper->SetKeyState(static_cast<uint32_t>(port),
        static_cast<uint32_t>(keycode), down, static_cast<uint32_t>(character));
}

int Libretro::GodotKeyToRetroKey(const Ref<InputEventKey>& event) const
{
    if (event.is_null())
        return 0;
    return static_cast<int>(Wrapper::GodotKeyToRetroKey(event));
}

void Libretro::SetSensorAccel(int port, float x, float y, float z, int index)
{
    m_wrapper->SetSensorAccel(static_cast<uint32_t>(port), x, y, z,
                              static_cast<uint32_t>(index));
}

void Libretro::SetSensorGyro(int port, float x, float y, float z, int index)
{
    m_wrapper->SetSensorGyro(static_cast<uint32_t>(port), x, y, z,
                             static_cast<uint32_t>(index));
}

void Libretro::SetJoypadExtraButtons(int port, int buttons)
{
    m_wrapper->SetJoypadExtraButtons(static_cast<uint32_t>(port), static_cast<uint16_t>(buttons));
}

void Libretro::PushMicrophoneFrames(const godot::PackedVector2Array& frames, double source_rate, double gain)
{
    m_wrapper->PushMicrophoneFrames(frames, source_rate, static_cast<float>(gain));
}

bool Libretro::IsMicrophoneActive() const
{
    return m_wrapper && m_wrapper->IsMicrophoneActive();
}

Vector2 Libretro::MeasureMicrophoneLevel(const godot::PackedVector2Array& frames,
                                         double source_rate, double gain)
{
    static_assert(sizeof(Vector2) == 2 * sizeof(float), "Vector2 must be a pair of floats");
    if (frames.is_empty())
        return Vector2();
    const MicrophoneLevel level = MicrophoneLevelMeter::Measure(
        reinterpret_cast<const float*>(frames.ptr()), static_cast<size_t>(frames.size()),
        source_rate, static_cast<float>(gain));
    return Vector2(level.rms, level.peak);
}

void Libretro::SetPointerState(int port, int x, int y, bool pressed)
{
    m_wrapper->SetPointerState(static_cast<uint32_t>(port),
        static_cast<int16_t>(x), static_cast<int16_t>(y), pressed);
}

void Libretro::SetPointerIndexState(int port, int index, int x, int y, bool pressed)
{
    m_wrapper->SetPointerIndexState(static_cast<uint32_t>(port), static_cast<uint32_t>(index),
        static_cast<int16_t>(x), static_cast<int16_t>(y), pressed);
}

void Libretro::SetNetplayMode(bool enabled, int port_mask, int64_t start_frame)
{
    m_wrapper->SetNetplayMode(enabled, static_cast<uint32_t>(port_mask), start_frame);
}

void Libretro::PostNetplayInputs(int64_t frame, const godot::PackedInt32Array& inputs)
{
    m_wrapper->PostNetplayInputs(frame, inputs);
}

void Libretro::SetNetplayRollback(bool enabled, int local_mask, int max_ahead)
{
    m_wrapper->SetNetplayRollback(enabled, static_cast<uint32_t>(local_mask), max_ahead);
}

bool Libretro::ScheduleNetplayLocalMask(int64_t frame, int local_mask)
{
    return m_wrapper->ScheduleNetplayLocalMask(frame, static_cast<uint32_t>(local_mask));
}

godot::PackedInt32Array Libretro::TakeNetplayLocalRecords()
{
    return m_wrapper->TakeNetplayLocalRecords();
}

void Libretro::RequestSaveState()
{
    m_wrapper->RequestSaveState();
}

void Libretro::RequestLoadState(const godot::PackedByteArray& data, int64_t frame)
{
    m_wrapper->RequestLoadState(data, frame);
}

void Libretro::SetSramPath(const godot::String& path)
{
    m_wrapper->SetSramPath(path);
}


void Libretro::SetSramBPath(const godot::String& path, int64_t memory_id)
{
    m_wrapper->SetSramBPath(path, static_cast<unsigned>(memory_id));
}

void Libretro::SetRtcPath(const godot::String& path)
{
    m_wrapper->SetRtcPath(path);
}

void Libretro::SetSramRegionPath(int index, const godot::String& path, int64_t offset, int64_t length)
{
    m_wrapper->SetSramRegionPath(index, path, offset, length);
}


void Libretro::ClearSramRegion(int index)
{
    m_wrapper->ClearSramRegion(index);
}


void Libretro::SetTransferPak(int port, const godot::String& rom_path, const godot::String& ram_path)
{
    m_wrapper->SetTransferPak(port, rom_path, ram_path);
}


void Libretro::SetTransferPakClock(int port, const godot::String& rtc_path)
{
    m_wrapper->SetTransferPakClock(port, rtc_path);
}


void Libretro::ClearTransferPak(int port)
{
    m_wrapper->ClearTransferPak(port);
}


void Libretro::SetPackPath(const godot::String& path)
{
    m_wrapper->SetPackPath(path);
}

void Libretro::SetSramData(const godot::PackedByteArray& data)
{
    m_wrapper->SetSramData(data);
}

void Libretro::SetRemovableStorage(bool removable)
{
    m_wrapper->SetRemovableStorage(removable);
}

void Libretro::RequestSramFlush()
{
    m_wrapper->RequestSramFlush();
}

void Libretro::RequestReset()
{
    m_wrapper->RequestReset();
}

void Libretro::ScheduleReset(int64_t frame)
{
    m_wrapper->ScheduleReset(frame);
}

void Libretro::RequestDiskInfo()
{
    m_wrapper->RequestDiskInfo();
}

void Libretro::SetDiskEjectState(bool ejected)
{
    m_wrapper->SetDiskEjectState(ejected);
}

void Libretro::ReplaceDiskImage(int64_t index, const godot::String& path)
{
    m_wrapper->ReplaceDiskImage(static_cast<uint32_t>(index < 0 ? 0 : index), path);
}

void Libretro::ScheduleDiscOp(int64_t frame, int64_t op, int64_t index, const godot::String& path)
{
    m_wrapper->ScheduleDiscOp(frame, static_cast<int32_t>(op),
        static_cast<uint32_t>(index < 0 ? 0 : index), path);
}

Dictionary Libretro::SnapshotMappedRam() const
{
    return m_wrapper ? m_wrapper->SnapshotMappedRam() : Dictionary();
}

void Libretro::SetNetplayCrcInterval(int64_t frames)
{
    if (m_wrapper)
        m_wrapper->SetNetplayCrcInterval(frames);
}

void Libretro::SetNetplayCrcFromState(bool from_state)
{
    if (m_wrapper)
        m_wrapper->SetNetplayCrcFromState(from_state);
}

int64_t Libretro::GetFrameCount() const
{
    return m_wrapper->GetFrameCount();
}

Dictionary Libretro::GetNetplayRollbackStats() const
{
    return m_wrapper ? m_wrapper->GetNetplayRollbackStats() : Dictionary();
}

int64_t Libretro::GetNetplayRollbackCount() const
{
    return m_wrapper->GetNetplayRollbackCount();
}

Dictionary Libretro::GetCoreIdentity() const
{
    return m_wrapper ? m_wrapper->GetCoreIdentity() : Dictionary();
}

double Libretro::GetDeclaredFps() const
{
    return m_wrapper->GetDeclaredFps();
}

double Libretro::GetDeclaredSampleRate() const
{
    return m_wrapper->GetDeclaredSampleRate();
}

int64_t Libretro::GetDroppedFrameCount() const
{
    return m_wrapper->GetDroppedFrameCount();
}

int64_t Libretro::GetAudioBufferOccupancy() const
{
    return static_cast<int64_t>(m_wrapper->GetAudioBufferOccupancy());
}

double Libretro::GetAudioBrakeMs() const
{
    return m_wrapper->GetAudioBrakeMs();
}

void Libretro::_exit_tree()
{
    // Synchronous, unlike StopContent(): leaving the tree means this node can be
    // freed at any point after this returns, and deferring the join to _process()
    // (which will never run again) would leave the teardown to ~Wrapper. The frame
    // hitch does not matter on the way out.
    //
    // Bounded, though. A core that never leaves retro_run cannot be joined, and
    // waiting for it here is what turned a wedged Dolphin into an application
    // that could not even quit itself.
    if (!m_wrapper->ShutdownForExit(kStopBudgetMs))
        AbandonWrapper();
}

bool Libretro::RaClaimSession(int console_id)
{
    RetroAchievements* ra = RetroAchievements::GetSingleton();
    if (ra == nullptr || console_id <= 0)
        return false;
    return ra->ClaimSession(m_wrapper.get(), static_cast<uint32_t>(console_id));
}

bool Libretro::RaHoldsSession() const
{
    RetroAchievements* ra = RetroAchievements::GetSingleton();
    return ra != nullptr && ra->HoldsSession(m_wrapper.get());
}

void Libretro::_process(double delta)
{
    m_wrapper->_process(delta);

    // rc_client's pending queue (unlocks that failed to send, session pings) is
    // serviced by do_frame while a game runs and by idle when one does not. Once a
    // second is what rcheevos asks for; every frame would be wasted work.
    m_ra_idle_accumulator += delta;
    if (m_ra_idle_accumulator >= 1.0)
    {
        m_ra_idle_accumulator = 0.0;
        if (RetroAchievements* ra = RetroAchievements::GetSingleton())
            ra->Idle();
    }
}

void Libretro::NotifyOptionsReady()
{
    auto categories     = ConvertOptionCategories(m_wrapper->GetOptionCategories());
    auto definitions    = ConvertOptionDefinitions(m_wrapper->GetOptionDefinitions());
    auto current_values = ConvertOptionValues(m_wrapper->GetOptionValues());
    call_deferred("emit_signal", "options_ready", categories, definitions, current_values);
}

void Libretro::NotifySramFlushed(const String& path, int64_t size, bool final_flush)
{
    call_deferred("emit_signal", "sram_flushed", path, size, final_flush);
}

void Libretro::NotifyLedState(int32_t led, bool on)
{
    call_deferred("emit_signal", "led_state", static_cast<int64_t>(led), on);
}

void Libretro::NotifyContentLoadFailed(const String& reason)
{
    call_deferred("emit_signal", "content_load_failed", reason);
}

Dictionary Libretro::PeekCoreOptions(const String& root_directory, const String& core_name)
{
    Dictionary result;

    const std::string root = std::string(root_directory.utf8().get_data());
    const std::string core = std::string(core_name.utf8().get_data());
    const std::string core_path = Wrapper::ResolveCorePath(root, core);

    // The same three a real run of this core is given (see Wrapper::StartContent),
    // so a core that decides what to publish from what it finds in them decides it
    // here the way it will decide it at launch.
    Xenu::PeekDirectories directories;
    directories.system_directory      = std::filesystem::path(root).append("system").append(core).string();
    directories.save_directory        = std::filesystem::path(root).append("save").append(core).string();
    directories.core_assets_directory = std::filesystem::path(root).append("core_assets").append(core).string();

    // Qualified: the member function name would otherwise hide the free one.
    OptionsHandler options;
    const std::filesystem::path options_path =
        std::filesystem::path(root) / "core_options" / (core + ".opt");
    // Peeking may read this core's saved choices, but merely opening the menu
    // must not create a defaults file or touch another running instance.
    options.SetPersistencePath(options_path.string(), false);
    if (!Xenu::PeekCoreOptions(core_path, directories, options))
        return result;

    result["categories"]  = ConvertOptionCategories(options.GetCategories());
    result["definitions"] = ConvertOptionDefinitions(options.GetDefinitions());
    result["values"]      = ConvertOptionValues(options.GetValues());
    return result;
}

Dictionary Libretro::ConvertOptionCategories(const std::unordered_map<std::string, OptionCategory>& categories)
{
    Dictionary result;
    for (const auto& [key, value] : categories)
    {
        Ref<LibretroOptionCategory> category = memnew(LibretroOptionCategory);
        category->m_desc = value.desc.c_str();
        category->m_info = value.info.c_str();
        result[String(key.c_str())] = category;
    }
    return result;
}

Dictionary Libretro::ConvertOptionDefinitions(const std::unordered_map<std::string, OptionDefinition>& definitions)
{
    Dictionary result;
    for (const auto& [key, value] : definitions)
    {
        Ref<LibretroOptionDefinition> definition = memnew(LibretroOptionDefinition);
        definition->m_desc = value.desc.c_str();
        definition->m_desc_categorized = value.desc_categorized.c_str();
        definition->m_info = value.info.c_str();
        definition->m_info_categorized = value.info_categorized.c_str();
        definition->m_category_key = value.category_key.c_str();
        definition->m_values = Array();
        for (const auto& val : value.values)
        {
            Ref<LibretroOptionValue> option_value = memnew(LibretroOptionValue);
            option_value->m_value = val.value.c_str();
            option_value->m_label = val.label.c_str();
            definition->m_values.append(option_value);
        }
        definition->m_default_value = value.default_value.c_str();
        result[String(key.c_str())] = definition;
    }
    return result;
}

Dictionary Libretro::ConvertOptionValues(const std::unordered_map<std::string, std::string>& values)
{
    Dictionary result;
    for (const auto& [key, value] : values)
        result[String(key.c_str())] = String(value.c_str());
    return result;
}

void Libretro::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("ConnectOptionsReady", "callable", "flags"), &Libretro::ConnectOptionsReady, DEFVAL(0u));
    ClassDB::bind_method(D_METHOD("StartContent", "root_directory", "core_name", "game_path"), &Libretro::StartContent);
    ClassDB::bind_method(D_METHOD("StartSubsystemContent", "root_directory", "core_name", "game_path", "subsystem_ident", "subsystem_paths"), &Libretro::StartSubsystemContent);
    ClassDB::bind_method(D_METHOD("StopContent"), &Libretro::StopContent);
    ClassDB::bind_method(D_METHOD("LinkConnect", "other", "port", "other_port"), &Libretro::LinkConnect, DEFVAL(0u), DEFVAL(0u));
    ClassDB::bind_method(D_METHOD("LinkDisconnect", "port"), &Libretro::LinkDisconnect, DEFVAL(0u));
    ClassDB::bind_method(D_METHOD("LinkConnectGroup", "others", "ports"), &Libretro::LinkConnectGroup);
    ClassDB::bind_method(D_METHOD("LinkCaptureGroup", "others", "ports"), &Libretro::LinkCaptureGroup);
    ClassDB::bind_method(D_METHOD("LinkRestoreGroup", "others", "ports", "states"), &Libretro::LinkRestoreGroup);
    ClassDB::bind_method(D_METHOD("LinkPeerCount", "port"), &Libretro::LinkPeerCount, DEFVAL(0u));
    ClassDB::bind_method(D_METHOD("LinkTraffic", "port"), &Libretro::LinkTraffic, DEFVAL(0u));
    ClassDB::bind_method(D_METHOD("LinkSent", "port"), &Libretro::LinkSent, DEFVAL(0u));
    ClassDB::bind_method(D_METHOD("GetVideoTexture"), &Libretro::GetVideoTexture);
    ClassDB::bind_method(D_METHOD("HasControllerScreens"), &Libretro::HasControllerScreens);
    ClassDB::bind_method(D_METHOD("GetControllerScreenTexture", "port", "index"), &Libretro::GetControllerScreenTexture, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("GetVideoImage"), &Libretro::GetVideoImage);
    ClassDB::bind_method(D_METHOD("SetAudioPlaying", "playing"), &Libretro::SetAudioPlaying);
    ClassDB::bind_method(D_METHOD("SetCoreOption", "key", "value"), &Libretro::SetCoreOption);
    ClassDB::bind_method(D_METHOD("PeekCoreOptions", "root_directory", "core_name"), &Libretro::PeekCoreOptions);
    ClassDB::bind_method(D_METHOD("SetInputEnabled", "enabled"), &Libretro::SetInputEnabled);
    ClassDB::bind_static_method("Libretro", D_METHOD("SetPreferredHwRender", "context_type"), &Libretro::SetPreferredHwRender);
    ClassDB::bind_static_method("Libretro", D_METHOD("SetNoContentPassesNull", "passes_null"), &Libretro::SetNoContentPassesNull);
    ClassDB::bind_method(D_METHOD("GetControllerInfo"), &Libretro::GetControllerInfo);
    ClassDB::bind_method(D_METHOD("GetAudioVoiceIds"), &Libretro::GetAudioVoiceIds);
    ClassDB::bind_method(D_METHOD("GetControllerAudioVoiceId", "port", "index"), &Libretro::GetControllerAudioVoiceId, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("IsAudioReady"), &Libretro::IsAudioReady);
    ClassDB::bind_method(D_METHOD("SetAudioChannelMode", "mode"), &Libretro::SetAudioChannelMode);
    ClassDB::bind_method(D_METHOD("SetSurroundEnabled", "on"), &Libretro::SetSurroundEnabled);
    ClassDB::bind_method(D_METHOD("SetControllerPortDevice", "port", "device"), &Libretro::SetControllerPortDevice);
    ClassDB::bind_method(D_METHOD("SetLightgunPosition", "port", "x", "y"), &Libretro::SetLightgunPosition);
    ClassDB::bind_method(D_METHOD("SetLightgunIsOffscreen", "port", "offscreen"), &Libretro::SetLightgunIsOffscreen);
    ClassDB::bind_method(D_METHOD("SetLightgunButton", "port", "button_id", "pressed"), &Libretro::SetLightgunButton);
    ClassDB::bind_method(D_METHOD("SetJoypadState", "port", "button_mask", "analog_lx", "analog_ly", "analog_rx", "analog_ry"), &Libretro::SetJoypadState);
    ClassDB::bind_method(D_METHOD("PeekJoypadState", "port"), &Libretro::PeekJoypadState);
    ClassDB::bind_method(D_METHOD("SetMouseState", "port", "dx", "dy", "buttons"), &Libretro::SetMouseState);
    ClassDB::bind_method(D_METHOD("SetKeyState", "port", "keycode", "down", "character"), &Libretro::SetKeyState);
    ClassDB::bind_method(D_METHOD("GodotKeyToRetroKey", "event"), &Libretro::GodotKeyToRetroKey);
    ClassDB::bind_method(D_METHOD("SetSensorAccel", "port", "x", "y", "z", "index"), &Libretro::SetSensorAccel, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("SetSensorGyro", "port", "x", "y", "z", "index"), &Libretro::SetSensorGyro, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("SetPointerState", "port", "x", "y", "pressed"), &Libretro::SetPointerState);
    ClassDB::bind_method(D_METHOD("SetPointerIndexState", "port", "index", "x", "y", "pressed"), &Libretro::SetPointerIndexState);
    ClassDB::bind_method(D_METHOD("SetJoypadExtraButtons", "port", "buttons"), &Libretro::SetJoypadExtraButtons);
    ClassDB::bind_method(D_METHOD("PushMicrophoneFrames", "frames", "source_rate", "gain"), &Libretro::PushMicrophoneFrames, DEFVAL(1.0));
    ClassDB::bind_method(D_METHOD("IsMicrophoneActive"), &Libretro::IsMicrophoneActive);
    ClassDB::bind_static_method("Libretro", D_METHOD("MeasureMicrophoneLevel", "frames", "source_rate", "gain"),
        &Libretro::MeasureMicrophoneLevel, DEFVAL(1.0));
    ClassDB::bind_method(D_METHOD("SetNetplayMode", "enabled", "port_mask", "start_frame"), &Libretro::SetNetplayMode);
    ClassDB::bind_method(D_METHOD("PostNetplayInputs", "frame", "inputs"), &Libretro::PostNetplayInputs);
    ClassDB::bind_method(D_METHOD("SetNetplayRollback", "enabled", "local_mask", "max_ahead"), &Libretro::SetNetplayRollback);
    ClassDB::bind_method(D_METHOD("ScheduleNetplayLocalMask", "frame", "local_mask"), &Libretro::ScheduleNetplayLocalMask);
    ClassDB::bind_method(D_METHOD("TakeNetplayLocalRecords"), &Libretro::TakeNetplayLocalRecords);
    ClassDB::bind_method(D_METHOD("RequestSaveState"), &Libretro::RequestSaveState);
    ClassDB::bind_method(D_METHOD("RequestLoadState", "data", "frame"), &Libretro::RequestLoadState);
    ClassDB::bind_method(D_METHOD("SnapshotMappedRam"), &Libretro::SnapshotMappedRam);
    ClassDB::bind_method(D_METHOD("SetNetplayCrcInterval", "frames"), &Libretro::SetNetplayCrcInterval);
    ClassDB::bind_method(D_METHOD("SetNetplayCrcFromState", "from_state"), &Libretro::SetNetplayCrcFromState);
    ClassDB::bind_method(D_METHOD("GetFrameCount"), &Libretro::GetFrameCount);
    ClassDB::bind_method(D_METHOD("GetCoreIdentity"), &Libretro::GetCoreIdentity);
    ClassDB::bind_method(D_METHOD("GetNetplayRollbackCount"), &Libretro::GetNetplayRollbackCount);
    ClassDB::bind_method(D_METHOD("GetNetplayRollbackStats"), &Libretro::GetNetplayRollbackStats);
    ClassDB::bind_method(D_METHOD("GetDeclaredFps"), &Libretro::GetDeclaredFps);
    ClassDB::bind_method(D_METHOD("GetDeclaredSampleRate"), &Libretro::GetDeclaredSampleRate);
    ClassDB::bind_method(D_METHOD("GetDroppedFrameCount"), &Libretro::GetDroppedFrameCount);
    ClassDB::bind_method(D_METHOD("GetAudioBufferOccupancy"), &Libretro::GetAudioBufferOccupancy);
    ClassDB::bind_method(D_METHOD("GetAudioBrakeMs"), &Libretro::GetAudioBrakeMs);
    ClassDB::bind_method(D_METHOD("SetSramPath", "path"), &Libretro::SetSramPath);
    ClassDB::bind_method(D_METHOD("SetPackPath", "path"), &Libretro::SetPackPath);
    ClassDB::bind_method(D_METHOD("SetSramBPath", "path", "memory_id"), &Libretro::SetSramBPath,
        DEFVAL(static_cast<int64_t>(Wrapper::SRAM_B_SUFAMI_TURBO)));
    ClassDB::bind_method(D_METHOD("SetRtcPath", "path"), &Libretro::SetRtcPath);
    // Named so a caller passes a region rather than a magic number; each core
    // publishes its own id for a second save region.
    ClassDB::bind_integer_constant(get_class_static(), StringName(),
        "SRAM_B_SUFAMI_TURBO", Wrapper::SRAM_B_SUFAMI_TURBO);
    ClassDB::bind_integer_constant(get_class_static(), StringName(),
        "SRAM_B_PCSX_MEMCARD2", Wrapper::SRAM_B_PCSX_MEMCARD2);
    ClassDB::bind_method(D_METHOD("SetSramRegionPath", "index", "path", "offset", "length"), &Libretro::SetSramRegionPath);
    ClassDB::bind_method(D_METHOD("ClearSramRegion", "index"), &Libretro::ClearSramRegion);
    ClassDB::bind_method(D_METHOD("SetTransferPak", "port", "rom_path", "ram_path"), &Libretro::SetTransferPak);
    ClassDB::bind_method(D_METHOD("SetTransferPakClock", "port", "rtc_path"), &Libretro::SetTransferPakClock);
    ClassDB::bind_method(D_METHOD("ClearTransferPak", "port"), &Libretro::ClearTransferPak);
    ClassDB::bind_method(D_METHOD("SetSramData", "data"), &Libretro::SetSramData);
    ClassDB::bind_method(D_METHOD("SetRemovableStorage", "removable"), &Libretro::SetRemovableStorage);
    ClassDB::bind_method(D_METHOD("RequestSramFlush"), &Libretro::RequestSramFlush);
    ClassDB::bind_method(D_METHOD("RequestReset"), &Libretro::RequestReset);
    ClassDB::bind_method(D_METHOD("ScheduleReset", "frame"), &Libretro::ScheduleReset);
    ClassDB::bind_method(D_METHOD("RequestDiskInfo"), &Libretro::RequestDiskInfo);
    ClassDB::bind_method(D_METHOD("SetDiskEjectState", "ejected"), &Libretro::SetDiskEjectState);
    ClassDB::bind_method(D_METHOD("ReplaceDiskImage", "index", "path"), &Libretro::ReplaceDiskImage);
    ClassDB::bind_method(D_METHOD("ScheduleDiscOp", "frame", "op", "index", "path"), &Libretro::ScheduleDiscOp);
    ClassDB::bind_method(D_METHOD("ScheduleLinkOp", "frame", "op", "others", "ports"), &Libretro::ScheduleLinkOp);
    ClassDB::bind_method(D_METHOD("RaClaimSession", "console_id"), &Libretro::RaClaimSession);
    ClassDB::bind_method(D_METHOD("RaHoldsSession"), &Libretro::RaHoldsSession);

    ADD_SIGNAL(MethodInfo("savestate_ready",
        PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data"),
        PropertyInfo(Variant::INT, "frame")));
    ADD_SIGNAL(MethodInfo("savestate_loaded", PropertyInfo(Variant::BOOL, "ok")));
    ADD_SIGNAL(MethodInfo("disk_control_ready",
        PropertyInfo(Variant::BOOL, "has_control"),
        PropertyInfo(Variant::INT, "count"),
        PropertyInfo(Variant::INT, "current_index"),
        PropertyInfo(Variant::BOOL, "ejected")));
    ADD_SIGNAL(MethodInfo("netplay_crc",
        PropertyInfo(Variant::INT, "frame"),
        PropertyInfo(Variant::INT, "crc")));
    ADD_SIGNAL(MethodInfo("netplay_error", PropertyInfo(Variant::STRING, "message")));
    ADD_SIGNAL(MethodInfo("led_state",
        PropertyInfo(Variant::INT, "led"), PropertyInfo(Variant::BOOL, "on")));
    /// SAVE_RAM reached disk. Only fires when the dirty check found a change,
    /// so it is the real "the game saved" event, not a timer tick. `final` is
    /// the flush at core shutdown.
    ADD_SIGNAL(MethodInfo("sram_flushed",
        PropertyInfo(Variant::STRING, "path"),
        PropertyInfo(Variant::INT,    "size"),
        PropertyInfo(Variant::BOOL,   "final")));

    /// The run never started: the core would not load, the content was
    /// unreadable, or the core refused it. Fires instead of, never alongside,
    /// a successful start -- a listener should power the machine back off.
    ADD_SIGNAL(MethodInfo("content_load_failed", PropertyInfo(Variant::STRING, "reason")));

    ADD_SIGNAL(MethodInfo("options_ready", PropertyInfo(Variant::DICTIONARY, "categories"), PropertyInfo(Variant::DICTIONARY, "definitions"), PropertyInfo(Variant::DICTIONARY, "current_values")));
    ADD_SIGNAL(MethodInfo("rumble_state_changed",
        PropertyInfo(Variant::INT,   "port"),
        PropertyInfo(Variant::FLOAT, "weak"),
        PropertyInfo(Variant::FLOAT, "strong")));
}

bool Libretro::LinkConnect(Libretro* other, uint32_t port, uint32_t other_port)
{
    if (!other || other == this)
    {
        LogError("LinkConnect: a machine cannot be cabled to itself.");
        return false;
    }
    if (!m_wrapper || !other->m_wrapper)
    {
        // Not the "console is switched off" case, which is ordinary and joins
        // fine: a wrapper exists from the moment the node does. This is a node
        // being torn down, and worth saying out loud because from inside the
        // room the cable still looks seated.
        LogError("LinkConnect: a machine is being torn down.");
        return false;
    }

    return LinkCoordinator::Get().Connect(m_wrapper.get(), port, other->m_wrapper.get(), other_port);
}

void Libretro::LinkDisconnect(uint32_t port)
{
    if (m_wrapper)
    {
        LinkCoordinator::Get().Disconnect(m_wrapper.get(), port);
    }
}

uint32_t Libretro::LinkPeerCount(uint32_t port)
{
    if (!m_wrapper)
    {
        return 0;
    }

    unsigned count = 0;
    LinkCoordinator::Get().PeersFor(m_wrapper.get(), port, &count);
    return static_cast<uint32_t>(count);
}

bool Libretro::LinkConnectGroup(const godot::Array& others, const godot::PackedInt32Array& ports)
{
    if (!m_wrapper)
    {
        return false;
    }
    if (ports.size() != others.size() + 1)
    {
        // One port per machine, this one included. Getting this wrong would
        // silently cable the wrong sockets together.
        LogError("LinkConnectGroup: expected one port per machine.");
        return false;
    }

    std::vector<std::pair<Wrapper*, unsigned>> group;
    group.emplace_back(m_wrapper.get(), static_cast<unsigned>(ports[0]));

    for (int i = 0; i < others.size(); ++i)
    {
        Libretro* other = godot::Object::cast_to<Libretro>(others[i]);
        if (!other || other == this || !other->m_wrapper)
        {
            continue;
        }
        group.emplace_back(other->m_wrapper.get(), static_cast<unsigned>(ports[i + 1]));
    }

    return LinkCoordinator::Get().ConnectGroup(group);
}

godot::Array Libretro::LinkCaptureGroup(const godot::Array& others,
                                        const godot::PackedInt32Array& ports)
{
    godot::Array result;
    if (!m_wrapper || ports.size() != others.size() + 1)
        return result;

    std::vector<std::pair<Wrapper*, unsigned>> group;
    group.emplace_back(m_wrapper.get(), static_cast<unsigned>(ports[0]));
    for (int i = 0; i < others.size(); ++i)
    {
        Libretro* other = godot::Object::cast_to<Libretro>(others[i]);
        if (!other || other == this || !other->m_wrapper)
            return godot::Array();
        group.emplace_back(other->m_wrapper.get(), static_cast<unsigned>(ports[i + 1]));
    }

    std::vector<LinkCoordinator::EndpointState> states;
    if (!LinkCoordinator::Get().CaptureGroup(group, states))
        return result;
    for (const auto& state : states)
    {
        godot::Dictionary encoded;
        encoded["published"] = state.published;
        encoded["origin"] = static_cast<int64_t>(state.origin);
        encoded["local_delta"] = static_cast<int64_t>(state.local_delta);
        encoded["safe_delta"] = static_cast<int64_t>(state.safe_delta);
        encoded["last_grant"] = static_cast<int64_t>(state.last_grant);
        godot::Array inbox;
        for (const auto& message : state.inbox)
        {
            godot::Dictionary item;
            item["tick"] = static_cast<int64_t>(message.tick);
            item["from"] = static_cast<int64_t>(message.from);
            godot::PackedByteArray data;
            data.resize(static_cast<int64_t>(message.size));
            if (message.size > 0)
                std::copy_n(message.Data(), message.size, data.ptrw());
            item["data"] = data;
            inbox.append(item);
        }
        encoded["inbox"] = inbox;
        result.append(encoded);
    }
    return result;
}

bool Libretro::LinkRestoreGroup(const godot::Array& others,
                                const godot::PackedInt32Array& ports,
                                const godot::Array& encoded_states)
{
    if (!m_wrapper || ports.size() != others.size() + 1 ||
        encoded_states.size() != ports.size())
        return false;

    std::vector<std::pair<Wrapper*, unsigned>> group;
    group.emplace_back(m_wrapper.get(), static_cast<unsigned>(ports[0]));
    for (int i = 0; i < others.size(); ++i)
    {
        Libretro* other = godot::Object::cast_to<Libretro>(others[i]);
        if (!other || other == this || !other->m_wrapper)
            return false;
        group.emplace_back(other->m_wrapper.get(), static_cast<unsigned>(ports[i + 1]));
    }

    std::vector<LinkCoordinator::EndpointState> states;
    states.reserve(static_cast<size_t>(encoded_states.size()));
    for (int i = 0; i < encoded_states.size(); ++i)
    {
        godot::Dictionary encoded = encoded_states[i];
        const int64_t origin = encoded.get("origin", -1);
        const int64_t local_delta = encoded.get("local_delta", -1);
        const int64_t safe_delta = encoded.get("safe_delta", -1);
        const int64_t last_grant = encoded.get("last_grant", -1);
        if (origin < 0 || local_delta < 0 || safe_delta < 0 || last_grant < 0)
            return false;
        LinkCoordinator::EndpointState state;
        state.published = encoded.get("published", false);
        state.origin = static_cast<uint64_t>(origin);
        state.local_delta = static_cast<uint64_t>(local_delta);
        state.safe_delta = static_cast<uint64_t>(safe_delta);
        state.last_grant = static_cast<uint64_t>(last_grant);
        godot::Array inbox = encoded.get("inbox", godot::Array());
        if (inbox.size() > 65536)
            return false;
        for (int j = 0; j < inbox.size(); ++j)
        {
            godot::Dictionary item = inbox[j];
            const int64_t tick = item.get("tick", -1);
            const int64_t from = item.get("from", -1);
            godot::PackedByteArray data = item.get("data", godot::PackedByteArray());
            if (tick < 0 || from < 0 || from >= encoded_states.size() ||
                data.size() > static_cast<int64_t>(LinkCoordinator::MAX_PAYLOAD))
                return false;
            LinkCoordinator::Message message;
            message.tick = static_cast<uint64_t>(tick);
            message.from = static_cast<unsigned>(from);
            message.Assign(data.ptr(), static_cast<size_t>(data.size()));
            state.inbox.push_back(std::move(message));
        }
        states.push_back(std::move(state));
    }
    return LinkCoordinator::Get().RestoreGroup(group, states);
}

void Libretro::ScheduleLinkOp(int64_t frame, int64_t op, const godot::Array& others,
                              const godot::PackedInt32Array& ports)
{
    if (!m_wrapper)
        return;
    if (ports.size() != others.size() + 1)
    {
        LogError("ScheduleLinkOp: expected one port per machine.");
        return;
    }
    std::vector<std::pair<Wrapper*, unsigned>> group;
    group.emplace_back(m_wrapper.get(), static_cast<unsigned>(ports[0]));
    for (int i = 0; i < others.size(); ++i)
    {
        Libretro* other = godot::Object::cast_to<Libretro>(others[i]);
        if (!other || other == this || !other->m_wrapper)
            continue;
        group.emplace_back(other->m_wrapper.get(), static_cast<unsigned>(ports[i + 1]));
    }
    m_wrapper->ScheduleLinkOp(frame, static_cast<int32_t>(op), group);
}

uint64_t Libretro::LinkTraffic(uint32_t port)
{
    return m_wrapper ? LinkCoordinator::Get().Delivered(m_wrapper.get(), port) : 0;
}

uint64_t Libretro::LinkSent(uint32_t port)
{
    return m_wrapper ? LinkCoordinator::Get().Sent(m_wrapper.get(), port) : 0;
}
}
