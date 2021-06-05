#ifndef AFINA_COROUTINE_ENGINE_H
#define AFINA_COROUTINE_ENGINE_H

#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <tuple>

#include <setjmp.h>

namespace Afina 
{
namespace Coroutine 
{

/* Точка входа в библиотеку корутин
 * Позволяет запускать корутину и планировать ее выполнение. Не потокобезопасно
 */
class Engine final 
{
public:
    using unblocker_func = std::function<void(Engine &)>;

private:
    /**
     * Один экземпляр корутины, который можно распланировать для выполнения
     * должен быть размещен в куче
     */
    struct context;
    typedef struct context 
    {
        // адресс, где начинается стек корутины
        char *Low = nullptr;

        // адресс, где заканчивается стек корутины
        char *Hight = nullptr;

        // память, выделенная под стек корутины
        std::tuple<char *, uint32_t> Stack = std::make_tuple(nullptr, 0);

        // сохранение регистров в буфер
        jmp_buf Environment;

        // заблокирована или нет
        bool block_flag = false;

        // двунаправленный список для удобства
        struct context *prev = nullptr;
        struct context *next = nullptr;
    } context;

    // начало стеков корутин
    char *StackBottom;

    // исполняемая корутина
    context *cur_routine;

    // список живых корутин
    context *alive;

    // список заблокированных корутин (нельзя разблокировать до внешней команды)
    context *blocked;

    // корутина, которая нужна когда все заблокированны
    context *idle_ctx;

    // вызывается, когда все корутины заблокированны
    unblocker_func _unblocker;

protected:
    // сохраняет стек текущей корутины
    void Store(context &ctx);

    // восстанавливает стек поданной корутины и отдает контроль
    void Restore(context &ctx);

    static void null_unblocker(Engine &) {}

public:
    Engine(unblocker_func unblocker = null_unblocker)
        : StackBottom(0), cur_routine(nullptr), alive(nullptr), _unblocker(unblocker), blocked(nullptr) {}
    Engine(Engine &&) = delete;
    Engine(const Engine &) = delete;
    ~Engine();

    /**
     * Gives up current routine execution and let engine to schedule other one. It is not defined when
     * routine will get execution back, for example if there are no other coroutines then executing could
     * be trasferred back immediately (yield turns to be noop).
     *
     * Also there are no guarantee what coroutine will get execution, it could be caller of the current one or
     * any other which is ready to run
     */
     // ставит корутину на паузу и отадет управление другой случайной (которая alive)
    void yield();

    /**
     * Suspend current routine and transfers control to the given one, resumes its execution from the point
     * when it has been suspended previously.
     *
     * If routine to pass execution to is not specified (nullptr) then method should behaves like yield. In case
     * if passed routine is the current one method does nothing
     */
    // ставит корутину на паузу и отадет управление выбранной (если nullptr, то yiled)
    void sched(void *routine);

    /**
     * Blocks current routine so that is can't be scheduled anymore
     * If it was a currently running corountine, then do yield to select new one to be run instead.
     *
     * If argument is nullptr then block current coroutine
     */
    void block(void *coro = nullptr);

    /**
     * Put coroutine back to list of alive, so that it could be scheduled later
     */
    void unblock(void *coro);

    // здесь будем сохранять стек текущей корутины и восстанавливать стек следующей корутины
    void SaveAndStart(context * next);

    // для удаления корутины из списка живых или списка заблокированных
    void delete_routine(context *& start, context *& element);

    // для добавления корутины в список живых или список заблокированных
    void add_routine(context *& start, context *& element);

    /**
     * Entry point into the engine. Prepare all internal mechanics and starts given function which is
     * considered as main.
     *
     * Once control returns back to caller of start all coroutines are done execution, in other words,
     * this function doesn't return control until all coroutines are done.
     *
     * @param pointer to the main coroutine
     * @param arguments to be passed to the main coroutine
     */
    template <typename... Ta> void start(void (*main)(Ta...), Ta &&... args) {
        // To acquire stack begin, create variable on stack and remember its address
        // начало стека, запоминаем адресс
        char StackStartsHere;
        this->StackBottom = &StackStartsHere;

        // выполняем корутину
        void *pc = run(main, std::forward<Ta>(args)...);

        idle_ctx = new context();
        idle_ctx->Low = idle_ctx->Hight = StackBottom;
        if (setjmp(idle_ctx->Environment) > 0) 
        {
            if (alive == nullptr) 
            {
                _unblocker(*this);
            }

            // Here: correct finish of the coroutine section
            cur_routine = idle_ctx;
            yield();
        } 
        else if (pc != nullptr) 
        {
            Store(*idle_ctx);
            cur_routine = idle_ctx;
            sched(pc);
        }

        // Shutdown runtime
        delete[] std::get<0>(idle_ctx->Stack);
        delete idle_ctx;
        this->StackBottom = 0;
    }

    /**
     * Register new coroutine. It won't receive control until scheduled explicitely or implicitly. In case of some
     * errors function returns -1
     */
    template <typename... Ta> void *run(void (*func)(Ta...), Ta &&... args) 
    {
        char addr;
        return run1(&addr, func, std::forward<Ta>(args)...); // нам нужен адрес начала стека
    }
    template <typename... Ta> void *run1(char *addr, void (*func)(Ta...), Ta &&... args) 
    {
        if (this->StackBottom == 0) 
        {
            // Engine wasn't initialized yet
            return nullptr;
        }

        // New coroutine context that carries around all information enough to call function
        context *pc = new context();
        pc->Low = pc->Hight = addr;

        // Store current state right here, i.e just before enter new coroutine, later, once it gets scheduled
        // execution starts here. Note that we have to acquire stack of the current function call to ensure
        // that function parameters will be passed along
        if (setjmp(pc->Environment) > 0) 
        {
            // Created routine got control in order to start execution. Note that all variables, such as
            // context pointer, arguments and a pointer to the function comes from restored stack

            // invoke routine
            func(std::forward<Ta>(args)...);

            // Routine has completed its execution, time to delete it. Note that we should be extremely careful in where
            // to pass control after that. We never want to go backward by stack as that would mean to go backward in
            // time. Function run() has already return once (when setjmp returns 0), so return second return from run
            // would looks a bit awkward
            if (pc->prev != nullptr) 
            {
                pc->prev->next = pc->next;
            }

            if (pc->next != nullptr) 
            {
                pc->next->prev = pc->prev;
            }

            if (alive == cur_routine) 
            {
                alive = alive->next;
            }

            // current coroutine finished, and the pointer is not relevant now
            cur_routine = nullptr;
            pc->prev = pc->next = nullptr;
            delete std::get<0>(pc->Stack);
            delete pc;

            // We cannot return here, as this function "returned" once already, so here we must select some other
            // coroutine to run. As current coroutine is completed and can't be scheduled anymore, it is safe to
            // just give up and ask scheduler code to select someone else, control will never returns to this one
            Restore(*idle_ctx);
        }

        // setjmp remembers position from which routine could starts execution, but to make it correctly
        // it is neccessary to save arguments, pointer to body function, pointer to context, e.t.c - i.e
        // save stack.
        Store(*pc);

        // Add routine as alive double-linked list
        pc->next = alive;
        alive = pc;
        if (pc->next != nullptr) 
        {
            pc->next->prev = pc;
        }

        return pc;
    }
};

} // namespace Coroutine
} // namespace Afina

#endif // AFINA_COROUTINE_ENGINE_H
