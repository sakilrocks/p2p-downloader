
#include "network.hpp"
#include "peer.hpp"
#include "utils.hpp"

#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>
#include <cstring>
#include <vector>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

Network::Network(int listen_port) : port(listen_port) {}

Network::~Network() {
    running = false;
    if (udp_broadcast_socket != -1) close(udp_broadcast_socket);
    if (udp_listen_socket != -1) close(udp_listen_socket);
    if (tcp_listen_socket != -1) close(tcp_listen_socket);
}

void Network::start_broadcast(const std::string& shared_folder) {
    broadcaster_thread = std::thread(&Network::broadcaster_loop, this, shared_folder);
}

void Network::start_listen_peers() {
    udp_listener_thread = std::thread(&Network::udp_listener_loop, this);
}

void Network::start_tcp_server(const std::string& shared_folder) {
    tcp_server_thread = std::thread(&Network::tcp_server_loop, this, shared_folder);
}

std::vector<PeerInfo> Network::get_peers_snapshot() {
    std::lock_guard<std::mutex> lk(peers_m);
    std::vector<PeerInfo> out;
    for (auto &p : peers) out.push_back(p.second);
    return out;
}

std::string Network::make_announce_message(const std::string& shared_folder) {
    // Format: P2P|PORT|file1:size1,file2:size2,...
    std::stringstream ss;
    ss << "P2P|" << port << "|";
    bool first = true;
    for (auto &f : list_files_in_folder(shared_folder)) {
        if (!first) ss << ",";
        first = false;
        uint64_t sz = file_size(shared_folder + "/" + f);
        ss << f << ":" << sz;
    }
    return ss.str();
}

void Network::broadcaster_loop(const std::string& shared_folder) {
    udp_broadcast_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_broadcast_socket < 0) {
        perror("udp socket");
        return;
    }
    int broadcastEnable = 1;
    setsockopt(udp_broadcast_socket, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

    sockaddr_in baddr{};
    baddr.sin_family = AF_INET;
    baddr.sin_port = htons(10000); // discovery port
    baddr.sin_addr.s_addr = inet_addr("255.255.255.255");

    while (running) {
        std::string msg = make_announce_message(shared_folder);
        sendto(udp_broadcast_socket, msg.c_str(), msg.size(), 0, (sockaddr*)&baddr, sizeof(baddr));
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

void Network::udp_listener_loop() {
    udp_listen_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_listen_socket < 0) {
        perror("udp listen socket");
        return;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(10000);
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(udp_listen_socket, (sockaddr*)&local, sizeof(local)) < 0) {
        perror("bind udp");
        return;
    }

    char buf[8192];
    while (running) {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(udp_listen_socket, buf, sizeof(buf)-1, 0, (sockaddr*)&src, &slen);
        if (n <= 0) continue;
        buf[n] = 0;
        std::string msg(buf);
        if (msg.rfind("P2P|", 0) != 0) continue;
        // parse
        // P2P|PORT|file:size,file2:size2
        auto parts = split(msg, '|');
        if (parts.size() < 3) continue;
        int peerport = std::stoi(parts[1]);
        std::string filespart = parts[2];
        auto fileitems = split(filespart, ',');
        PeerInfo info;
        char addrbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, addrbuf, sizeof(addrbuf));
        info.addr = std::string(addrbuf);
        info.port = peerport;
        for (auto &it : fileitems) {
            if (it.empty()) continue;
            auto kv = split(it, ':');
            if (kv.size() == 2) {
                info.files[kv[0]] = std::stoull(kv[1]);
            }
        }
        std::string key = info.addr + ":" + std::to_string(info.port);
        {
            std::lock_guard<std::mutex> lk(peers_m);
            peers[key] = info;
        }
    }
}

void Network::tcp_server_loop(const std::string& shared_folder) {
    tcp_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_listen_socket < 0) { perror("tcp socket"); return; }
    int opt = 1;
    setsockopt(tcp_listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    srv.sin_addr.s_addr = INADDR_ANY;
    if (bind(tcp_listen_socket, (sockaddr*)&srv, sizeof(srv)) < 0) { perror("bind tcp"); return; }
    if (listen(tcp_listen_socket, 10) < 0) { perror("listen"); return; }

    while (running) {
        sockaddr_in cli{};
        socklen_t clilen = sizeof(cli);
        int client_sock = accept(tcp_listen_socket, (sockaddr*)&cli, &clilen);
        if (client_sock < 0) continue;
        std::thread(&Network::handle_tcp_client, this, client_sock, shared_folder).detach();
    }
}

void send_all(int sock, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(sock, data + sent, len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

void Network::handle_tcp_client(int client_sock, const std::string& shared_folder) {
    // Read a single line request
    char buf[1024];
    ssize_t r = recv(client_sock, buf, sizeof(buf)-1, 0);
    if (r <= 0) { close(client_sock); return; }
    buf[r] = 0;
    std::string req(buf);
    // expected "GET filename start end\n"
    auto parts = split(req, ' ');
    if (parts.size() < 3 || parts[0] != "GET") {
        std::string err = "ERR\n";
        send_all(client_sock, err.c_str(), err.size());
        close(client_sock);
        return;
    }
    std::string filename = parts[1];
    uint64_t start = 0, end = 0;
    if (parts.size() >= 4) start = std::stoull(parts[2]);
    if (parts.size() >= 5) end = std::stoull(parts[3]);
    std::string path = shared_folder + "/" + filename;
    if (!fs::exists(path)) {
        std::string err = "NOFILE\n";
        send_all(client_sock, err.c_str(), err.size());
        close(client_sock);
        return;
    }
    uint64_t fsize = file_size(path);
    if (end == 0 || end > fsize) end = fsize;
    if (start >= end) { close(client_sock); return; }

    // send header: OK size\n
    std::stringstream hdr;
    hdr << "OK " << (end - start) << "\n";
    std::string h = hdr.str();
    send_all(client_sock, h.c_str(), h.size());

    // send bytes
    std::ifstream ifs(path, std::ios::binary);
    ifs.seekg(start);
    const size_t bufsize = 64 * 1024;
    std::vector<char> buffer(bufsize);
    uint64_t tosend = end - start;
    while (tosend > 0 && ifs) {
        size_t rd = (size_t)std::min<uint64_t>(bufsize, tosend);
        ifs.read(buffer.data(), rd);
        size_t actually = ifs.gcount();
        if (actually == 0) break;
        send_all(client_sock, buffer.data(), actually);
        tosend -= actually;
    }

    close(client_sock);
}