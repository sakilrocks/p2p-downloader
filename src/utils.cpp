
#include "utils.hpp"

#include <sstream>

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        out.push_back(item);
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, char delim) {
    std::string out;
    for (size_t i=0;i<parts.size();++i) {
        out += parts[i];
        if (i+1<parts.size()) out.push_back(delim);
    }
    return out;
}
