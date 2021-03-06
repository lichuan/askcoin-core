#ifndef TIMER
#define TIMER

#include <memory>
#include <set>
#include <unordered_map>
#include <mutex>
#include "fly/base/common.hpp"

class Timer
{
public:
    struct Comp
    {
        bool operator()(const std::shared_ptr<Timer> &a, const std::shared_ptr<Timer> &b)
        {
            return a->m_tick < b->m_tick;
        }
    };
    
    Timer(uint64 id, uint64 tick, std::function<void()> cb, uint32 interval_tick, bool oneshot = false);
    ~Timer();
    static uint64 now_msec();
    uint64 m_tick;
    uint64 m_id;
    uint32 m_interval_tick;
    bool m_oneshot;
    std::function<void()> m_cb;
};

class Timer_Controller
{
public:
    Timer_Controller();
    ~Timer_Controller();
    uint64 add_timer(std::function<void()> cb, uint32 interval, bool oneshot = false);
    void reset_timer(uint64 id);
    void del_timer(uint64 id);
    bool run();
    void clear();
    
private:
    std::multiset<std::shared_ptr<Timer>, Timer::Comp> m_timers;
    std::unordered_map<uint64, std::shared_ptr<Timer>> m_timer_map;
    fly::base::ID_Allocator m_id_allocator;
    std::mutex m_mutex;
};

#endif
