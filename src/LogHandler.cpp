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
void LogHandler::NoteError(const std::string& line)
{
    if (line.empty())
        return;
    std::lock_guard<std::mutex> lock(m_recent_mutex);
    if (!m_recent_errors.empty() && m_recent_errors.back() == line)
        return;
    m_recent_errors.push_back(line.size() > 240 ? line.substr(0, 240) : line);
    while (m_recent_errors.size() > 8)
        m_recent_errors.pop_front();
}

std::string LogHandler::RecentErrors(size_t max_lines) const
{
    std::lock_guard<std::mutex> lock(m_recent_mutex);
    std::string out;
    const size_t n = m_recent_errors.size();
    for (size_t i = n > max_lines ? n - max_lines : 0; i < n; ++i)
    {
        if (!out.empty())
            out += '\n';
        out += m_recent_errors[i];
    }
    return out;
}

void LogHandler::ClearRecentErrors()
{
    std::lock_guard<std::mutex> lock(m_recent_mutex);
    m_recent_errors.clear();
}

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

    // Cores write UTF-8; String(const char*) would read it as Latin-1.
    const String text = String::utf8(buffer, length);

    switch (level)
    {
    case RETRO_LOG_DEBUG:
        print_line_rich("[color=white][Core][Debug]:[/color] " + text);
        break;
    case RETRO_LOG_INFO:
        print_line_rich("[color=white][Core][Info]:[/color] " + text);
        break;
    case RETRO_LOG_WARN:
        print_line_rich("[color=orange][Core][Warning]:[/color] " + text);
        break;
    case RETRO_LOG_ERROR:
        print_line_rich("[color=red][Core][Error]:[/color] " + text);
        w->m_log_handler->NoteError(std::string(buffer, length));
        break;
    default:
        break;
    }

    va_end(args);
}
}
