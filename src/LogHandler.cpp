#include "LogHandler.hpp"

#include <godot_cpp/core/print_string.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "Wrapper.hpp"
#include "CallbackTrampolines.hpp"

using namespace godot;

namespace Xenu
{
bool LogHandler::GetLogInterface(retro_log_callback* callback)
{
    if (!callback)
        return true;

    auto* w = Wrapper::GetCurrentThreadWrapper();
    if (w && w->m_trampolines)
        callback->log = w->m_trampolines->GetLogCallback();
    else
        callback->log = LogInterfaceLog;  // fallback to static handler
    return true;
}

void LogHandler::LogInterfaceLog(retro_log_level level, const char* fmt, ...)
{
    auto* w = Wrapper::GetCurrentThreadWrapper();
    if (!w)
        return;
    if (level < w->m_log_handler->m_log_level)
        return;

    va_list args;
    va_start(args, fmt);
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    // A libretro core ends a message with its own newline, because RetroArch
    // prints the text as it arrives. print_line_rich adds one too, which left a
    // blank line under every message from every core that follows the
    // convention.
    size_t length = strnlen(buffer, sizeof(buffer));
    while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r'))
        buffer[--length] = '\0';

    switch (level)
    {
    case RETRO_LOG_DEBUG:
        print_line_rich("[color=white][Core][Debug]:[/color] " + String(buffer));
        break;
    case RETRO_LOG_INFO:
        print_line_rich("[color=white][Core][Info]:[/color] " + String(buffer));
        break;
    case RETRO_LOG_WARN:
        print_line_rich("[color=orange][Core][Warning]:[/color] " + String(buffer));
        break;
    case RETRO_LOG_ERROR:
        print_line_rich("[color=red][Core][Error]:[/color] " + String(buffer));
        break;
    default:
        break;
    }

    va_end(args);
}
}
