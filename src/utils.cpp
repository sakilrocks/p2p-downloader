
#include "utils.hpp"

#include <sstream>
#include <algorithm>
#include <cctype>

std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

std::string join(const std::vector<std::string> &parts, const std::string &sep) {
    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        oss << parts[i];
        if (i + 1 < parts.size()) oss << sep;
    }
    return oss.str();
}

std::string trim(const std::string &s) {
    if (s.empty()) return s;
    size_t start = 0;
    size_t end = s.size() - 1;

    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(s[end]))) {
        --end;
    }
    return s.substr(start, end - start + 1);
}
