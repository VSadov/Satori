// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <pthread.h>
#include <errno.h>
#include "config.gc.h"
#include "common.h"

#include "gcenv.structs.h"
#include "gcenv.base.h"
#include "gcenv.os.h"
#include "globals.h"

namespace
{

#if HAVE_PTHREAD_CONDATTR_SETCLOCK
void TimeSpecAdd(timespec* time, uint32_t milliseconds)
{
    uint64_t nsec = time->tv_nsec + (uint64_t)milliseconds * tccMilliSecondsToNanoSeconds;
    if (nsec >= tccSecondsToNanoSeconds)
    {
        time->tv_sec += nsec / tccSecondsToNanoSeconds;
        nsec %= tccSecondsToNanoSeconds;
    }

    time->tv_nsec = nsec;
}
#endif // HAVE_PTHREAD_CONDATTR_SETCLOCK

#if HAVE_CLOCK_GETTIME_NSEC_NP
// Convert nanoseconds to the timespec structure
// Parameters:
//  nanoseconds - time in nanoseconds to convert
//  t           - the target timespec structure
void NanosecondsToTimeSpec(uint64_t nanoseconds, timespec* t)
{
    t->tv_sec = nanoseconds / tccSecondsToNanoSeconds;
    t->tv_nsec = nanoseconds % tccSecondsToNanoSeconds;
}
#endif // HAVE_CLOCK_GETTIME_NSEC_NP

} // anonymous namespace

class GCEvent::Impl
{
    pthread_cond_t m_condition;
    pthread_mutex_t m_mutex;
    bool m_manualReset;
    bool m_state;
    bool m_isValid;

public:

    Impl(bool manualReset, bool initialState)
    : m_manualReset(manualReset),
      m_state(initialState),
      m_isValid(false)
    {
    }

    bool Initialize()
    {
        pthread_condattr_t attrs;
        int st = pthread_condattr_init(&attrs);
        if (st != 0)
        {
            assert(!"Failed to initialize UnixEvent condition attribute");
            return false;
        }

        // TODO(segilles) implement this for CoreCLR
        //PthreadCondAttrHolder attrsHolder(&attrs);

#if HAVE_PTHREAD_CONDATTR_SETCLOCK && !HAVE_CLOCK_GETTIME_NSEC_NP
        // Ensure that the pthread_cond_timedwait will use CLOCK_MONOTONIC
        st = pthread_condattr_setclock(&attrs, CLOCK_MONOTONIC);
        if (st != 0)
        {
            assert(!"Failed to set UnixEvent condition variable wait clock");
            return false;
        }
#endif // HAVE_PTHREAD_CONDATTR_SETCLOCK && !HAVE_CLOCK_GETTIME_NSEC_NP

        st = pthread_mutex_init(&m_mutex, NULL);
        if (st != 0)
        {
            assert(!"Failed to initialize UnixEvent mutex");
            return false;
        }

        st = pthread_cond_init(&m_condition, &attrs);
        if (st != 0)
        {
            assert(!"Failed to initialize UnixEvent condition variable");

            st = pthread_mutex_destroy(&m_mutex);
            assert(st == 0 && "Failed to destroy UnixEvent mutex");
            return false;
        }

        m_isValid = true;

        return true;
    }

    void CloseEvent()
    {
        if (m_isValid)
        {
            int st = pthread_mutex_destroy(&m_mutex);
            assert(st == 0 && "Failed to destroy UnixEvent mutex");

            st = pthread_cond_destroy(&m_condition);
            assert(st == 0 && "Failed to destroy UnixEvent condition variable");
        }
    }

