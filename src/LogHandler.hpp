#pragma once

#include <libretro.h>

#include <deque>
#include <mutex>
#include <string>

namespace Xenu
{
class LogHandler
{
public:
    bool GetLogInterface(retro_log_callback* callback);
    static void LogInterfaceLog(retro_log_level level, const char* fmt, ...);

    /// The last few ERROR-level lines the core reported, newest last, one per line.
    /// A core that refuses a game usually says why just before (a missing BIOS or
    /// device ROM set names the file), and "this core refused the game" alone leaves
    /// the player nothing to act on. Duplicated lines are kept once.
    std::string RecentErrors(size_t max_lines = 3) const;
    void ClearRecentErrors();

private:
    retro_log_level m_log_level = RETRO_LOG_WARN;
    void NoteError(const std::string& line);

    mutable std::mutex m_recent_mutex;
    std::deque<std::string> m_recent_errors;
};
}
