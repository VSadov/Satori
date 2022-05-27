// Copyright (c) 2022 Vladimir Sadov
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//
// SatoriQueue.h
//

#ifndef __SATORI_QUEUE_H__
#define __SATORI_QUEUE_H__

#include "common.h"
#include "../gc.h"
#include "SatoriLock.h"

enum class QueueKind
{
    Allocator,
    WorkChunk,

    RecyclerEphemeral,
    RecyclerEphemeralFinalizationTracking,

    RecyclerTenured,
    RecyclerTenuredFinalizationTracking,

    RecyclerNursery,

    RecyclerFinalizationPending,
    RecyclerFinalizationScanComplete,

    RecyclerStaying,
    RecyclerRelocating,
    RecyclerRelocated,
    RecyclerRelocatedToHigherGen,
    RecyclerRelocationTarget,

    RecyclerDeferredSweep,
    RecyclerReusable,
    RecyclerDemoted,
};

template <class T>
class SatoriQueue
{
public:

    SatoriQueue(QueueKind kind) :
        m_kind(kind), m_lock(), m_head(), m_tail(), m_count()
    {
        m_lock.Initialize();
    };

    void Push(T* item)
    {
        _ASSERTE(item->m_next == nullptr);
        _ASSERTE(item->m_prev == nullptr);
        _ASSERTE(item->m_containingQueue == nullptr);

        SatoriLockHolder<SatoriSpinLock> holder(&m_lock);
        m_count++;
        item->m_containingQueue = this;
        if (m_head == nullptr)
        {
            _ASSERTE(m_tail == nullptr);
            m_tail = item;
        }
        else
        {
            item->m_next = m_head;
            m_head->m_prev = item;
        }

        m_head = item;
    }

    T* TryPop()
    {
        if (IsEmpty())
        {
            return nullptr;
        }

        T* result;
        {
            SatoriLockHolder<SatoriSpinLock> holder(&m_lock);
            result = m_head;
            if (result == nullptr)
            {
                return nullptr;
            }

            m_count--;
            result->m_containingQueue = nullptr;
            m_head = result->m_next;
            if (m_head == nullptr)
            {
                m_tail = nullptr;
            }
            else
            {
                m_head->m_prev = nullptr;
            }
        }

        _ASSERTE(result->m_prev == nullptr);
        result->m_next = nullptr;

        return result;
    }

    void Enqueue(T* item)
    {
        _ASSERTE(item->m_next == nullptr);
        _ASSERTE(item->m_prev == nullptr);
        _ASSERTE(item->m_containingQueue == nullptr);

        SatoriLockHolder<SatoriSpinLock> holder(&m_lock);
        m_count++;
        item->m_containingQueue = this;
        if (m_tail == nullptr)
        {
            _ASSERTE(m_head == nullptr);
            m_head = item;
        }
        else
        {
            item->m_prev = m_tail;
            m_tail->m_next = item;
        }

        m_tail = item;
    }

    void AppendUnsafe(SatoriQueue<T>* other)
    {
        size_t otherCount = other->Count();
        if (otherCount == 0)
        {
            return;
        }

        m_count += otherCount;

        if (m_tail == nullptr)
        {
            _ASSERTE(m_head == nullptr);
            m_head = other->m_head;
        }
        else
        {
            other->m_head->m_prev = m_tail;
            m_tail->m_next = other->m_head;
        }

        m_tail = other->m_tail;
        other->m_head = other->m_tail = nullptr;
        other->m_count = 0;
    }

    bool TryRemove(T* item)
    {
        {
            SatoriLockHolder<SatoriSpinLock> holder(&m_lock);
            if (!Contains(item))
            {
                return false;
            }

            m_count--;
            item->m_containingQueue = nullptr;
            if (item->m_prev == nullptr)
            {
                m_head = item->m_next;
            }
            else
            {
                item->m_prev->m_next = item->m_next;
            }

            if (item->m_next == nullptr)
            {
                m_tail = item->m_prev;
            }
            else
            {
                item->m_next->m_prev = item->m_prev;
            }
        }

        item->m_next = nullptr;
        item->m_prev = nullptr;
        return true;
    }

    bool Contains(T* item)
    {
        return item->m_containingQueue == this;
    }

    size_t Count()
    {
        return m_count;
    }

    bool IsEmpty()
    {
        return m_head == nullptr;
    }

    QueueKind Kind()
    {
        return m_kind;
    }

    template <typename F>
    void ForEachRegion(F lambda)
    {
        T* item = m_head;
        while (item)
        {
            lambda(item);
            item = item->m_next;
        }
    }

protected:
    QueueKind m_kind;
    SatoriSpinLock m_lock;
    T* m_head;
    T* m_tail;
    size_t m_count;
};

#endif
