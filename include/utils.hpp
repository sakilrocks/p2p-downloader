
#ifndef UTILS_HPP
#define UTILS_HPP

#include <string>
#include <vector>
#include <cstdint>

std::vector<std::string> split(const std::string& s, char delim);
std::string join(const std::vector<std::string>& parts, char delim);


#endif