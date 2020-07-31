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

template<class T>
class SatoriQueue
{
public:

    SatoriQueue() :
        m_lock(), m_head(), m_tail()
    {
        m_lock.Initialize();
    };

    void Push(T* item)
    {
        SatoriLockHolder<SatoriLock> holder(&m_lock);
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
        T* result;
        {
            SatoriLockHolder<SatoriLock> holder(&m_lock);
            result = m_head;
            if (result == nullptr)
            {
                return nullptr;
            }

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
        SatoriLockHolder<SatoriLock> holder(&m_lock);
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

    bool CanPop()
    {
        return m_head != nullptr;
    }

    bool CanDequeue()
    {
        return m_tail != nullptr;
    }

protected:
    SatoriLock m_lock;
    T* m_head;
    T* m_tail;
};

#endif
