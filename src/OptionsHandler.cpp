#include "OptionsHandler.hpp"

#include <fstream>
#include <filesystem>

#include "Wrapper.hpp"

using namespace godot;

namespace Xenu
{
void OptionsHandler::SetPersistencePath(const std::string& path, bool create_if_missing)
{
    m_persistence_path = path;
    m_create_persistence_if_missing = create_if_missing;
}

std::string OptionsHandler::ResolvePersistencePath() const
{
    if (!m_persistence_path.empty())
        return m_persistence_path;

    auto* wrapper = Wrapper::GetCurrentThreadWrapper();
    if (!wrapper || !wrapper->m_core)
        return {};

    return (std::filesystem::path(wrapper->GetRootDirectory()) / "core_options"
        / (wrapper->m_core->GetName() + ".opt")).string();
}

bool OptionsHandler::GetCoreOptionsVersion(uint32_t* version)
{
    if (!version)
        return true;

    *version = OptionsHandler::SUPPORTED_CORE_OPTIONS_VERSION;
    return true;
}

bool OptionsHandler::GetVariable(retro_variable* variable)
{
    if (!variable || !variable->key)
        return true;

    std::string key(variable->key);
    if (!m_variables.contains(key))
    {
        // libretro.h: the frontend sets value to NULL for an option it does not
        // hold. Leaving the OUT parameter alone hands the core whatever was on
        // its stack.
        variable->value = nullptr;
        return true;
    }

    // NOTE: variable->value is an OUT parameter, and cores pass it UNINITIALIZED
    // (legacy melonds on Android hands us stack garbage). Never read it.
    variable->value = m_variables[key].c_str();
    return true;
}

bool OptionsHandler::GetVariableUpdate(bool* variable_update)
{
    if (!variable_update)
        return true;

    *variable_update = m_variable_update;
    return SetVariableUpdate(false);
}

bool OptionsHandler::SetVariable(const retro_variable* variable)
{
    if (!variable)
        return true;

    if (!variable->key
     || strlen(variable->key) == 0
     || !variable->value
     || strlen(variable->value) == 0
     || !m_variables.contains(variable->key))
        return false;

    m_variables[variable->key] = variable->value;

    return SetVariableUpdate(true);
}

// Legacy (v0) options API, still used by cores that never adopted core options
// v1/v2 (the beetle/mednafen family among them). Each value string is
// "Description; value1|value2|value3" with the FIRST value as the default.
// Synthesize full definitions so the options panel lists these cores too;
// only filling m_variables leaves the UI showing "no options".
bool OptionsHandler::SetVariables(const retro_variable* variables)
{
    m_categories.clear();
    m_definitions.clear();
    m_variables.clear();

    if (!variables)
        return true;

    for (int i = 0; variables[i].key; ++i)
    {
        if (!variables[i].value)
            continue;

        auto parts = String(variables[i].value).split(";", true, 1);
        if (parts.size() < 2)
            continue;

        std::string desc = parts[0].utf8().get_data();
        auto value_list = parts[1].trim_prefix(" ").split("|");
        std::vector<OptionValue> values;
        for (int v = 0; v < value_list.size(); ++v)
            // Brace init, not emplace_back(): OptionValue is an aggregate with
            // no two-argument constructor, and emplace_back's parenthesized
            // construction of aggregates (C++20 P0960) is not implemented by
            // Xcode 15's libc++ construct_at - it fails to compile on macOS.
            if (!value_list[v].is_empty())
                values.push_back({value_list[v].utf8().get_data(), ""});
        if (values.empty())
            continue;

        std::string default_value = values[0].value;
        m_definitions.emplace(variables[i].key,
            OptionDefinition{ desc, "", "", "", "", std::move(values), default_value });
        m_variables.emplace(variables[i].key, default_value);
    }

    DeserializeFromFile();
    ReapplyFrontendValues();

    return true;
}

bool OptionsHandler::SetVariableUpdate(bool update)
{
    m_variable_update = update;
    return true;
}

// Core options v1: like v2 but without categories. Build definitions too, since
// m_variables alone leaves the options panel empty for v1 cores.
bool OptionsHandler::SetCoreOptions(const retro_core_option_definition* definitions)
{
    m_categories.clear();
    m_definitions.clear();
    m_variables.clear();

    if (!definitions)
        return true;

    for (auto definition = definitions; definition->key; ++definition)
    {
        std::vector<OptionValue> values;
        for (auto value = definition->values; value->value; ++value)
            values.emplace_back(value->value, value->label ? value->label : "");

        // A NULL default_value means the FIRST value, the rule the v0 path above
        // applies to its "a|b|c" list. Dropping the key instead leaves it out of
        // m_variables, and then every route to it fails silently: the file value
        // is skipped, SetVariable refuses it and GetVariable never answers.
        // mupen64plus-next declares all 14 of its ParaLLEl-RDP options this way.
        std::string default_value = definition->default_value ? definition->default_value
                                  : (values.empty() ? std::string() : values[0].value);

        m_definitions.emplace(definition->key,
            OptionDefinition{ definition->desc          ? definition->desc : "",
                              "",
                              definition->info          ? definition->info : "",
                              "",
                              "",
                              std::move(values),
                              default_value });

        m_variables[definition->key] = default_value;
    }

    DeserializeFromFile();
    ReapplyFrontendValues();

    return true;
}

bool OptionsHandler::SetCoreOptions(const retro_core_options_intl* options_intl)
{
    if (!options_intl)
        return SetCoreOptions(static_cast<const retro_core_option_definition*>(nullptr));
    return SetCoreOptions(options_intl->local ? options_intl->local : options_intl->us);
}

bool OptionsHandler::SetCoreOptionsV2(const retro_core_options_v2* options)
{
    m_categories.clear();
    m_definitions.clear();
    m_variables.clear();

    if (!options)
        return true;

    // desc and info are optional, and neocd ships every category with a null info.
    // Constructing a std::string from that is undefined behaviour: it corrupts the
    // heap and takes the process down a few allocations later.
    if (options->categories)
        for (auto category = options->categories; category->key; ++category)
            m_categories.emplace(category->key,
                OptionCategory{ category->desc ? category->desc : "",
                                category->info ? category->info : "" });

    if (!options->definitions)
        return true;

    for (auto definition = options->definitions; definition->key; ++definition)
    {
        std::vector<OptionValue> option_definition_values;
        for (auto option_definition_value = definition->values; option_definition_value->value; ++option_definition_value)
            option_definition_values.emplace_back(option_definition_value->value ? option_definition_value->value : "",
                                                  option_definition_value->label ? option_definition_value->label : "");

        // A NULL default_value is the first value here too; see SetCoreOptions.
        std::string default_value = definition->default_value
                                  ? definition->default_value
                                  : (option_definition_values.empty()
                                        ? std::string()
                                        : option_definition_values[0].value);

        m_definitions.emplace(definition->key, OptionDefinition{ definition->desc             ? definition->desc : "",
                                                                 definition->desc_categorized ? definition->desc_categorized : "",
                                                                 definition->info             ? definition->info : "",
                                                                 definition->info_categorized ? definition->info_categorized : "",
                                                                 definition->category_key     ? definition->category_key : "",
                                                                 std::move(option_definition_values),
                                                                 default_value });

        m_variables[definition->key] = default_value;
    }

    DeserializeFromFile();
    ReapplyFrontendValues();

    return true;
}

bool OptionsHandler::SetCoreOptionsV2Intl(const retro_core_options_v2_intl* options)
{
    if (!options)
        return SetCoreOptionsV2(nullptr);
    return SetCoreOptionsV2(options->local ? options->local : options->us);
}

bool OptionsHandler::SetCoreOptionsUpdateDisplayCallback(const retro_core_options_update_display_callback* update_display_callback)
{
    m_core_options_update_display_callback = nullptr;

    if (update_display_callback)
        m_core_options_update_display_callback = update_display_callback->callback;

    return true;
}

void OptionsHandler::SetVariable(const std::string& key, const std::string& value)
{
    if (key.empty())   { LogError("SetVariable: key is empty");   return; }
    if (value.empty()) { LogError("SetVariable: value is empty (key " + key + ")"); return; }
    m_frontend_values[key] = value;
    if (!m_variables.contains(key))
    {
        // Not an error: a frontend-side key the core never declared (a PS2 card
        // slot before the core scanned its directory, say) lands here. It is
        // kept, and applied if a later declaration names it.
        LogWarning("SetVariable: core has not declared '" + key + "', value '" + value + "' held");
        return;
    }

    Log("SetVariable: " + key + " = " + value);
    m_variables[key] = value;
    m_variable_update = true;
    SerializeToFile();
}

void OptionsHandler::ReapplyFrontendValues()
{
    bool applied = false;
    for (const auto& [key, value] : m_frontend_values)
    {
        auto it = m_variables.find(key);
        if (it == m_variables.end() || it->second == value)
            continue;
        Log("SetVariable: " + key + " = " + value + " (held, now declared)");
        it->second = value;
        applied = true;
    }
    if (applied)
    {
        m_variable_update = true;
        SerializeToFile();
    }
}

void OptionsHandler::SerializeToFile()
{
    const std::string resolved_path = ResolvePersistencePath();
    if (resolved_path.empty())
    {
        LogError("SerializeToFile: no options persistence path");
        return;
    }
    std::filesystem::path file_path(resolved_path);
    
    if (!std::filesystem::is_regular_file(file_path))
    {
        if (!std::filesystem::is_directory(file_path.parent_path()))
        {
            std::error_code ec;
            std::filesystem::create_directories(file_path.parent_path(), ec);
            if (ec)
            {
                LogError(ec.message());
                return;
            }
        }
    }

    std::ofstream file(file_path, std::ofstream::trunc);
    if (!file.is_open())
    {
        LogError("Failed to open options file for writing: " + file_path.string());
        return;
    }

    for (const auto& [key, value] : m_variables)
        file << key << " = \"" << value << "\"\n";
    file.close();
    if (!file)
        LogError("Options: short write to " + file_path.string());
    else
        Log("Options: wrote " + std::to_string(m_variables.size()) + " key(s) to " + file_path.string());
}

void OptionsHandler::DeserializeFromFile()
{
    const std::string resolved_path = ResolvePersistencePath();
    if (resolved_path.empty())
    {
        LogError("DeserializeFromFile: no options persistence path");
        return;
    }
    std::filesystem::path file_path(resolved_path);

    if (!std::filesystem::is_regular_file(file_path))
    {
        Log("Options: no file at " + file_path.string() + ", core defaults in force");
        if (m_create_persistence_if_missing)
            SerializeToFile();
        return;
    }

    std::ifstream file(file_path);
    if (!file.is_open())
    {
        LogError("Failed to open options file: " + file_path.string());
        return;
    }

    std::string line_str;
    size_t applied = 0;
    size_t stale = 0;
    while (std::getline(file, line_str))
    {
        std::string_view line(line_str);

        auto eq_pos = line.find('=');
        if (eq_pos == std::string_view::npos)
            continue;

        auto key = line.substr(0, eq_pos);
        auto key_begin = key.find_first_not_of(" \t");
        auto key_end = key.find_last_not_of(" \t");
        if (key_begin == std::string_view::npos || key_end == std::string_view::npos)
            continue;
        key = key.substr(key_begin, key_end - key_begin + 1);
        if (key.empty())
            continue;

        auto value = line.substr(eq_pos + 1);
        auto value_begin = value.find_first_not_of(" \t");
        auto value_end = value.find_last_not_of(" \t");
        if (value_begin == std::string_view::npos || value_end == std::string_view::npos)
            continue;
        value = value.substr(value_begin, value_end - value_begin + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);

        if (m_variables.contains(std::string(key)))
        {
            m_variables[std::string(key)] = std::string(value);
            ++applied;
        }
        else
        {
            ++stale;
        }
    }
    Log("Options: read " + file_path.string() + ", " + std::to_string(applied) + " key(s) applied" +
        (stale ? ", " + std::to_string(stale) + " unknown to this core" : std::string()));
}
}
