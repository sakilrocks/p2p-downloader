
#include "peer.hpp"
#include "utils.hpp"

#include <filesystem>
#include <vector>
#include <string>

namespace fs = std::filesystem;

std::vector<std::string> list_files_in_folder(const std::string& folder) {
    std::vector<std::string> files;
    try {
        for (auto &p : fs::directory_iterator(folder)) {
            if (fs::is_regular_file(p.status())) {
                files.push_back(p.path().filename().string());
            }
        }
    } catch(...) {}
    return files;
}

uint64_t file_size(const std::string& path) {
    try {
        return (uint64_t)fs::file_size(path);
    } catch(...) {
        return 0;
    }
}