#include "Connection.h"
#include "ServerImpl.h"

#include <atomic>
#include <iostream>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace Afina 
{
namespace Network 
{
namespace MTnonblock 
{

// See Connection.h
void Connection::Start() 
{ 
    _is_alive.store(true, std::memory_order_relaxed); // Упрощенная операция: нет ограничений на синхронизацию или упорядочение, налагаемых на другие операции чтения или записи, гарантируется только атомарность этой операции.
    _event.events = EPOLLIN | EPOLLRDHUP; // чтение или обрыв
    _w_offset = 0;
    _r_offset = 0;
}

// See Connection.h
void Connection::OnError() 
{ 
    _is_alive.store(false, std::memory_order_relaxed);
}

// See Connection.h
void Connection::OnClose() 
{ 
    _is_alive.store(false, std::memory_order_relaxed);
}

// See Connection.h
void Connection::DoRead() 
{
    std::atomic_thread_fence(std::memory_order_acquire); // с упорядочением memory_order_release предотвращает перемещение всех предшествующих операций записи за пределы всех последующих хранилищ
    //Устанавливает порядок синхронизации памяти для неатомарных и упрощенных атомарных доступов в соответствии с инструкциями по порядку, без связанной атомарной операции.

    // Единственный случай, когда read должен записывать не с начала client_buffer, а с оффсета, когда Parse() вернул 0
    // Во всех случаях в следующем read() read_offset == 0

    // читаем команды пока сокет жив, выполняем их, отправляем ответ
    try 
    {
        // читаем, начиная с отступа
        int readed_bytes = read(_socket, client_buffer + _r_offset, sizeof(client_buffer) - _r_offset);
        if (readed_bytes > 0)
        {
            readed_bytes += _r_offset; // число прочитанных + отступ, который уже был
            std::size_t cur_offset = 0;
            while (readed_bytes > 0) 
            {
                // There is no command yet
                if (!command_to_execute) 
                {
                    std::size_t parsed = 0;
                    if (_parser.Parse(client_buffer + cur_offset, readed_bytes, parsed)) 
                    {
                        // There is no command to be launched, continue to parse input stream
                        // Here we are, current chunk finished some command, process it
                        command_to_execute = _parser.Build(arg_remains);
                        if (arg_remains > 0) 
                        {
                            arg_remains += 2;
                        }
                    }

                    // Parsed might fails to consume any bytes from input stream. In real life that could happens,
                    // for example, because we are working with UTF-16 chars and only 1 byte left in stream
                    if (parsed == 0) 
                    {
                        _r_offset = readed_bytes;
                        std::memmove(client_buffer, client_buffer + cur_offset, _r_offset);
                        break;
                    } 
                    else 
                    {
                        readed_bytes -= parsed;
                        cur_offset += parsed;
                    }
                }
                // There is command, but we still wait for argument to arrive...
                if (command_to_execute && arg_remains > 0) 
                {
                    // There is some parsed command, and now we are reading argument
                    std::size_t to_read = std::min(arg_remains, std::size_t(readed_bytes));
                    argument_for_command.append(client_buffer + cur_offset, to_read);
                    cur_offset += to_read;
                    arg_remains -= to_read;
                    readed_bytes -= to_read;
                }

                // Thre is command & argument - RUN!
                if (command_to_execute && arg_remains == 0) 
                {
                    std::string result;
                    if (argument_for_command.size()) 
                    {
                        argument_for_command.resize(argument_for_command.size() - 2);
                    }
                    command_to_execute->Execute(*pStorage, argument_for_command, result);
                    // Send response
                    result += "\r\n";
                    if (_output_queue.empty()) // если очередь пуста
                    {
                        _event.events |= EPOLLOUT;
                    }
                    _output_queue.push_back(result); // записываем результат в очередь
                    if (_output_queue.size() >= 100) // верхняя граница очереди с лекции
                    {
                        _event.events &= ~EPOLLIN;
                    }
                    // Prepare for the next command
                    command_to_execute.reset();
                    argument_for_command.resize(0);
                    _parser.Reset();
                }
            } // while (readed_bytes)
            if (readed_bytes == 0) 
            {
                _r_offset = 0;
            }
        }
        else if (readed_bytes == 0) 
        {
            //_logger->debug("Connection closed");
        }
        else if (!(errno == EAGAIN || errno == EINTR)) 
        {
            throw std::runtime_error(std::string(strerror(errno)));
        }
    }
    catch (std::runtime_error &ex) 
    {
        //_logger->error("Failed to process connection on descriptor {}: {}", _socket, ex.what());
        std::cerr << ("Failed to process connection on descriptor {}: {}", _socket, ex.what()) << std::endl;
        _is_alive.store(false, std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_release);
}

// See Connection.h
void Connection::DoWrite() 
{
    std::atomic_thread_fence(std::memory_order_acquire);
    struct iovec iovs[_output_queue.size()] = {};
    size_t i = 0;
    for (i = 0; i < _output_queue.size(); ++i)
    {
        iovs[i].iov_base = &(_output_queue[i][0]); // указатель на буфер
        iovs[i].iov_len = _output_queue[i].size(); // длина буфера
    }
    iovs[0].iov_base = static_cast<char *>(iovs[0].iov_base) + _w_offset;
    iovs[0].iov_len -= _w_offset;
    int written = writev(_socket, iovs, i); // сколько байт записали
    if (written < 0 && !(errno == EAGAIN || errno == EINTR)) 
    {
        this->OnError();
    }
    else
    {
        i = 0;
        for (auto j: _output_queue) 
        {
            if (written - j.size() >= 0)  /// записываем сколько можем
            {
                i++;
                written -= j.size();
            } 
            else 
            {
                break;
            }
        }
        _output_queue.erase(_output_queue.begin(), _output_queue.begin() + i); // удаляем сколько записали
        _w_offset = written;
    }
    if (_output_queue.empty()) 
    {
        _event.events &= ~EPOLLOUT;
    }
    if (_output_queue.size() < 90) // на лекции говорили, что нижняя граница должна чуть отличаться
    {
        _event.events |= EPOLLIN;
    }
    std::atomic_thread_fence(std::memory_order_release);
    // memory_order_acquire 
    // Операция загрузки с этим порядком памяти выполняет операцию получения в затронутой области памяти: 
    // никакие операции чтения или записи в текущем потоке не могут быть переупорядочены до этой загрузки. 
    // Все записи в других потоках, которые освобождают ту же атомарную переменную, видны в текущем потоке.
    
    // memory_order_release
    // Операция сохранения с этим порядком памяти выполняет операцию освобождения: 
    // никакие операции чтения или записи в текущем потоке не могут быть переупорядочены после этого сохранения. 
    // Все записи в текущем потоке видны в других потоках, которые получают ту же атомарную переменную,
    // а записи, которые несут зависимость в атомарную переменную, становятся видимыми в других потоках, которые используют такую же атомарную переменную 
}

} // namespace MTnonblock
} // namespace Network
} // namespace Afina
