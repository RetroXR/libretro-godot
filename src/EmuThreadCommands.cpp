#include "EmuThreadCommands.hpp"

#include "Wrapper.hpp"
#include "Debug.hpp"

namespace Xenu
{
void EmuThreadCommandSaveState::Execute(Wrapper& wrapper)
{
    godot::PackedByteArray buffer;
    bool ok = false;
    int64_t frame = wrapper.m_frame_counter.load(std::memory_order_relaxed);

    if (wrapper.m_core && wrapper.m_core->retro_serialize_size && wrapper.m_core->retro_serialize)
    {
        size_t size = wrapper.m_core->retro_serialize_size();
        if (size > 0)
        {
            buffer.resize(static_cast<int64_t>(size));
            ok = wrapper.m_core->retro_serialize(buffer.ptrw(), size);
            if (!ok)
                LogWarning("SaveState: core refused retro_serialize (" + std::to_string(size) +
                           " bytes) at frame " + std::to_string(frame));
        }
        else
        {
            LogWarning("SaveState: core reports serialize_size 0 at frame " + std::to_string(frame));
        }
    }
    else
    {
        LogWarning("SaveState: core does not export retro_serialize");
    }

    if (!ok)
        buffer = godot::PackedByteArray();
    else
        Log("SaveState: " + std::to_string(buffer.size()) + " bytes captured at frame " +
            std::to_string(frame));

    godot::Array args;
    args.append(buffer);
    args.append(frame);
    wrapper.EmitSignalOnMainThread("savestate_ready", args);
}

void EmuThreadCommandSaveState::Abandon(Wrapper& wrapper)
{
    wrapper.AnswerNoSaveState();
}

void EmuThreadCommandLoadState::Execute(Wrapper& wrapper)
{
    bool ok = false;
    if (!wrapper.m_core || !wrapper.m_core->retro_unserialize)
        LogWarning("LoadState: core does not export retro_unserialize");
    else if (m_data.size() == 0)
        LogWarning("LoadState: empty state buffer, nothing to restore");
    else
    {
        ok = wrapper.m_core->retro_unserialize(m_data.ptr(), static_cast<size_t>(m_data.size()));
        if (!ok)
            LogWarning("LoadState: core refused retro_unserialize (" + std::to_string(m_data.size()) +
                       " bytes) for frame " + std::to_string(m_frame));
    }

    if (ok)
    {
        Log("LoadState: " + std::to_string(m_data.size()) + " bytes restored, frame counter " +
            std::to_string(wrapper.m_frame_counter.load(std::memory_order_relaxed)) + " -> " +
            std::to_string(m_frame) + "; netplay schedule and disc schedule reset");
        // Restart the netplay schedule from the state's frame, including the
        // rollback bookkeeping (this runs on the emulation thread, which owns
        // the states/used/crc structures).
        wrapper.m_np_states.clear();
        wrapper.m_np_used.clear();
        wrapper.m_np_crc_pending.clear();
        wrapper.m_np_watermark = m_frame - 1;
        wrapper.m_np_verified = m_frame - 1;
        std::lock_guard<std::mutex> lock(wrapper.m_np_mutex);
        wrapper.m_np_inputs.clear();
        wrapper.m_np_local_records.clear();
        wrapper.m_disc_schedule.clear();
        wrapper.m_frame_counter.store(m_frame, std::memory_order_relaxed);
    }

    godot::Array args;
    args.append(ok);
    wrapper.EmitSignalOnMainThread("savestate_loaded", args);
}

void EmuThreadCommandLoadState::Abandon(Wrapper& wrapper)
{
    wrapper.AnswerNoLoadState();
}

void EmuThreadCommandReset::Execute(Wrapper& wrapper)
{
    if (wrapper.m_core && wrapper.m_core->retro_reset)
        wrapper.m_core->retro_reset();
    else
        LogWarning("Reset failed (no core running?)");
}

void EmuThreadCommandSetPortDevice::Execute(Wrapper& wrapper)
{
    if (!wrapper.m_core || !wrapper.m_input_handler)
        return;
    Log("SetPortDevice (emu thread): port=" + std::to_string(m_port)
        + " device=" + std::to_string(m_device));
    wrapper.m_input_handler->SetPortDevice(m_port, m_device);
    wrapper.m_core->retro_set_controller_port_device(m_port, m_device);
}

void EmuThreadCommandSetCoreOption::Execute(Wrapper& wrapper)
{
    if (!wrapper.m_options_handler)
        return;
    wrapper.m_options_handler->SetVariable(m_key, m_value);
}

void EmuThreadCommandKeyboardEvent::Execute(Wrapper& wrapper)
{
    if (!wrapper.m_input_handler)
        return;
    wrapper.m_input_handler->SetKeyState(m_port, m_keycode, m_down);
    wrapper.m_input_handler->CallKeyboardEventCallback(
        m_down, m_keycode, m_character,
        wrapper.m_input_handler->GetKeyModifiers(m_port));
}

void EmuThreadCommandSetSram::Execute(Wrapper& wrapper)
{
    wrapper.ApplySramSwap(m_path);
}

void EmuThreadCommandSetSramB::Execute(Wrapper& wrapper)
{
    wrapper.ApplySramBSwap(m_path, m_memory_id);
}

void EmuThreadCommandFlushSram::Execute(Wrapper& wrapper)
{
    wrapper.FlushSramIfDirty();
    wrapper.FlushSramBIfDirty();
    wrapper.FlushRtcIfDirty();
}

void EmuThreadCommandDiskInfo::Execute(Wrapper& wrapper)
{
    wrapper.EmitDiskInfo();
}

void EmuThreadCommandDiskInfo::Abandon(Wrapper& wrapper)
{
    wrapper.AnswerNoDiskInfo();
}

void EmuThreadCommandSetDiskEjected::Execute(Wrapper& wrapper)
{
    if (wrapper.m_environment_handler)
    {
        if (wrapper.m_environment_handler->SetDiskEjected(m_ejected))
            Log(m_ejected ? "Disk tray opened (virtual eject)"
                          : "Disk tray closed");
        else
            LogWarning(m_ejected ? "SetDiskEjected(true) refused by core"
                                 : "SetDiskEjected(false) refused by core");
    }
    wrapper.EmitDiskInfo();
}

void EmuThreadCommandSetDiskEjected::Abandon(Wrapper& wrapper)
{
    wrapper.AnswerNoDiskInfo();
}

void EmuThreadCommandReplaceDisk::Execute(Wrapper& wrapper)
{
    if (wrapper.m_environment_handler)
    {
        if (wrapper.m_environment_handler->ReplaceDiskImage(m_index, m_path))
            LogOK("Disk image " + std::to_string(m_index) + " replaced: " + m_path);
        else
            LogWarning("ReplaceDiskImage refused by core: " + m_path);
    }
    wrapper.EmitDiskInfo();
}

void EmuThreadCommandReplaceDisk::Abandon(Wrapper& wrapper)
{
    wrapper.AnswerNoDiskInfo();
}
}
