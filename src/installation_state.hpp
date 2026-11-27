#pragma once
#include "../third_party/tomlplusplus/toml.hpp"
#include <sstream>
#include <string>
#include <string_view>

namespace QuestRewardReset {
inline auto InstallationComplete(std::string_view text) -> bool {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    try {
        const auto state = toml::parse(text);
        return state["installation"]["complete"].value<bool>().value_or(false);
    } catch (const toml::parse_error&) {
        return false;
    }
}
inline auto CompletedInstallationState(std::string_view text, std::string_view version) -> std::string {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    toml::table state;
    try { state = toml::parse(text); }
    catch (const toml::parse_error&) { /* Rebuild invalid dedicated state after successful setup. */ }
    auto* installation = state["installation"].as_table();
    if (!installation) {
        state.insert_or_assign("installation", toml::table{});
        installation = state["installation"].as_table();
    }
    installation->insert_or_assign("complete", true);
    installation->insert_or_assign("plugin_version", std::string(version));
    std::ostringstream output;
    output << state << '\n';
    return output.str();
}
} // namespace QuestRewardReset
