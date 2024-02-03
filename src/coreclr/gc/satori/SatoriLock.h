// Copyright (c) 2024 Vladimir Sadov
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
// SatoriLock.h
//

#ifndef __SATORI_LOCK_H__
#define __SATORI_LOCK_H__

#include "common.h"
#include "../gc.h"
#include "SatoriUtil.h"

class SatoriLock
{
private:
    CLRCriticalSection m_cs;

public:
    void Initialize()
    {
        m_cs.Initialize();
    }

    void Destroy()
    {
        m_cs.Destroy();
    }

    void Enter()
    {
        m_cs.Enter();
    }

    void Leave()
    {
        m_cs.Leave();
    }
};

class SatoriSpinLock
{
private:
    int m_backoff;

public:
    void Initialize()
    {
        m_backoff = 0;
    }

    void Enter()
    {
        if (!CompareExchangeAcq(&m_backoff, 1, 0))
        {
            EnterSpin();
        }
    }

    bool TryEnter()
    {
        return CompareExchangeAcq(&m_backoff, 1, 0);
    }

    void Leave()
    {
        _ASSERTE(m_backoff);
        VolatileStore(&m_backoff, 0);
    }

private:

    NOINLINE
    void EnterSpin()
    {
        int localBackoff = 0;
        while (VolatileLoadWithoutBarrier(&m_backoff) ||
            !CompareExchangeAcq(&m_backoff, 1, 0))
        {
            localBackoff = Backoff(localBackoff);
        }
    }

    int Backoff(int backoff)
    {
        // TUNING: do we care about 1-proc machines?

        for (int i = 0; i < backoff; i++)
        {
            YieldProcessor();

            if ((i & 0x3FF) == 0x3FF)
            {
                GCToOSInterface::YieldThread(0);
            }
        }

        return (backoff * 2 + 1) & 0x3FFF;
    }

    static bool CompareExchangeAcq(int volatile* destination, int exchange, int comparand)
    {
#ifdef _MSC_VER
#if defined(TARGET_AMD64)
        return _InterlockedCompareExchange((long*)destination, exchange, comparand) == comparand;
#else
        return _InterlockedCompareExchange_acq((long*)destination, exchange, comparand) == comparand;
#endif
#else
        return __atomic_compare_exchange_n(destination, &comparand, exchange, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
#endif
    }
};

template <typename T>
class SatoriLockHolder : public Satori::StackOnly {
private:
    T* const m_lock;

public:
    // Disallow copying
    SatoriLockHolder& operator=(const SatoriLockHolder&) = delete;
    SatoriLockHolder(const SatoriLockHolder&) = delete;

    SatoriLockHolder(T* lock)
        : m_lock(lock)
    {
        m_lock->Enter();
    }

    SatoriLockHolder(T* lock, bool isLocked)
        : m_lock(lock)
    {
        if (!isLocked)
            m_lock->Enter();
    }

    ~SatoriLockHolder()
    {
        m_lock->Leave();
    }
};

#endif