    uint32_t Wait(uint32_t milliseconds, bool alertable)
    {
        UNREFERENCED_PARAMETER(alertable);

        timespec endTime;
#if HAVE_CLOCK_GETTIME_NSEC_NP
        uint64_t endMachTime;
        if (milliseconds != INFINITE)
        {
            uint64_t nanoseconds = (uint64_t)milliseconds * tccMilliSecondsToNanoSeconds;
            NanosecondsToTimeSpec(nanoseconds, &endTime);
            endMachTime = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + nanoseconds;
        }
#elif HAVE_PTHREAD_CONDATTR_SETCLOCK
        if (milliseconds != INFINITE)
        {
            clock_gettime(CLOCK_MONOTONIC, &endTime);
            TimeSpecAdd(&endTime, milliseconds);
        }
#else
#error "Don't know how to perform timed wait on this platform"
#endif

        int st = 0;

        pthread_mutex_lock(&m_mutex);
        while (!m_state)
        {
            if (milliseconds == INFINITE)
            {
                st = pthread_cond_wait(&m_condition, &m_mutex);
            }
            else
            {
#if HAVE_CLOCK_GETTIME_NSEC_NP
                // Since OSX doesn't support CLOCK_MONOTONIC, we use relative variant of the
                // timed wait and we need to handle spurious wakeups properly.
                st = pthread_cond_timedwait_relative_np(&m_condition, &m_mutex, &endTime);
                if ((st == 0) && !m_state)
                {
                    uint64_t machTime = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                    if (machTime < endMachTime)
                    {
                        // The wake up was spurious, recalculate the relative endTime
                        uint64_t remainingNanoseconds = endMachTime - machTime;
                        NanosecondsToTimeSpec(remainingNanoseconds, &endTime);
                    }
                    else
                    {
                        // Although the timed wait didn't report a timeout, time calculated from the
                        // mach time shows we have already reached the end time. It can happen if
                        // the wait was spuriously woken up right before the timeout.
                        st = ETIMEDOUT;
                    }
                }
#else // HAVE_CLOCK_GETTIME_NSEC_NP
                st = pthread_cond_timedwait(&m_condition, &m_mutex, &endTime);
#endif // HAVE_CLOCK_GETTIME_NSEC_NP
            }

            if (st != 0)
            {
                // wait failed or timed out
                break;
            }
        }

        if ((st == 0) && !m_manualReset)
        {
            // Clear the state for auto-reset events so that only one waiter gets released
            m_state = false;
        }

        pthread_mutex_unlock(&m_mutex);

        uint32_t waitStatus;

        if (st == 0)
        {
            waitStatus = WAIT_OBJECT_0;
        }
        else if (st == ETIMEDOUT)
        {
            waitStatus = WAIT_TIMEOUT;
        }
        else
        {
            waitStatus = WAIT_FAILED;
        }

        return waitStatus;
    }

    void Set()
    {
        pthread_mutex_lock(&m_mutex);
        m_state = true;
        // Unblock all threads waiting for the condition variable
        pthread_cond_broadcast(&m_condition);
        pthread_mutex_unlock(&m_mutex);
    }

    void Reset()
    {
        pthread_mutex_lock(&m_mutex);
        m_state = false;
        pthread_mutex_unlock(&m_mutex);
    }
};

GCEvent::GCEvent()
  : m_impl(nullptr)
{
}

void GCEvent::CloseEvent()
{
    assert(m_impl != nullptr);
    m_impl->CloseEvent();
}

void GCEvent::Set()
{
    assert(m_impl != nullptr);
    m_impl->Set();
}

void GCEvent::Reset()
{
    assert(m_impl != nullptr);
    m_impl->Reset();
}

uint32_t GCEvent::Wait(uint32_t timeout, bool alertable)
{
    assert(m_impl != nullptr);
    return m_impl->Wait(timeout, alertable);
}

bool GCEvent::CreateAutoEventNoThrow(bool initialState)
{
    // This implementation of GCEvent makes no distinction between
    // host-aware and non-host-aware events (since there will be no host).
    return CreateOSAutoEventNoThrow(initialState);
}

bool GCEvent::CreateManualEventNoThrow(bool initialState)
{
    // This implementation of GCEvent makes no distinction between
    // host-aware and non-host-aware events (since there will be no host).
    return CreateOSManualEventNoThrow(initialState);
}

bool GCEvent::CreateOSAutoEventNoThrow(bool initialState)
{
    assert(m_impl == nullptr);
    GCEvent::Impl* event(new (nothrow) GCEvent::Impl(false, initialState));
    if (!event)
    {
        return false;
    }

    if (!event->Initialize())
    {
        delete event;
        return false;
    }

    m_impl = event;
    return true;
}

bool GCEvent::CreateOSManualEventNoThrow(bool initialState)
{
    assert(m_impl == nullptr);
    GCEvent::Impl* event(new (nothrow) GCEvent::Impl(true, initialState));
    if (!event)
    {
        return false;
    }

    if (!event->Initialize())
    {
        delete event;
        return false;
    }

    m_impl = event;
    return true;
}

#define _INC_PTHREADS
#include "../satori/SatoriGate.h"

#if defined(TARGET_LINUX)

#include <linux/futex.h>      /* Definition of FUTEX_* constants */
#include <sys/syscall.h>      /* Definition of SYS_* constants */
#include <unistd.h>

#ifndef  INT_MAX
#define INT_MAX 2147483647
#endif

SatoriGate::SatoriGate()
{
    m_state = s_blocking;
}

