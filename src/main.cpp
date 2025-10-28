#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>

#include "network.hpp"
#include "peer.hpp"
#include "utils.hpp"

static Network *net = nullptr;
static std::string shared_folder = ".";

void print_help() {
    std::cout << "Usage:\n";
    std::cout << "  p2p share <folder>             # start sharing folder (runs services)\n";
    std::cout << "  p2p list                       # list discovered peers and files\n";
    std::cout << "  p2p get <filename> [threads]   # download file using parallel threads\n";
}

std::vector<std::pair<std::string,uint64_t>> gather_available_files() {
    std::vector<std::pair<std::string,uint64_t>> out;
    if (!net) return out;
    auto peers = net->get_peers_snapshot();
    for (auto &p : peers) {
        for (auto &kv : p.files) {
            out.emplace_back(p.addr + ":" + std::to_string(p.port) + "|" + kv.first, kv.second);
        }
    }
    return out;
}

bool download_range(const std::string& host, int port, const std::string& filename, uint64_t start, uint64_t end, std::fstream &ofs) {
    // create socket and connect
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &srv.sin_addr) <= 0) { close(sock); return false; }

    if (connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) { close(sock); return false; }

    // send request: GET filename start end\n
    {
        std::stringstream ss;
        ss << "GET " << filename << " " << start << " " << end << "\n";
        std::string req = ss.str();
        ssize_t s = send(sock, req.c_str(), (size_t)req.size(), 0);
        if (s != (ssize_t)req.size()) { close(sock); return false; }
    }

    // Read header line "OK <len>\n" robustly (header is ASCII terminated by '\n')
    std::string header;
    char ch;
    while (true) {
        ssize_t r = recv(sock, &ch, 1, 0);
        if (r <= 0) { close(sock); return false; }
        header.push_back(ch);
        if (ch == '\n') break;
        // safety: avoid overly long header
        if (header.size() > 1024) { close(sock); return false; }
    }

    if (header.rfind("OK ", 0) != 0) {
        close(sock);
        return false;
    }
    auto hparts = split(header, ' ');
    if (hparts.size() < 2) { close(sock); return false; }
    uint64_t expected = 0;
    try {
        expected = std::stoull(hparts[1]);
    } catch(...) { close(sock); return false; }

    // Now stream expected bytes from the socket into the file at offset `start`.
    const size_t bufsize = 64 * 1024;
    std::vector<char> buffer(bufsize);
    uint64_t received = 0;

    // If there is any extra data already in the socket's receive buffer after the '\n'
    // we already consumed exactly up to '\n' and no extra data is in 'header' (we read byte-by-byte).
    // So continue receiving the rest.
    while (received < expected) {
        ssize_t r = recv(sock, buffer.data(), (size_t)std::min<uint64_t>(bufsize, expected - received), 0);
        if (r <= 0) break;
        ofs.seekp((std::streampos)(start + received));
        ofs.write(buffer.data(), r);
        if (!ofs) break;
        received += (uint64_t)r;
    }

    close(sock);
    return received == expected;
}

int main(int argc, char** argv) {
    if (argc < 2) { print_help(); return 1; }
    std::string cmd = argv[1];

    const int SERVICE_PORT = 12000;
    net = new Network(SERVICE_PORT);

    if (cmd == "share") {
        if (argc < 3) {
            std::cout << "Provide folder to share\n";
            return 1;
        }
        shared_folder = argv[2];
        std::cout << "Sharing folder: " << shared_folder << "\n";
        net->start_broadcast(shared_folder);
        net->start_listen_peers();
        net->start_tcp_server(shared_folder);

        std::cout << "Services started. Press Ctrl+C to stop.\n";
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    } else if (cmd == "list") {
        net->start_listen_peers();
        std::cout << "Listening for peers for 4 seconds...\n";
        std::this_thread::sleep_for(std::chrono::seconds(4));
        auto peers = net->get_peers_snapshot();
        for (auto &p : peers) {
            std::cout << p.addr << ":" << p.port << "\n";
            for (auto &kv : p.files) {
                std::cout << "  - " << kv.first << " (" << kv.second << " bytes)\n";
            }
        }
        if (peers.empty()) std::cout << "No peers found.\n";
    } else if (cmd == "get") {
        if (argc < 3) {
            std::cout << "Usage: p2p get <filename> [threads]\n";
            return 1;
        }
        int threads = 4;
        if (argc >= 4) threads = std::max(1, atoi(argv[3]));
        std::string filename = argv[2];

        net->start_listen_peers();
        std::this_thread::sleep_for(std::chrono::seconds(3));
        auto peers = net->get_peers_snapshot();
        std::string host; int port = 0; uint64_t size = 0;
        for (auto &p : peers) {
            auto it = p.files.find(filename);
            if (it != p.files.end()) {
                host = p.addr; port = p.port; size = it->second;
                break;
            }
        }
        if (host.empty()) { std::cout << "No peer has that file.\n"; return 1; }
        std::cout << "Found on " << host << ":" << port << " size=" << size << " bytes\n";

        // prepare destination file (pre-allocate)
        {
            std::fstream ofs(filename, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!ofs) { std::cout << "Failed to create output file\n"; return 1; }
            if (size > 0) {
                // write a single zero byte at position size-1 to allocate file
                ofs.seekp((std::streampos)(size - 1));
                char zero = 0;
                ofs.write(&zero, 1);
            }
            ofs.close();
        }

        // compute ranges
        uint64_t chunk = (size + threads - 1) / threads; // ceil division
        std::vector<std::pair<uint64_t,uint64_t>> ranges;
        uint64_t cur = 0;
        for (int i = 0; i < threads; ++i) {
            uint64_t start = cur;
            uint64_t end = std::min<uint64_t>(size, cur + chunk);
            if (start >= end) { ranges.emplace_back(0,0); } else ranges.emplace_back(start, end);
            cur = end;
        }

        std::vector<std::thread> ths;
        std::vector<bool> success(threads,false);
        for (int i=0;i<threads;++i) {
            ths.emplace_back([&,i](){
                auto [s,e] = ranges[i];
                if (s >= e) { success[i] = true; return; } // nothing to do for this thread
                std::fstream localfs(filename, std::ios::in | std::ios::out | std::ios::binary);
                if (!localfs) { success[i]=false; return; }
                bool ok = download_range(host, port, filename, s, e, localfs);
                success[i] = ok;
                std::cout << "Thread " << i << " finished " << (ok?"OK":"FAIL") << "\n";
            });
        }
        for (auto &t: ths) if (t.joinable()) t.join();
        bool allok = true;
        for (auto v: success) if (!v) allok=false;
        if (allok) std::cout << "Download completed: " << filename << "\n";
        else std::cout << "Download incomplete or failed.\n";
    } else {
        print_help();
    }

    delete net;
    return 0;
}