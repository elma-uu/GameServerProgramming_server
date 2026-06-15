#pragma once
#pragma warning(disable: 4819)
#include "EXP_OVER.h"
#include <tbb/concurrent_queue.h>

// Lock-free EXP_OVER reuse pool — eliminates malloc/free on every send and NPC tick.
class ObjectPool {
public:
    static EXP_OVER* AcquireSend();
    static void      ReleaseSend(EXP_OVER* o);
    static EXP_OVER* AcquireNpcMove();
    static void      ReleaseNpcMove(EXP_OVER* o);

private:
    inline static tbb::concurrent_queue<EXP_OVER*> sSendPool;
    inline static tbb::concurrent_queue<EXP_OVER*> sNpcPool;
};

inline EXP_OVER* ObjectPool::AcquireSend()
{
    EXP_OVER* o = nullptr;
    if (!sSendPool.try_pop(o)) {
        o = new EXP_OVER(IO_SEND);
    } else {
        ZeroMemory(&o->m_over, sizeof(WSAOVERLAPPED));
        o->m_iotype       = IO_SEND;
        o->m_wsa.buf      = o->m_ring_buffer.buffer;
        o->m_ring_buffer.head = 0;
        o->m_ring_buffer.tail = 0;
        o->m_ring_buffer.size = 0;
    }
    return o;
}

inline void ObjectPool::ReleaseSend(EXP_OVER* o)
{
    sSendPool.push(o);
}

inline EXP_OVER* ObjectPool::AcquireNpcMove()
{
    EXP_OVER* o = nullptr;
    if (!sNpcPool.try_pop(o)) {
        o = new EXP_OVER(IO_NPC_MOVE);
    } else {
        ZeroMemory(&o->m_over, sizeof(WSAOVERLAPPED));
        o->m_iotype = IO_NPC_MOVE;
    }
    return o;
}

inline void ObjectPool::ReleaseNpcMove(EXP_OVER* o)
{
    sNpcPool.push(o);
}
