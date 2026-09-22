#include "Wrapper.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

#include "Libretro.hpp"
#include "LinkCoordinator.hpp"
#include "Debug.hpp"

using namespace godot;

namespace Xenu
{
bool Wrapper::IsNetplayPortManaged(uint32_t port) const
{
    return port < 4 &&
        m_netplay_enabled.load(std::memory_order_acquire) &&
        (m_np_port_mask.load(std::memory_order_relaxed) & (1u << port));
}

// ── Netplay (deterministic lockstep) ─────────────────────────────────────────

void Wrapper::SetNetplayMode(bool enabled, uint32_t port_mask, int64_t start_frame)
{
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        m_np_inputs.clear();
        m_disc_schedule.clear();
        m_reset_schedule.clear();
        m_np_local_mask_schedule.clear();
        // Link operations retain Wrapper pointers until they land.  They must
        // not survive a stopped session or reach a later core invocation.
        m_link_schedule.clear();
    }
    m_np_port_mask.store(port_mask == 0 ? 0x1u : port_mask, std::memory_order_relaxed);
    if (start_frame >= 0)
        m_frame_counter.store(start_frame, std::memory_order_relaxed);
    m_netplay_enabled.store(enabled, std::memory_order_release);
    m_microphone_handler->ResetAll();
    if (enabled && m_input_handler)
        m_input_handler->ClearJoypadExtraButtons();
    m_np_cv.notify_all();
    Log("Netplay mode " + std::string(enabled ? "ON" : "OFF") +
        " mask=" + std::to_string(port_mask) + " start_frame=" + std::to_string(start_frame));
}

void Wrapper::PostNetplayInputs(int64_t frame, const godot::PackedInt32Array& flat)
{
    if (flat.size() < 20)
        return;
    NpFrame values{};
    const int n = flat.size() < NP_FRAME_INTS ? static_cast<int>(flat.size()) : NP_FRAME_INTS;
    for (int i = 0; i < n; ++i)
        values[i] = flat[i];
    const bool rollback = m_np_rollback.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        const int64_t current = m_frame_counter.load(std::memory_order_relaxed);
        const int64_t oldest = rollback && current > std::numeric_limits<int64_t>::min() + NP_ROLLBACK_HISTORY
            ? current - NP_ROLLBACK_HISTORY
            : (rollback ? std::numeric_limits<int64_t>::min() : current);
        const int64_t newest = current < std::numeric_limits<int64_t>::max() - NP_MAX_FUTURE_INPUTS
            ? current + NP_MAX_FUTURE_INPUTS
            : std::numeric_limits<int64_t>::max();

        // Bound the schedule by frame distance, not total map size. A global
        // size guard lets far-future packets fill the map and then rejects the
        // current frame forever, so the consumer can never advance and prune.
        m_np_inputs.erase(m_np_inputs.begin(), m_np_inputs.lower_bound(oldest));
        m_np_inputs.erase(m_np_inputs.upper_bound(newest), m_np_inputs.end());
        if (frame < oldest || frame > newest)
            return;
        // Lockstep: inputs for already-run frames are stale. Rollback: they are
        // CONFIRMATIONS for speculatively-run frames, which is the point.
        if (!rollback && frame < current)
            return;
        m_np_inputs[frame] = values;
    }
    m_np_cv.notify_all();
}

void Wrapper::SetNetplayRollback(bool enabled, uint32_t local_mask, int max_ahead)
{
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        m_np_live_local.fill(0);
        m_np_local_records.clear();
        m_np_initial_local_mask = local_mask;
        m_np_local_mask_schedule.clear();
    }
    m_np_local_mask.store(local_mask, std::memory_order_relaxed);
    m_np_max_ahead = std::clamp(max_ahead, 2, NP_MAX_AHEAD_LIMIT);
    m_np_rollback.store(enabled, std::memory_order_release);
    m_microphone_handler->ResetAll();
    if (enabled && m_input_handler)
        m_input_handler->ClearJoypadExtraButtons();
    m_np_cv.notify_all();
    Log("Netplay rollback " + std::string(enabled ? "ON" : "OFF") +
        " local_mask=" + std::to_string(local_mask) +
        " max_ahead=" + std::to_string(max_ahead));
}

bool Wrapper::ScheduleNetplayLocalMask(int64_t frame, uint32_t local_mask)
{
    if (!m_np_rollback.load(std::memory_order_acquire))
        return false;
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        if (frame < m_frame_counter.load(std::memory_order_relaxed))
            return false;
        m_np_local_mask_schedule[frame] = local_mask;
    }
    m_np_cv.notify_all();
    Log("Netplay local ownership mask=" + std::to_string(local_mask) +
        " @frame " + std::to_string(frame));
    return true;
}

godot::PackedInt32Array Wrapper::TakeNetplayLocalRecords()
{
    godot::PackedInt32Array out;
    std::lock_guard<std::mutex> lock(m_np_mutex);
    if (m_np_local_records.empty())
        return out;
    out.resize(static_cast<int64_t>(m_np_local_records.size()));
    for (size_t i = 0; i < m_np_local_records.size(); ++i)
        out[static_cast<int64_t>(i)] = m_np_local_records[i];
    m_np_local_records.clear();
    return out;
}

// Self-contained CRC32 (polynomial 0xEDB88320). libretro-common's crc32.c is
// not part of this build, and the table init must be thread-safe (multiple
// emulation threads may race the first call).
static const uint32_t* Crc32Table()
{
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, []
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j)
                crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
            table[i] = crc;
        }
    });
    return table;
}

