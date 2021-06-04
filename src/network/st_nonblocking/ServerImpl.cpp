#include "ServerImpl.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <spdlog/logger.h>

#include <afina/Storage.h>
#include <afina/logging/Service.h>

#include "Connection.h"
#include "Utils.h"

namespace Afina 
{
namespace Network 
{
namespace STnonblock 
{

// See Server.h
ServerImpl::ServerImpl(std::shared_ptr<Afina::Storage> ps, std::shared_ptr<Logging::Service> pl) : Server(ps, pl) {}

// See Server.h
ServerImpl::~ServerImpl() 
{
    Stop();
    Join();
}

// See Server.h
void ServerImpl::Start(uint16_t port, uint32_t n_acceptors, uint32_t n_workers) 
{
    _logger = pLogging->select("network");
    _logger->info("Start st_nonblocking network service");

    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &sig_mask, NULL) != 0) 
    {
        throw std::runtime_error("Unable to mask SIGPIPE");
    }

    // Create server socket
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_port = htons(port);       // TCP port number
    server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to any address

    // создаем серверный сокет
    _server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_server_socket == -1) 
    {
        throw std::runtime_error("Failed to open socket: " + std::string(strerror(errno)));
    }

    int opts = 1;
    if (setsockopt(_server_socket, SOL_SOCKET, (SO_KEEPALIVE), &opts, sizeof(opts)) == -1) 
    {
        close(_server_socket);
        throw std::runtime_error("Socket setsockopt() failed: " + std::string(strerror(errno)));
    }

