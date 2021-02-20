#include "SimpleLRU.h"
#include "iostream"
using namespace std;

namespace Afina 
{
namespace Backend 
{

void SimpleLRU::ToHead(lru_node * node)
{
    if (node == _lru_head.get())
    {
        return; // голова
    }
    auto save = move(node->prev->next); // запоминаем
    if (node->next == nullptr) // если в конце
    {
    	_lru_last = save->prev; // последний теперь предпоследний
    }
    else
    {
    	save->next->prev = save->prev; // если в середине, то двигаем на 1 назад
    }
    save->prev->next = move(save->next); // передвигаем следующий на 1 назад
    save->next = move(_lru_head); // тащим голову
    save->next->prev = save.get();
    save->prev = nullptr;
    _lru_head = move(save); // настраиваем остатки
}

void SimpleLRU::DeleteLast()
{
    lru_node * last = _lru_last;
    _current_size -= last->key.size(); // удаляем последний элемент
    _current_size -= last->value.size();
    _lru_index.erase(last->key);
    if (last->prev == nullptr)
    {
        // если последний элемент это голова
        _lru_head.reset(); // Destroys the object currently managed by the unique_ptr (if any) and takes ownership of p.
    }
    else
    {
        _lru_last = last->prev;
	_lru_last->next.reset();
    }
}

bool SimpleLRU::UpdateValue(SimpleLRU::lru_node & node, const string & value)
{
    size_t new_size = value.size();
    size_t old_size = node.value.size();
    if (node.key.size() + new_size > _max_size)
    {
        return false;
    }
    ToHead(& node); // тащим ноду в начало
    while(_current_size + new_size - old_size > _max_size)
    {
        DeleteLast(); // удаляем хвост пока не поместится
    }
    _current_size -= old_size;
    _current_size += new_size; 
    node.value = value;
    return true;
}

bool SimpleLRU::PutNewPair(const string & key, const string & value)
{
    size_t new_size = key.size() + value.size();
    while (new_size + _current_size > _max_size)
	{
		DeleteLast(); // удаляем хвост пока не поместится
	}
    auto new_node = new lru_node{key, value, nullptr, nullptr};
    if (_lru_head != nullptr)
    {
        _lru_head->prev = new_node;
    }
    else
    {
    	_lru_last = new_node;
    }
    new_node->next = move(_lru_head);
    _lru_index.emplace(make_pair(reference_wrapper<const string>(new_node->key), reference_wrapper<lru_node>(*new_node)));
    _lru_head = unique_ptr<lru_node>(new_node);
    _current_size += new_size;
    return true; 
}

// Put, безусловно сохраняет пару ключ/значение
bool SimpleLRU::Put(const string & key, const string & value) 
{
    size_t cur_size = key.size() + value.size();
    if (cur_size > _max_size)
    {
        return false;
    }
    // If no such element is found, past-the-end (see end()) iterator is returned.
    auto result = _lru_index.find(key);
    if (result != _lru_index.end())
    {
        return SimpleLRU::UpdateValue((result->second).get(), value);
    }
    else
    {	
        return SimpleLRU::PutNewPair(key, value);
    }
}

// PutIfAbsent, сохраняет пару только если в контейнере еще нет такого ключа
bool SimpleLRU::PutIfAbsent(const string & key, const string & value) 
{
    size_t cur_size = key.size() + value.size();
    if (cur_size > _max_size)
    {
        return false;
    }
    auto result = _lru_index.find(key);
    if (result == _lru_index.end())
    {
        return SimpleLRU::PutNewPair(key, value);
    }
    return false; 
}

// Set, устанавливает новое значение для ключа. Работает только если ключ уже представлен в хранилище
bool SimpleLRU::Set(const string & key, const string & value)
{
    size_t cur_size = key.size() + value.size();
    if (cur_size > _max_size)
    {
        return false;
    }
    // If no such element is found, past-the-end (see end()) iterator is returned.
    auto result = _lru_index.find(key);
    if (result != _lru_index.end())
    {
        return SimpleLRU::UpdateValue((result->second).get(), value);
    }
    return false; 
}

// Delete, удаляет пару ключ/значение из хранилища
bool SimpleLRU::Delete(const string & key)
{ 
    if (_lru_head == nullptr) 
    {
        return false;
    }
    auto result = _lru_index.find(key);
    if (result == _lru_index.end()) 
    {
        return false;
    }
    auto node_ref = result->second;
    size_t node_size = node_ref.get().key.size() + node_ref.get().value.size();
    _lru_index.erase(result);
    if (node_ref.get().next != nullptr) 
    {
        node_ref.get().next->prev = node_ref.get().prev; // предыдущий к следующему - предыдущий к нынешнему
    }
    else 
    {
        // удаляем в конце
        _lru_last = node_ref.get().prev;
    }
    if (node_ref.get().prev != nullptr) 
    {
        node_ref.get().prev->next = move(node_ref.get().next); // следующий к предыдущему - следующий к нынешнему
    }
    else 
    {
        // находимся в голове
        _lru_head = move(node_ref.get().next);
    }
    _current_size -= node_size;
    return true;
}

// Get, возвращает значение для ключа
bool SimpleLRU::Get(const string & key, string & value)
{
    auto result = _lru_index.find(key);
	if (result ==  _lru_index.end())
	{
        return false;
	}
	value = result->second.get().value;
	ToHead(& result->second.get()); // обратились, поэтому сдвигаем
	return true;
}

} // namespace Backend
} // namespace Afina