// Folds one more block into a running CRC. `crc` is the RAW register rather than
// a finished value: start at 0xFFFFFFFF, feed as many blocks as there are, and
// invert once at the end. That is what lets a hash span several disjoint regions
// without hashing a hash.
static uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t size)
{
    const uint32_t* table = Crc32Table();
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static uint32_t Crc32(const uint8_t* data, size_t size)
{
    return Crc32Update(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
}

uint32_t Wrapper::ComputeNetplayCrc(bool& ok)
{
    return m_np_crc_from_state ? StateCrc(ok) : ComputeRamCrc(ok);
}

uint32_t Wrapper::StateCrc(bool& ok)
{
    ok = false;
    if (!m_core || !m_core->retro_serialize || !m_core->retro_serialize_size)
        return 0;
    // Asked each time rather than cached: a core may grow its state as a game
    // loads more, and a stale size would hash a truncated one.
    const size_t size = m_core->retro_serialize_size();
    if (size == 0)
        return 0;
    if (m_np_crc_state_buffer.size() < size)
        m_np_crc_state_buffer.resize(size);
    if (!m_core->retro_serialize(m_np_crc_state_buffer.data(), size))
        return 0;
    ok = true;
    return Crc32(m_np_crc_state_buffer.data(), size);
}

uint32_t Wrapper::ComputeRamCrc(bool& ok) const
{
    ok = false;
    if (!m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return 0;
    void* ram = m_core->retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t size = m_core->retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (ram != nullptr && size != 0)
    {
        ok = true;
        return Crc32(static_cast<const uint8_t*>(ram), size);
    }
    return MappedRamCrc(ok);
}

uint32_t Wrapper::MappedRamCrc(bool& ok) const
{
    // Not every core answers RETRO_MEMORY_SYSTEM_RAM, and one that does not is
    // not thereby exempt from being checked for determinism. mGBA is the case in
    // hand: it returns NULL for SYSTEM_RAM and publishes a memory MAP instead,
    // so before this the netplay CRC signal simply never fired for it and the
    // determinism spike sat there waiting for checkpoints that were never coming.
    //
    // Everything writable, in the order the core listed it. CONST descriptors
    // are ROM and BIOS and cannot change, so hashing them would cost time and
    // tell nobody anything. What is left, on a Game Boy Advance, is IWRAM,
    // EWRAM, the save, VRAM, palette, OAM and the IO block. That is a STRICTER
    // oracle than a flat SYSTEM_RAM: a divergence that has only reached a sprite
    // table shows up here and would not show up there.
    //
    // Order is the core's own and does not vary between runs of the same build,
    // which is all a comparison needs. The number is not portable across cores
    // or across core versions, and nothing asks it to be: every use compares two
    // runs of one build against each other.
    uint32_t crc = 0xFFFFFFFFu;
    size_t hashed = 0;
    for (const retro_memory_descriptor& descriptor : m_memory_descriptors)
    {
        if (descriptor.ptr == nullptr || descriptor.len == 0)
            continue;
        if (descriptor.flags & RETRO_MEMDESC_CONST)
            continue;
        const uint8_t* base = static_cast<const uint8_t*>(descriptor.ptr) + descriptor.offset;
        crc = Crc32Update(crc, base, descriptor.len);
        hashed += descriptor.len;
    }
    ok = hashed > 0;
    return ok ? (crc ^ 0xFFFFFFFFu) : 0;
}

void Wrapper::EmitNetplayCrc(int64_t frame)
{
    bool ok = false;
    uint32_t crc = ComputeNetplayCrc(ok);
    if (!ok)
        return;
    godot::Array args;
    args.append(frame);
    args.append(static_cast<int64_t>(crc));
    EmitSignalOnMainThread("netplay_crc", args);
}

// ── Rollback engine (all emulation-thread) ───────────────────────────────────

void Wrapper::ApplyNetplayInputs(const NpFrame& inputs, uint32_t mask)
{
    for (uint32_t port = 0; port < 4; ++port)
    {
        if (!(mask & (1u << port)))
            continue;
        const int32_t* v = inputs.data() + port * 5;
        // Route by the port's device type: a RETRO_DEVICE_MOUSE port carries
        // {buttons, dx, dy, -, -} in its five slots (deltas accumulate until
        // the core's next read, so replaying a frame re-supplies exactly that
        // frame's delta, deterministic under both lockstep and rollback).
        uint32_t device = m_input_handler->GetPortDevice(port) & RETRO_DEVICE_MASK;
        if (device == RETRO_DEVICE_MOUSE)
        {
            m_input_handler->SetMousePosition(port, static_cast<int16_t>(v[1]), static_cast<int16_t>(v[2]));
            m_input_handler->SetMouseButtons(port, static_cast<uint32_t>(v[0]));
            continue;
        }
        if (device == RETRO_DEVICE_LIGHTGUN)
        {
            m_input_handler->SetLightgunButtons(port, static_cast<uint32_t>(v[0]));
            m_input_handler->SetLightgunPosition(port,
                static_cast<int16_t>(v[1]), static_cast<int16_t>(v[2]));
            m_input_handler->SetLightgunIsOffscreen(port, v[3] ? 1 : 0);
            continue;
        }
        m_input_handler->SetJoypadButtonStates(port, static_cast<uint16_t>(v[0]));
        m_input_handler->SetAnalogLeft(port, static_cast<int16_t>(v[1]), static_cast<int16_t>(v[2]));
        m_input_handler->SetAnalogRight(port, static_cast<int16_t>(v[3]), static_cast<int16_t>(v[4]));
    }
}

/// Apply every port's sensor and pointer block on the emulation thread, strictly
/// before the frame executes, so motion and touch share the deterministic line.
void Wrapper::ApplyNetplayAux(const NpFrame& inputs)
{
    const uint32_t mask = m_np_port_mask.load(std::memory_order_relaxed);
    for (uint32_t port = 0; port < NP_PORTS; ++port)
    {
        if (!(mask & (1u << port)))
            continue;
        const int base = NP_AUX_OFFSET + static_cast<int>(port) * NP_AUX_INTS_PER_PORT;
        const int32_t flags = inputs[base];
        for (uint32_t index = 0; index < NP_SENSOR_COUNT; ++index)
        {
            const int accel = base + 1 + static_cast<int>(index) * 3;
            const int gyro = base + 7 + static_cast<int>(index) * 3;
            if (flags & (1 << index))
                m_input_handler->SetSensorAccel(port,
                    inputs[accel] / 1000.0f, inputs[accel + 1] / 1000.0f,
                    inputs[accel + 2] / 1000.0f, index);
            else
                m_input_handler->SetSensorAccel(port, 0.0f, 0.0f, 1.0f, index);
            if (flags & (1 << (2 + index)))
                m_input_handler->SetSensorGyro(port,
                    inputs[gyro] / 100.0f, inputs[gyro + 1] / 100.0f,
                    inputs[gyro + 2] / 100.0f, index);
            else
                m_input_handler->SetSensorGyro(port, 0.0f, 0.0f, 0.0f, index);
        }
        for (uint32_t index = 0; index < NP_POINTER_COUNT; ++index)
        {
            const int pointer = base + 13 + static_cast<int>(index) * 3;
            if (flags & (1 << (4 + index)))
                m_input_handler->SetPointerIndexState(port, index,
                    static_cast<int16_t>(inputs[pointer]),
                    static_cast<int16_t>(inputs[pointer + 1]),
                    inputs[pointer + 2] != 0);
            else
                m_input_handler->SetPointerIndexState(port, index, 0, 0, false);
        }
    }
    // Key events: applied in slot order on the emulation thread, so the poll
    // bitset AND the core's keyboard event callback see the identical sequence
    // on every peer.
    for (int slot = 0; slot < NP_KEY_SLOTS; ++slot)
    {
        int32_t packed = inputs[NP_KEY_OFFSET + slot * 2];
        uint32_t keycode = static_cast<uint32_t>(packed) & 0xFFFF;
        if (keycode == 0)
            continue;
        bool down = (static_cast<uint32_t>(packed) >> 16) & 1;
        uint32_t character = static_cast<uint32_t>(inputs[NP_KEY_OFFSET + slot * 2 + 1]);
        m_input_handler->SetKeyState(0, keycode, down);
        m_input_handler->CallKeyboardEventCallback(down, keycode, character,
            m_input_handler->GetKeyModifiers(0));
    }
}

/// Serialize the machine state at the START of `frame` into the ring.
bool Wrapper::SaveRollbackState(int64_t frame)
{
    if (!m_np_states.empty() && m_np_states.back().frame >= frame)
        return true;   // already have it (rollback re-entry at the anchor frame)
    size_t size = m_core->retro_serialize_size();
    if (size == 0)
        return false;
    std::vector<uint8_t> buffer(size);
    const auto ser_t0 = std::chrono::steady_clock::now();
    const bool ser_ok = m_core->retro_serialize(buffer.data(), size);
    m_np_serialize_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - ser_t0).count(), std::memory_order_relaxed);
    m_np_serialize_n.fetch_add(1, std::memory_order_relaxed);
    if (!ser_ok)
        return false;
    m_np_await_anchor = false;
    m_np_states.push_back(RollbackState{
        frame,
        std::move(buffer),
        m_input_handler->CaptureNetplayState()
    });
    while (m_np_states.size() > static_cast<size_t>(StateRingDepth()))
        m_np_states.pop_front();
    m_np_state_slots.store(static_cast<int64_t>(m_np_states.size()),
                           std::memory_order_relaxed);
    int64_t resident = 0;
    for (const auto& s : m_np_states)
        resident += static_cast<int64_t>(s.core.size());
    m_np_state_bytes.store(resident, std::memory_order_relaxed);
    return true;
}

void Wrapper::FailNetplayRollback(const std::string& reason)
{
    LogError("Rollback cannot recover safely: " + reason);
    if (Libretro* node = LiveLibretroNode())
        node->call_deferred("emit_signal", "netplay_error",
            godot::String(reason.c_str()));
    // Continuing from a timeline that failed to rewind would silently desync.
    // Use the same deferred-stop path as RETRO_ENVIRONMENT_SHUTDOWN so teardown
    // remains on the main thread and never joins the emulation thread itself.
    StopEmulationThread(false);
}

/// Emit captured RAM CRCs once their frame is confirmed-verified; a CRC of a
/// speculative frame would false-positive against peers.
void Wrapper::FlushNetplayCrcs()
{
    for (auto it = m_np_crc_pending.begin(); it != m_np_crc_pending.end();)
    {
        if (it->first - 1 > m_np_verified)
            break;   // ordered map, so nothing later qualifies either
        godot::Array args;
        args.append(it->first);
        args.append(static_cast<int64_t>(it->second));
        EmitSignalOnMainThread("netplay_crc", args);
        it = m_np_crc_pending.erase(it);
    }
}

uint32_t Wrapper::NetplayLocalMaskForFrameLocked(int64_t frame) const
{
    auto next = m_np_local_mask_schedule.upper_bound(frame);
    if (next == m_np_local_mask_schedule.begin())
        return m_np_initial_local_mask;
    return std::prev(next)->second;
}

uint32_t Wrapper::ApplyScheduledNetplayLocalMask(int64_t frame)
{
    std::lock_guard<std::mutex> lock(m_np_mutex);
    const uint32_t next = NetplayLocalMaskForFrameLocked(frame);
    const uint32_t previous = m_np_local_mask.exchange(next, std::memory_order_acq_rel);
    const uint32_t changed = previous ^ next;
    for (uint32_t port = 0; port < NP_PORTS; ++port)
    {
        if (changed & (1u << port))
            std::fill_n(m_np_live_local.data() + port * NP_INPUT_INTS_PER_PORT,
                        NP_INPUT_INTS_PER_PORT, 0);
    }
    return next;
}

/// Rewind to `to_frame` and silently replay up to the current frame with
/// corrected inputs. Audio from replayed frames is dropped (it already played
/// from the mispredicted run); video is skipped except for the final frame.
bool Wrapper::NetplayRollbackReplay(int64_t to_frame, uint32_t mask)
{
    const auto rb_t0 = std::chrono::steady_clock::now();
    if (!RollbackRestoreAnchor(to_frame))
        return false;
    const int64_t current = m_frame_counter.load(std::memory_order_relaxed);
    for (int64_t x = to_frame; x < current; ++x)
    {
        if (!RollbackReplayFrame(x, current, mask))
            return false;
    }
    RollbackFinishReplay(to_frame, current, rb_t0);
    return true;
}

/// First third of a rewind: put this core back at the start of `to_frame`, and
/// forget everything the mispredicted timeline produced after it. The frame
/// counter is left where it was; the replay walks back up to it.
bool Wrapper::RollbackRestoreAnchor(int64_t to_frame)
{
    auto state_it = std::find_if(m_np_states.begin(), m_np_states.end(),
        [&](const auto& s) { return s.frame == to_frame; });
    if (state_it == m_np_states.end())
    {
        LogWarning("Rollback: no saved state for frame " + std::to_string(to_frame));
        return false;
    }
    if (!m_core->retro_unserialize(state_it->core.data(), state_it->core.size()))
    {
        LogError("Rollback: unserialize failed at frame " + std::to_string(to_frame));
        return false;
    }
    m_input_handler->RestoreNetplayState(state_it->input);

    // Drop everything the rewind invalidated: states after the anchor and CRCs
    // captured on the mispredicted timeline.
    m_np_states.erase(std::next(state_it), m_np_states.end());
    m_np_crc_pending.erase(m_np_crc_pending.upper_bound(to_frame), m_np_crc_pending.end());
    m_np_replaying = true;
    return true;
}

/// Re-run frame `x` of a replay that ends at `current` with the inputs it
/// should have had. One frame at a time so a group of cabled cores can stop
/// together at every edge (see NetplayGroupIteration).
bool Wrapper::RollbackReplayFrame(int64_t x, int64_t current, uint32_t mask)
{
    NpFrame inputs{};
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        const uint32_t local_mask = NetplayLocalMaskForFrameLocked(x) & mask;
        auto confirmed = m_np_inputs.find(x);
        auto& prev_used = m_np_used;
        for (uint32_t port = 0; port < 4; ++port)
        {
            if (!(mask & (1u << port)))
                continue;
            int32_t* dst = inputs.data() + port * 5;
            if (local_mask & (1u << port))
            {
                // Local input is ground truth: replay exactly what was pressed.
                auto used = prev_used.find(x);
                if (used != prev_used.end())
                    std::copy_n(used->second.data() + port * 5, 5, dst);
            }
            else if (confirmed != m_np_inputs.end())
            {
                std::copy_n(confirmed->second.data() + port * 5, 5, dst);
            }
            else
            {
                // Beyond the watermark: re-predict by holding the previous
                // frame's (now corrected) value.
                auto prev = prev_used.find(x - 1);
                if (prev != prev_used.end())
                    std::copy_n(prev->second.data() + port * 5, 5, dst);
            }
        }
        if (confirmed != m_np_inputs.end())
            std::copy(confirmed->second.begin() + NP_AUX_OFFSET,
                      confirmed->second.end(), inputs.begin() + NP_AUX_OFFSET);
    }
    m_np_used[x] = inputs;
    if (!SaveRollbackState(x))   // no-op for the anchor, re-saves the rest
    {
        m_np_replaying = false;
        m_np_replay_mute_video = false;
        return false;
    }
    m_np_replay_mute_video = (x != current - 1);
    ApplyNetplayInputs(inputs, mask);
    ApplyNetplayAux(inputs);
    RunNetplayFrame(x);
    if (m_np_crc_interval > 0 && (x + 1) % m_np_crc_interval == 0)
    {
        bool ok = false;
        uint32_t crc = ComputeNetplayCrc(ok);
        if (ok)
            m_np_crc_pending[x + 1] = crc;
    }
    return true;
}

