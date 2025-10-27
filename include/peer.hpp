
#ifndef PEER_HPP
#define PEER_HPP

#include <string>
#include <vector>
#include <map>

struct RemoteFile {
    std::string filename;
    std::string host;
    int port;
    uint64_t size;
};

std::vector<std::string> list_files_in_folder(const std::string& folder);
uint64_t file_size(const std::string& path);


#endif 