    if (bind(_server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) 
    {
        close(_server_socket);
        throw std::runtime_error("Socket bind() failed: " + std::string(strerror(errno)));
    }

    make_socket_non_blocking(_server_socket);
    if (listen(_server_socket, 5) == -1) 
    {
        close(_server_socket);
        throw std::runtime_error("Socket listen() failed: " + std::string(strerror(errno)));
    }

    _event_fd = eventfd(0, EFD_NONBLOCK); // специальный файловый дескриптор, где ничего не происходит пока его не разбудят, используется как conditional variable
    if (_event_fd == -1) 
    {
        throw std::runtime_error("Failed to create epoll file descriptor: " + std::string(strerror(errno)));
    }
    // вызывает метод OnRun
    _work_thread = std::thread(&ServerImpl::OnRun, this);
}

// See Server.h
void ServerImpl::Stop() 
{
    _logger->warn("Stop network service");
    for (auto connection : _connections) 
    {
        shutdown(connection->_socket, SHUT_RD);
    }
    // Wakeup threads that are sleep on epoll_wait
    if (eventfd_write(_event_fd, 1)) // чтобы разубдить
    {
        throw std::runtime_error("Failed to wakeup workers");
    }
}

// See Server.h
void ServerImpl::Join() 
{
    // Wait for work to be complete
    if (_work_thread.joinable()) 
    {
        _work_thread.join();
    }
}

// See ServerImpl.h
void ServerImpl::OnRun() 
{
    _logger->info("Start acceptor");
    int epoll_descr = epoll_create1(0); // создаем epoll дескриптор
    if (epoll_descr == -1) 
    {
        throw std::runtime_error("Failed to create epoll file descriptor: " + std::string(strerror(errno)));
    }
    // запихиваем туда серверный сокет с интересом на чтение
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = _server_socket;
    if (epoll_ctl(epoll_descr, EPOLL_CTL_ADD, _server_socket, &event)) 
    {
        throw std::runtime_error("Failed to add file descriptor to epoll");
    }
    // затем запихиваем туда _event_fd с интересом на чтение (чтобы можно было снаружи разбудить epoll)
    struct epoll_event event2;
    event2.events = EPOLLIN;
    event2.data.fd = _event_fd;
    if (epoll_ctl(epoll_descr, EPOLL_CTL_ADD, _event_fd, &event2)) 
    {
        throw std::runtime_error("Failed to add file descriptor to epoll");
    }
    // создаем массив eppol_event, где мы будем слушать, какие события произошли
    bool run = true;
    std::array<struct epoll_event, 64> mod_list;
    while (run) 
    {
        // вызываем epoll_wait
        int nmod = epoll_wait(epoll_descr, &mod_list[0], mod_list.size(), -1);
        _logger->debug("Acceptor wokeup: {} events", nmod);
        for (int i = 0; i < nmod; i++) 
        {
            struct epoll_event &current_event = mod_list[i];
            // когда мы выходим из wait, мы смотрим если случилось _event_fd, значит, что кто-то вызвал stop, нас хотят разубдить
            if (current_event.data.fd == _event_fd) 
            {
                _logger->debug("Break acceptor due to stop signal");
                run = false; // говорим, что исполняться больше не надо и выходим
                continue;
            } 
            else if (current_event.data.fd == _server_socket) 
            {
                // если это случилось на серверном сокете, то значит есть новое соединение
                OnNewConnection(epoll_descr);
                continue;
            }
            // если это  не _event_fd и не серверный сокет, значит это connetion
            Connection *pc = static_cast<Connection *>(current_event.data.ptr); // из поля data.ptr достаем указатель на соответствующий connetion

            auto old_mask = pc->_event.events; // запоминаем с каким интересом был connection
            if ((current_event.events & EPOLLERR) || (current_event.events & EPOLLHUP)) // если epoll сказал, что ошибка вызываем OnError()
            {
                pc->OnError();
            } 
            else if (current_event.events & EPOLLRDHUP) // если соединение оборвалось, вызываем OnClose()
            {
                pc->OnClose();
            } 
            else 
            {
                // если это не ошибка и соединение не оборвалось
                if (current_event.events & EPOLLIN) // если доступен на чтение, вызываем DoRead()
                {
                    pc->DoRead();
                }
                if (current_event.events & EPOLLOUT)  // если доступен для записи, вызываем DoWrite()
                {
                    pc->DoWrite();
                }
            }

            // после того как все обработали, epoll спрашивает, жив ли connection
            if (!pc->isAlive())  // если false, то поток удаляет дескриптор из epoll
            {
                if (epoll_ctl(epoll_descr, EPOLL_CTL_DEL, pc->_socket, &pc->_event)) 
                {
                    _logger->error("Failed to delete connection from epoll");
                }
                _connections.erase(pc);
                // закрывает сокет
                close(pc->_socket);
                // говорит, что сокет был закрыт
                pc->OnClose();
                // удаляет connetion
                delete pc;
            } 
            else if (pc->_event.events != old_mask)  // если соединение еще живо, то проверяем не изменилась ли маска евентов
            {
                // если маска изменилась, то перерегистрируем ее
                if (epoll_ctl(epoll_descr, EPOLL_CTL_MOD, pc->_socket, &pc->_event)) 
                {
                    _logger->error("Failed to change connection event mask");
                    _connections.erase(pc);
                    close(pc->_socket);
                    pc->OnClose();

                    delete pc;
                }
            }
        }
    }
    close(_server_socket);
    _logger->warn("Acceptor stopped");
    for (auto connection : _connections) 
    {
        close(connection->_socket);
        _connections.erase(connection);
        connection->OnClose();
        delete connection;
    }
}

void ServerImpl::OnNewConnection(int epoll_descr) 
{
    for (;;) 
    {
        struct sockaddr in_addr;
        socklen_t in_len;

        // No need to make these sockets non blocking since accept4() takes care of it.
        in_len = sizeof in_addr;
        // делаем accept
        int infd = accept4(_server_socket, &in_addr, &in_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (infd == -1) 
        {
            // до тех пор пока не получим EAGAIN или EWOULDBLOCK
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) 
            {
                break; // We have processed all incoming connections.
            } 
            else 
            {
                _logger->error("Failed to accept socket");
                break;
            }
        }

        // Print host and service info.
        char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
        int retval =
            getnameinfo(&in_addr, in_len, hbuf, sizeof hbuf, sbuf, sizeof sbuf, NI_NUMERICHOST | NI_NUMERICSERV);
        if (retval == 0) 
        {
            _logger->info("Accepted connection on descriptor {} (host={}, port={})\n", infd, hbuf, sbuf);
        }

        // Register the new FD to be monitored by epoll.
        // для каждого соединения, которое пришло, создаем экземпляр класса Connetion
        Connection *pc = new(std::nothrow) Connection(infd, pStorage);
        if (pc == nullptr) 
        {
            throw std::runtime_error("Failed to allocate connection");
        }
        _connections.insert(pc);
        // Register connection in worker's epoll
        // у connection вызываем метод start, если после этого он еще живой, то добавляем его в epoll дескриптор с теми интересами, которые этот connection хочет
        pc->Start();
        if (pc->isAlive()) 
        {
            if (epoll_ctl(epoll_descr, EPOLL_CTL_ADD, pc->_socket, &pc->_event)) 
            {
                close(pc->_socket);
                pc->OnError();
                _connections.erase(pc);
                delete pc;
            }
        }
    }
}

} // namespace STnonblock
} // namespace Network
} // namespace Afina