void Wrapper::RollbackFinishReplay(int64_t to_frame, int64_t current,
                                   std::chrono::steady_clock::time_point rb_t0)
{
    m_np_replaying = false;
    m_np_replay_mute_video = false;
    // Every replayed frame ≤ watermark ran with confirmed inputs.
    m_np_verified = std::min(m_np_watermark, current - 1);
    m_np_rollback_count.fetch_add(1, std::memory_order_relaxed);
    const int64_t depth = current - to_frame;
    m_np_replay_frames.fetch_add(depth, std::memory_order_relaxed);
    m_np_replay_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - rb_t0).count(), std::memory_order_relaxed);
    // How deep a rewind actually goes is what the ring has to be sized for,
    // and it is a measurement rather than a thing to reason about.
    int64_t seen = m_np_max_depth.load(std::memory_order_relaxed);
    while (depth > seen
           && !m_np_max_depth.compare_exchange_weak(seen, depth,
                                                    std::memory_order_relaxed))
    {
    }
}

/// retro_run for netplay frame `frame`, unless this machine is not switched on
/// yet. A held machine still counts the frame, so every member of a session
/// keeps one frame clock; it just runs nothing on it (see SetNetplayPowerOnFrame).
void Wrapper::RunNetplayFrame(int64_t frame)
{
    if (frame < m_np_power_on_frame.load(std::memory_order_relaxed))
        return;
    m_core->retro_run();
    m_core_ran_frame = true;
}

