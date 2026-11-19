#include "Wrapper.hpp"

#include <algorithm>

#include "LinkCoordinator.hpp"
#include "Libretro.hpp"
#include "Debug.hpp"

using namespace godot;

namespace Xenu
{
void Wrapper::AnswerNoDiskInfo()
{
    if (Libretro* node = LiveLibretroNode())
        node->call_deferred("emit_signal", "disk_control_ready",
            false, static_cast<int64_t>(0), static_cast<int64_t>(0), false);
}

// ── Disk control (multi-disc games) ──────────────────────────────────────────

void Wrapper::RequestDiskInfo()
{
    if (!AcceptsEmuCommands())
    {
        AnswerNoDiskInfo();
        return;
    }
    m_emu_thread_commands_queue.enqueue(std::make_unique<EmuThreadCommandDiskInfo>());
}

void Wrapper::SetDiskEjectState(bool ejected)
{
    if (!AcceptsEmuCommands())
        return;
    m_emu_thread_commands_queue.enqueue(std::make_unique<EmuThreadCommandSetDiskEjected>(ejected));
}

void Wrapper::ReplaceDiskImage(uint32_t index, const godot::String& path)
{
    if (!AcceptsEmuCommands())
        return;
    m_emu_thread_commands_queue.enqueue(
        std::make_unique<EmuThreadCommandReplaceDisk>(index, std::string(path.utf8().get_data())));
}

// ── Accessory media (amiibo, e-Reader strips) ────────────────────────────────

void Wrapper::RequestAccessoryReaders()
{
    if (!AcceptsEmuCommands())
    {
        if (Libretro* node = LiveLibretroNode())
            node->call_deferred("emit_signal", "accessory_readers_changed", godot::Array());
        return;
    }
    m_emu_thread_commands_queue.enqueue(std::make_unique<EmuThreadCommandAccessoryReaders>());
}

void Wrapper::InsertAccessoryMedia(uint32_t reader, const godot::String& path)
{
    if (!AcceptsEmuCommands())
    {
        LogWarning("Accessory media: nothing running to take " + std::string(path.utf8().get_data()));
        if (Libretro* node = LiveLibretroNode())
            node->call_deferred("emit_signal", "accessory_media_done", static_cast<int64_t>(reader), true, false);
        return;
    }
    m_emu_thread_commands_queue.enqueue(
        std::make_unique<EmuThreadCommandAccessoryMedia>(reader, true, std::string(path.utf8().get_data())));
}

void Wrapper::RemoveAccessoryMedia(uint32_t reader)
{
    if (!AcceptsEmuCommands())
    {
        if (Libretro* node = LiveLibretroNode())
            node->call_deferred("emit_signal", "accessory_media_done", static_cast<int64_t>(reader), false, false);
        return;
    }
    m_emu_thread_commands_queue.enqueue(std::make_unique<EmuThreadCommandAccessoryMedia>(reader, false, std::string()));
}

/// Emulation thread. Each reader as a Dictionary: index, kind, port (-1 for one
/// on the console), flags, momentary, writes, name, and present (1 a medium is
/// on it, 0 none, -1 the core cannot tell).
void Wrapper::EmitAccessoryReaders()
{
    godot::Array readers;
    if (m_environment_handler)
    {
        const auto& list = m_environment_handler->GetAccessoryReaders();
        for (size_t i = 0; i < list.size(); ++i)
        {
            const EnvironmentHandler::AccessoryReader& r = list[i];
            godot::Dictionary d;
            d["index"] = static_cast<int64_t>(i);
            d["kind"] = static_cast<int64_t>(r.kind);
            d["port"] = r.port == RETRO_ACCESSORY_PORT_CONSOLE ? int64_t(-1) : static_cast<int64_t>(r.port);
            d["flags"] = static_cast<int64_t>(r.flags);
            d["momentary"] = (r.flags & RETRO_ACCESSORY_READER_MOMENTARY) != 0;
            d["writes"] = (r.flags & RETRO_ACCESSORY_READER_WRITES) != 0;
            d["name"] = godot::String::utf8(r.name.c_str());
            d["present"] = static_cast<int64_t>(m_environment_handler->AccessoryMediaPresent(static_cast<unsigned>(i)));
            readers.append(d);
        }
    }
    EmitSignalOnMainThread("accessory_readers_changed", godot::Array::make(readers));
}

void Wrapper::AnswerAccessoryMediaDone(uint32_t reader, bool inserted, bool ok)
{
    EmitSignalOnMainThread("accessory_media_done",
        godot::Array::make(static_cast<int64_t>(reader), inserted, ok));
}

void Wrapper::ScheduleDiscOp(int64_t frame, int32_t op, uint32_t index, const godot::String& path)
{
    if (!m_core || !m_running)
        return;
    Log("Disc op " + std::to_string(op) + " scheduled for frame " + std::to_string(frame)
        + " (index " + std::to_string(index) + ")");
    std::lock_guard<std::mutex> lock(m_np_mutex);
    m_disc_schedule.emplace(frame, DiscOp{ op, index, std::string(path.utf8().get_data()) });
}

void Wrapper::ScheduleLinkOp(int64_t frame, int32_t op,
                             const std::vector<std::pair<Wrapper*, unsigned>>& group)
{
    if (!m_core || !m_running || group.empty())
        return;
    Log("Link op " + std::to_string(op) + " scheduled for frame " + std::to_string(frame)
        + " over " + std::to_string(group.size()) + " machine(s)");
    std::lock_guard<std::mutex> lock(m_np_mutex);
    m_link_schedule.emplace(frame, LinkOp{ op, group });
}

/// Emulation thread: apply any netplay-scheduled link change whose frame has
/// arrived, strictly before running `frame`.
///
/// LinkCoordinator says this is the caller's job and it is right: it cannot
/// enforce that Connect and Disconnect land on the same emulated frame on every
/// peer, and a cable applied whenever a hand happened to move is a different
/// frame on each of them. It takes its own lock, so calling it from here is
/// safe even though the room schedules from the main thread.
void Wrapper::ApplyScheduledLinkOps(int64_t frame)
{
    std::vector<LinkOp> pending;
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        auto it = m_link_schedule.begin();
        while (it != m_link_schedule.end() && it->first <= frame)
        {
            pending.push_back(std::move(it->second));
            it = m_link_schedule.erase(it);
        }
    }
    for (const LinkOp& op : pending)
    {
        if (op.group.empty())
            continue;
        if (op.op == 0)
        {
            LinkCoordinator::Get().Disconnect(op.group[0].first, op.group[0].second);
            Log("Netplay link PULL applied @frame " + std::to_string(frame));
        }
        else
        {
            const bool ok = LinkCoordinator::Get().ConnectGroup(op.group);
            LogOK("Netplay link JOIN applied @frame " + std::to_string(frame)
                + " over " + std::to_string(op.group.size()) + " machine(s), ok="
                + std::string(ok ? "true" : "false"));
        }
    }
}

