#include "Debug.hpp"

#include <godot_cpp/core/print_string.hpp>

using namespace godot;

namespace Xenu
{
// Messages are UTF-8 (paths come in through String::utf8(), cores write UTF-8).
// String(const char*) reads Latin-1, which turned an em dash into an "a" with a
// circumflex and a Japanese ROM name into noise.
void Debug::Log_(const std::string& message, const char* caller)
{
    print_line_rich(String("[color=cyan][") + caller + "][/color] " + String::utf8(message.c_str(), message.size()));
}

void Debug::LogOK_(const std::string& message, const char* caller)
{
    print_line_rich(String("[color=green][") + caller + "][/color] " + String::utf8(message.c_str(), message.size()));
}

void Debug::LogWarning_(const std::string& message, const char* caller)
{
    print_line_rich(String("[color=orange][") + caller + "][/color] " + String::utf8(message.c_str(), message.size()));
}

void Debug::LogError_(const std::string& message, const char* caller)
{
    print_line_rich(String("[color=red][") + caller + "][/color] " + String::utf8(message.c_str(), message.size()));
}
}