/// What rollback costs here. Read by a probe; safe to call at any time.
godot::Dictionary Wrapper::GetNetplayRollbackStats() const
{
    godot::Dictionary d;
    const int64_t ser_n = m_np_serialize_n.load(std::memory_order_relaxed);
    const int64_t ser_us = m_np_serialize_us.load(std::memory_order_relaxed);
    const int64_t rb_n = m_np_rollback_count.load(std::memory_order_relaxed);
    const int64_t rb_us = m_np_replay_us.load(std::memory_order_relaxed);
    const int64_t rb_f = m_np_replay_frames.load(std::memory_order_relaxed);
    d["serialize_count"] = ser_n;
    d["serialize_us_total"] = ser_us;
    d["serialize_us_mean"] = ser_n > 0 ? double(ser_us) / double(ser_n) : 0.0;
    d["rollback_count"] = rb_n;
    d["replay_us_total"] = rb_us;
    d["replay_us_mean"] = rb_n > 0 ? double(rb_us) / double(rb_n) : 0.0;
    d["replay_frames"] = rb_f;
    d["max_depth"] = m_np_max_depth.load(std::memory_order_relaxed);
    d["state_bytes"] = m_np_state_bytes.load(std::memory_order_relaxed);
    d["state_slots"] = m_np_state_slots.load(std::memory_order_relaxed);
    d["ring_limit"] = static_cast<int64_t>(StateRingDepth());
    d["max_ahead"] = static_cast<int64_t>(m_np_max_ahead);
    return d;
}

