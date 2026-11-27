#include "../src/installation_state.hpp"
#include <cstdio>
#include <utility>
#include <vector>
int main() {
    using namespace QuestRewardReset;
    const std::vector<std::pair<std::string,bool>> cases{
        {"",false},{"[installation]\ncomplete=false\n",false},
        {"[installation]\ncomplete=true\n",true},
        {"[installation]\ncomplete=\"true\"\n",false},
        {"[installation\ncomplete=true\n",false},
        {"[installation]\ncomplete=true\ncomplete=false\n",false},
        {"[other]\ncomplete=true\n",false},
        {"\xef\xbb\xbf[installation]\r\ncomplete=true # done\r\n",true},
        {"[installation]\nmessage=\"\"\"\ncomplete=true\n\"\"\"\n",false}
    };
    for (const auto& [input,expected]:cases) {
        if (InstallationComplete(input)!=expected) { std::fputs("Unexpected installation state\n",stderr);return 1; }
    }
    const auto merged=CompletedInstallationState("[installation]\ncomplete=false\n[iht]\nexclude=true\nplugin_id=\"keep\"\n","1.1.1");
    const auto parsed=toml::parse(merged);
    if (!InstallationComplete(merged) || parsed["iht"]["plugin_id"].value<std::string>()!="keep"
            || parsed["installation"]["plugin_version"].value<std::string>()!="1.1.1") return 2;
    return 0;
}
