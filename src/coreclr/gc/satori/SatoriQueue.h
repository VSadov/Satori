// Copyright (c) 2025 Vladimir Sadov
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

    // Off-to-the-side staging for items that will be handed to a queue in one splice.
    // Has no lock of its own - the items are private to the building thread. They are
    // already stamped with their eventual owner though, so TryRemove/Contains must not
    // run against that owner while a batch is outstanding.
    class Batch
    {
        friend class SatoriQueue<T>;

    public:
        Batch() : m_head(), m_tail(), m_count() {}

        void Push(T* item, SatoriQueue<T>* eventualQueue)
        {
            _ASSERTE(item->m_next == nullptr);
            _ASSERTE(item->m_prev == nullptr);
            _ASSERTE(item->m_containingQueue == nullptr);

            m_count++;
            if (m_head == nullptr)
            {
                m_tail = item;
            }
            else
            {
                item->m_next = m_head;
                m_head->m_prev = item;
            }

            m_head = item;
            item->m_containingQueue = eventualQueue;
        }

        void Enqueue(T* item, SatoriQueue<T>* eventualQueue)
        {
            _ASSERTE(item->m_next == nullptr);
            _ASSERTE(item->m_prev == nullptr);
            _ASSERTE(item->m_containingQueue == nullptr);

            m_count++;
            if (m_tail == nullptr)
            {
                m_head = item;
            }
            else
            {
                item->m_prev = m_tail;
                m_tail->m_next = item;
            }

            m_tail = item;
            item->m_containingQueue = eventualQueue;
        }

        bool IsEmpty()
        {
            return m_head == nullptr;
        }

    private:
        T* m_head;
        T* m_tail;
        size_t m_count;
    };

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

        SatoriLockHolder holder(&m_lock);
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

    void PushNoLock(T* item)
    {
        _ASSERTE(item->m_next == nullptr);
        _ASSERTE(item->m_prev == nullptr);
        _ASSERTE(item->m_containingQueue == nullptr);

        size_t oldCount = m_count;
        Interlocked::Increment(&m_count);

        T* head = Interlocked::ExchangePointer(&m_head, item);
        if (head == nullptr)
        {
            _ASSERTE(m_tail == nullptr);
            m_tail = item;
        }
        else
        {
            item->m_next = head;
            head->m_prev = item;
        }

        item->m_containingQueue = this;
        _ASSERTE(m_count > oldCount);
    }

    // Pushes with no locks or interlocked ops, stamping the item with the queue it will
    // end up in. For building a thread-local queue to later Append to that eventual owner.
    void PushUnsafe(T* item, SatoriQueue<T>* eventualQueue)
    {
        _ASSERTE(item->m_next == nullptr);
        _ASSERTE(item->m_prev == nullptr);
        _ASSERTE(item->m_containingQueue == nullptr);

        m_count++;
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
        item->m_containingQueue = eventualQueue;
    }

    T* TryPop()
    {
        if (IsEmpty())
        {
            return nullptr;
        }

        T* result;
        {
            SatoriLockHolder holder(&m_lock);
            result = m_head;
            if (result == nullptr)
            {
                return nullptr;
            }

            T* next = result->m_next;
            m_count--;
            m_head = next;
            result->m_containingQueue = nullptr;
            if (next == nullptr)
            {
                m_tail = nullptr;
            }
            else
            {
                next->m_prev = nullptr;
            }
        }

        _ASSERTE(result->m_prev == nullptr);
        result->m_next = nullptr;

        return result;
    }

    // same as TryPop, but on empty pushes the item.
    // emptines is checked under a lock.
    T* PopOrPush(T* item)
    {
        _ASSERTE(item->m_next == nullptr);
        _ASSERTE(item->m_prev == nullptr);
        _ASSERTE(item->m_containingQueue == nullptr);

        T* result;
        {
            SatoriLockHolder holder(&m_lock);
            result = m_head;
            if (result == nullptr)
            {
                // push the item and return nullptr.
                _ASSERTE(m_tail == nullptr);
                m_count++;
                item->m_containingQueue = this;
                m_tail = item;
                m_head = item;
                return nullptr;
            }

            // pop the result
            T* next = result->m_next;
            m_count--;
            m_head = next;
            result->m_containingQueue = nullptr;
            if (next == nullptr)
            {
                m_tail = nullptr;
            }
            else
            {
                next->m_prev = nullptr;
            }
        }

        _ASSERTE(result->m_prev == nullptr);
        result->m_next = nullptr;

        return result;
    }

    T* TryPopWithTryEnter()
    {
        if (IsEmpty())
        {
            return nullptr;
        }

        T* result;
        {
            if (!m_lock.TryEnter())
            {
                return nullptr;
            }

            result = m_head;
            if (result == nullptr)
            {
                m_lock.Leave();
                return nullptr;
            }

            T* next = result->m_next;
            m_count--;
            m_head = next;
            result->m_containingQueue = nullptr;
            if (next == nullptr)
            {
                m_tail = nullptr;
            }
            else
            {
                next->m_prev = nullptr;
            }

            m_lock.Leave();
        }

        _ASSERTE(result->m_prev == nullptr);
        result->m_next = nullptr;

        return result;
    }

    // Pops with no locks or interlocked ops. Only valid when this thread is the only one
    // touching the queue. Leaves m_tail and the new head's m_prev stale, so the queue needs
    // ResetAfterUnsafeDrain when done.
    T* TryPopUnsafe()
    {
        T* result = m_head;
        if (result == nullptr)
        {
            return nullptr;
        }

        T* next = result->m_next;

        // items are far apart, so walking the list is a chain of cache misses.
        // start fetching the link we will need next while the caller works on this one.
        if (next != nullptr)
        {
            SatoriUtil::Prefetch(&next->m_next);
        }

        m_head = next;
        m_count--;
        result->m_containingQueue = nullptr;
        result->m_next = nullptr;
        result->m_prev = nullptr;
        return result;
    }

    void ResetAfterUnsafeDrain()
    {
        _ASSERTE(m_head == nullptr);
        m_tail = nullptr;
        m_count = 0;
    }

    // Concurrent pop for a queue that takes no pushes while the drain is in progress.
    // With no producers an item cannot re-enter the queue, so a bare CAS on the head
    // cannot see ABA and needs no lock.
    //
    // Back links and the tail are deliberately left stale: writing next->m_prev would
    // touch a line this thread has no other reason to read, and the tail only matters to
    // appends. The queue is unusable until ResetAfterUnsafeDrain, which the drain must
    // call once its workers have joined.
    //
    // NB: a racing popper may read m_next out of an item another thread has already
    //     claimed. That read is harmless - the CAS below will fail and retry - but it
    //     does require the item to stay mapped for the duration of the drain.
    //     This is why the drain may only run in the blocking phase: outside of it the
    //     trimmer could coalesce an already claimed region and decommit its header.
    T* TryPopDrainOnly()
    {
        T* result = VolatileLoadWithoutBarrier(&m_head);
        uint32_t collisions = 0;
        while (result != nullptr)
        {
            T* next = result->m_next;
            T* prev = Interlocked::CompareExchangePointer(&m_head, next, result);
            if (prev == result)
            {
                // items are far apart, so walking the list is a chain of cache misses.
                // start fetching the link the next popper will need.
                if (next != nullptr)
                {
                    SatoriUtil::Prefetch(&next->m_next);
                }

#if _DEBUG
                // the drain ends empty regardless, so outside of debugging the count is not
                // worth a second contended line next to the head.
                Interlocked::Decrement(&m_count);
#endif
                result->m_containingQueue = nullptr;
                result->m_next = nullptr;
                result->m_prev = nullptr;
                return result;
            }

            // Collisions are rare, so this seldom fires, but a failed CAS still takes the
            // head exclusively - backing off keeps a degenerate case from amplifying that.
            SatoriLock::CollisionBackoff(++collisions);

            // the pause makes the value the CAS returned stale, so re-read rather than reuse it
            result = VolatileLoadWithoutBarrier(&m_head);
        }

        return nullptr;
    }

    void Enqueue(T* item)
    {
        _ASSERTE(item->m_next == nullptr);
        _ASSERTE(item->m_prev == nullptr);
        _ASSERTE(item->m_containingQueue == nullptr);

        SatoriLockHolder holder(&m_lock);
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

    // does not take locks, does not update containing queue.
    // only used for intermediate merging of queues before consuming.
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

    // Appends a queue that no other thread can see - typically built with PushUnsafe,
    // which has already stamped the items with this queue as their owner.
    void Append(SatoriQueue<T>* other)
    {
        T* otherHead = other->m_head;
        if (otherHead == nullptr)
        {
            _ASSERTE(other->m_count == 0);
            return;
        }

#if _DEBUG
        // the items are ours until published, so this needs no lock
        for (T* item = otherHead; item != nullptr; item = item->m_next)
        {
            _ASSERTE(item->m_containingQueue == this);
        }
#endif

        SatoriLockHolder holder(&m_lock);
        m_count += other->m_count;
        if (m_tail == nullptr)
        {
            _ASSERTE(m_head == nullptr);
            m_head = otherHead;
        }
        else
        {
            otherHead->m_prev = m_tail;
            m_tail->m_next = otherHead;
        }

        m_tail = other->m_tail;
        other->m_head = other->m_tail = nullptr;
        other->m_count = 0;
    }

    // Splices a locally built batch onto the tail in one lock acquisition.
    // The batch items are already stamped with this queue as their owner.
    void Append(Batch* other)
    {
        T* otherHead = other->m_head;
        if (otherHead == nullptr)
        {
            _ASSERTE(other->m_count == 0);
            return;
        }

        {
            SatoriLockHolder holder(&m_lock);
            m_count += other->m_count;
            if (m_tail == nullptr)
            {
                _ASSERTE(m_head == nullptr);
                m_head = otherHead;
            }
            else
            {
                otherHead->m_prev = m_tail;
                m_tail->m_next = otherHead;
            }

            m_tail = other->m_tail;
        }

        other->m_head = other->m_tail = nullptr;
        other->m_count = 0;
    }

    // Same, onto the head - for batches whose items should be consumed first.
    void Prepend(Batch* other)
    {
        T* otherTail = other->m_tail;
        if (otherTail == nullptr)
        {
            _ASSERTE(other->m_count == 0);
            return;
        }

        {
            SatoriLockHolder holder(&m_lock);
            m_count += other->m_count;
            if (m_head == nullptr)
            {
                _ASSERTE(m_tail == nullptr);
                m_tail = otherTail;
            }
            else
            {
                otherTail->m_next = m_head;
                m_head->m_prev = otherTail;
            }

            m_head = other->m_head;
        }

        other->m_head = other->m_tail = nullptr;
        other->m_count = 0;
    }

    bool TryRemove(T* item)
    {
        {
            SatoriLockHolder holder(&m_lock);
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
    SatoriLock m_lock;
    // Contenders spin on the lock word, which keeps its line Shared on every one of them.
    // Without this, each write to m_head inside the critical section has to invalidate them all.
    uint8_t m_padding[64];
    T* m_head;
    T* m_tail;
    size_t m_count;
};

#endif
