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

public:

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

    ~SatoriLockHolder()
    {
        m_lock->Leave();
    }
};

#endif
