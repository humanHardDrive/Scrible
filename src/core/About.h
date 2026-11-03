#pragma once

// What the About dialog says: the app, this build, and its license.

#include <string>
#include <vector>

namespace scrible {

struct BuildInfo {
    std::string version;  // "0.1.0", from CMakeLists.txt
    std::string commit;   // git describe of the build: "v0.1.0", "76489ba", "76489ba-dirty"; empty if unknown
    std::string date;     // that commit's date, "2026-11-02"
};
BuildInfo buildInfo();

// The dialog's paragraphs (an empty one is a gap). `platform` comes from the front
// end (the device, its libraries); `notebooksDir` is where the notebooks are.
std::vector<std::string> aboutText(const std::vector<std::string>& platform, const std::string& notebooksDir);

}  // namespace scrible
