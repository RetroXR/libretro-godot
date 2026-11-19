#pragma once

#include <cstdint>
#include <string>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace Xenu
{
class Wrapper;

/// Commands posted by the main thread and executed on the EMULATION thread,
/// strictly between retro_run() calls (drained at the top of each loop
/// iteration). Mirror of ThreadCommand, which flows the other direction.
class EmuThreadCommand
{
public:
    EmuThreadCommand() = default;
    virtual ~EmuThreadCommand() = default;

    virtual void Execute(Wrapper& wrapper) = 0;

    /// The emulation thread is going away without ever running this command.
    /// A command whose caller is waiting on a signal must answer it here or that
    /// caller waits forever; the rest do nothing, which is why this is not pure.
    virtual void Abandon(Wrapper& wrapper) { (void)wrapper; }
};

/// retro_serialize the core on the emulation thread, then emit
/// savestate_ready(data, frame) on the main thread (empty data on failure).
class EmuThreadCommandSaveState : public EmuThreadCommand
{
public:
    void Execute(Wrapper& wrapper) override;
    void Abandon(Wrapper& wrapper) override;
};

/// retro_unserialize a savestate on the emulation thread, reset the netplay
/// input schedule and frame counter, then emit savestate_loaded(ok).
class EmuThreadCommandLoadState : public EmuThreadCommand
{
public:
    EmuThreadCommandLoadState(godot::PackedByteArray data, int64_t frame)
        : m_data(std::move(data))
        , m_frame(frame)
    {
    }

    void Execute(Wrapper& wrapper) override;
    void Abandon(Wrapper& wrapper) override;

private:
    godot::PackedByteArray m_data;
    int64_t m_frame;
};

/// retro_reset on the emulation thread: the front-panel reset switch. The core
/// stays loaded and its thread keeps running, so nothing is torn down and
/// nothing is joined — a core that owns internal threads (Dolphin) cannot wedge
/// the caller the way a stop/start restart does.
class EmuThreadCommandReset : public EmuThreadCommand
{
public:
    void Execute(Wrapper& wrapper) override;
};

/// retro_set_controller_port_device on the emulation thread: a live plug/
/// unplug while the core runs must not call into the core from the main
/// thread mid-retro_run.
class EmuThreadCommandSetPortDevice : public EmuThreadCommand
{
public:
    EmuThreadCommandSetPortDevice(uint32_t port, uint32_t device)
        : m_port(port)
        , m_device(device)
    {
    }

    void Execute(Wrapper& wrapper) override;

private:
    uint32_t m_port;
    uint32_t m_device;
};

/// Change and persist a core option between retro_run calls. The core may hold
/// the returned value pointer until its next query, so the options map must
/// never be mutated concurrently from the main thread.
class EmuThreadCommandSetCoreOption : public EmuThreadCommand
{
public:
    EmuThreadCommandSetCoreOption(std::string key, std::string value)
        : m_key(std::move(key))
        , m_value(std::move(value))
    {
    }

    void Execute(Wrapper& wrapper) override;

private:
    std::string m_key;
    std::string m_value;
};

/// Update keyboard poll state and invoke the core's event callback between
/// frames. Calling the callback directly from Godot's main thread can re-enter
/// a core while retro_run is active.
class EmuThreadCommandKeyboardEvent : public EmuThreadCommand
{
public:
    EmuThreadCommandKeyboardEvent(uint32_t port, uint32_t keycode, bool down, uint32_t character)
        : m_port(port)
        , m_keycode(keycode)
        , m_down(down)
        , m_character(character)
    {
    }

    void Execute(Wrapper& wrapper) override;

private:
    uint32_t m_port;
    uint32_t m_keycode;
    bool m_down;
    uint32_t m_character;
};

/// Hot-swap the SRAM backing file mid-run (a physical memory-card swap):
/// flush the old file, point at the new path, load its content into SAVE_RAM.
class EmuThreadCommandSetSram : public EmuThreadCommand
{
public:
    explicit EmuThreadCommandSetSram(std::string path)
        : m_path(std::move(path))
    {
    }

    void Execute(Wrapper& wrapper) override;

private:
    std::string m_path;
};

/// Hot-swap of the SECOND save region's file (a PlayStation's slot-2 card).
class EmuThreadCommandSetSramB : public EmuThreadCommand
{
public:
    EmuThreadCommandSetSramB(std::string path, unsigned memory_id)
        : m_path(std::move(path)), m_memory_id(memory_id)
    {
    }

    void Execute(Wrapper& wrapper) override;

private:
    std::string m_path;
    unsigned m_memory_id;
};

/// Dirty-check flush of SRAM to its backing file, on demand.
class EmuThreadCommandFlushSram : public EmuThreadCommand
{
public:
    void Execute(Wrapper& wrapper) override;
};

/// Read the core's disk-control state and emit
/// disk_control_ready(has_control, count, current_index, ejected).
class EmuThreadCommandDiskInfo : public EmuThreadCommand
{
public:
    void Execute(Wrapper& wrapper) override;
    void Abandon(Wrapper& wrapper) override;
};

/// Open/close the core's virtual disc tray (set_eject_state), then re-emit
/// disk_control_ready with the updated state.
class EmuThreadCommandSetDiskEjected : public EmuThreadCommand
{
public:
    explicit EmuThreadCommandSetDiskEjected(bool ejected)
        : m_ejected(ejected)
    {
    }

    void Execute(Wrapper& wrapper) override;
    void Abandon(Wrapper& wrapper) override;

private:
    bool m_ejected;
};

/// Read the core's accessory media readers and emit accessory_readers_changed.
class EmuThreadCommandAccessoryReaders : public EmuThreadCommand
{
public:
    void Execute(Wrapper& wrapper) override;
    void Abandon(Wrapper& wrapper) override;
};

/// Put a medium on an accessory reader (inserted) or take it off, then emit
/// accessory_media_done and the readers.
class EmuThreadCommandAccessoryMedia : public EmuThreadCommand
{
public:
    EmuThreadCommandAccessoryMedia(uint32_t reader, bool insert, std::string path)
        : m_reader(reader)
        , m_insert(insert)
        , m_path(std::move(path))
    {
    }

    void Execute(Wrapper& wrapper) override;
    void Abandon(Wrapper& wrapper) override;

private:
    uint32_t m_reader;
    bool m_insert;
    std::string m_path;
};

/// Hand the core a brand-new disc file at image `index`
/// (replace_image_index, RetroArch's "Load New Disc"), then re-emit
/// disk_control_ready. The tray must be open; the caller closes it after.
class EmuThreadCommandReplaceDisk : public EmuThreadCommand
{
public:
    EmuThreadCommandReplaceDisk(uint32_t index, std::string path)
        : m_index(index)
        , m_path(std::move(path))
    {
    }

    void Execute(Wrapper& wrapper) override;
    void Abandon(Wrapper& wrapper) override;

private:
    uint32_t m_index;
    std::string m_path;
};
}
