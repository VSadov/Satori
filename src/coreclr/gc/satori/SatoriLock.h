// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
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
