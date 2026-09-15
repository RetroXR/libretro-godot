#include "MicrophoneHandler.hpp"

#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>

#include "Debug.hpp"
#include "MicrophoneStream.hpp"
#include "Wrapper.hpp"

// libretro.h declares this at global scope.
struct retro_microphone
{
    std::mutex mutex;
    Xenu::MicrophoneHandler* owner = nullptr;
    unsigned rate = 0;
    bool enabled = false;
    Xenu::MicrophoneStream stream;
};

namespace Xenu
{
namespace
{
retro_microphone* Pool()
{
    static retro_microphone* const pool = new retro_microphone[MicrophoneHandler::k_pool_size];
    return pool;
}

int Resolve(const retro_microphone* microphone)
{
    if (!microphone)
        return -1;
    const uintptr_t base = reinterpret_cast<uintptr_t>(Pool());
    const uintptr_t address = reinterpret_cast<uintptr_t>(microphone);
    if (address < base)
        return -1;
    const uintptr_t offset = address - base;
    if (offset % sizeof(retro_microphone) != 0)
        return -1;
    const uintptr_t index = offset / sizeof(retro_microphone);
    return index < MicrophoneHandler::k_pool_size ? static_cast<int>(index) : -1;
}

unsigned CountBits(uint32_t mask)
{
    unsigned count = 0;
    for (; mask; mask &= mask - 1)
        ++count;
    return count;
}
}

MicrophoneHandler::MicrophoneHandler(Wrapper& wrapper)
    : m_wrapper(wrapper)
{
}

MicrophoneHandler::~MicrophoneHandler()
{
    ReleaseAll();
}

bool MicrophoneHandler::GetMicrophoneInterface(retro_microphone_interface* iface)
{
    if (!iface)
        return false;
    if (iface->interface_version != RETRO_MICROPHONE_INTERFACE_VERSION)
    {
        LogWarning("Core asked for microphone interface version " + std::to_string(iface->interface_version));
        return false;
    }

    *iface = {};
    iface->interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;
    const bool trampolined = m_wrapper.m_trampolines && m_wrapper.m_trampolines->IsValid();
    iface->open_mic      = trampolined ? m_wrapper.m_trampolines->GetOpenMicCallback() : &OpenMic;
    iface->close_mic     = &CloseMic;
    iface->get_params    = &GetParams;
    iface->set_mic_state = &SetMicState;
    iface->get_mic_state = &GetMicState;
    iface->read_mic      = &ReadMic;
    return true;
}

retro_microphone_t* MicrophoneHandler::OpenMic(const retro_microphone_params_t* params)
{
    Wrapper* wrapper = Wrapper::GetCurrentThreadWrapper();
    if (!wrapper)
    {
        LogError("open_mic arrived on a thread with no Wrapper");
        return nullptr;
    }
    unsigned rate = params && params->rate ? params->rate : k_default_rate;
    rate = std::clamp(rate, k_min_rate, k_max_rate);
    return wrapper->m_microphone_handler->Open(rate);
}

retro_microphone_t* MicrophoneHandler::Open(unsigned rate)
{
    if (CountBits(m_owned.load(std::memory_order_acquire)) >= k_max_per_core)
    {
        LogWarning("Microphone refused: this core already holds " + std::to_string(k_max_per_core));
        return nullptr;
    }

    retro_microphone* pool = Pool();
    for (unsigned i = 0; i < k_pool_size; ++i)
    {
        retro_microphone& slot = pool[i];
        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            if (slot.owner)
                continue;
            slot.owner = this;
            slot.rate = rate;
            slot.enabled = false;
            slot.stream.Configure(rate, MicrophoneStream::LimitsForRate(rate));
            m_owned.fetch_or(1u << i, std::memory_order_acq_rel);
        }
        LogOK("Microphone opened at " + std::to_string(rate) + " Hz (slot " + std::to_string(i) + ")");
        return &slot;
    }
    LogWarning("Microphone refused: all " + std::to_string(k_pool_size) + " slots are in use");
    return nullptr;
}

void MicrophoneHandler::CloseMic(retro_microphone_t* microphone)
{
    const int index = Resolve(microphone);
    if (index < 0)
    {
        if (microphone)
            LogErrorOnce("close_mic was handed a pointer that is not a microphone");
        return;
    }
    const uint32_t bit = 1u << index;
    retro_microphone& slot = Pool()[index];
    {
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (!slot.owner)
            return;
        slot.owner->m_owned.fetch_and(~bit, std::memory_order_acq_rel);
        slot.owner->m_enabled.fetch_and(~bit, std::memory_order_acq_rel);
        slot.owner = nullptr;
        slot.enabled = false;
        slot.stream.Release();
    }
    Log("Microphone closed (slot " + std::to_string(index) + ")");
}

