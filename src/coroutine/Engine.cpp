#include <afina/coroutine/Engine.h>

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

namespace Afina 
{
namespace Coroutine 
{

void Engine::Store(context &ctx) 
{
    char begin;
    if(&begin <= ctx.Low)
    {
        ctx.Low = &begin;
    }
    else
    {
        ctx.Hight = &begin;
    }
    auto cur_size = ctx.Hight - ctx.Low;
    // если выделено слишком мало или слишком много, то для перевыделяем на нужный размер
    if (cur_size > std::get<1>(ctx.Stack) || (cur_size * 2) < std::get<1>(ctx.Stack)) 
    {
        delete[] std::get<0>(ctx.Stack); // удаляем то, что есть
        std::get<0>(ctx.Stack) = new char[cur_size]; // выделяем с нужным размером
        std::get<1>(ctx.Stack) = cur_size; // запоминаем размер
    }
    memcpy(std::get<0>(ctx.Stack), ctx.Low, cur_size); // переносим содержимое
}

void Engine::Restore(context &ctx) 
{
    char begin;
    // сохраняем стек restored корутины
    if (&begin >= ctx.Low && &begin <= ctx.Hight) 
    {
        Restore(ctx);
    }
    auto cur_size = ctx.Hight - ctx.Low;
    // теперь можно восстановить стак поданной корутины, ничего не портя
    memcpy(ctx.Low, std::get<0>(ctx.Stack), cur_size);
    cur_routine = &ctx;
    // запускаем корутину с момента, где она остановилась
    longjmp(ctx.Environment, 1);
}

void Engine::SaveAndStart(context * next)
{
    if (cur_routine != idle_ctx) // если у нас не осталась та самя, которая нужна, когда все заблокированны
    {
        if (setjmp(cur_routine->Environment) > 0) // пытаемся восстановить регистры
        {
            return;
        }
        Store(*cur_routine); // сохраняем стек текущей корутины
    }
    Restore(*next); // восстанавливаем стек следующей корутины
}

void Engine::yield() 
{
    if (!alive) // если нет живых
    {
    	return;
    }
    if (cur_routine == alive && !alive->next) // если текущая живая, а больше живых нет
    {
        return;
    }
    context * next = alive;
    if (cur_routine == alive) //  если текущая живая, то будем запускать следующую живую
    {
        next = alive->next;
    }
    SaveAndStart(next);
}

void Engine::sched(void *routine_) 
{
    auto next = static_cast<context *>(routine_);
    if (next == nullptr) // если nullptr, то предыдущая функция
    {
        yield();
    }
    
    if (next->block_flag) // если заблокирована
    {
        return;
    }
    if (next == cur_routine) // если является текущей
    {
        return;
    }
    SaveAndStart(next);
}

void Engine::delete_routine(context *& start, context *& element) 
{
    if (start == element) // если элемент в голове
    {
        start = start->next;
    }
    if (element->prev != nullptr) // если не в голове, то перекидываем ссылку предыдущего элемента на следующий за элементом
    {
        element->prev->next = element->next;
    }
    if (element->next != nullptr) // если не в конце, то перекидываем ссылку назад следующего элемента, на предыдущий элемент
    {
        element->next->prev = element->prev;
    }
}

void Engine::add_routine(context *& start, context *& element)
{
    if (start == nullptr) // если ничего не было, то добавляем первый элемент
    {
        start = element;
        start->next = nullptr;
    } 
    else // если что-то было
    {
        start->prev = element; // добавляем элемент слева
        element->next = start;
        start = element; //  меняем стартовую точку
    }
    start->prev = nullptr; // слева у нас в любом случае nullptr, т к start это начало
}

void Engine::block(void *coro) 
{
    context * block_routine = static_cast<context *>(coro);
    
    if (block_routine == nullptr) // по описанию функции в таком случае надо заблокировать текущую
    {
        block_routine = cur_routine;
    }
    if(block_routine->block_flag) // зачем блокировать заблокированное
    {
        return;
    }
    block_routine->block_flag = true;
    // удаляем корутину из списка живых
    delete_routine(alive, block_routine);
    // добавляем в список заблокированных
    add_routine(blocked, block_routine);

    if (block_routine == cur_routine) 
    {
        yield(); // из описания функции block: "If it was a currently running corountine, then do yield to select new one to be run instead."
    }
}

void Engine::unblock(void *coro) 
{
    context * unblock_routine = static_cast<context *>(coro);
    if (!unblock_routine)
    {
        return;
    }
    // если уже живая, то зачем ее разблокировать
    if(!unblock_routine->block_flag) 
    {
        return;
    }
    unblock_routine->block_flag = false;
    // удаляем из списка заблокированных
    delete_routine(blocked, unblock_routine);
    // добавляем в список живых
    add_routine(alive, unblock_routine);
}

Engine::~Engine() 
{
    for (auto routine = alive;;) 
    {
        if(routine == nullptr) // идем по списку живых пока не дойдем до nullptr
        {
            break;
        }
        auto buf = routine;
        // удаляем голову и все что с ней связано
        routine = routine->next;
        delete[] std::get<0>(buf->Stack);
        delete buf;
    }

    for (auto routine = blocked;;) 
    {
        if(routine == nullptr) // идем по списку живых пока не дойдем до nullptr
        {
            break;
        }
        auto buf = routine;
        // удаляем голову и все что с ней связано
        routine = routine->next;
        delete[] std::get<0>(buf->Stack);
        delete buf;
    }
}
} // namespace Coroutine
} // namespace Afina