void Wrapper::NetplayRollbackIteration(double frame_duration_ms, double& accumulator)
{
    const uint32_t mask = m_np_port_mask.load(std::memory_order_relaxed);
    int64_t frame = m_frame_counter.load(std::memory_order_relaxed);

    // 1. Advance the confirmed watermark and verify executed frames against
    //    confirmations; the first contradiction sets the rollback anchor.
    int64_t rollback_to = -1;
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        while (m_np_inputs.count(m_np_watermark + 1))
            ++m_np_watermark;
        const int64_t verify_upto = std::min(m_np_watermark, frame - 1);
        for (int64_t x = m_np_verified + 1; x <= verify_upto; ++x)
        {
            const uint32_t local_mask = NetplayLocalMaskForFrameLocked(x) & mask;
            auto confirmed = m_np_inputs.find(x);
            auto used = m_np_used.find(x);
            if (confirmed == m_np_inputs.end() || used == m_np_used.end())
            {
                m_np_verified = x;
                continue;
            }
            bool match = true;
            for (uint32_t port = 0; port < 4 && match; ++port)
            {
                // This peer's live local input is authoritative; the assembled
                // confirmation cannot correct it and must not cause a replay.
                if (!(mask & (1u << port)) || (local_mask & (1u << port)))
                    continue;
                match = std::equal(confirmed->second.data() + port * 5,
                                   confirmed->second.data() + port * 5 + 5,
                                   used->second.data() + port * 5);
            }
            if (match)
                match = std::equal(confirmed->second.begin() + NP_AUX_OFFSET,
                                   confirmed->second.end(),
                                   used->second.begin() + NP_AUX_OFFSET);
            if (!match)
            {
                rollback_to = x;
                break;
            }
            m_np_verified = x;
        }
    }
    if (rollback_to >= 0 && !NetplayRollbackReplay(rollback_to, mask))
    {
        FailNetplayRollback("failed to restore frame " + std::to_string(rollback_to));
        return;
    }

    // 2. Speculation throttle: never run more than max_ahead past the last
    //    confirmed frame (waits briefly; the outer loop re-enters and keeps
    //    draining commands / re-checking stop, so shutdown can't deadlock).
    frame = m_frame_counter.load(std::memory_order_relaxed);
    {
        std::unique_lock<std::mutex> lock(m_np_mutex);
        while (m_np_inputs.count(m_np_watermark + 1))
            ++m_np_watermark;
        auto can_run = [&]
        {
            while (m_np_inputs.count(m_np_watermark + 1))
                ++m_np_watermark;
            const bool speculation_ok = frame <= m_np_watermark
                + (m_np_await_anchor ? 0 : m_np_max_ahead);
            const bool disc_due = !m_disc_schedule.empty() && m_disc_schedule.begin()->first <= frame;
            const bool reset_due = !m_reset_schedule.empty() && *m_reset_schedule.begin() <= frame;
            // Disc state lives partly outside retro_serialize. Do not cross a
            // scheduled swap or reset speculatively: wait until this frame is
            // confirmed, apply it once, and it can never sit behind a later
            // rollback anchor. Confirmed is not enough: every frame before it
            // must also have been VERIFIED (step 1). Confirmations arriving
            // during this wait advance the watermark here, and running the op
            // then would leave a misprediction just before it to be found
            // after -- a rewind to behind the op that the replay never
            // re-applies (a swiped e-Reader card simply vanished).
            const bool boundary_ok = (!disc_due && !reset_due)
                || (frame <= m_np_watermark && m_np_verified + 1 >= frame);
            return m_stop_requested.load() || (speculation_ok && boundary_ok);
        };
        if (!can_run())
        {
            m_np_cv.wait_for(lock, std::chrono::milliseconds(4), can_run);
            if (!can_run())
                return;   // still stalled; outer loop spins us back in
        }
        if (m_stop_requested.load())
            return;
    }

    // 3. Pace to the core's fps.
    if (accumulator < frame_duration_ms)
        return;
    if (accumulator > frame_duration_ms * 4.0)
        accumulator = frame_duration_ms * 4.0;
    accumulator -= frame_duration_ms;

    ApplyScheduledDiscOps(frame);
    ApplyScheduledResets(frame);
    const uint32_t local_mask = ApplyScheduledNetplayLocalMask(frame) & mask;
    if (!SaveRollbackState(frame))
    {
        // A core can have nothing to serialize before its first retro_run
        // (mupen64plus-next: the machine lives in a coroutine that the first
        // frame starts). With no anchor behind it a frame cannot be rewound,
        // so run it only once its inputs are CONFIRMED -- a confirmed frame is
        // never rolled back -- and speculate again from the first state.
        if (!m_np_states.empty())
        {
            FailNetplayRollback("core failed to serialize frame " + std::to_string(frame));
            return;
        }
        bool confirmed;
        {
            std::lock_guard<std::mutex> lock(m_np_mutex);
            confirmed = frame <= m_np_watermark;
        }
        if (!confirmed)
        {
            m_np_await_anchor = true;
            return;
        }
    }

    // 4. Build this frame's inputs: live local, confirmed remote if already
    //    here, otherwise hold-last prediction. Record local values for the wire.
    NpFrame inputs{};
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        auto confirmed = m_np_inputs.find(frame);
        for (uint32_t port = 0; port < 4; ++port)
        {
            if (!(mask & (1u << port)))
                continue;
            int32_t* dst = inputs.data() + port * 5;
            if (local_mask & (1u << port))
            {
                std::copy_n(m_np_live_local.data() + port * 5, 5, dst);
                m_np_local_records.push_back(static_cast<int32_t>(frame));
                m_np_local_records.push_back(static_cast<int32_t>(port));
                for (int i = 0; i < 5; ++i)
                    m_np_local_records.push_back(dst[i]);
                if ((m_input_handler->GetPortDevice(port) & RETRO_DEVICE_MASK) == RETRO_DEVICE_MOUSE)
                {
                    // Relative deltas belong to exactly one emulated frame;
                    // buttons remain held until the next main-thread update.
                    m_np_live_local[port * 5 + 1] = 0;
                    m_np_live_local[port * 5 + 2] = 0;
                }
            }
            else if (confirmed != m_np_inputs.end())
            {
                std::copy_n(confirmed->second.data() + port * 5, 5, dst);
            }
            else
            {
                auto prev = m_np_used.find(frame - 1);
                if (prev != m_np_used.end())
                    std::copy_n(prev->second.data() + port * 5, 5, dst);
            }
        }
        if (confirmed != m_np_inputs.end())
            std::copy(confirmed->second.begin() + NP_AUX_OFFSET,
                      confirmed->second.end(), inputs.begin() + NP_AUX_OFFSET);
        // Prune consumed confirmations well behind the verified point.
        m_np_inputs.erase(m_np_inputs.begin(), m_np_inputs.lower_bound(m_np_verified - NP_ROLLBACK_HISTORY));
    }
    m_np_used[frame] = inputs;
    m_np_used.erase(m_np_used.begin(), m_np_used.lower_bound(m_np_verified - NP_ROLLBACK_HISTORY));

    // 5. Run and count. The core + frontend input snapshot was captured above,
    // after any confirmed disc operation and before this frame's inputs.
    ApplyNetplayInputs(inputs, mask);
    ApplyNetplayAux(inputs);
    m_audio_handler->CallAudioBufferStatusCallback();
    RunNetplayFrame(frame);
    const int64_t frame_done = m_frame_counter.fetch_add(1, std::memory_order_relaxed) + 1;

    if (m_np_crc_interval > 0 && frame_done % m_np_crc_interval == 0)
    {
        bool ok = false;
        uint32_t crc = ComputeNetplayCrc(ok);
        if (ok)
            m_np_crc_pending[frame_done] = crc;
    }
    FlushNetplayCrcs();
}

