
#include "network.hpp"
#include "utils.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <chrono>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

namespace fs = std::filesystem;

static constexpr int DISCOVERY_PORT = 10000; // UDP discovery port

Network::Network(int service_port) : service_port_(service_port) {}
Network::~Network() {
    running_ = false;
    // join threads if running
    try {
        if (broadcast_thread_.joinable()) broadcast_thread_.join();
        if (listener_thread_.joinable()) listener_thread_.join();
        if (tcp_server_thread_.joinable()) tcp_server_thread_.join();
    } catch (...) {}
}


// ---------------------------------------------------------------
// Start Threads
// ---------------------------------------------------------------
void Network::start_broadcast(const std::string& shared_folder) {
    broadcast_thread_ = std::thread(&Network::broadcast_worker, this, shared_folder);
}
void Network::start_listen_peers() {
    listener_thread_ = std::thread(&Network::listener_worker, this);
}
void Network::start_tcp_server(const std::string& shared_folder) {
    tcp_server_thread_ = std::thread(&Network::tcp_server_worker, this, shared_folder);
}


// ---------------------------------------------------------------
// Thread-safe peer snapshot
// ---------------------------------------------------------------
std::vector<PeerInfo> Network::get_peers_snapshot() {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return peers_;
}


// ---------------------------------------------------------------
// Broadcast (UDP)
// ---------------------------------------------------------------
void Network::broadcast_worker(const std::string& shared_folder) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "[broadcast] failed to create socket\n";
        return;
    }

    int broadcastEnable = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable)) < 0) {
        std::cerr << "[broadcast] setsockopt SO_BROADCAST failed\n";
        // continue; it's not always fatal
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = inet_addr("255.255.255.255");

    while (running_) {
        std::stringstream ss;
        // Format: "PEER <tcp_port> filename1:size1,filename2:size2,"
        ss << "PEER " << service_port_ << " ";
        bool first = true;
        try {
            for (auto &entry : fs::directory_iterator(shared_folder)) {
                if (!entry.is_regular_file()) continue;
                if (!first) ss << ",";
                first = false;
                ss << entry.path().filename().string() << ":" << fs::file_size(entry.path());
            }
        } catch (const std::exception &e) {
            // ignore errors reading folder
        }

        std::string msg = ss.str();
        ssize_t sent = sendto(sock, msg.c_str(), msg.size(), 0, (sockaddr*)&addr, sizeof(addr));
        (void)sent;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    close(sock);
}


// ---------------------------------------------------------------
// Listen for peers (UDP)
// ---------------------------------------------------------------
static inline std::string ltrim(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}

void Network::listener_worker() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "[listener] failed to create socket\n";
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(DISCOVERY_PORT);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[listener] bind failed\n";
        close(sock);
        return;
    }

    char buf[8192];
    while (running_) {
        sockaddr_in src{};
        socklen_t len = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&src, &len);
        if (n <= 0) continue;
        buf[n] = '\0';

        std::string msg(buf);
        // ensure it starts with "PEER "
        if (msg.size() < 5) continue;
        if (msg.compare(0, 5, "PEER ") != 0) continue;

        std::istringstream iss(msg);
        std::string tag;
        int peer_tcp_port = 0;
        if (!(iss >> tag >> peer_tcp_port)) continue;

        std::string rest;
        std::getline(iss, rest);
        rest = ltrim(rest); // removes leading spaces

        std::string ip = inet_ntoa(src.sin_addr);

        PeerInfo peer;
        peer.addr = ip;
        peer.port = peer_tcp_port;

        // parse files: "name:size,name2:size2,..." (maybe trailing comma)
        if (!rest.empty()) {
            // split by comma
            auto items = split(rest, ',');
            for (auto &item : items) {
                if (item.empty()) continue;
                auto kv = split(item, ':');
                if (kv.size() != 2) continue;
                std::string fname = kv[0];
                std::string ssize = kv[1];
                try {
                    uint64_t fsize = std::stoull(ssize);
                    peer.files[fname] = fsize;
                } catch (...) {
                    // ignore bad size parse
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            bool found = false;
            for (auto &existing : peers_) {
                if (existing.addr == peer.addr && existing.port == peer.port) {
                    existing.files = peer.files;
                    found = true;
                    break;
                }
            }
            if (!found) peers_.push_back(peer);
        }
    }
    close(sock);
}


// ---------------------------------------------------------------
// TCP Server (File Sender)
// ---------------------------------------------------------------
void Network::tcp_server_worker(const std::string& shared_folder) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[tcp_server] socket create failed\n";
        return;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        // not fatal
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(service_port_);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[tcp_server] bind failed\n";
        close(server_fd);
        return;
    }

    if (listen(server_fd, 10) < 0) {
        std::cerr << "[tcp_server] listen failed\n";
        close(server_fd);
        return;
    }

    std::cout << "[tcp_server] listening on port " << service_port_ << "\n";

    while (running_) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        int cfd = accept(server_fd, (sockaddr*)&client, &len);
        if (cfd < 0) {
            // accept can be interrupted by signals; continue
            continue;
        }

        // handle each client in detached thread
        std::thread([cfd, shared_folder]() {
            // Read a full line request (ends with '\n')
            std::string req;
            char ch;
            ssize_t r;
            // read until newline or buffer full
            while (true) {
                r = recv(cfd, &ch, 1, 0);
                if (r <= 0) { close(cfd); return; }
                req.push_back(ch);
                if (ch == '\n') break;
                if (req.size() > 4096) { close(cfd); return; }
            }

            // tokenize by whitespace robustly
            std::istringstream iss(req);
            std::string cmd;
            if (!(iss >> cmd)) { close(cfd); return; }
            if (cmd != "GET") { close(cfd); return; }

            std::string filename;
            uint64_t start = 0, end = 0;
            if (!(iss >> filename >> start >> end)) {
                // If client didn't send start/end, treat as entire file request
                // Reset stream and try parsing filename only
                iss.clear();
                iss.str(req);
                // re-extract
                iss >> cmd >> filename;
                // leave start=0; end=0 means send full file
            }

            std::string path = shared_folder + "/" + filename;
            if (!fs::exists(path)) {
                std::string err = "ERR nofile\n";
                send(cfd, err.c_str(), (size_t)err.size(), 0);
                close(cfd);
                return;
            }

            uint64_t fsize = 0;
            try { fsize = fs::file_size(path); } catch(...) { fsize = 0; }

            if (end == 0 || end > fsize) end = fsize;
            if (start >= end) { close(cfd); return; }

            uint64_t tosend = end - start;
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) { close(cfd); return; }
            ifs.seekg((std::streampos)start);

            // send header
            std::ostringstream hdr;
            hdr << "OK " << tosend << "\n";
            std::string hdrs = hdr.str();
            send(cfd, hdrs.c_str(), hdrs.size(), 0);

            const size_t bufsize = 64 * 1024;
            std::vector<char> buffer(bufsize);
            uint64_t sent = 0;
            while (sent < tosend && ifs) {
                size_t want = (size_t)std::min<uint64_t>(bufsize, tosend - sent);
                ifs.read(buffer.data(), want);
                size_t got = (size_t)ifs.gcount();
                if (got == 0) break;
                ssize_t s = send(cfd, buffer.data(), got, 0);
                if (s <= 0) break;
                sent += (uint64_t)s;
            }
            close(cfd);
        }).detach();
    }

    close(server_fd);
}