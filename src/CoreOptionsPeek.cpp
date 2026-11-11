#include "CoreOptionsPeek.hpp"

#include <cstdarg>
#include <cstdio>
#include <filesystem>

#include <libretro.h>

#include "Debug.hpp"
#include "DynLib.hpp"
#include "Wrapper.hpp"

namespace Xenu
{
namespace
{
// Everything the environment callback may need to answer from. Its strings must
// outlive every call the core makes, so it lives for the whole peek and the
// c_str()s handed out point into it.
struct PeekContext
{
    OptionsHandler*  options = nullptr;
    PeekDirectories  directories;
    std::string      core_path;
};

// retro_environment_t is a plain C function pointer with no user-data argument,
// so the destination travels in a thread_local, the same pattern Wrapper uses
// for its own callbacks. A peek runs start to finish on the calling thread.
thread_local PeekContext* t_peek = nullptr;

void RETRO_CALLCONV PeekLog(retro_log_level level, const char* fmt, ...)
{
    if (level < RETRO_LOG_WARN || !fmt)
        return;

    va_list args;
    va_start(args, fmt);
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    LogWarning(std::string("[peeked core] ") + buffer);
}

// Reported without being created: browsing the core manager must not leave a tree
// of empty directories behind for cores that were only looked at.
bool AnswerDirectory(const std::string& directory, void* data)
{
    if (data)
        *static_cast<const char**>(data) = directory.c_str();
    return true;
}

bool PeekEnvironment(unsigned cmd, void* data)
{
    PeekContext* peek = t_peek;
    if (!peek || !peek->options)
        return false;

    switch (cmd)
    {
    // Must be answered: a core that gets no version back assumes 0 and falls
    // down to the legacy retro_variable format, which carries no categories, no
    // per-value labels and no declared defaults.
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        return peek->options->GetCoreOptionsVersion(static_cast<uint32_t*>(data));

    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        return peek->options->SetCoreOptionsV2(static_cast<const retro_core_options_v2*>(data));
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        return peek->options->SetCoreOptionsV2Intl(static_cast<const retro_core_options_v2_intl*>(data));
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        return peek->options->SetCoreOptions(static_cast<const retro_core_option_definition*>(data));
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        return peek->options->SetCoreOptions(static_cast<const retro_core_options_intl*>(data));
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        return peek->options->SetVariables(static_cast<const retro_variable*>(data));

    // Left unanswered on purpose. With no stored value the core keeps the default
    // it just declared, which is the value a peek is meant to report.
    case RETRO_ENVIRONMENT_GET_VARIABLE:
        return false;

    // Cores read these out-parameters without checking the return value, so a
    // refusal leaves them holding whatever the pointer was initialised to rather
    // than a "no" (pcsx2 dereferences the system directory during retro_init).
    // The paths are the ones a real run of this core would be given.
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        return AnswerDirectory(peek->directories.system_directory, data);
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        return AnswerDirectory(peek->directories.save_directory, data);
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        return AnswerDirectory(peek->directories.core_assets_directory, data);
    case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
        return AnswerDirectory(peek->core_path, data);

    // Same reasoning: a refused log interface leaves an uninitialised function
    // pointer a core may call anyway.
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        if (data)
            static_cast<retro_log_callback*>(data)->log = PeekLog;
        return true;

    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        if (data)
            *static_cast<unsigned*>(data) = Wrapper::GetLanguage();
        return true;

    default:
        return false;
    }
}
}

bool PeekCoreOptions(const std::string& core_path, const PeekDirectories& directories, OptionsHandler& out)
{
    if (!std::filesystem::is_regular_file(core_path))
    {
        LogError("PeekCoreOptions: core not found: " + core_path);
        return false;
    }

    void* handle = DynLib_Open(core_path.c_str());
    if (!handle)
    {
        LogError("PeekCoreOptions: failed to open core: " + core_path);
        return false;
    }

    auto set_environment = reinterpret_cast<decltype(&::retro_set_environment)>(
        DynLib_Sym(handle, "retro_set_environment"));
    if (!set_environment)
    {
        LogError("PeekCoreOptions: no retro_set_environment in " + core_path);
        DynLib_Close(handle);
        return false;
    }

    PeekContext peek;
    peek.options     = &out;
    peek.directories = directories;
    peek.core_path   = core_path;

    t_peek = &peek;
    set_environment(PeekEnvironment);

    // Most cores register from retro_set_environment, but not all: dosbox_pure,
    // pcsx2, neocd and mednafen_lynx return nothing until retro_init has run.
    // RetroArch reaches its options menu the same way (set_environment, then
    // init), so this is a supported order rather than a trick. It is skipped when
    // the first call already produced the set, since retro_init is far more work
    // than a peek should do by default.
    if (out.GetDefinitions().empty())
    {
        auto core_init = reinterpret_cast<decltype(&::retro_init)>(DynLib_Sym(handle, "retro_init"));
        auto core_deinit = reinterpret_cast<decltype(&::retro_deinit)>(DynLib_Sym(handle, "retro_deinit"));
        if (core_init && core_deinit)
        {
            core_init();
            core_deinit();
        }
    }

    t_peek = nullptr;

    DynLib_Close(handle);

    const size_t count = out.GetDefinitions().size();
    if (count == 0)
    {
        LogWarning("PeekCoreOptions: core registered no options: " + core_path);
        return false;
    }

    Log("PeekCoreOptions: " + std::to_string(count) + " definitions from " + core_path);
    return true;
}
}
