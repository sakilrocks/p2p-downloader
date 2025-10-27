
#ifndef NETWORK_HPP
#define NETWORK_HPP

#include <string>
#include <vector>
#include <map>
#include <mutex>

struct PeerInfo {
    std::string addr;
    int port;
        // filename -> size
    std::map<std::string, uint64_t> files;
};

class Network {

public:
    Network(int listen_port);
    ~Network();

    void start_broadcast(const std::string& shared_folder);
    void start_listen_peers();
    void start_tcp_server(const std::string& shared_folder);
    std::vector<PeerInfo> get_peers_snapshot();

private:
    int udp_broadcast_socket = -1;
    int udp_listen_socket = -1;
    int tcp_listen_socket = -1;
    int port;
    bool running = true;

    std::mutex peers_m;
    std::map<std::string, PeerInfo> peers;   // key = addr:port

    std::thread broadcaster_thread;
    std::thread udp_listener_thread;
    std::thread tcp_server_thread;

    void broadcaster_loop(const std::string& shared_folder);
    void udp_listener_loop();
    void tcp_server_loop(const std::string& shared_folder);

    std::string make_announce_message(const std::string& shared_folder);
    void handle_tcp_client(int client_sock, const std::string& shared_folder);
};


#endif 