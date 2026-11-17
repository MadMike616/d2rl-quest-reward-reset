#pragma once

#include <string_view>

namespace D2RL {
struct PluginContext;
}

namespace QuestRewardReset {

enum class InstallResult { AlreadyInstalled, Installed, Failed };

auto RunFirstRunInstall(const D2RL::PluginContext& context, std::string_view pluginVersion) noexcept -> InstallResult;

} // namespace QuestRewardReset
