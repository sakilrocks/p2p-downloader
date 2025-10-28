
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>
#include <chrono>

#include "network.hpp"
#include "peer.hpp"
#include "utils.hpp"

static Network *net = nullptr;
static std::string shared_folder = ".";

void print_help() {
    std::cout << "  Usage:\n";
    std::cout << "  p2p share <folder>        # start sharing folder (runs services)\n";
    std::cout << "  p2p list                 # list discovered peers and files\n";
    std::cout << "  p2p get <filename> <threads>  # download file using parallel threads\n";
}

std::vector<std::pair<std::string,uint64_t>> gather_available_files() {
    std::vector<std::pair<std::string,uint64_t>> out;
    auto peers = net->get_peers_snapshot();
    for (auto &p : peers) {
        for (auto &kv : p.files) {
            // store as host:port|filename:size  - but keep simple
            out.emplace_back(p.addr + ":" + std::to_string(p.port) + "|" + kv.first, kv.second);
        }
    }
    return out;
}

bool download_range(const std::string& host, int port, const std::string& filename, uint64_t start, uint64_t end, std::fstream &ofs) {
    // connect
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &srv.sin_addr) <= 0) { close(sock); return false; }

    if (connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) { close(sock); return false; }

    // send: GET filename start end\n
    std::stringstream ss;
    ss << "GET " << filename << " " << start << " " << end << "\n";
    std::string req = ss.str();
    send(sock, req.c_str(), req.size(), 0);

    // read header OK <len>\n
    char hdrbuf[64];
    ssize_t n = recv(sock, hdrbuf, sizeof(hdrbuf)-1, 0);
    if (n <= 0) { close(sock); return false; }
    hdrbuf[n] = 0;
    std::string hdr(hdrbuf);
    if (hdr.rfind("OK ", 0) != 0) { close(sock); return false; }
    // header may include only part of payload; find newline
    auto pos = hdr.find('\n');
    std::string firstline = hdr.substr(0, pos);
    // parse len
    auto sp = split(firstline, ' ');
    uint64_t expected = std::stoull(sp[1]);

    // the rest of hdr after newline is the beginning of data (if pos != n)
    size_t data_start = (pos == std::string::npos) ? n : (pos + 1);
    size_t already = (data_start < (size_t)n) ? (n - data_start) : 0;
    if (already > 0) {
        // write at offset start
        ofs.seekp(start);
        ofs.write(hdrbuf + data_start, already);
    }

    uint64_t received = already;
    const size_t bufsize = 64*1024;
    std::vector<char> buffer(bufsize);
    while (received < expected) {
        ssize_t r = recv(sock, buffer.data(), bufsize, 0);
        if (r <= 0) break;
        ofs.seekp(start + received);
        ofs.write(buffer.data(), r);
        received += r;
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
            // optionally show known peers
            // auto peers = net->get_peers_snapshot();
        }
    } else if (cmd == "list") {
        //  start listen only to populate peers for a short time
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

        // find a peer that has this file
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

        // prepare destination file
        std::fstream ofs(filename, std::ios::binary | std::ios::out | std::ios::trunc);
        ofs.seekp(size-1);
        ofs.write("",1);
        ofs.flush();

        // compute ranges
        uint64_t chunk = size / threads;
        std::vector<std::pair<uint64_t,uint64_t>> ranges;
        uint64_t cur = 0;
        for (int i=0;i<threads;++i) {
            uint64_t start = cur;
            uint64_t end = (i == threads-1) ? size : (cur + chunk);
            ranges.emplace_back(start, end);
            cur = end;
        }

        std::vector<std::thread> ths;
        std::vector<bool> success(threads,false);
        for (int i=0;i<threads;++i) {
            ths.emplace_back([&,i](){
                std::fstream localfs(filename, std::ios::in | std::ios::out | std::ios::binary);
                if (!localfs) { success[i]=false; return; }
                auto [s,e] = ranges[i];
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
