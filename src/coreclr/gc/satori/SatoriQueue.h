// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// TQueue.h
//

#ifndef __SATORI_QUEUE_H__
#define __SATORI_QUEUE_H__

#include "common.h"
#include "../gc.h"
#include "SatoriLock.h"

enum class QueueKind
{
    Allocator,
    MarkChunk,

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
};

template<class T>
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

        SatoriLockHolder<SatoriLock> holder(&m_lock);
        m_count++;
        item->m_containingQueue = this;
        if (m_head == nullptr)
        {
            _ASSERTE(m_tail == nullptr);
            m_head = m_tail = item;
        }
        else
        {
            item->m_next = m_head;
            m_head->m_prev = item;
            m_head = item;
        }
    }

    T* TryPop()
    {
        if (IsEmpty())
        {
            return nullptr;
        }

        T* result;
        {
            SatoriLockHolder<SatoriLock> holder(&m_lock);
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

        SatoriLockHolder<SatoriLock> holder(&m_lock);
        m_count++;
        item->m_containingQueue = this;
        if (m_tail == nullptr)
        {
            _ASSERTE(m_head == nullptr);
            m_head = m_tail = item;
        }
        else
        {
            item->m_prev = m_tail;
            m_tail->m_next = item;
            m_tail = item;
        }
    }

    bool TryRemove(T* item)
    {
        {
            SatoriLockHolder<SatoriLock> holder(&m_lock);
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
        return m_count == 0;
    }

    QueueKind Kind()
    {
        return m_kind;
    }

protected:
    QueueKind m_kind;
    SatoriLock m_lock;
    T* m_head;
    T* m_tail;
    size_t m_count;
};

#endif