bool MicrophoneHandler::GetParams(const retro_microphone_t* microphone, retro_microphone_params_t* params)
{
    const int index = Resolve(microphone);
    if (index < 0 || !params)
        return false;
    retro_microphone& slot = Pool()[index];
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (!slot.owner)
        return false;
    params->rate = slot.rate;
    return true;
}

bool MicrophoneHandler::SetMicState(retro_microphone_t* microphone, bool state)
{
    const int index = Resolve(microphone);
    if (index < 0)
        return false;
    const uint32_t bit = 1u << index;
    retro_microphone& slot = Pool()[index];
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (!slot.owner)
            return false;
        if (slot.enabled != state)
        {
            slot.enabled = state;
            slot.stream.Reset();
            if (state)
                slot.owner->m_enabled.fetch_or(bit, std::memory_order_acq_rel);
            else
                slot.owner->m_enabled.fetch_and(~bit, std::memory_order_acq_rel);
            changed = true;
        }
    }
    if (changed)
        Log(std::string("Microphone ") + (state ? "on" : "off") + " (slot " + std::to_string(index) + ")");
    return true;
}

bool MicrophoneHandler::GetMicState(const retro_microphone_t* microphone)
{
    const int index = Resolve(microphone);
    if (index < 0)
        return false;
    retro_microphone& slot = Pool()[index];
    std::lock_guard<std::mutex> lock(slot.mutex);
    return slot.owner && slot.enabled;
}

int MicrophoneHandler::ReadMic(retro_microphone_t* microphone, int16_t* samples, size_t num_samples)
{
    const int index = Resolve(microphone);
    if (index < 0)
        return -1;
    if (num_samples > 0 && !samples)
        return -1;
    retro_microphone& slot = Pool()[index];
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (!slot.owner)
        return -1;
    return slot.stream.Read(samples, num_samples, slot.enabled && !slot.owner->Silenced());
}

void MicrophoneHandler::Push(const godot::PackedVector2Array& frames, double source_rate, float gain)
{
    if (frames.is_empty() || Silenced())
        return;
    if (!std::isfinite(source_rate) || source_rate <= 0.0)
        return;
    gain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 64.0f) : 0.0f;

    const godot::Vector2* data = frames.ptr();
    const size_t count = static_cast<size_t>(frames.size());
    retro_microphone* pool = Pool();
    uint32_t mask = m_enabled.load(std::memory_order_acquire);
    for (unsigned i = 0; mask != 0; ++i, mask >>= 1)
    {
        if (!(mask & 1u))
            continue;
        retro_microphone& slot = pool[i];
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (slot.owner == this && slot.enabled)
            slot.stream.PushStereo(data, count, source_rate, gain);
    }
}

bool MicrophoneHandler::IsActive() const
{
    return m_enabled.load(std::memory_order_acquire) != 0 && !Silenced();
}

void MicrophoneHandler::SetSuspended(bool suspended)
{
    if (m_suspended.exchange(suspended, std::memory_order_acq_rel) != suspended)
        ResetAll();
}

void MicrophoneHandler::ResetAll()
{
    retro_microphone* pool = Pool();
    uint32_t mask = m_owned.load(std::memory_order_acquire);
    for (unsigned i = 0; mask != 0; ++i, mask >>= 1)
    {
        if (!(mask & 1u))
            continue;
        retro_microphone& slot = pool[i];
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (slot.owner == this)
            slot.stream.Reset();
    }
}

void MicrophoneHandler::ReleaseAll()
{
    retro_microphone* pool = Pool();
    unsigned released = 0;
    for (unsigned i = 0; i < k_pool_size; ++i)
    {
        retro_microphone& slot = pool[i];
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (slot.owner != this)
            continue;
        slot.owner = nullptr;
        slot.enabled = false;
        slot.stream.Release();
        ++released;
    }
    m_owned.store(0, std::memory_order_release);
    m_enabled.store(0, std::memory_order_release);
    if (released > 0)
        Log("Released " + std::to_string(released) + " microphone(s) the core left open");
}

bool MicrophoneHandler::Silenced() const
{
    return m_suspended.load(std::memory_order_acquire)
        || m_wrapper.m_netplay_enabled.load(std::memory_order_acquire)
        || m_wrapper.m_np_rollback.load(std::memory_order_acquire);
}
}
