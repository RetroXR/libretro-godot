#include "AudioHandler.hpp"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include "Wrapper.hpp"
#include "Libretro.hpp"
#include "Debug.hpp"

extern "C" {
#include <audio/audio_resampler.h>
}

using namespace godot;

namespace Xenu
{
namespace
{
/// Widest the resampling ratio may be trimmed to steer the sink's depth. Half a
/// percent is far below an audible pitch change and is the same bound RetroArch
/// defaults its rate control to; it is a correction for buffer drift, not a
/// speed control, and the pacing brake still owns the coarse rate.
constexpr double k_drc_max_delta = 0.005;

// How stale a published brake may be before the pacing loop measures the sink
// itself instead. Between pushes the sink only drains and the extrapolation is
// exact, so this is not an accuracy budget; it is the guard for cores that drive
// the single-sample callback and so never reach the batch path that publishes.
constexpr double k_brake_sample_max_age_ms = 100.0;
}

void AudioHandler::SampleCallback(int16_t left, int16_t right)
{
    auto instance = Wrapper::GetCurrentThreadWrapper();
    if (!instance)
    {
        LogError("SampleCallback: Null Instance.");
        return;
    }

    // Rollback replay: this frame's audio already played on the first
    // (mispredicted) run, so re-emitting it would double up.
    if (instance->IsNetplayReplaying())
        return;

    instance->m_audio_handler->m_frames_produced.fetch_add(1, std::memory_order_relaxed);

    // Single-sample path: too short to resample meaningfully, and cores that
    // use it are rare. Push straight through at the core's rate.
    const float frame[2] = { left / 32768.0f, right / 32768.0f };
    instance->m_audio_handler->PushFrames(frame, 1);
}

size_t AudioHandler::SampleBatchCallback(const int16_t* data, size_t frames)
{
    if (!data)
        return frames;

    auto instance = Wrapper::GetCurrentThreadWrapper();
    if (!instance)
    {
        LogError("SampleBatchCallback: Null Instance.");
        return frames;
    }

    AudioHandler* self = instance->m_audio_handler.get();

    // Rollback replay: drop re-run audio (already played on the first run).
    if (instance->IsNetplayReplaying())
        return frames;

    // Emulated-time clock, counted at the core's rate, so before resampling.
    std::lock_guard<std::recursive_mutex> processing_lock(self->m_sink_mutex);
    if (!self->m_accept_audio.load(std::memory_order_relaxed))
        return frames;
    self->m_frames_produced.fetch_add(frames, std::memory_order_relaxed);

    // Sink depth drives both the occupancy the core is told about and the rate trim
    // below, so read it once.
    const uint32_t queued = self->QueuedFrames();

    const uint32_t total = self->EffectiveTotalFrames();
    if (total > 0)
    {
        uint32_t occupancy = static_cast<uint32_t>(100.0f * static_cast<float>(queued) / static_cast<float>(total));
        if (occupancy > 100)
            occupancy = 100;
        self->m_audio_buffer_occupancy.store(occupancy, std::memory_order_relaxed);
        if (total > self->m_audio_buffer_total_frames)
            self->m_audio_buffer_total_frames = total;
    }

    // Dynamic rate control. The pacing brake is one-sided (it only ever holds the
    // core back when the sink is above target), so nothing stops the ring draining
    // when a heavy frame or a main-thread hitch outruns the target fill, and the
    // mixer gets a gap. Resampling fractionally fast while the sink is short and
    // fractionally slow while it is long corrects the depth continuously, instead of
    // only at the coarse grain of running or stalling a whole frame. It also absorbs
    // the drift between a core's nominal rate and the mixer's real one, which are
    // separate crystals and never exactly agree.
    double drc_adjust = 1.0;
    if (self->m_sink_target_frames > 0)
    {
        const double target = static_cast<double>(self->m_sink_target_frames);
        double direction = (target - static_cast<double>(queued)) / target;
        if (direction > 1.0)
            direction = 1.0;
        else if (direction < -1.0)
            direction = -1.0;
        drc_adjust = 1.0 + k_drc_max_delta * direction;
    }

    // s16 interleaved -> float interleaved.
    self->m_in_float.resize(frames * 2);
    for (size_t i = 0; i < frames * 2; ++i)
        self->m_in_float[i] = data[i] / 32768.0f;

    if (self->m_resampler_backend && self->m_resampler)
    {
        const double ratio = self->m_resample_ratio * drc_adjust;
        self->m_last_ratio = ratio;

        // Slack on the output: the sinc resampler can emit a frame or two more
        // than the ratio implies, depending on its internal phase. Sized off the
        // trimmed ratio, not the nominal one, or the trim can overrun the buffer.
        const size_t cap = static_cast<size_t>(frames * ratio) + 32;
        self->m_out_float.resize(cap * 2);

        struct resampler_data rd = {};
        rd.data_in      = self->m_in_float.data();
        rd.data_out     = self->m_out_float.data();
        rd.input_frames = frames;
        rd.ratio        = ratio;
        self->m_resampler_backend->process(self->m_resampler, &rd);
        self->PushFrames(self->m_out_float.data(), rd.output_frames);
    }
    else
    {
        self->PushFrames(self->m_in_float.data(), frames);
    }

    // Measured here, after the push, so it reflects the audio this batch just
    // added. Sampling before the push reads the sink one frame short and the loop
    // then decays that further, which is a brake that never engages.
    self->PublishBrake();

    return frames;
}

void AudioHandler::PushFrames(const float* interleaved, size_t frames)
{
    if (frames == 0 || !m_accept_audio.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::recursive_mutex> lock(m_sink_mutex);
    if (!m_accept_audio.load(std::memory_order_relaxed))
        return;

    if (m_use_sdk)
    {
        Object* mx = LiveMx();
        if (mx == nullptr || m_voice_l < 0)
            return;
        FillPushBuffer(interleaved, frames);
        // Here, and not before the resampler: this point is already past the rate
        // conversion and the DRC rate trim, and Dolphin found a block decoder and
        // time-stretching together "produces bad sound".
        if (m_surround.load(std::memory_order_relaxed) && m_decoder.is_valid())
        {
            m_decoded = m_decoder->call("decode", m_push_buf);
            if (m_decoded.size() == k_surround_channels)
            {
                for (int ch = 0; ch < k_surround_channels; ++ch)
                {
                    if (m_surround_voices[ch] >= 0)
                        mx->call("push_voice_frames", m_surround_voices[ch], m_decoded[ch]);
                }
                return;
            }
            // A decoder that answered with the wrong shape is a bug, not a
            // reason to go quiet: fall through to stereo.
        }
        mx->call("push_stereo_frames", m_voice_l, m_voice_r, m_push_buf,
                   m_channel_mode.load(std::memory_order_relaxed));
        return;
    }

    if (m_audio_stream_generator_playback.is_null())
        return;

    // One push_buffer rather than a push_frame per sample-frame. Each push_frame
    // is a ptrcall across the GDExtension boundary, so a 44.1 kHz core was making
    // about 44,100 of them a second where this makes one per batch - and
    // m_push_buf, which the SDK path above already stages into, was sitting
    // unused on this branch.
    //
    // The one behavioural difference: push_frame was called in a loop with its
    // result ignored, so a full sink dropped individual frames, whereas
    // push_buffer drops the whole batch. Overflow is what the rate control in
    // SampleBatchCallback exists to prevent, and a partial batch is a click
    // either way.
    FillPushBuffer(interleaved, frames);
    m_audio_stream_generator_playback->push_buffer(m_push_buf);
}

void AudioHandler::FillPushBuffer(const float* interleaved, size_t frames)
{
    FillStereoBuffer(m_push_buf, interleaved, frames);
}

void AudioHandler::FillStereoBuffer(PackedVector2Array& buf, const float* interleaved, size_t frames)
{
    // Sized to the batch exactly, because both sinks consume the whole array -
    // this cannot become a grow-only buffer without slicing, which would
    // allocate the saving straight back.
    if (static_cast<size_t>(buf.size()) != frames)
        buf.resize(static_cast<int64_t>(frames));

    Vector2* dst = buf.ptrw();

    // A Vector2 is two reals laid out exactly like one interleaved stereo frame,
    // so in the ordinary single-precision build this is a copy, not a
    // conversion. Guarded rather than asserted: godot-cpp can be built with
    // precision=double, where real_t is 8 bytes and the copy would be wrong.
    if constexpr (sizeof(Vector2) == 2 * sizeof(float))
    {
        memcpy(dst, interleaved, frames * sizeof(Vector2));
    }
    else
    {
        for (size_t i = 0; i < frames; ++i)
            dst[i] = Vector2(interleaved[i * 2], interleaved[i * 2 + 1]);
    }
}

uint32_t AudioHandler::QueuedFrames() const
{
    if (!m_accept_audio.load(std::memory_order_acquire))
        return 0;
    std::lock_guard<std::recursive_mutex> lock(m_sink_mutex);
    if (!m_accept_audio.load(std::memory_order_relaxed))
        return 0;

    if (m_use_sdk)
    {
        Object* mx = LiveMx();
        if (mx == nullptr || m_voice_l < 0)
            return 0;
        const int q = static_cast<int>(mx->call("voice_frames_available", m_voice_l));
        return q > 0 ? static_cast<uint32_t>(q) : 0;
    }
    if (m_audio_stream_generator_playback.is_null())
        return 0;
    const int32_t avail = m_audio_stream_generator_playback->get_frames_available();
    const uint32_t total = m_audio_buffer_total_frames;
    return (avail >= 0 && total > static_cast<uint32_t>(avail)) ? total - static_cast<uint32_t>(avail) : 0;
}

double AudioHandler::MeasureBrakeMs() const
{
    if (m_mix_rate <= 0.0)
        return 0.0;
    if (!m_accept_audio.load(std::memory_order_acquire))
        return 0.0;

    std::lock_guard<std::recursive_mutex> lock(m_sink_mutex);
    if (!m_accept_audio.load(std::memory_order_relaxed))
        return 0.0;

    if (m_use_sdk)
    {
        Object* mx = LiveMx();
        if (mx == nullptr || m_voice_l < 0)
            return 0.0;
        // The voice reports what it still wants against its own target fill, so the
        // target lives in one place rather than being restated here.
        if (static_cast<int>(mx->call("voice_frames_wanted", m_voice_l)) > 0)
            return 0.0;

        const int queued = static_cast<int>(mx->call("voice_frames_available", m_voice_l));
        const double over = static_cast<double>(queued) - static_cast<double>(m_sink_target_frames);
        return over > 0.0 ? 1000.0 * over / m_mix_rate : 0.0;
    }

    if (m_audio_stream_generator_playback.is_null())
        return 0.0;

    // The generator has no target of its own; treat its whole buffer as the target
    // and wait out whatever sits above the half mark.
    const int32_t avail = m_audio_stream_generator_playback->get_frames_available();
    if (avail > 0)
        return 0.0;
    return 500.0 * static_cast<double>(m_audio_buffer_total_frames) / m_mix_rate;
}


void AudioHandler::PublishBrake()
{
    const double brake = MeasureBrakeMs();
    m_brake_at_sample_ms.store(brake, std::memory_order_relaxed);
    // Released last: the pacing loop acquires this, so a non-zero stamp guarantees
    // the brake beside it is the one measured with it.
    m_brake_sampled_at_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_release);
}

double AudioHandler::MsUntilSinkWantsFrames() const
{
    if (m_mix_rate <= 0.0)
        return 0.0;
    if (!m_accept_audio.load(std::memory_order_acquire))
        return 0.0;

    const int64_t sampled_at_ns = m_brake_sampled_at_ns.load(std::memory_order_acquire);
    if (sampled_at_ns != 0)
    {
        const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const double age_ms = static_cast<double>(now_ns - sampled_at_ns) / 1.0e6;
        if (age_ms <= k_brake_sample_max_age_ms)
        {
            // The sink drains at the mixer's rate, so the brake is worth what it was
            // measured at minus the real time since. Decaying it is what stops a held
            // value from re-braking at full strength on every pass and starving the
            // core of the retro_run that would refresh it.
            const double remaining_ms = m_brake_at_sample_ms.load(std::memory_order_relaxed) - age_ms;
            return remaining_ms > 0.0 ? remaining_ms : 0.0;
        }
    }

    return MeasureBrakeMs();
}

uint32_t AudioHandler::EffectiveTotalFrames() const
{
    // Occupancy is reported to the core as a percentage of this, and cores use it to
    // decide frameskip, so it has to answer "how full are you against where the
    // frontend wants you" rather than against any physical size.
    //
    // The voice ring (32768 frames, ~0.68 s) is mostly headroom; the depth is held at
    // the sink's target fill, a small fraction of it. Against the whole ring, sitting
    // exactly on target reads ~6% and trips the core's underrun warning permanently.
    // Against the target alone it reads 100% and looks permanently full. Twice the
    // target puts the setpoint in the middle, which is the shape a core expects:
    // RetroArch's buffer is sized near its own target, so half full is normal there.
    if (m_use_sdk)
        return m_sink_target_frames > 0
             ? m_sink_target_frames * 2
             : static_cast<uint32_t>(m_audio_buffer_capacity_sec * m_mix_rate);

    // The generator's buffer is already about one target deep, so its true size is
    // the right denominator and needs no correction.
    return m_audio_buffer_total_frames;
}

void AudioHandler::Init(float buffer_capacity_sec, double sample_rate)
{
    m_accept_audio.store(false, std::memory_order_release);
    m_sink_ready.store(false, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> sink_lock(m_sink_mutex);
    // Before m_use_sdk and m_mx_id are reset, which is what lets a previous run's
    // controller voices still be found and handed back.
    ReleaseControllerVoices(true);
    // Same reason, and a machine starts on stereo: the set re-applies its audio
    // route after content loads, so nothing is lost by not carrying it over.
    ReleaseSurroundVoices();

    m_audio_buffer_capacity_sec = buffer_capacity_sec;
    m_audio_sample_rate = sample_rate;
    m_frames_produced.store(0, std::memory_order_relaxed);
    m_audio_buffer_occupancy.store(0, std::memory_order_relaxed);
    m_last_brake_ms.store(0.0, std::memory_order_relaxed);
    m_audio_buffer_total_frames = 0;

    AudioServer* audio = AudioServer::get_singleton();
    m_mix_rate = audio ? audio->get_mix_rate() : 48000.0;

    // Prefer Meta XR Audio; fall through silently when the extension or its
    // native library is absent, which is the point of having a fallback.
    m_mx_id = 0;
    m_use_sdk = false;
    Engine* engine = Engine::get_singleton();
    if (engine && engine->has_singleton("MetaXRAudio"))
    {
        Object* mx = engine->get_singleton("MetaXRAudio");
        if (mx && static_cast<bool>(mx->call("is_available")))
        {
            // Two voices: a console's sound comes out of a TV, and a TV has two
            // speakers. GDScript places them; this only owns their lifetime.
            const int l = static_cast<int>(mx->call("create_voice"));
            const int r = (l >= 0) ? static_cast<int>(mx->call("create_voice")) : -1;
            if (l >= 0)
            {
                m_mx_id = static_cast<uint64_t>(mx->get_instance_id());
                m_voice_l = l;
                m_voice_r = r;
                m_use_sdk = true;
            }
        }
    }

    m_sink_target_frames = 0;
    if (Object* mx = m_use_sdk ? LiveMx() : nullptr)
    {
        const double target_ms = static_cast<double>(mx->call("get_target_latency_ms"));
        m_sink_target_frames = static_cast<uint32_t>(target_ms * m_mix_rate / 1000.0);
    }

    if (m_use_sdk)
    {
        // Allocated even when the rates already match. Rate control steers the sink's
        // depth by trimming this ratio, so a core running at the mixer's own rate
        // needs the resampler present at 1:1 or there is no knob to turn. It costs a
        // sinc pass those cores did not pay before.
        const bool rates_differ = m_audio_sample_rate > 0.0 && m_mix_rate > 0.0
                               && static_cast<int>(m_audio_sample_rate) != static_cast<int>(m_mix_rate);

        if (m_audio_sample_rate > 0.0 && m_mix_rate > 0.0)
        {
            m_resample_ratio = m_mix_rate / m_audio_sample_rate;
            m_last_ratio = m_resample_ratio;
            if (!retro_resampler_realloc(&m_resampler, &m_resampler_backend, "sinc",
                                         RESAMPLER_QUALITY_NORMAL, m_resample_ratio))
            {
                m_resampler = nullptr;
                m_resampler_backend = nullptr;

                if (rates_differ)
                {
                    // Mismatched rates cannot be pushed straight through, so the SDK
                    // path has to be given up entirely, as it always was.
                    LogWarning("AudioHandler: resampler init failed, using Godot panning instead.");
                    if (Object* mx = LiveMx())
                    {
                        mx->call("destroy_voice", m_voice_l);
                        if (m_voice_r >= 0)
                            mx->call("destroy_voice", m_voice_r);
                    }
                    m_voice_l = m_voice_r = -1;
                    m_mx_id = 0;
                    m_use_sdk = false;
                }
                else
                {
                    // At 1:1 the audio still plays correctly straight through; only
                    // the rate trim is lost. Not worth abandoning the spatial path for.
                    LogWarning("AudioHandler: resampler init failed at 1:1, running without rate control.");
                }
            }
        }
        if (m_use_sdk)
        {
            Log("AudioHandler: Meta XR Audio, core " + std::to_string(static_cast<int>(m_audio_sample_rate))
                + " Hz -> " + std::to_string(static_cast<int>(m_mix_rate)) + " Hz, voices "
                + std::to_string(m_voice_l) + "/" + std::to_string(m_voice_r));
            m_sink_ready.store(true, std::memory_order_release);
            m_accept_audio.store(
                m_playing.load(std::memory_order_relaxed) && m_audio_sample_rate > 0.0,
                std::memory_order_release);
            return;
        }
    }

    m_audio_stream_generator.instantiate();
    // Silent cores such as 2048 legitimately declare 0 Hz. Give the dormant
    // generator a valid rate so a later SET_SYSTEM_AV_INFO can enable it.
    m_audio_stream_generator->set_mix_rate(
        m_audio_sample_rate > 0.0 ? m_audio_sample_rate : m_mix_rate);
    m_audio_stream_generator->set_buffer_length(m_audio_buffer_capacity_sec);

    AudioStreamPlayer3D* player = LivePlayer();
    if (!player)
    {
        LogError("AudioHandler: fallback AudioStreamPlayer3D is missing");
        return;
    }
    player->set_stream(m_audio_stream_generator);
    player->play();

    m_audio_stream_generator_playback = player->get_stream_playback();
    if (m_audio_stream_generator_playback.is_null())
    {
        LogError("AudioHandler: failed to start fallback audio stream");
        player->stop();
        return;
    }
    m_sink_ready.store(true, std::memory_order_release);
    const bool playing = m_playing.load(std::memory_order_relaxed);
    if (!playing || m_audio_sample_rate <= 0.0)
        player->stop();
    m_accept_audio.store(playing && m_audio_sample_rate > 0.0,
                         std::memory_order_release);
}

bool AudioHandler::ReinitSampleRate(double sample_rate)
{
    m_accept_audio.store(false, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> sink_lock(m_sink_mutex);

    if (!m_sink_ready.load(std::memory_order_relaxed))
        return false;

    if (sample_rate == 0.0)
    {
        // No-audio mode is valid libretro timing (the official 2048 core uses
        // it). Preserve the configured backend for a later nonzero rate, but
        // stop/flush it so stale samples cannot pace or sound after the change.
        Object* mx = m_use_sdk ? LiveMx() : nullptr;
        if (mx && m_voice_l >= 0)
        {
            mx->call("flush_voice", m_voice_l);
            if (m_voice_r >= 0)
                mx->call("flush_voice", m_voice_r);
            FlushControllerVoices();
        }
        else if (AudioStreamPlayer3D* player = LivePlayer())
        {
            player->stop();
        }
        m_audio_sample_rate = 0.0;
        m_frames_produced.store(0, std::memory_order_relaxed);
        m_audio_buffer_occupancy.store(0, std::memory_order_relaxed);
        m_last_brake_ms.store(0.0, std::memory_order_relaxed);
        m_audio_buffer_total_frames = 0;
        m_in_float.clear();
        m_out_float.clear();
        return true;
    }

    if (m_use_sdk)
    {
        void* replacement_resampler = nullptr;
        const struct retro_resampler* replacement_backend = nullptr;
        const double replacement_ratio = m_mix_rate / sample_rate;
        const bool rates_differ = static_cast<int>(sample_rate) != static_cast<int>(m_mix_rate);
        if (!retro_resampler_realloc(&replacement_resampler, &replacement_backend, "sinc",
                                     RESAMPLER_QUALITY_NORMAL, replacement_ratio))
        {
            replacement_resampler = nullptr;
            replacement_backend = nullptr;
            if (rates_differ)
            {
                m_accept_audio.store(m_playing.load(std::memory_order_relaxed),
                                     std::memory_order_release);
                return false;
            }
            LogWarning("AudioHandler: resampler reinit failed at 1:1, running without rate control.");
        }

        if (m_resampler && m_resampler_backend)
            m_resampler_backend->free(m_resampler);
        m_resampler = replacement_resampler;
        m_resampler_backend = replacement_backend;
        m_resample_ratio = replacement_ratio;
        m_last_ratio = replacement_ratio;
        // Reallocated at the new ratio by the next block each device sends.
        ReleaseControllerVoices(false);
    }
    else
    {
        AudioStreamPlayer3D* player = LivePlayer();
        if (!player)
        {
            m_sink_ready.store(false, std::memory_order_release);
            return false;
        }

        Ref<AudioStreamGenerator> replacement;
        replacement.instantiate();
        replacement->set_mix_rate(sample_rate);
        replacement->set_buffer_length(m_audio_buffer_capacity_sec);

        const Ref<AudioStreamGenerator> previous_generator = m_audio_stream_generator;
        const Ref<AudioStreamGeneratorPlayback> previous_playback =
            m_audio_stream_generator_playback;
        player->stop();
        player->set_stream(replacement);
        player->play();
        Ref<AudioStreamGeneratorPlayback> replacement_playback =
            player->get_stream_playback();
        if (replacement_playback.is_null())
        {
            player->stop();
            player->set_stream(previous_generator);
            m_audio_stream_generator = previous_generator;
            m_audio_stream_generator_playback = previous_playback;
            bool restored = previous_generator.is_valid() && previous_playback.is_valid();
            if (m_playing.load(std::memory_order_relaxed))
            {
                player->play();
                m_audio_stream_generator_playback =
                    player->get_stream_playback();
                restored = m_audio_stream_generator_playback.is_valid();
            }
            m_sink_ready.store(restored, std::memory_order_release);
            m_accept_audio.store(m_playing.load(std::memory_order_relaxed) && restored,
                                 std::memory_order_release);
            if (!restored)
                LogError("AudioHandler: failed to restore the previous fallback stream");
            return false;
        }

        m_audio_stream_generator = replacement;
        m_audio_stream_generator_playback = replacement_playback;
        if (!m_playing.load(std::memory_order_relaxed))
            player->stop();
    }

    m_audio_sample_rate = sample_rate;
    m_frames_produced.store(0, std::memory_order_relaxed);
    m_audio_buffer_occupancy.store(0, std::memory_order_relaxed);
    m_last_brake_ms.store(0.0, std::memory_order_relaxed);
    m_audio_buffer_total_frames = 0;
    m_in_float.clear();
    m_out_float.clear();
    m_accept_audio.store(m_playing.load(std::memory_order_relaxed) && sample_rate > 0.0,
                         std::memory_order_release);
    return true;
}

Object* AudioHandler::LiveMx() const
{
    if (m_mx_id == 0)
        return nullptr;
    return ObjectDB::get_instance(m_mx_id);
}

AudioStreamPlayer3D* AudioHandler::LivePlayer() const
{
    if (m_audio_stream_player_id == 0)
        return nullptr;
    return Object::cast_to<AudioStreamPlayer3D>(ObjectDB::get_instance(m_audio_stream_player_id));
}

void AudioHandler::SilenceForTeardown()
{
    m_accept_audio.store(false, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> sink_lock(m_sink_mutex);
    Object* mx = m_use_sdk ? LiveMx() : nullptr;
    if (mx && m_voice_l >= 0)
    {
        mx->call("flush_voice", m_voice_l);
        if (m_voice_r >= 0)
            mx->call("flush_voice", m_voice_r);
    }
    FlushControllerVoices();
}

void AudioHandler::DeInit()
{
    m_accept_audio.store(false, std::memory_order_release);
    m_sink_ready.store(false, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> sink_lock(m_sink_mutex);

    ReleaseControllerVoices(true);
    if (Object* mx = m_use_sdk ? LiveMx() : nullptr)
    {
        if (m_voice_l >= 0)
            mx->call("destroy_voice", m_voice_l);
        if (m_voice_r >= 0)
            mx->call("destroy_voice", m_voice_r);
    }
    m_voice_l = m_voice_r = -1;
    m_mx_id = 0;
    m_use_sdk = false;

    if (m_resampler && m_resampler_backend)
        m_resampler_backend->free(m_resampler);
    m_resampler = nullptr;
    m_resampler_backend = nullptr;

    // No stop() here: the player is being freed either way, and ObjectDB still
    // reports a node as alive while its own destructor runs, so the call lands in
    // an already-destroyed Vector<Ref<AudioStreamPlayback>>.

    if (m_audio_stream_generator_playback.is_valid())
    {
        m_audio_stream_generator_playback->stop();
        m_audio_stream_generator_playback.unref();
    }

    if (m_audio_stream_generator.is_valid())
        m_audio_stream_generator.unref();

    // Only free it if it is still there: at application exit the SceneTree may have
    // destroyed this child already, and freeing it twice crashes on the way out.
    if (AudioStreamPlayer3D* player = LivePlayer())
    {
        if (Node* parent = player->get_parent())
            parent->remove_child(player);
        memdelete(player);
    }
    m_audio_stream_player_id = 0;
}

void AudioHandler::SetPlaying(bool playing)
{
    m_playing.store(playing, std::memory_order_release);
    if (!playing)
        m_accept_audio.store(false, std::memory_order_release);

    std::lock_guard<std::recursive_mutex> lock(m_sink_mutex);
    if (m_use_sdk)
    {
        // Nothing to start or stop: a voice with an empty ring is silent. Only
        // flush, so a stale tail cannot replay when the core resumes.
        Object* mx = LiveMx();
        if (!playing && mx && m_voice_l >= 0)
        {
            mx->call("flush_voice", m_voice_l);
            if (m_voice_r >= 0)
                mx->call("flush_voice", m_voice_r);
        }
        if (!playing)
            FlushControllerVoices();
        if (playing && m_audio_sample_rate > 0.0 &&
            m_sink_ready.load(std::memory_order_relaxed) &&
            mx && m_voice_l >= 0)
            m_accept_audio.store(true, std::memory_order_release);
        return;
    }

    AudioStreamPlayer3D* player = LivePlayer();
    if (!player)
        return;
    if (playing && m_audio_sample_rate <= 0.0)
    {
        player->stop();
        return;
    }
    if (playing)
    {
        player->play();
        m_audio_stream_generator_playback = player->get_stream_playback();
        m_accept_audio.store(m_audio_stream_generator_playback.is_valid(), std::memory_order_release);
    }
    else
    {
        player->stop();
    }
}

PackedInt32Array AudioHandler::GetVoiceIds() const
{
    PackedInt32Array ids;
    if (!m_use_sdk)
        return ids;
    // While surround is engaged these are the six the sound comes out of, in the
    // decoder's own order, and GDScript places them from the set's six positions.
    // m_voice_l/m_voice_r are still alive underneath and still carry the brake;
    // they are simply not being pushed to.
    if (m_surround.load(std::memory_order_relaxed) && m_surround_voices[0] >= 0)
    {
        for (int ch = 0; ch < k_surround_channels; ++ch)
            ids.push_back(m_surround_voices[ch]);
        return ids;
    }
    if (m_voice_l >= 0)
    {
        ids.push_back(m_voice_l);
        if (m_voice_r >= 0)
            ids.push_back(m_voice_r);
    }
    return ids;
}

uint32_t AudioHandler::SurroundLatencyFrames() const
{
    return m_surround.load(std::memory_order_relaxed) ? m_surround_latency : 0u;
}

bool AudioHandler::SetSurroundEnabled(bool on)
{
    std::lock_guard<std::recursive_mutex> lock(m_sink_mutex);
    if (!on)
    {
        ReleaseSurroundVoices();
        return false;
    }
    if (m_surround.load(std::memory_order_relaxed))
        return true;
    // The fallback backend has no voices to place, so there is nothing for six
    // channels to come out of. Documented rule: Linux and macOS get no surround.
    if (!m_use_sdk)
        return false;

    Engine* engine = Engine::get_singleton();
    if (engine == nullptr || !engine->has_singleton("SurroundAudio"))
        return false;
    Object* factory = engine->get_singleton("SurroundAudio");
    if (factory == nullptr)
        return false;

    // 1024 at the mixer's rate: N/2 of inherent delay, so 10.7 ms at 48 kHz,
    // against 5.3 at 512 and 42.7 at 4096. Dolphin measured 1024 as having less
    // steering glitch and less crosstalk than 512, and sample_rate/N is the bin
    // width per-bin steering exists to buy — so this is the corner of the trade.
    const int block = 1024;
    Variant made = factory->call("create_decoder", block, static_cast<int>(m_mix_rate));
    Ref<RefCounted> dec = made;
    if (dec.is_null())
        return false;

    if (!AcquireSurroundVoices())
        return false;

    m_decoder = dec;
    // What produces LFE at all, rather than a nicety: without it the sub channel
    // is silent and the band stays in the fronts.
    m_decoder->call("set_bass_redirection", true);
    m_surround_latency = static_cast<uint32_t>(static_cast<int>(m_decoder->call("latency_frames")));
    m_surround.store(true, std::memory_order_release);
    LogOK("Audio: surround engaged, " + std::to_string(block) + "-frame blocks, "
          + std::to_string(m_surround_latency) + " frames of latency");
    return true;
}

bool AudioHandler::AcquireSurroundVoices()
{
    Object* mx = LiveMx();
    if (mx == nullptr)
        return false;
    // The front pair takes voices of its own rather than reusing
    // m_voice_l/m_voice_r, so the stereo path stays byte-identical and the brake
    // goes on measuring the one voice it always has. Six new voices on a mixer
    // that already holds two.
    int made[k_surround_channels];
    for (int ch = 0; ch < k_surround_channels; ++ch)
    {
        made[ch] = static_cast<int>(mx->call("create_voice"));
        if (made[ch] >= 0)
            continue;
        // Out of voices. Hand back the ones already taken and stay on stereo —
        // degrade, never go silent.
        for (int done = 0; done < ch; ++done)
            mx->call("destroy_voice", made[done]);
        LogWarning("Audio: surround refused, the mixer has no voices left");
        return false;
    }
    for (int ch = 0; ch < k_surround_channels; ++ch)
        m_surround_voices[ch] = made[ch];
    return true;
}

void AudioHandler::ReleaseSurroundVoices()
{
    const bool was_on = m_surround.exchange(false, std::memory_order_acq_rel);
    if (Object* mx = LiveMx())
    {
        for (int ch = 0; ch < k_surround_channels; ++ch)
        {
            if (m_surround_voices[ch] >= 0)
                mx->call("destroy_voice", m_surround_voices[ch]);
        }
    }
    for (int ch = 0; ch < k_surround_channels; ++ch)
        m_surround_voices[ch] = -1;
    m_decoder.unref();
    m_decoded = Array();
    m_surround_latency = 0;
    if (was_on)
        Log("Audio: surround released");
}

bool AudioHandler::PushControllerFrames(unsigned port, unsigned index, const int16_t* data, size_t frames)
{
    if (port >= k_controller_ports || index >= k_controller_devices)
        return false;
    if (!data || frames == 0)
        return true;

    std::lock_guard<std::recursive_mutex> lock(m_sink_mutex);
    // No sink yet means the main batch is dropped as well, so the core mixing this
    // in instead would change nothing.
    if (!m_sink_ready.load(std::memory_order_relaxed))
        return true;
    Object* mx = m_use_sdk ? LiveMx() : nullptr;
    if (mx == nullptr)
        return false;
    if (!m_accept_audio.load(std::memory_order_relaxed))
        return true;

    ControllerVoice& cv = m_controller_voices[port * k_controller_devices + index];
    int voice = cv.voice.load(std::memory_order_relaxed);
    if (voice < 0)
    {
        // A device that never makes a sound never costs a voice.
        bool silent = true;
        for (size_t i = 0; i < frames * 2 && silent; ++i)
            silent = data[i] == 0;
        if (silent)
            return true;

        // Safe off the main thread: the main voices already brought the mixer up,
        // so this is only an atomic claim on a free slot.
        voice = static_cast<int>(mx->call("create_voice"));
        if (voice < 0)
            return false;
        cv.voice.store(voice, std::memory_order_release);
        Log("AudioHandler: controller " + std::to_string(port) + "." + std::to_string(index)
            + " plays on voice " + std::to_string(voice));
    }

    const bool rates_differ = static_cast<int>(m_audio_sample_rate) != static_cast<int>(m_mix_rate);
    if (!cv.resampler && m_audio_sample_rate > 0.0 && m_mix_rate > 0.0 &&
        !retro_resampler_realloc(&cv.resampler, &cv.backend, "sinc", RESAMPLER_QUALITY_NORMAL, m_resample_ratio))
    {
        cv.resampler = nullptr;
        cv.backend = nullptr;
    }
    if (!cv.resampler && rates_differ)
        return false;

    cv.in_float.resize(frames * 2);
    for (size_t i = 0; i < frames * 2; ++i)
        cv.in_float[i] = data[i] / 32768.0f;

    const float* out = cv.in_float.data();
    size_t out_frames = frames;
    if (cv.resampler)
    {
        const size_t cap = static_cast<size_t>(frames * m_last_ratio) + 32;
        cv.out_float.resize(cap * 2);
        struct resampler_data rd = {};
        rd.data_in      = cv.in_float.data();
        rd.data_out     = cv.out_float.data();
        rd.input_frames = frames;
        rd.ratio        = m_last_ratio;
        cv.backend->process(cv.resampler, &rd);
        out = cv.out_float.data();
        out_frames = rd.output_frames;
    }
    if (out_frames == 0)
        return true;

    // The mixer plays a voice with no prebuffer and counts an underrun on every
    // block it finds one empty. An empty controller voice is therefore first
    // filled with silence to the main voice's depth, which also lands its sound
    // at the same moment as the game's.
    if (static_cast<int>(mx->call("voice_frames_available", voice)) == 0)
    {
        // Plus the decoder's own delay while surround is engaged. The game's sound
        // is now held back a window before it reaches a voice; a controller voice
        // is not decoded and would otherwise lead the game by exactly that.
        const uint32_t depth = QueuedFrames() + SurroundLatencyFrames();
        if (depth > 0)
        {
            cv.push_buf.resize(static_cast<int64_t>(depth));
            cv.push_buf.fill(Vector2());
            mx->call("push_stereo_frames", voice, -1, cv.push_buf, 0);
        }
    }

    // A right voice of -1 downmixes to the one voice.
    FillStereoBuffer(cv.push_buf, out, out_frames);
    mx->call("push_stereo_frames", voice, -1, cv.push_buf, 0);
    return true;
}

int AudioHandler::GetControllerVoiceId(unsigned port, unsigned index) const
{
    if (port >= k_controller_ports || index >= k_controller_devices)
        return -1;
    return m_controller_voices[port * k_controller_devices + index].voice.load(std::memory_order_acquire);
}

void AudioHandler::ReleaseControllerVoices(bool destroy_voices)
{
    Object* mx = m_use_sdk ? LiveMx() : nullptr;
    for (ControllerVoice& cv : m_controller_voices)
    {
        if (cv.resampler && cv.backend)
            cv.backend->free(cv.resampler);
        cv.resampler = nullptr;
        cv.backend = nullptr;
        if (!destroy_voices)
            continue;
        const int voice = cv.voice.exchange(-1, std::memory_order_acq_rel);
        if (mx && voice >= 0)
            mx->call("destroy_voice", voice);
    }
}

void AudioHandler::FlushControllerVoices()
{
    Object* mx = m_use_sdk ? LiveMx() : nullptr;
    if (!mx)
        return;
    for (const ControllerVoice& cv : m_controller_voices)
    {
        const int voice = cv.voice.load(std::memory_order_acquire);
        if (voice >= 0)
            mx->call("flush_voice", voice);
    }
}

bool AudioHandler::SetAudioBufferStatusCallback(const retro_audio_buffer_status_callback* callback)
{
    m_audio_buffer_status_callback = callback ? callback->callback : nullptr;
    return true;
}

bool AudioHandler::SetMinimumAudioLatency(const uint32_t* minimum_audio_latency)
{
    if (minimum_audio_latency)
        m_minimum_audio_latency = *minimum_audio_latency;
    return true;
}

void AudioHandler::CallAudioBufferStatusCallback()
{
    if (m_audio_buffer_status_callback)
    {
        const uint32_t occupancy = m_audio_buffer_occupancy.load(std::memory_order_relaxed);
        m_audio_buffer_status_callback(true, occupancy, occupancy <= 10);
    }
}
}
