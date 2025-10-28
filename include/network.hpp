
#ifndef NETWORK_HPP
#define NETWORK_HPP

#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>

struct PeerInfo {
    std::string addr;
    int port;
    std::map<std::string, uint64_t> files;
};

class Network {
public:
    explicit Network(int service_port);
    ~Network();

    void start_broadcast(const std::string& shared_folder);
    void start_listen_peers();
    void start_tcp_server(const std::string& shared_folder);

    // Thread-safe snapshot of all known peers
    std::vector<PeerInfo> get_peers_snapshot();

private:
    int service_port_;
    std::atomic<bool> running_{true};

    std::vector<PeerInfo> peers_;
    std::mutex peers_mutex_;

    // Threads
    std::thread broadcast_thread_;
    std::thread listener_thread_;
    std::thread tcp_server_thread_;

    // Internal workers
    void broadcast_worker(const std::string& shared_folder);
    void listener_worker();
    void tcp_server_worker(const std::string& shared_folder);
};


#endif 