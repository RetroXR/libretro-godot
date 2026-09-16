#include "Wrapper.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Libretro.hpp"
#include "Debug.hpp"

using namespace godot;

namespace Xenu
{
// ── Battery saves (SRAM) ─────────────────────────────────────────────────────

void Wrapper::SetSramPath(const godot::String& path)
{
    std::string p = path.utf8().get_data();
    if (m_running)
    {
        // Hot-swap on the emulation thread (memory-card insert/remove).
        m_emu_thread_commands_queue.enqueue(std::make_unique<EmuThreadCommandSetSram>(p));
        Log("SRAM: hot-swap queued to " + (p.empty() ? std::string("<none>") : p));
        return;
    }
    m_sram_path = p;
    Log("SRAM: path set to " + (p.empty() ? std::string("<none>") : p));
}

void Wrapper::SetSramData(const godot::PackedByteArray& data)
{
    m_sram_pending = data;
    Log("SRAM: " + std::to_string(data.size()) + " injected bytes pending for the next content load");
}

void Wrapper::SetRemovableStorage(bool removable)
{
    m_removable_storage = removable;
}

void Wrapper::RequestSramFlush()
{
    if (m_core && m_running)
    {
        m_emu_thread_commands_queue.enqueue(std::make_unique<EmuThreadCommandFlushSram>());
        return;
    }
    Log("SRAM: flush requested with no running core - nothing to flush");
}

/// Emu thread: fill SAVE_RAM from the pending bytes (netplay) or the backing
/// file, then snapshot the shadow copy used for dirty checks.
void Wrapper::LoadSramFromSource()
{
    m_sram_shadow.clear();
    if (!m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return;
    void* sram = m_core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = m_core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (sram == nullptr || size == 0)
    {
        // A path with no region behind it is the bsnes shape: the core saves
        // through its own VFS and nothing written here will ever reach disk.
        if (!m_sram_path.empty() || m_sram_pending.size() > 0)
            LogWarning("SRAM: core exposes no SAVE_RAM region - " +
                       (m_sram_path.empty() ? std::string("injected bytes dropped")
                                            : m_sram_path + " will never be written"));
        return;
    }

    if (m_sram_pending.size() > 0)
    {
        // Netplay-injected content: every peer boots with identical SRAM.
        size_t n = std::min(size, static_cast<size_t>(m_sram_pending.size()));
        std::memcpy(sram, m_sram_pending.ptr(), n);
        Log("SRAM: applied " + std::to_string(n) + " injected bytes (netplay)");
    }
    else if (!m_sram_path.empty() && std::filesystem::is_regular_file(m_sram_path))
    {
        std::ifstream file(m_sram_path, std::ios::binary | std::ios::ate);
        if (file)
        {
            size_t file_size = static_cast<size_t>(file.tellg());
            file.seekg(0, std::ios::beg);
            size_t n = std::min(size, file_size);
            file.read(reinterpret_cast<char*>(sram), n);
            if (!file)
                LogError("SRAM: short read from " + m_sram_path + " (" +
                         std::to_string(file.gcount()) + " of " + std::to_string(n) + " bytes)");
            else
                Log("SRAM: loaded " + std::to_string(n) + " bytes from " + m_sram_path);
            if (file_size != size)
                LogWarning("SRAM: " + m_sram_path + " is " + std::to_string(file_size) +
                           " bytes but the core's region is " + std::to_string(size));
        }
        else
        {
            LogError("SRAM: cannot open " + m_sram_path + " for reading");
        }
    }
    else if (!m_sram_path.empty())
    {
        Log("SRAM: no file yet at " + m_sram_path + " (fresh save, " +
            std::to_string(size) + " byte region)");
    }
    else if (m_removable_storage)
    {
        // Nothing plugged in. Cores hand back their own idea of an empty save,
        // and pcsx_rearmed's is a fully FORMATTED card that a game will write to
        // and then lose at power-off. Blank it so the machine reports
        // unformatted media instead.
        std::memset(sram, 0, size);
        Log("SRAM: no removable media seated - SAVE_RAM blanked");
    }

    m_sram_shadow.assign(static_cast<uint8_t*>(sram), static_cast<uint8_t*>(sram) + size);
}

/// Emu thread: write SAVE_RAM to the backing file iff it changed since the
/// last flush. Never deletes or truncates an existing file to nothing.
void Wrapper::FlushSramIfDirty(bool final_flush)
{
    if (m_sram_path.empty() || !m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return;
    void* sram = m_core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = m_core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (sram == nullptr || size == 0)
        return;
    if (m_sram_shadow.size() == size &&
        std::memcmp(m_sram_shadow.data(), sram, size) == 0)
    {
        // Only said at shutdown: the periodic check runs every 600 frames and
        // "unchanged" is its normal answer.
        if (final_flush)
            Log("SRAM: unchanged since last flush, " + m_sram_path + " left as is");
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_sram_path).parent_path(), ec);
    if (ec)
        LogWarning("SRAM: cannot create directory for " + m_sram_path + " - " + ec.message());
    std::ofstream file(m_sram_path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        LogError("SRAM: cannot write " + m_sram_path);
        return;
    }
    file.write(static_cast<const char*>(sram), size);
    file.close();
    if (!file)
    {
        // The shadow is deliberately NOT updated: the next check sees the same
        // dirty bytes and tries again rather than believing a truncated file.
        LogError("SRAM: short write to " + m_sram_path + " (" + std::to_string(size) +
                 " bytes wanted); will retry on the next flush");
        return;
    }
    m_sram_shadow.assign(static_cast<uint8_t*>(sram), static_cast<uint8_t*>(sram) + size);
    Log("SRAM: flushed " + std::to_string(size) + " bytes to " + m_sram_path +
        (final_flush ? " (final)" : ""));

    // Closed above so the file is complete on disk before anyone is told about
    // it; a listener that uploads must never read a half-written save.
    if (Libretro* node = LiveLibretroNode())
        node->NotifySramFlushed(
            godot::String(m_sram_path.c_str()), static_cast<int64_t>(size), final_flush);
}

/// Emu thread: memory-card hot-swap. Flush the old card, adopt the new one.
void Wrapper::ApplySramSwap(const std::string& new_path)
{
    // Final for the outgoing file: nothing will write to it again this run, so
    // a listener should treat it as committed rather than debounce it.
    FlushSramIfDirty(true);
    m_sram_path = new_path;
    m_sram_pending = godot::PackedByteArray();
    LoadSramFromSource();
    Log("SRAM: swapped to " + (new_path.empty() ? std::string("<none>") : new_path));
}

/// snes9x's id for the BS-X 8M Memory Pack: 1 MB of removable flash in the BS-X
/// cartridge's own slot. Core-specific, so it is not in libretro.h.
///
/// Deliberately NOT index 2. That one is PRAM, which names the cartridge's
/// 512 KB PSRAM -- a different chip, a different size, and not where a download
/// is stored. The pack was read from there while nothing implemented the real
/// PSRAM, which is right data behind a wrong label; both sides moved to 7.
static constexpr unsigned RETRO_MEMORY_SNES_BSX_PACK = (7 << 8) | RETRO_MEMORY_SAVE_RAM;

void Wrapper::SetPackPath(const godot::String& path)
{
    m_pack_path = path.utf8().get_data();
}

/// Emu thread: remember the pack as the content load left it.
///
/// There is deliberately no read from disk here, unlike SRAM. The pack IS the
/// content -- the core was handed the .bs and loaded it into flash -- so reading
/// the file back over it would at best be a no-op and at worst overwrite the
/// medium with a stale copy of itself.
void Wrapper::SnapshotPack()
{
    m_pack_shadow.clear();
    if (m_pack_path.empty() || !m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return;
    void* pack = m_core->retro_get_memory_data(RETRO_MEMORY_SNES_BSX_PACK);
    size_t size = m_core->retro_get_memory_size(RETRO_MEMORY_SNES_BSX_PACK);
    if (pack == nullptr || size == 0)
        return;
    m_pack_shadow.assign(static_cast<uint8_t*>(pack), static_cast<uint8_t*>(pack) + size);
    Log("Pack: watching " + std::to_string(size) + " bytes for " + m_pack_path);
}

/// Emu thread: write the pack back over its own file iff the core changed it.
///
/// Written to a temporary beside the target and renamed, because this overwrites
/// the player's medium in place rather than a save file kept alongside it: a
/// half-written pack is a destroyed one, and there is no other copy.
void Wrapper::FlushPackIfDirty(bool final_flush)
{
    if (m_pack_path.empty() || !m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return;
    void* pack = m_core->retro_get_memory_data(RETRO_MEMORY_SNES_BSX_PACK);
    size_t size = m_core->retro_get_memory_size(RETRO_MEMORY_SNES_BSX_PACK);
    if (pack == nullptr || size == 0)
        return;
    if (m_pack_shadow.size() == size &&
        std::memcmp(m_pack_shadow.data(), pack, size) == 0)
    {
        if (final_flush)
            Log("Pack: unchanged, " + m_pack_path + " left as is");
        return;
    }

    std::error_code ec;
    std::filesystem::path target(m_pack_path);
    std::filesystem::path tmp = target;
    tmp += ".part";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            LogError("Pack: cannot write " + tmp.string());
            return;
        }
        file.write(static_cast<const char*>(pack), size);
        if (!file)
        {
            file.close();
            std::filesystem::remove(tmp, ec);
            LogError("Pack: short write to " + tmp.string());
            return;
        }
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec)
    {
        std::filesystem::remove(tmp, ec);
        LogError("Pack: cannot replace " + m_pack_path);
        return;
    }
    m_pack_shadow.assign(static_cast<uint8_t*>(pack), static_cast<uint8_t*>(pack) + size);
    Log("Pack: flushed " + std::to_string(size) + " bytes to " + m_pack_path +
        (final_flush ? " (final)" : ""));

    if (Libretro* node = LiveLibretroNode())
        node->NotifySramFlushed(
            godot::String(m_pack_path.c_str()), static_cast<int64_t>(size), final_flush);
}

/// The A-slot id is deliberately never used for the Sufami Turbo: snes9x answers
/// it and plain RETRO_MEMORY_SAVE_RAM from the same switch case, so slot A is
/// already covered by the ordinary SRAM path above and asking for it twice would
/// write one cartridge's save to two files. pcsx_rearmed is the same shape --
/// SAVE_RAM is card 1, SRAM_B_PCSX_MEMCARD2 is card 2.

void Wrapper::SetSramBPath(const godot::String& path, unsigned memory_id)
{
    std::string p = path.utf8().get_data();
    if (m_running)
    {
        m_emu_thread_commands_queue.enqueue(
            std::make_unique<EmuThreadCommandSetSramB>(p, memory_id));
        Log("SRAM B: hot-swap queued to " + (p.empty() ? std::string("<none>") : p) +
            " (memory id " + std::to_string(memory_id) + ")");
        return;
    }
    m_sram_b_path = p;
    m_sram_b_id = memory_id;
    Log("SRAM B: path set to " + (p.empty() ? std::string("<none>") : p) +
        " (memory id " + std::to_string(memory_id) + ")");
}

/// Emu thread: fill the second region from its file, then snapshot it for the
/// dirty check. Silent and harmless on every machine that has no second region
/// -- the core answers size 0 for the id and there is nothing to do.
void Wrapper::LoadSramBFromSource()
{
    m_sram_b_shadow.clear();
    if (!m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return;
    // No file and nothing removable to blank: this machine has no second region.
    if (m_sram_b_path.empty() && !m_removable_storage)
        return;
    void* sram = m_core->retro_get_memory_data(m_sram_b_id);
    size_t size = m_core->retro_get_memory_size(m_sram_b_id);
    if (sram == nullptr || size == 0)
    {
        // Said out loud, because "no second save was written" has two very
        // different causes and they look identical from outside: the core may
        // have no such region at all, or the game may simply not have touched it
        // yet. Only the first is a fault.
        Log("SRAM B: core reports no second region (nothing to save)");
        return;
    }

    if (!m_sram_b_path.empty())
    {
        Log("SRAM B: watching " + std::to_string(size) + " bytes for " + m_sram_b_path);
        if (std::filesystem::is_regular_file(m_sram_b_path))
        {
            std::ifstream file(m_sram_b_path, std::ios::binary | std::ios::ate);
            if (file)
            {
                size_t file_size = static_cast<size_t>(file.tellg());
                file.seekg(0, std::ios::beg);
                size_t n = std::min(size, file_size);
                file.read(reinterpret_cast<char*>(sram), n);
                if (!file)
                    LogError("SRAM B: short read from " + m_sram_b_path);
                else
                    Log("SRAM B: loaded " + std::to_string(n) + " bytes from " + m_sram_b_path);
                if (file_size != size)
                    LogWarning("SRAM B: " + m_sram_b_path + " is " + std::to_string(file_size) +
                               " bytes but the core's region is " + std::to_string(size));
            }
            else
            {
                LogError("SRAM B: cannot open " + m_sram_b_path + " for reading");
            }
        }
        else
        {
            Log("SRAM B: no file yet at " + m_sram_b_path + " (fresh save)");
        }
    }
    else
    {
        // Same rule as SAVE_RAM's: an empty slot must report unformatted media,
        // not the formatted card pcsx_rearmed hands back when the frontend
        // supplies nothing.
        std::memset(sram, 0, size);
        Log("SRAM B: no removable media seated - region blanked");
    }
    m_sram_b_shadow.assign(static_cast<uint8_t*>(sram), static_cast<uint8_t*>(sram) + size);
}

/// Emu thread: write the second region to its own file iff it changed.
void Wrapper::FlushSramBIfDirty(bool final_flush)
{
    if (m_sram_b_path.empty() || !m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return;
    void* sram = m_core->retro_get_memory_data(m_sram_b_id);
    size_t size = m_core->retro_get_memory_size(m_sram_b_id);
    if (sram == nullptr || size == 0)
        return;
    if (m_sram_b_shadow.size() == size &&
        std::memcmp(m_sram_b_shadow.data(), sram, size) == 0)
    {
        if (final_flush)
            Log("SRAM B: unchanged since last flush, " + m_sram_b_path + " left as is");
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_sram_b_path).parent_path(), ec);
    if (ec)
        LogWarning("SRAM B: cannot create directory for " + m_sram_b_path + " - " + ec.message());
    std::ofstream file(m_sram_b_path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        LogError("SRAM B: cannot write " + m_sram_b_path);
        return;
    }
    file.write(static_cast<const char*>(sram), size);
    file.close();
    if (!file)
    {
        LogError("SRAM B: short write to " + m_sram_b_path + "; will retry on the next flush");
        return;
    }
    m_sram_b_shadow.assign(static_cast<uint8_t*>(sram), static_cast<uint8_t*>(sram) + size);
    Log("SRAM B: flushed " + std::to_string(size) + " bytes to " + m_sram_b_path +
        (final_flush ? " (final)" : ""));

    if (Libretro* node = LiveLibretroNode())
        node->NotifySramFlushed(
            godot::String(m_sram_b_path.c_str()), static_cast<int64_t>(size), final_flush);
}

/// Emu thread: second-region hot-swap, the mirror of ApplySramSwap.
void Wrapper::ApplySramBSwap(const std::string& new_path, unsigned memory_id)
{
    FlushSramBIfDirty(true);
    m_sram_b_path = new_path;
    m_sram_b_id = memory_id;
    LoadSramBFromSource();
    Log("SRAM B: swapped to " + (new_path.empty() ? std::string("<none>") : new_path));
}

// -- Real-time clock (RETRO_MEMORY_RTC) --------------------------------------

void Wrapper::SetRtcPath(const godot::String& path)
{
    std::string p = path.utf8().get_data();
    std::lock_guard<std::mutex> lock(m_rtc_mutex);
    m_rtc_next_path = p;
    Log("RTC: path set to " + (p.empty() ? std::string("<none>") : p) +
        (m_running ? " (from the next content load)" : ""));
}

void Wrapper::LoadRtcFromSource()
{
    {
        std::lock_guard<std::mutex> lock(m_rtc_mutex);
        m_rtc_path = m_rtc_next_path;
    }
    m_rtc_shadow.clear();
    m_rtc_on_disk = false;
    if (m_rtc_path.empty() || !m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return;
    uint8_t* rtc = static_cast<uint8_t*>(m_core->retro_get_memory_data(RETRO_MEMORY_RTC));
    size_t size = m_core->retro_get_memory_size(RETRO_MEMORY_RTC);
    // No clock on this cartridge, or none this core exposes: no file is kept.
    if (rtc == nullptr || size == 0)
        return;

    if (std::filesystem::is_regular_file(m_rtc_path))
    {
        std::ifstream file(m_rtc_path, std::ios::binary | std::ios::ate);
        const size_t file_size = file ? static_cast<size_t>(file.tellg()) : 0;
        std::vector<uint8_t> bytes(size);
        if (!file)
        {
            LogError("RTC: cannot open " + m_rtc_path + " for reading");
        }
        // A clock is the core's own struct: a file of another size is not this
        // core's, and part of one is not a time.
        else if (file_size != size)
        {
            LogWarning("RTC: " + m_rtc_path + " is " + std::to_string(file_size) +
                       " bytes but the core's clock is " + std::to_string(size) +
                       "; the clock starts fresh");
        }
        else if (!file.seekg(0, std::ios::beg).read(reinterpret_cast<char*>(bytes.data()), size))
        {
            LogError("RTC: short read from " + m_rtc_path + "; the clock starts fresh");
        }
        else
        {
            std::memcpy(rtc, bytes.data(), size);
            m_rtc_on_disk = true;
            Log("RTC: loaded " + std::to_string(size) + " bytes from " + m_rtc_path);
        }
    }
    else
    {
        Log("RTC: no file yet at " + m_rtc_path + " (the clock starts fresh)");
    }
    m_rtc_shadow.assign(rtc, rtc + size);
}

void Wrapper::FlushRtcIfDirty(bool final_flush)
{
    if (m_rtc_shadow.empty() || !m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return;
    uint8_t* rtc = static_cast<uint8_t*>(m_core->retro_get_memory_data(RETRO_MEMORY_RTC));
    size_t size = m_core->retro_get_memory_size(RETRO_MEMORY_RTC);
    if (rtc == nullptr || size != m_rtc_shadow.size())
        return;
    // A clock that has never been written is written once even if unchanged: a
    // clock's value is often a start time, and not keeping it restarts the clock
    // at zero every power-on.
    if (m_rtc_on_disk && std::memcmp(m_rtc_shadow.data(), rtc, size) == 0)
        return;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_rtc_path).parent_path(), ec);
    std::ofstream file(m_rtc_path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        LogError("RTC: cannot write " + m_rtc_path);
        return;
    }
    file.write(reinterpret_cast<const char*>(rtc), size);
    file.close();
    if (!file)
    {
        LogError("RTC: short write to " + m_rtc_path + "; will retry on the next flush");
        return;
    }
    m_rtc_shadow.assign(rtc, rtc + size);
    m_rtc_on_disk = true;
    // Only the last write is announced: a clock that counts seconds is dirty at
    // every periodic check.
    if (final_flush)
        Log("RTC: flushed " + std::to_string(size) + " bytes to " + m_rtc_path + " (final)");
}

void Wrapper::SetMemoryDescriptors(const retro_memory_map* memory_maps)
{
    m_memory_descriptors.clear();
    m_memory_addrspaces.clear();
    if (memory_maps == nullptr || memory_maps->descriptors == nullptr)
        return;

    const size_t count = memory_maps->num_descriptors;
    // Both reserved up front. The descriptors hold char* into the strings, so a
    // reallocation of either vector mid-loop would leave dangling pointers behind.
    m_memory_descriptors.reserve(count);
    m_memory_addrspaces.reserve(count);

    for (size_t i = 0; i < count; ++i)
        m_memory_addrspaces.emplace_back(memory_maps->descriptors[i].addrspace
            ? memory_maps->descriptors[i].addrspace : "");

    for (size_t i = 0; i < count; ++i)
    {
        retro_memory_descriptor descriptor = memory_maps->descriptors[i];
        // ptr is the core's own allocation and stays valid for the session, so it
        // is carried across as-is. Only addrspace has to be re-pointed at our copy.
        descriptor.addrspace = m_memory_addrspaces[i].empty()
            ? nullptr : m_memory_addrspaces[i].c_str();
        m_memory_descriptors.push_back(descriptor);
    }

    Log("Memory map: captured " + std::to_string(count) + " descriptor(s)");
}

void Wrapper::GetCoreMemory(uint32_t id, uint8_t*& out_data, size_t& out_size) const
{
    out_data = nullptr;
    out_size = 0;
    if (!m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return;
    out_data = static_cast<uint8_t*>(m_core->retro_get_memory_data(id));
    out_size = m_core->retro_get_memory_size(id);
}

retro_memory_map Wrapper::GetMemoryMap() const
{
    retro_memory_map map = {};
    map.descriptors = m_memory_descriptors.empty() ? nullptr : m_memory_descriptors.data();
    map.num_descriptors = static_cast<unsigned>(m_memory_descriptors.size());
    return map;
}

godot::Dictionary Wrapper::SnapshotMappedRam() const
{
    godot::Dictionary out;
    godot::PackedByteArray data;
    godot::Array regions;
    int64_t offset = 0;
    for (const retro_memory_descriptor& descriptor : m_memory_descriptors)
    {
        if (descriptor.ptr == nullptr || descriptor.len == 0)
            continue;
        if (descriptor.flags & RETRO_MEMDESC_CONST)
            continue;
        const uint8_t* base = static_cast<const uint8_t*>(descriptor.ptr) + descriptor.offset;
        const int64_t len = static_cast<int64_t>(descriptor.len);
        const int64_t at = data.size();
        data.resize(at + len);
        std::memcpy(data.ptrw() + at, base, static_cast<size_t>(len));
        godot::Dictionary region;
        region["offset"] = at;
        region["len"] = len;
        region["start"] = static_cast<int64_t>(descriptor.start);
        region["addrspace"] = descriptor.addrspace ? godot::String(descriptor.addrspace) : godot::String();
        regions.append(region);
        offset += len;
    }
    out["data"] = data;
    out["regions"] = regions;
    return out;
}

// -- Controller Paks: 32 KiB slices of the one SAVE_RAM block ----------------
//
// Ordering is the correctness constraint. LoadSramFromSource fills the WHOLE
// blob from the cartridge .srm, pak bytes included, so every region must be
// overlaid after it or a stale copy of the pak wins. FlushSramIfDirty then keeps
// writing a duplicate of those bytes into the .srm, which is harmless only
// because the overlay always runs last.

void Wrapper::SetSramRegionPath(int index, const godot::String& path, int64_t offset, int64_t length)
{
    if (index < 0 || index >= RETRO_TRANSFER_PAK_PORTS)
        return;
    std::lock_guard<std::mutex> lock(m_sram_region_mutex);
    SramRegion staged;
    staged.path   = path.utf8().get_data();
    staged.offset = offset > 0 ? static_cast<size_t>(offset) : 0;
    staged.length = length > 0 ? static_cast<size_t>(length) : 0;
    m_sram_region_pending[index]     = staged;
    m_sram_region_has_pending[index] = true;
    m_sram_region_dirty.store(true, std::memory_order_release);
}

void Wrapper::ClearSramRegion(int index)
{
    SetSramRegionPath(index, godot::String(), 0, 0);
}

void Wrapper::ApplySramRegionSwaps()
{
    if (!m_sram_region_dirty.exchange(false, std::memory_order_acq_rel))
        return;

    for (int i = 0; i < RETRO_TRANSFER_PAK_PORTS; ++i)
    {
        SramRegion staged;
        {
            std::lock_guard<std::mutex> lock(m_sram_region_mutex);
            if (!m_sram_region_has_pending[i])
                continue;
            staged = m_sram_region_pending[i];
            m_sram_region_has_pending[i] = false;
        }
        if (m_sram_regions[i].path == staged.path &&
            m_sram_regions[i].offset == staged.offset &&
            m_sram_regions[i].length == staged.length)
            continue;

        // Final for the outgoing pak: nothing writes to that file again for this
        // seating, so a pak pulled mid-game keeps what was on it.
        FlushSramRegionIfDirty(i, true);
        m_sram_regions[i].path   = staged.path;
        m_sram_regions[i].offset = staged.offset;
        m_sram_regions[i].length = staged.length;
        m_sram_regions[i].shadow.clear();
        LoadSramRegion(i);
        Log("PAK " + std::to_string(i) + ": bound " +
            (staged.path.empty() ? std::string("<none>") : staged.path));
    }
}

uint8_t* Wrapper::SramRegionWindow(int index, size_t& out_len)
{
    out_len = 0;
    const SramRegion& r = m_sram_regions[index];
    if (r.path.empty() || r.length == 0)
        return nullptr;
    if (!m_core || !m_core->retro_get_memory_data || !m_core->retro_get_memory_size)
        return nullptr;
    void* sram  = m_core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = m_core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (sram == nullptr || size == 0)
        return nullptr;
    if (r.offset + r.length > size)
    {
        // Said out loud: a slip here does not error, it writes a perfectly valid
        // pak image over the EEPROM or over the pak next door, and that is
        // invisible from inside the game.
        LogError("PAK " + std::to_string(index) + ": region " +
                 std::to_string(r.offset) + "+" + std::to_string(r.length) +
                 " does not fit in SAVE_RAM (" + std::to_string(size) + ")");
        return nullptr;
    }
    out_len = r.length;
    return static_cast<uint8_t*>(sram) + r.offset;
}

void Wrapper::LoadSramRegion(int index)
{
    size_t len = 0;
    uint8_t* window = SramRegionWindow(index, len);
    if (window == nullptr)
        return;
    SramRegion& r = m_sram_regions[index];

    std::vector<uint8_t> bytes;
    if (std::filesystem::is_regular_file(r.path))
    {
        std::ifstream file(r.path, std::ios::binary | std::ios::ate);
        if (file)
        {
            std::streamsize on_disk = file.tellg();
            file.seekg(0, std::ios::beg);
            bytes.resize(static_cast<size_t>(on_disk));
            file.read(reinterpret_cast<char*>(bytes.data()), on_disk);
            if (!file)
            {
                LogError("PAK " + std::to_string(index) + ": short read from " + r.path);
                bytes.clear();
            }
        }
        else
        {
            LogError("PAK " + std::to_string(index) + ": cannot open " + r.path + " for reading");
        }
    }

    if (bytes.empty())
    {
        // No image on disk. Leave whatever the cartridge .srm already held: a
        // pak whose file has gone runs unbacked rather than wiping the port.
        r.shadow.assign(window, window + len);
        Log("PAK " + std::to_string(index) + ": no image at " + r.path + " (running unbacked)");
        return;
    }

    size_t n = bytes.size() < len ? bytes.size() : len;
    std::memcpy(window, bytes.data(), n);
    r.shadow.assign(window, window + len);
    Log("PAK " + std::to_string(index) + ": loaded " + std::to_string(n) + " bytes from " + r.path);
    if (bytes.size() != len)
        LogWarning("PAK " + std::to_string(index) + ": " + r.path + " is " +
                   std::to_string(bytes.size()) + " bytes but the region is " + std::to_string(len));
}

void Wrapper::LoadSramRegionsFromSource()
{
    for (int i = 0; i < RETRO_TRANSFER_PAK_PORTS; ++i)
        LoadSramRegion(i);
}

void Wrapper::FlushSramRegionIfDirty(int index, bool final_flush)
{
    size_t len = 0;
    uint8_t* window = SramRegionWindow(index, len);
    if (window == nullptr)
        return;
    SramRegion& r = m_sram_regions[index];
    if (r.shadow.size() == len && std::memcmp(r.shadow.data(), window, len) == 0)
    {
        if (final_flush)
            Log("PAK " + std::to_string(index) + ": unchanged, " + r.path + " left as is");
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(r.path).parent_path(), ec);
    if (ec)
        LogWarning("PAK " + std::to_string(index) + ": cannot create directory for " + r.path +
                   " - " + ec.message());
    std::ofstream file(r.path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        LogError("PAK " + std::to_string(index) + ": cannot write " + r.path);
        return;
    }
    file.write(reinterpret_cast<const char*>(window), len);
    file.close();
    if (!file)
    {
        LogError("PAK " + std::to_string(index) + ": short write to " + r.path +
                 "; will retry on the next flush");
        return;
    }
    r.shadow.assign(window, window + len);
    Log("PAK " + std::to_string(index) + ": flushed " + std::to_string(len) + " bytes to " + r.path +
        (final_flush ? " (final)" : ""));

    if (Libretro* node = LiveLibretroNode())
        node->NotifySramFlushed(
            godot::String(r.path.c_str()), static_cast<int64_t>(len), final_flush);
}

void Wrapper::FlushSramRegionsIfDirty(bool final_flush)
{
    for (int i = 0; i < RETRO_TRANSFER_PAK_PORTS; ++i)
        FlushSramRegionIfDirty(i, final_flush);
}

// -- Transfer Pak media, per port -------------------------------------------

void Wrapper::SetTransferPak(int port, const godot::String& rom_path, const godot::String& ram_path)
{
    if (port < 0 || port >= RETRO_TRANSFER_PAK_PORTS)
        return;
    std::lock_guard<std::mutex> lock(m_transfer_pak_mutex);
    std::string rom = rom_path.utf8().get_data();
    std::string ram = ram_path.utf8().get_data();
    if (m_transfer_pak_rom[port] == rom && m_transfer_pak_ram[port] == ram)
        return;
    m_transfer_pak_rom[port] = rom;
    m_transfer_pak_ram[port] = ram;
    Log("Transfer Pak " + std::to_string(port) + ": rom=" + (rom.empty() ? std::string("<none>") : rom) +
        " ram=" + (ram.empty() ? std::string("<none>") : ram));

    // The CORE opens this file, and it will not create the directory first: it
    // calls write_to_file on the path we hand back and gives up. Every other
    // save in this file is written by the frontend, which is why they all go
    // through create_directories above -- this is the one path handed out for
    // somebody else to open, so it is the one that has to be made ready here.
    //
    // Missed, the symptom is not a missing save. mupen64plus-core cannot write
    // the cartridge's battery, so the pak reports itself unusable and the game
    // says the Transfer Pak is not set properly and to check the connections --
    // which reads like a seating bug in the room, not a mkdir.
    if (!ram.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(ram).parent_path(), ec);
        if (ec)
            LogError("Transfer Pak: cannot create save directory for " + ram);
    }
    // Only a swap made WHILE THE CORE IS RUNNING is a generation change.
    //
    // The core zeroes its own copy of these counters in retro_init and then
    // signals a cartridge swap whenever ours disagrees. The initial set now
    // happens BEFORE StartContent, so that the pak is present when the core
    // first reads it -- but bumping there left the frontend on 1 against the
    // core's 0, and the first poll after boot therefore reported a cartridge
    // swap that never happened.
    //
    // That fires the delayed eject/insert underneath a game already reading the
    // pak, and Pokemon Stadium answers a cartridge pulled mid-transfer with
    // "The Transfer Pak is not set properly. Please turn the N64 Control Deck
    // OFF and check all connections" -- which reads as a seating fault in the
    // room and is actually this counter.
    //
    // A cartridge swapped while the pak stays seated still bumps, which is the
    // case the counter exists for: the core only re-reads on a pak TYPE
    // transition and would otherwise never notice.
    if (m_running)
        ++m_transfer_pak_generation[port];
}

void Wrapper::ClearTransferPak(int port)
{
    SetTransferPakClock(port, godot::String());
    SetTransferPak(port, godot::String(), godot::String());
}

void Wrapper::SetTransferPakClock(int port, const godot::String& rtc_path)
{
    if (port < 0 || port >= RETRO_TRANSFER_PAK_PORTS)
        return;
    std::lock_guard<std::mutex> lock(m_transfer_pak_mutex);
    m_transfer_pak_rtc[port] = rtc_path.utf8().get_data();
}

const char* Wrapper::TransferPakRtcFor(unsigned port)
{
    if (port >= RETRO_TRANSFER_PAK_PORTS)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_transfer_pak_mutex);
    m_transfer_pak_rtc_view[port] = m_transfer_pak_rtc[port];
    return m_transfer_pak_rtc_view[port].empty() ? nullptr : m_transfer_pak_rtc_view[port].c_str();
}

const char* Wrapper::TransferPakRomFor(unsigned port)
{
    if (port >= RETRO_TRANSFER_PAK_PORTS)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_transfer_pak_mutex);
    m_transfer_pak_rom_view[port] = m_transfer_pak_rom[port];
    return m_transfer_pak_rom_view[port].empty() ? nullptr : m_transfer_pak_rom_view[port].c_str();
}

const char* Wrapper::TransferPakRamFor(unsigned port)
{
    if (port >= RETRO_TRANSFER_PAK_PORTS)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_transfer_pak_mutex);
    m_transfer_pak_ram_view[port] = m_transfer_pak_ram[port];
    return m_transfer_pak_ram_view[port].empty() ? nullptr : m_transfer_pak_ram_view[port].c_str();
}

unsigned Wrapper::TransferPakGenerationFor(unsigned port)
{
    if (port >= RETRO_TRANSFER_PAK_PORTS)
        return 0;
    std::lock_guard<std::mutex> lock(m_transfer_pak_mutex);
    return m_transfer_pak_generation[port];
}

}