/// Emulation thread: read the disk-control state and emit disk_control_ready.
void Wrapper::EmitDiskInfo()
{
    bool has = false;
    int64_t count = 0;
    int64_t index = 0;
    bool ejected = false;
    if (m_environment_handler)
    {
        has = m_environment_handler->HasDiskControl();
        if (has)
        {
            count = static_cast<int64_t>(m_environment_handler->GetDiskImageCount());
            index = static_cast<int64_t>(m_environment_handler->GetDiskImageIndex());
            ejected = m_environment_handler->GetDiskEjected();
        }
    }
    Log("Disk control: has=" + std::string(has ? "yes" : "no")
        + " images=" + std::to_string(count)
        + " index=" + std::to_string(index)
        + " ejected=" + std::string(ejected ? "yes" : "no"));
    godot::Array args;
    args.append(has);
    args.append(count);
    args.append(index);
    args.append(ejected);
    EmitSignalOnMainThread("disk_control_ready", args);
}

/// Emulation thread: apply any netplay-scheduled disc op whose frame has
/// arrived, strictly before running `frame`, so every netplay peer swaps on
/// the identical frame. Rollback waits for this boundary to be confirmed first.
void Wrapper::ApplyScheduledDiscOps(int64_t frame)
{
    std::vector<DiscOp> pending;
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        auto it = m_disc_schedule.begin();
        while (it != m_disc_schedule.end() && it->first <= frame)
        {
            pending.push_back(it->second);
            it = m_disc_schedule.erase(it);
        }
    }
    if (pending.empty() || !m_environment_handler)
        return;
    for (const DiscOp& op : pending)
    {
        if (op.op == 0)
        {
            Log("Netplay disc EJECT applied @frame " + std::to_string(frame));
            m_environment_handler->SetDiskEjected(true);
        }
        else
        {
            LogOK("Netplay disc SWAP applied @frame " + std::to_string(frame)
                + " -> " + op.path);
            m_environment_handler->SetDiskEjected(true);   // idempotent if already open
            m_environment_handler->ReplaceDiskImage(op.index, op.path);
            m_environment_handler->SetDiskEjected(false);
        }
    }
    EmitDiskInfo();
}

/// Emulation thread: apply every front-panel reset due at this confirmed frame.
/// Rollback never crosses the boundary: the scheduler stalls until confirmation,
/// then the pre-reset history is discarded before the new anchor is serialized.
void Wrapper::ApplyScheduledResets(int64_t frame)
{
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        auto end = m_reset_schedule.upper_bound(frame);
        count = static_cast<size_t>(std::distance(m_reset_schedule.begin(), end));
        m_reset_schedule.erase(m_reset_schedule.begin(), end);
    }
    if (count == 0 || !m_core || !m_core->retro_reset)
        return;
    for (size_t i = 0; i < count; ++i)
        m_core->retro_reset();
    m_np_states.clear();
    m_np_used.clear();
    m_np_crc_pending.clear();
    LogOK("Netplay reset applied @frame " + std::to_string(frame));
}

void Wrapper::RequestReset()
{
    if (m_core && m_running)
        m_emu_thread_commands_queue.enqueue(std::make_unique<EmuThreadCommandReset>());
}

void Wrapper::ScheduleReset(int64_t frame)
{
    if (!m_core || !m_running)
        return;
    Log("Reset scheduled for frame " + std::to_string(frame));
    std::lock_guard<std::mutex> lock(m_np_mutex);
    m_reset_schedule.insert(frame);
    m_np_cv.notify_all();
}
}
