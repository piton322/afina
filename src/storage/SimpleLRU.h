#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <afina/Storage.h>

namespace Afina 
{
namespace Backend 
{

/**
 * # Map based implementation
 * That is NOT thread safe implementaiton!!
 */
class SimpleLRU : public Afina::Storage 
{
public:
    SimpleLRU(size_t max_size = 1024) : _max_size(max_size) {}

    ~SimpleLRU() 
    {
        _lru_index.clear();
        while (_lru_head != nullptr)
        {
            _lru_head = std::move(_lru_head->next);
        }
        _lru_last = nullptr;
    }

    // Implements Afina::Storage interface
    bool Put(const std::string & key, const std::string & value) override;

    // Implements Afina::Storage interface
    bool PutIfAbsent(const std::string & key, const std::string & value) override;

    // Implements Afina::Storage interface
    bool Set(const std::string & key, const std::string & value) override;

    // Implements Afina::Storage interface
    bool Delete(const std::string & key) override;

    // Implements Afina::Storage interface
    bool Get(const std::string & key, std::string & value) override;

private:
    // LRU cache node
    using lru_node = struct lru_node 
    {
        const std::string key; // не меняем иначе проблемы
        std::string value;
        lru_node * prev;
        std::unique_ptr<lru_node> next; // только один unique должен быть
    };

    // Maximum number of bytes could be stored in this cache.
    // i.e all (keys+values) must be not greater than the _max_size
    const size_t _max_size;
    size_t _current_size = 0;

    // Main storage of lru_nodes, elements in this list ordered descending by "freshness": in the head
    // element that wasn't used for longest time.
    //
    // List owns all nodes
    std::unique_ptr<lru_node> _lru_head;
    lru_node * _lru_last = nullptr; // потребуется указатель на хвост для быстрого доступа

    // Index of nodes from list above, allows fast random access to elements by lru_node#key
    std::map<std::reference_wrapper<const std::string>, std::reference_wrapper<lru_node>, std::less<std::string>> _lru_index;
    // reference_wrapper - обертка над ссылкой на объект или на функцию
    // map — отсортированный ассоциативный контейнер, который содержит пары ключ-значение с неповторяющимися ключами. 
    
    void ToHead(lru_node * node);

    void DeleteLast();

    bool UpdateValue(lru_node & node, const std::string & value);

    bool PutNewPair(const std::string & key, const std::string & value);
};

} // namespace Backend
} // namespace Afina
