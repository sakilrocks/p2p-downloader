
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

Network::Network(int service_port) : service_port_(service_port) {}
Network::~Network() {
    running_ = false;
    if (broadcast_thread_.joinable()) broadcast_thread_.join();
    if (listener_thread_.joinable()) listener_thread_.join();
    if (tcp_server_thread_.joinable()) tcp_server_thread_.join();
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
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(service_port_);
    addr.sin_addr.s_addr = inet_addr("255.255.255.255");

    while (running_) {
        std::stringstream ss;
        ss << "PEER " << service_port_ << " ";
        for (auto &entry : fs::directory_iterator(shared_folder)) {
            if (entry.is_regular_file()) {
                ss << entry.path().filename().string() << ":" << fs::file_size(entry) << ",";
            }
        }

        std::string msg = ss.str();
        sendto(sock, msg.c_str(), msg.size(), 0, (sockaddr*)&addr, sizeof(addr));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    close(sock);
}


// ---------------------------------------------------------------
// Listen for peers (UDP)
// ---------------------------------------------------------------
void Network::listener_worker() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "[listener] failed to create socket\n";
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(service_port_);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[listener] bind failed\n";
        close(sock);
        return;
    }

    char buf[4096];
    while (running_) {
        sockaddr_in src{};
        socklen_t len = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&src, &len);
        if (n <= 0) continue;
        buf[n] = '\0';

        std::string msg(buf);
        if (!msg.starts_with("PEER ")) continue;

        std::istringstream iss(msg);
        std::string tag;
        int port;
        iss >> tag >> port;

        std::string rest;
        std::getline(iss, rest);
        std::string ip = inet_ntoa(src.sin_addr);

        PeerInfo peer;
        peer.addr = ip;
        peer.port = port;

        // parse files: "name:size,name2:size2,"
        auto parts = split(rest, ',');
        for (auto &p : parts) {
            auto kv = split(p, ':');
            if (kv.size() == 2) {
                try {
                    peer.files[kv[0]] = std::stoull(kv[1]);
                } catch (...) {}
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
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(service_port_);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[tcp_server] bind failed\n";
        close(server_fd);
        return;
    }

    if (listen(server_fd, 5) < 0) {
        std::cerr << "[tcp_server] listen failed\n";
        close(server_fd);
        return;
    }

    std::cout << "[tcp_server] listening on port " << service_port_ << "\n";

    while (running_) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        int cfd = accept(server_fd, (sockaddr*)&client, &len);
        if (cfd < 0) continue;

        std::thread([cfd, shared_folder]() {
            char buf[1024];
            ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) { close(cfd); return; }
            buf[n] = '\0';
            std::string req(buf);

            if (!req.starts_with("GET ")) {
                close(cfd);
                return;
            }

            auto parts = split(req, ' ');
            if (parts.size() < 4) { close(cfd); return; }
            std::string filename = parts[1];
            uint64_t start = std::stoull(parts[2]);
            uint64_t end = std::stoull(parts[3]);

            std::string path = shared_folder + "/" + filename;
            if (!fs::exists(path)) {
                std::string err = "ERR nofile\n";
                send(cfd, err.c_str(), err.size(), 0);
                close(cfd);
                return;
            }

            uint64_t size = fs::file_size(path);
            if (end > size) end = size;
            if (start >= end) { close(cfd); return; }

            uint64_t len_to_send = end - start;
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) { close(cfd); return; }
            ifs.seekg((std::streampos)start);

            std::stringstream header;
            header << "OK " << len_to_send << "\n";
            std::string hdr = header.str();
            send(cfd, hdr.c_str(), hdr.size(), 0);

            const size_t bufsize = 64 * 1024;
            std::vector<char> buffer(bufsize);
            uint64_t sent = 0;
            while (sent < len_to_send && ifs) {
                size_t toread = std::min<uint64_t>(bufsize, len_to_send - sent);
                ifs.read(buffer.data(), toread);
                size_t got = ifs.gcount();
                if (got == 0) break;
                send(cfd, buffer.data(), got, 0);
                sent += got;
            }
            close(cfd);
        }).detach();
    }

    close(server_fd);
}