bool Wrapper::SetNetplayRollbackGroup(std::shared_ptr<NetplayRollbackGroup> group, size_t index)
{
    if (m_running && m_thread.joinable())
    {
        LogError("SetNetplayRollbackGroup: only before the core starts.");
        return false;
    }
    m_np_group = std::move(group);
    m_np_group_index = index;
    return true;
}

/// Rollback for one machine of a cabled group: NetplayRollbackIteration, with
/// every decision about the TIMELINE taken for the whole cable at once.
///
/// Each member does its own inputs, its own verification and its own core, as
/// a lone machine does. What changes is that they meet at every frame edge
/// (NetplayRollbackGroup::Rendezvous) and there:
///
///   - the rewind anchor is the EARLIEST mismatch on any member, and every
///     member rewinds to it. A machine whose own inputs were all right still
///     heard the other end of the cable say things it will now not say;
///   - a frame counts as verified only when EVERY member has verified it, for
///     the same reason, so no CRC leaves for a frame the cable could still undo;
///   - the frame runs only if every member can run it, so they never drift
///     apart and the next edge is one instant on the wire again;
///   - the bus is snapshotted at the start of each frame and restored with the
///     cores, which is the part a lone rollback has no way to do.
///
/// A replay stops at every edge too, capturing the bus again as it goes, so the
/// ring always describes the timeline the cores are actually on.
void Wrapper::NetplayGroupIteration(double frame_duration_ms, double& accumulator)
{
    NetplayRollbackGroup& group = *m_np_group;
    const uint32_t mask = m_np_port_mask.load(std::memory_order_relaxed);
    const int64_t frame = m_frame_counter.load(std::memory_order_relaxed);
    auto stop = [this] { return m_stop_requested.load(); };

    if (group.IsBroken())
    {
        FailNetplayRollback("linked rollback ended: " + group.Reason());
        return;
    }

    // Pace BEFORE meeting. The group moves at the pace of its slowest member
    // anyway; arriving early only parks this thread where it drains nothing.
    if (accumulator < frame_duration_ms)
        return;

    // 1. What this member knows: the first contradiction in its own ports, how
    //    far it has checked, and whether it could run this frame.
    NetplayRollbackGroup::Proposal mine;
    mine.frame = frame;
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        while (m_np_inputs.count(m_np_watermark + 1))
            ++m_np_watermark;
        mine.watermark = m_np_watermark;
        mine.verified = m_np_verified;
        const int64_t verify_upto = std::min(m_np_watermark, frame - 1);
        for (int64_t x = m_np_verified + 1; x <= verify_upto; ++x)
        {
            const uint32_t local_mask = NetplayLocalMaskForFrameLocked(x) & mask;
            auto confirmed = m_np_inputs.find(x);
            auto used = m_np_used.find(x);
            if (confirmed == m_np_inputs.end() || used == m_np_used.end())
            {
                mine.verified = x;
                continue;
            }
            bool match = true;
            for (uint32_t port = 0; port < 4 && match; ++port)
            {
                if (!(mask & (1u << port)) || (local_mask & (1u << port)))
                    continue;
                match = std::equal(confirmed->second.data() + port * 5,
                                   confirmed->second.data() + port * 5 + 5,
                                   used->second.data() + port * 5);
            }
            if (match)
                match = std::equal(confirmed->second.begin() + NP_AUX_OFFSET,
                                   confirmed->second.end(),
                                   used->second.begin() + NP_AUX_OFFSET);
            if (!match)
            {
                mine.mismatch = x;
                break;
            }
            mine.verified = x;
        }
        const bool disc_due = !m_disc_schedule.empty() && m_disc_schedule.begin()->first <= frame;
        const bool reset_due = !m_reset_schedule.empty() && *m_reset_schedule.begin() <= frame;
        mine.link_due = !m_link_schedule.empty() && m_link_schedule.begin()->first <= frame;
        const bool boundary_ok = (!disc_due && !reset_due) || frame <= m_np_watermark;
        mine.can_run = frame <= m_np_watermark + m_np_max_ahead && boundary_ok;
    }

    // 2. Meet. Every member hands in its proposal; the last to arrive decides.
    const bool met = group.Rendezvous(
        [&]
        {
            group.proposals.resize(group.members.size());
            group.proposals[m_np_group_index] = mine;
        },
        [&]
        {
            int64_t anchor = -1;
            int64_t verified = std::numeric_limits<int64_t>::max();
            bool run = true;
            bool link_due = false;
            for (const auto& p : group.proposals)
            {
                if (p.frame != frame)
                {
                    group.broken = true;
                    group.reason = "linked machines reached different frames ("
                        + std::to_string(p.frame) + " and " + std::to_string(frame) + ")";
                    return;
                }
                if (p.mismatch >= 0 && (anchor < 0 || p.mismatch < anchor))
                    anchor = p.mismatch;
                verified = std::min(verified, p.verified);
                run = run && p.can_run;
                link_due = link_due || p.link_due;
            }
            // A cable moves only once every frame BEFORE it is confirmed, so no
            // rewind ever reaches back across it: verification has reached
            // frame-1 by now, and any anchor it found is taken first. A rewind
            // TO this frame is fine -- the snapshot below is taken after the
            // cable moved. (Not "this frame confirmed": a player's own input for
            // a frame only exists once that frame has run.)
            if (link_due)
            {
                for (const auto& p : group.proposals)
                    run = run && frame <= p.watermark + 1;
            }
            if (anchor >= 0)
                verified = std::min(verified, anchor - 1);
            group.anchor = anchor;
            group.verified = verified;
            group.run = run && anchor < 0;
            if (anchor >= 0)
            {
                if (!group.RestoreLocked(anchor))
                {
                    group.broken = true;
                    group.reason = "no bus snapshot for frame " + std::to_string(anchor);
                }
            }
            else if (run)
            {
                for (Wrapper* member : group.members)
                {
                    // One that arrives after its frame has gone lands on
                    // whatever frame each peer happens to be at. Refuse it
                    // rather than play on from different cables.
                    std::lock_guard<std::mutex> lock(member->m_np_mutex);
                    if (!member->m_link_schedule.empty()
                        && member->m_link_schedule.begin()->first < frame)
                    {
                        group.broken = true;
                        group.reason = "a cable change for frame "
                            + std::to_string(member->m_link_schedule.begin()->first)
                            + " arrived after it (at " + std::to_string(frame) + ")";
                        return;
                    }
                }
                for (Wrapper* member : group.members)
                    member->ApplyScheduledLinkOps(frame);
                group.CaptureLocked(frame);
            }
        },
        stop);
    if (!met)
    {
        if (!m_stop_requested.load())
            FailNetplayRollback("linked rollback ended: " + group.Reason());
        return;
    }
    // Read before this member can arrive again: nobody decides the next edge
    // until every member, this one included, is back at it.
    const int64_t anchor = group.anchor;
    const int64_t verified = group.verified;
    const bool run = group.run;
    m_np_verified = std::max(m_np_verified, verified);

    // 3. A rewind: every member back to the anchor (the bus already is), then
    //    forward again together, one frame and one edge at a time.
    if (anchor >= 0)
    {
        const auto rb_t0 = std::chrono::steady_clock::now();
        if (m_np_group_index == 0)
            Log("Linked rollback: " + std::to_string(group.members.size()) + " machines back to frame "
                + std::to_string(anchor) + " from " + std::to_string(frame));
        if (!RollbackRestoreAnchor(anchor))
        {
            group.Break("frame " + std::to_string(anchor) + " could not be restored");
            FailNetplayRollback("failed to restore frame " + std::to_string(anchor));
            return;
        }
        for (int64_t x = anchor; x < frame; ++x)
        {
            if (!RollbackReplayFrame(x, frame, mask))
            {
                group.Break("frame " + std::to_string(x) + " could not be replayed");
                FailNetplayRollback("failed to replay frame " + std::to_string(x));
                return;
            }
            if (!group.Rendezvous([&] { group.CaptureLocked(x + 1); }, stop))
            {
                m_np_replaying = false;
                m_np_replay_mute_video = false;
                if (!m_stop_requested.load())
                    FailNetplayRollback("linked rollback ended: " + group.Reason());
                return;
            }
        }
        RollbackFinishReplay(anchor, frame, rb_t0);
        // Verified for the GROUP, not by this member's watermark alone: the
        // next edge re-checks the replayed frames against what is confirmed.
        m_np_verified = anchor - 1;
        FlushNetplayCrcs();
        return;
    }

    if (!run)
    {
        std::unique_lock<std::mutex> lock(m_np_mutex);
        m_np_cv.wait_for(lock, std::chrono::milliseconds(2));
        return;
    }

    // 4. Run the frame, exactly as a lone machine does.
    if (accumulator > frame_duration_ms * 4.0)
        accumulator = frame_duration_ms * 4.0;
    accumulator -= frame_duration_ms;

    ApplyScheduledDiscOps(frame);
    ApplyScheduledResets(frame);
    const uint32_t local_mask = ApplyScheduledNetplayLocalMask(frame) & mask;
    if (!SaveRollbackState(frame))
    {
        group.Break("a linked core failed to serialize");
        FailNetplayRollback("core failed to serialize frame " + std::to_string(frame));
        return;
    }

    NpFrame inputs{};
    {
        std::lock_guard<std::mutex> lock(m_np_mutex);
        auto confirmed = m_np_inputs.find(frame);
        for (uint32_t port = 0; port < 4; ++port)
        {
            if (!(mask & (1u << port)))
                continue;
            int32_t* dst = inputs.data() + port * 5;
            if (local_mask & (1u << port))
            {
                std::copy_n(m_np_live_local.data() + port * 5, 5, dst);
                m_np_local_records.push_back(static_cast<int32_t>(frame));
                m_np_local_records.push_back(static_cast<int32_t>(port));
                for (int i = 0; i < 5; ++i)
                    m_np_local_records.push_back(dst[i]);
                if ((m_input_handler->GetPortDevice(port) & RETRO_DEVICE_MASK) == RETRO_DEVICE_MOUSE)
                {
                    m_np_live_local[port * 5 + 1] = 0;
                    m_np_live_local[port * 5 + 2] = 0;
                }
            }
            else if (confirmed != m_np_inputs.end())
            {
                std::copy_n(confirmed->second.data() + port * 5, 5, dst);
            }
            else
            {
                auto prev = m_np_used.find(frame - 1);
                if (prev != m_np_used.end())
                    std::copy_n(prev->second.data() + port * 5, 5, dst);
            }
        }
        if (confirmed != m_np_inputs.end())
            std::copy(confirmed->second.begin() + NP_AUX_OFFSET,
                      confirmed->second.end(), inputs.begin() + NP_AUX_OFFSET);
        m_np_inputs.erase(m_np_inputs.begin(), m_np_inputs.lower_bound(m_np_verified - NP_ROLLBACK_HISTORY));
    }
    m_np_used[frame] = inputs;
    m_np_used.erase(m_np_used.begin(), m_np_used.lower_bound(m_np_verified - NP_ROLLBACK_HISTORY));

    ApplyNetplayInputs(inputs, mask);
    ApplyNetplayAux(inputs);
    m_audio_handler->CallAudioBufferStatusCallback();
    RunNetplayFrame(frame);
    const int64_t frame_done = m_frame_counter.fetch_add(1, std::memory_order_relaxed) + 1;

    if (m_np_crc_interval > 0 && frame_done % m_np_crc_interval == 0)
    {
        bool ok = false;
        uint32_t crc = ComputeNetplayCrc(ok);
        if (ok)
            m_np_crc_pending[frame_done] = crc;
    }
    FlushNetplayCrcs();
}
}
