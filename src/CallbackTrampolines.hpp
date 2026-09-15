#pragma once

#include <libretro.h>
#include <cstddef>
#include <cstdint>

namespace Xenu
{
class Wrapper;

class CallbackTrampolines
{
public:
    explicit CallbackTrampolines(Wrapper* wrapper);
    ~CallbackTrampolines();

    CallbackTrampolines(const CallbackTrampolines&) = delete;
    CallbackTrampolines& operator=(const CallbackTrampolines&) = delete;
    CallbackTrampolines(CallbackTrampolines&&) = delete;
    CallbackTrampolines& operator=(CallbackTrampolines&&) = delete;

    bool IsValid() const { return m_code_page != nullptr; }

    retro_environment_t        GetEnvironmentCallback() const;
    retro_video_refresh_t      GetVideoRefreshCallback() const;
    retro_audio_sample_t       GetAudioSampleCallback() const;
    retro_audio_sample_batch_t GetAudioSampleBatchCallback() const;
    retro_input_poll_t         GetInputPollCallback() const;
    retro_input_state_t        GetInputStateCallback() const;
    retro_log_printf_t         GetLogCallback() const;
    retro_open_mic_t           GetOpenMicCallback() const;

    static constexpr int TRAMPOLINE_COUNT = 8;

private:
    enum TrampolineIndex
    {
        IDX_ENVIRONMENT = 0,
        IDX_VIDEO_REFRESH,
        IDX_AUDIO_SAMPLE,
        IDX_AUDIO_SAMPLE_BATCH,
        IDX_INPUT_POLL,
        IDX_INPUT_STATE,
        IDX_LOG,
        IDX_OPEN_MIC,
    };

    void* m_code_page = nullptr;
    size_t m_code_size = 0;
    void* m_entry_points[TRAMPOLINE_COUNT] = {};

    void GenerateTrampolines(Wrapper* wrapper);

#ifdef _WIN32
    uint8_t* EmitTrampolineX64(uint8_t* cursor, void* wrapper_ptr, void* set_tls_func, void* handler);
#elif defined(__aarch64__)
    uint8_t* EmitTrampolineA64(uint8_t* cursor, void* wrapper_ptr, void* set_tls_func, void* handler);
#elif defined(__x86_64__)
    uint8_t* EmitTrampolineSysV(uint8_t* cursor, void* wrapper_ptr, void* set_tls_func, void* handler);
#endif
};
}
