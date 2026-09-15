#include "CallbackTrampolines.hpp"

#include "Wrapper.hpp"
#include "EnvironmentHandler.hpp"
#include "VideoHandler.hpp"
#include "AudioHandler.hpp"
#include "InputHandler.hpp"
#include "LogHandler.hpp"
#include "MicrophoneHandler.hpp"
#include "Debug.hpp"

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>

namespace Xenu
{

// Handler addresses for every trampolined callback, in TrampolineIndex order.
// These casts are safe for static member functions on all supported compilers.
static void* const k_handlers[] = {
    reinterpret_cast<void*>(&EnvironmentHandler::Callback),
    reinterpret_cast<void*>(&VideoHandler::RefreshCallback),
    reinterpret_cast<void*>(&AudioHandler::SampleCallback),
    reinterpret_cast<void*>(&AudioHandler::SampleBatchCallback),
    reinterpret_cast<void*>(&InputHandler::PollCallback),
    reinterpret_cast<void*>(&InputHandler::StateCallback),
    reinterpret_cast<void*>(&LogHandler::LogInterfaceLog),
    reinterpret_cast<void*>(&MicrophoneHandler::OpenMic),
};
static_assert(sizeof(k_handlers) / sizeof(k_handlers[0]) == CallbackTrampolines::TRAMPOLINE_COUNT,
              "Must match TRAMPOLINE_COUNT");

CallbackTrampolines::CallbackTrampolines(Wrapper* wrapper)
{
    GenerateTrampolines(wrapper);
}

CallbackTrampolines::~CallbackTrampolines()
{
    if (m_code_page)
    {
#ifdef _WIN32
        VirtualFree(m_code_page, 0, MEM_RELEASE);
#else
        munmap(m_code_page, m_code_size);
#endif
        m_code_page = nullptr;
    }
}

// ============================================================================
// x86-64 Windows implementation
// ============================================================================
#ifdef _WIN32

void CallbackTrampolines::GenerateTrampolines(Wrapper* wrapper)
{
    // One 4KB page holds every ~64-byte stub with room to spare
    m_code_size = 4096;
    m_code_page = VirtualAlloc(nullptr, m_code_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m_code_page)
    {
        LogError("Failed to allocate executable memory for trampolines");
        return;
    }

    auto set_tls = reinterpret_cast<void*>(&Wrapper::SetCurrentThreadWrapper);
    uint8_t* cursor = static_cast<uint8_t*>(m_code_page);

    for (int i = 0; i < TRAMPOLINE_COUNT; ++i)
    {
        m_entry_points[i] = cursor;
        cursor = EmitTrampolineX64(cursor, wrapper, set_tls, k_handlers[i]);

        // Align next trampoline to 16 bytes for icache friendliness
        uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
        cursor = reinterpret_cast<uint8_t*>((addr + 15) & ~uintptr_t(15));
    }

    DWORD old_protect;
    if (!VirtualProtect(m_code_page, m_code_size, PAGE_EXECUTE_READ, &old_protect))
    {
        LogError("Failed to make callback trampolines executable: " + std::to_string(GetLastError()));
        VirtualFree(m_code_page, 0, MEM_RELEASE);
        m_code_page = nullptr;
        for (void*& entry : m_entry_points)
            entry = nullptr;
        return;
    }
    if (!FlushInstructionCache(GetCurrentProcess(), m_code_page, m_code_size))
        LogWarning("Failed to flush callback trampoline instruction cache: " + std::to_string(GetLastError()));

    Log("Created " + std::to_string(TRAMPOLINE_COUNT) + " callback trampolines");
}

uint8_t* CallbackTrampolines::EmitTrampolineX64(uint8_t* cursor, void* wrapper_ptr, void* set_tls_func, void* handler)
{
    // Trampoline strategy:
    //   1. Save all 4 potential argument registers (RCX, RDX, R8, R9)
    //   2. Call SetCurrentThreadWrapper(wrapper_ptr)
    //   3. Restore argument registers
    //   4. Tail-jump to the original handler
    //
    // This works for ALL callback signatures including variadic (log) because
    // we restore the exact register state before jumping to the handler.
    //
    // Stack alignment proof:
    //   Entry: RSP ≡ 8 (mod 16), return address was pushed by caller
    //   After 4 pushes (32 bytes): RSP ≡ 8 (mod 16)
    //   After sub rsp, 0x28 (40 bytes): RSP ≡ 0 (mod 16) ✓
    //   (0x20 shadow space + 0x08 alignment padding)

    // push rcx
    *cursor++ = 0x51;
    // push rdx
    *cursor++ = 0x52;
    // push r8
    *cursor++ = 0x41; *cursor++ = 0x50;
    // push r9
    *cursor++ = 0x41; *cursor++ = 0x51;
    // sub rsp, 0x28
    *cursor++ = 0x48; *cursor++ = 0x83; *cursor++ = 0xEC; *cursor++ = 0x28;

    // movabs rcx, <wrapper_ptr>  (first arg to SetCurrentThreadWrapper)
    *cursor++ = 0x48; *cursor++ = 0xB9;
    std::memcpy(cursor, &wrapper_ptr, 8); cursor += 8;

    // movabs rax, <set_tls_func>
    *cursor++ = 0x48; *cursor++ = 0xB8;
    std::memcpy(cursor, &set_tls_func, 8); cursor += 8;

    // call rax
    *cursor++ = 0xFF; *cursor++ = 0xD0;

    // add rsp, 0x28
    *cursor++ = 0x48; *cursor++ = 0x83; *cursor++ = 0xC4; *cursor++ = 0x28;
    // pop r9
    *cursor++ = 0x41; *cursor++ = 0x59;
    // pop r8
    *cursor++ = 0x41; *cursor++ = 0x58;
    // pop rdx
    *cursor++ = 0x5A;
    // pop rcx
    *cursor++ = 0x59;

    // movabs rax, <handler>
    *cursor++ = 0x48; *cursor++ = 0xB8;
    std::memcpy(cursor, &handler, 8); cursor += 8;

    // jmp rax  (tail-call, stack is exactly as the caller left it)
    *cursor++ = 0xFF; *cursor++ = 0xE0;

    return cursor;
}

// ============================================================================
// AArch64 implementation
// ============================================================================
#elif defined(__aarch64__)

static void Emit32(uint8_t*& cursor, uint32_t instr)
{
    std::memcpy(cursor, &instr, 4);
    cursor += 4;
}

void CallbackTrampolines::GenerateTrampolines(Wrapper* wrapper)
{
    long page_size = sysconf(_SC_PAGESIZE);
    m_code_size = static_cast<size_t>(page_size > 0 ? page_size : 4096);
    m_code_page = mmap(nullptr, m_code_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m_code_page == MAP_FAILED)
    {
        m_code_page = nullptr;
        LogError("Failed to mmap executable memory for trampolines");
        return;
    }

    auto set_tls = reinterpret_cast<void*>(&Wrapper::SetCurrentThreadWrapper);
    uint8_t* cursor = static_cast<uint8_t*>(m_code_page);

    for (int i = 0; i < TRAMPOLINE_COUNT; ++i)
    {
        // Align to 8 bytes (required for literal pool loads)
        uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
        cursor = reinterpret_cast<uint8_t*>((addr + 7) & ~uintptr_t(7));

        m_entry_points[i] = cursor;
        cursor = EmitTrampolineA64(cursor, wrapper, set_tls, k_handlers[i]);
    }

    if (mprotect(m_code_page, m_code_size, PROT_READ | PROT_EXEC) != 0)
    {
        LogError("Failed to make callback trampolines executable: " + std::string(std::strerror(errno)));
        munmap(m_code_page, m_code_size);
        m_code_page = nullptr;
        m_code_size = 0;
        for (void*& entry : m_entry_points)
            entry = nullptr;
        return;
    }
    __builtin___clear_cache(
        reinterpret_cast<char*>(m_code_page),
        reinterpret_cast<char*>(m_code_page) + m_code_size
    );

    Log("Created " + std::to_string(TRAMPOLINE_COUNT) + " callback trampolines");
}

uint8_t* CallbackTrampolines::EmitTrampolineA64(uint8_t* cursor, void* wrapper_ptr, void* set_tls_func, void* handler)
{
    // AArch64 trampoline strategy (same as x86-64 but different registers):
    //   1. Save x0-x7 (all potential arg registers) and x30 (link register)
    //   2. Load embedded Wrapper* into x0, call SetCurrentThreadWrapper
    //   3. Restore all registers
    //   4. Tail-branch to original handler
    //
    // Layout: 15 instructions (60 bytes) + 1 NOP pad (4 bytes) + literal pool (24 bytes) = 88 bytes
    // Literal pool must be 8-byte aligned. 15 instructions = 60 bytes → NOT 8-aligned → NOP pad.
    // Pool starts at offset 64 from code_start.

    uint8_t* code_start = cursor;

    // Save registers onto stack (frame = 0x50 = 80 bytes, 16-byte aligned)
    Emit32(cursor, 0xA9BB07E0);  // stp x0, x1, [sp, #-0x50]!
    Emit32(cursor, 0xA9010FE2);  // stp x2, x3, [sp, #0x10]
    Emit32(cursor, 0xA90217E4);  // stp x4, x5, [sp, #0x20]
    Emit32(cursor, 0xA9031FE6);  // stp x6, x7, [sp, #0x30]
    Emit32(cursor, 0xF90023FE);  // str x30,    [sp, #0x40]

    // Load wrapper_ptr and set_tls from literal pool (PC-relative LDR)
    // Pool item 0 (wrapper_ptr) at offset 64. Instruction 5 at offset 20. Delta = 44.
    Emit32(cursor, 0x58000000 | ((44 / 4) << 5) | 0);  // ldr x0, [pc, #44]
    // Pool item 1 (set_tls_func) at offset 72. Instruction 6 at offset 24. Delta = 48.
    Emit32(cursor, 0x58000000 | ((48 / 4) << 5) | 9);  // ldr x9, [pc, #48]

    // Call set_tls
    Emit32(cursor, 0xD63F0120);  // blr x9

    // Restore registers
    Emit32(cursor, 0xF94023FE);  // ldr x30, [sp, #0x40]
    Emit32(cursor, 0xA9431FE6);  // ldp x6, x7, [sp, #0x30]
    Emit32(cursor, 0xA94217E4);  // ldp x4, x5, [sp, #0x20]
    Emit32(cursor, 0xA9410FE2);  // ldp x2, x3, [sp, #0x10]
    Emit32(cursor, 0xA8C507E0);  // ldp x0, x1, [sp], #0x50

    // Pool item 2 (handler) at offset 80. Instruction 13 at offset 52. Delta = 28.
    Emit32(cursor, 0x58000000 | ((28 / 4) << 5) | 9);  // ldr x9, [pc, #28]

    // Tail-branch
    Emit32(cursor, 0xD61F0120);  // br x9

    // NOP pad to align literal pool to 8 bytes (15 instructions = 60 bytes, need 64)
    Emit32(cursor, 0xD503201F);  // nop

    // Pool item 0: wrapper_ptr (offset 64 from code_start)
    std::memcpy(cursor, &wrapper_ptr, 8); cursor += 8;
    // Pool item 1: set_tls_func (offset 72)
    std::memcpy(cursor, &set_tls_func, 8); cursor += 8;
    // Pool item 2: handler (offset 80)
    std::memcpy(cursor, &handler, 8); cursor += 8;

    return cursor;
}

// ============================================================================
// x86-64 System V (Linux) implementation
// ============================================================================
#elif defined(__x86_64__)

void CallbackTrampolines::GenerateTrampolines(Wrapper* wrapper)
{
    long page_size = sysconf(_SC_PAGESIZE);
    m_code_size = static_cast<size_t>(page_size > 0 ? page_size : 4096);
    m_code_page = mmap(nullptr, m_code_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m_code_page == MAP_FAILED)
    {
        m_code_page = nullptr;
        LogError("Failed to mmap executable memory for trampolines");
        return;
    }

    auto set_tls = reinterpret_cast<void*>(&Wrapper::SetCurrentThreadWrapper);
    uint8_t* cursor = static_cast<uint8_t*>(m_code_page);

    for (int i = 0; i < TRAMPOLINE_COUNT; ++i)
    {
        m_entry_points[i] = cursor;
        cursor = EmitTrampolineSysV(cursor, wrapper, set_tls, k_handlers[i]);

        // Align next trampoline to 16 bytes for icache friendliness
        uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
        cursor = reinterpret_cast<uint8_t*>((addr + 15) & ~uintptr_t(15));
    }

    if (mprotect(m_code_page, m_code_size, PROT_READ | PROT_EXEC) != 0)
    {
        LogError("Failed to make callback trampolines executable: " + std::string(std::strerror(errno)));
        munmap(m_code_page, m_code_size);
        m_code_page = nullptr;
        m_code_size = 0;
        for (void*& entry : m_entry_points)
            entry = nullptr;
        return;
    }

    Log("Created " + std::to_string(TRAMPOLINE_COUNT) + " callback trampolines");
}

uint8_t* CallbackTrampolines::EmitTrampolineSysV(uint8_t* cursor, void* wrapper_ptr, void* set_tls_func, void* handler)
{
    // System V AMD64 trampoline. On entry the integer/pointer args are in
    // RDI, RSI, RDX, RCX, R8, R9; float/vector args in XMM0-7; and for a variadic
    // callee (the log callback) AL holds the number of vector regs used. We must
    // call SetCurrentThreadWrapper(wrapper) without disturbing any of those, then
    // tail-jump to the real handler with every argument register intact.
    //
    //   1. Save RDI,RSI,RDX,RCX,R8,R9 + RAX (AL), then XMM0-7
    //   2. RDI = wrapper_ptr; call SetCurrentThreadWrapper (via R11)
    //   3. Restore XMM0-7, then the GP regs
    //   4. Tail-jump to handler via R11 (so the restored RAX/AL survives)
    //
    // Stack alignment:
    //   Entry:                RSP ≡ 8 (mod 16)  (call pushed the return address)
    //   After 7 pushes (56B): RSP ≡ 0 (mod 16)
    //   After sub rsp,128:    RSP ≡ 0 (mod 16)  (aligned for the call) ✓

    // Save GP arg registers + RAX (AL) for variadic callees.
    *cursor++ = 0x57;                    // push rdi
    *cursor++ = 0x56;                    // push rsi
    *cursor++ = 0x52;                    // push rdx
    *cursor++ = 0x51;                    // push rcx
    *cursor++ = 0x41; *cursor++ = 0x50;  // push r8
    *cursor++ = 0x41; *cursor++ = 0x51;  // push r9
    *cursor++ = 0x50;                    // push rax

    // Save XMM0-7 (float/vector args, including variadic float args).
    // sub rsp, 0x80
    *cursor++ = 0x48; *cursor++ = 0x81; *cursor++ = 0xEC;
    *cursor++ = 0x80; *cursor++ = 0x00; *cursor++ = 0x00; *cursor++ = 0x00;
    // movups [rsp+disp8], xmmN: ModRM (mod=01,reg=N,rm=100/SIB), SIB=0x24 (base=rsp)
    static const uint8_t k_xmm_modrm[8] = { 0x44, 0x4C, 0x54, 0x5C, 0x64, 0x6C, 0x74, 0x7C };
    for (int i = 0; i < 8; ++i)
    {
        *cursor++ = 0x0F; *cursor++ = 0x11;
        *cursor++ = k_xmm_modrm[i]; *cursor++ = 0x24; *cursor++ = static_cast<uint8_t>(i * 16);
    }

    // rdi = wrapper_ptr  (first arg to SetCurrentThreadWrapper)
    *cursor++ = 0x48; *cursor++ = 0xBF;  // movabs rdi, imm64
    std::memcpy(cursor, &wrapper_ptr, 8); cursor += 8;
    // r11 = set_tls_func; call r11
    *cursor++ = 0x49; *cursor++ = 0xBB;  // movabs r11, imm64
    std::memcpy(cursor, &set_tls_func, 8); cursor += 8;
    *cursor++ = 0x41; *cursor++ = 0xFF; *cursor++ = 0xD3;  // call r11

    // Restore XMM0-7.
    for (int i = 0; i < 8; ++i)
    {
        *cursor++ = 0x0F; *cursor++ = 0x10;  // movups xmmN, [rsp+disp8]
        *cursor++ = k_xmm_modrm[i]; *cursor++ = 0x24; *cursor++ = static_cast<uint8_t>(i * 16);
    }
    // add rsp, 0x80
    *cursor++ = 0x48; *cursor++ = 0x81; *cursor++ = 0xC4;
    *cursor++ = 0x80; *cursor++ = 0x00; *cursor++ = 0x00; *cursor++ = 0x00;

    // Restore GP arg registers (reverse of the push order above).
    *cursor++ = 0x58;                    // pop rax
    *cursor++ = 0x41; *cursor++ = 0x59;  // pop r9
    *cursor++ = 0x41; *cursor++ = 0x58;  // pop r8
    *cursor++ = 0x59;                    // pop rcx
    *cursor++ = 0x5A;                    // pop rdx
    *cursor++ = 0x5E;                    // pop rsi
    *cursor++ = 0x5F;                    // pop rdi

    // movabs r11, <handler>  (r11 so the restored rax/al survives)
    *cursor++ = 0x49; *cursor++ = 0xBB;
    std::memcpy(cursor, &handler, 8); cursor += 8;
    // jmp r11  (tail-call, stack is exactly as the caller left it)
    *cursor++ = 0x41; *cursor++ = 0xFF; *cursor++ = 0xE3;

    return cursor;
}

#else
#error "CallbackTrampolines: unsupported platform"
#endif

// ============================================================================
// Getters: cast entry points to the appropriate libretro callback types
// ============================================================================

retro_environment_t CallbackTrampolines::GetEnvironmentCallback() const
{
    return reinterpret_cast<retro_environment_t>(m_entry_points[IDX_ENVIRONMENT]);
}

retro_video_refresh_t CallbackTrampolines::GetVideoRefreshCallback() const
{
    return reinterpret_cast<retro_video_refresh_t>(m_entry_points[IDX_VIDEO_REFRESH]);
}

retro_audio_sample_t CallbackTrampolines::GetAudioSampleCallback() const
{
    return reinterpret_cast<retro_audio_sample_t>(m_entry_points[IDX_AUDIO_SAMPLE]);
}

retro_audio_sample_batch_t CallbackTrampolines::GetAudioSampleBatchCallback() const
{
    return reinterpret_cast<retro_audio_sample_batch_t>(m_entry_points[IDX_AUDIO_SAMPLE_BATCH]);
}

retro_input_poll_t CallbackTrampolines::GetInputPollCallback() const
{
    return reinterpret_cast<retro_input_poll_t>(m_entry_points[IDX_INPUT_POLL]);
}

retro_input_state_t CallbackTrampolines::GetInputStateCallback() const
{
    return reinterpret_cast<retro_input_state_t>(m_entry_points[IDX_INPUT_STATE]);
}

retro_log_printf_t CallbackTrampolines::GetLogCallback() const
{
    return reinterpret_cast<retro_log_printf_t>(m_entry_points[IDX_LOG]);
}

retro_open_mic_t CallbackTrampolines::GetOpenMicCallback() const
{
    return reinterpret_cast<retro_open_mic_t>(m_entry_points[IDX_OPEN_MIC]);
}

} // namespace Xenu
