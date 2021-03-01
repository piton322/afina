#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <condition_variable>

#include <afina/network/Server.h>

namespace spdlog {
class logger;
}

namespace Afina {
namespace Network {
namespace MTblocking {

/**
 * # Network resource manager implementation
 * Server that is spawning a separate thread for each connection
 */
class ServerImpl : public Server {
public:
    ServerImpl(std::shared_ptr<Afina::Storage> ps, std::shared_ptr<Logging::Service> pl);
    ~ServerImpl();

    // See Server.h
    void Start(uint16_t port, uint32_t, uint32_t) override;

    // See Server.h
    void Stop() override;

    // See Server.h
    void Join() override;

protected:
    /**
     * Method is running in the connection acceptor thread
     */
    void OnRun();

    void ClientProcess(int client_socket);

private:
    // Logger instance
    std::shared_ptr<spdlog::logger> _logger;

    // Atomic flag to notify threads when it is time to stop. Note that
    // flag must be atomic in order to safely publisj changes cross thread
    // bounds
    std::atomic<bool> running;

    // Server socket to accept connections on
    int _server_socket;

    // Thread to run network on
    std::thread _thread;

    // limit of workers
    int limit_workers;

    // current number of workers
    std::atomic<int> workers_count;

    // to open and close socket
    std::mutex socket_mutex;

    // for opened clients sockets connections
    std::vector<int> sockets;
    
    // synchronization primitive to controll threads blocking
    std::condition_variable s_stop;
};

} // namespace MTblocking
} // namespace Network
} // namespace Afina
