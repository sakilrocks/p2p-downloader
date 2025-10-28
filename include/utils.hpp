
#ifndef UTILS_HPP
#define UTILS_HPP

#include <string>
#include <vector>

std::vector<std::string> split(const std::string &s, char delim);
std::string join(const std::vector<std::string> &parts, const std::string &sep);
std::string trim(const std::string &s);

#endif 