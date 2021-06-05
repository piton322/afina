#ifndef AFINA_NETWORK_MT_NONBLOCKING_CONNECTION_H
#define AFINA_NETWORK_MT_NONBLOCKING_CONNECTION_H

#include "afina/Storage.h"
#include "afina/logging/Service.h"
#include "afina/execute/Command.h"
#include "spdlog/logger.h"
#include <cstring>
#include <atomic>
#include <deque>
#include <sys/epoll.h>
#include "protocol/Parser.h"

namespace Afina 
{
namespace Network 
{
namespace MTnonblock 
{

class Connection 
{
public:
    Connection(int s, std::shared_ptr<Afina::Storage> &ps) 
        : _socket(s), _is_alive(true), pStorage(ps)
    {
        std::memset(&_event, 0, sizeof(struct epoll_event));
        _event.data.ptr = this;
    }

    inline bool isAlive() const { return _is_alive; } // периодически вызывается epoll'ом с целью понять, не хотим ли бы убить этот connetion

    void Start();

protected:
    void OnError();
    void OnClose();
    void DoRead();
    void DoWrite();

private:
    friend class Worker;
    friend class ServerImpl;

    int _socket; // сокет, который обрабатывается
    struct epoll_event _event; // евенты, с которыми connection хочет быть добавлен в epoll дескриптор
    std::atomic<bool> _is_alive;

    std::size_t _w_offset; // отступ при чтени
    std::size_t _r_offset; // отступ при записи
    std::deque<std::string> _output_queue; // очередь, куда будем записывать результаты

    std::shared_ptr<Afina::Storage> pStorage;
    // для чтения (взято из предыдущих дз)
    std::size_t arg_remains;
    Protocol::Parser _parser;
    std::string argument_for_command;
    std::unique_ptr<Execute::Command> command_to_execute;
    char client_buffer[4096] = "";
};

} // namespace MTnonblock
} // namespace Network
} // namespace Afina

#endif // AFINA_NETWORK_MT_NONBLOCKING_CONNECTION_H
