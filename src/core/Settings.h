#pragma once

#include <map>
#include <string>

namespace scrible {

// Preferences that survive restarts, kept as "key=value" lines in a small file.
class Settings {
public:
    explicit Settings(std::string path);

    std::string get(const std::string& key, const std::string& fallback = {}) const;
    void set(const std::string& key, const std::string& value);  // written right away (best effort)

private:
    std::string path_;
    std::map<std::string, std::string> values_;
};

}  // namespace scrible