// returns true if was woken up. false if timed out
bool SatoriGate::TimedWait(int timeout)
{
    timespec t;
    uint64_t nanoseconds = (uint64_t)timeout * tccMilliSecondsToNanoSeconds;
    t.tv_sec = nanoseconds / tccSecondsToNanoSeconds;
    t.tv_nsec = nanoseconds % tccSecondsToNanoSeconds;

    long waitResult = syscall(SYS_futex, &m_state, FUTEX_WAIT_PRIVATE, s_blocking, &t, NULL, 0);

    // woken, not blocking, interrupted, timeout
    assert(waitResult == 0 || errno == EAGAIN || errno == ETIMEDOUT || errno == EINTR);

    bool woken = waitResult == 0 || errno != ETIMEDOUT;
    if (woken)
    {
        // consume the wake
        m_state = s_blocking;
    }

    return woken;
}

void SatoriGate::Wait()
{
    syscall(SYS_futex, &m_state, FUTEX_WAIT_PRIVATE, s_blocking, NULL, NULL, 0);
}

void SatoriGate::WakeAll()
{
    m_state = s_open;
    syscall(SYS_futex, &m_state, FUTEX_WAKE_PRIVATE, s_blocking, INT_MAX , NULL, 0);
}

void SatoriGate::WakeOne()
{
    m_state = s_open;
    syscall(SYS_futex, &m_state, FUTEX_WAKE_PRIVATE, s_blocking, 1, NULL, 0);
}
#else
SatoriGate::SatoriGate()
{
    m_cs = new (nothrow) pthread_mutex_t();
    m_cv = new (nothrow) pthread_cond_t();

    pthread_mutex_init(m_cs, NULL);
    pthread_condattr_t attrs;
    pthread_condattr_init(&attrs);
#if HAVE_PTHREAD_CONDATTR_SETCLOCK && !HAVE_CLOCK_GETTIME_NSEC_NP
    // Ensure that the pthread_cond_timedwait will use CLOCK_MONOTONIC
    pthread_condattr_setclock(&attrs, CLOCK_MONOTONIC);
#endif // HAVE_PTHREAD_CONDATTR_SETCLOCK && !HAVE_CLOCK_GETTIME_NSEC_NP
    pthread_cond_init(m_cv, &attrs);
    pthread_condattr_destroy(&attrs);
}

// returns true if was woken up
bool SatoriGate::TimedWait(int timeout)
{
    timespec endTime;
#if HAVE_CLOCK_GETTIME_NSEC_NP
    uint64_t endNanoseconds;
    uint64_t nanoseconds = (uint64_t)timeout * tccMilliSecondsToNanoSeconds;
    NanosecondsToTimeSpec(nanoseconds, &endTime);
    endNanoseconds = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + nanoseconds;
#elif HAVE_PTHREAD_CONDATTR_SETCLOCK
    clock_gettime(CLOCK_MONOTONIC, &endTime);
    TimeSpecAdd(&endTime, timeout);
#else
#error "Don't know how to perform timed wait on this platform"
#endif

    int waitResult = 0;
    pthread_mutex_lock(m_cs);
#if HAVE_CLOCK_GETTIME_NSEC_NP
    // Since OSX doesn't support CLOCK_MONOTONIC, we use relative variant of the timed wait.
    waitResult = m_state == s_open ?
        0 :
        pthread_cond_timedwait_relative_np(m_cv, m_cs, &endTime);
#else // HAVE_CLOCK_GETTIME_NSEC_NP
    waitResult = m_state == SatoriGate::s_open ?
        0 :
        pthread_cond_timedwait(m_cv, m_cs, &endTime);
#endif // HAVE_CLOCK_GETTIME_NSEC_NP
    pthread_mutex_unlock(m_cs);
    assert(waitResult == 0 || waitResult == ETIMEDOUT);

    bool woken = waitResult == 0;
    if (woken)
    {
        // consume the wake
        m_state = s_blocking;
    }

    return woken;
}

void SatoriGate::Wait()
{
    int waitResult;
    pthread_mutex_lock(m_cs);

    waitResult = m_state == SatoriGate::s_open ?
        0 :
        pthread_cond_wait(m_cv, m_cs);

    pthread_mutex_unlock(m_cs);
    assert(waitResult == 0);

    m_state = s_blocking;
}

void SatoriGate::WakeAll()
{
    m_state = SatoriGate::s_open;
    pthread_mutex_lock(m_cs);
    pthread_cond_broadcast(m_cv);
    pthread_mutex_unlock(m_cs);
}

void SatoriGate::WakeOne()
{
    m_state = SatoriGate::s_open;
    pthread_mutex_lock(m_cs);
    pthread_cond_signal(m_cv);
    pthread_mutex_unlock(m_cs);
}
#endif

