#include "Settings.h"

#include <cstdio>
#include <fstream>

namespace scrible {

Settings::Settings(std::string path) : path_(std::move(path)) {
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

std::string Settings::get(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

void Settings::set(const std::string& key, const std::string& value) {
    if (get(key) == value && values_.count(key)) return;
    values_[key] = value;
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const auto& [k, v] : values_) out << k << '=' << v << '\n';
        if (!out) {
            std::fprintf(stderr, "scrible: can't write %s\n", tmp.c_str());
            return;
        }
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) std::fprintf(stderr, "scrible: can't write %s\n", path_.c_str());
}

}  // namespace scrible
