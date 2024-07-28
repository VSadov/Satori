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
#include "SatoriGate.h"

class SatoriLock
{
private:
    // m_state layout:
    //
    // bit 0: True if the lock is held, false otherwise.
    //
    // bit 1: True if nonwaiters must not get ahead of waiters when acquiring a contended lock.
    //
    // sign bit: True if we've set the event to wake a waiting thread.  The waiter resets this to false when it
    //        wakes up.  This avoids the overhead of setting the event multiple times.
    //
    // everything else: A count of the number of threads waiting on the event.
    static const uint32_t Unlocked = 0;
    static const uint32_t Locked = 1;
    static const uint32_t YieldToWaiters = 2;
    static const uint32_t WaiterCountIncrement = 4;
    static const uint32_t WaiterWoken = 1u << 31;

    volatile uint32_t _state;
    volatile uint16_t _spinCount;
    volatile uint16_t _wakeWatchDog;
    volatile size_t _owningThreadId;

    SatoriGate* _gate;

private:
    FORCEINLINE
    static bool CompareExchangeAcq(uint32_t volatile* destination, uint32_t exchange, uint32_t comparand)
    {
#ifdef _MSC_VER
#if defined(TARGET_AMD64)
        return _InterlockedCompareExchange((long*)destination, exchange, comparand) == (long)comparand;
#else
        return _InterlockedCompareExchange_acq((long*)destination, exchange, comparand) == (long)comparand;
#endif
#else
        return __atomic_compare_exchange_n(destination, &comparand, exchange, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
#endif
    }

    FORCEINLINE
    static uint32_t InterlockedDecRel(volatile uint32_t* arg)
    {
#ifdef _MSC_VER
#if defined(TARGET_AMD64)
        return (uint32_t)_InterlockedDecrement((long*)arg);
#else
        return (uint32_t)_InterlockedDecrement_rel((long*)arg);
#endif
#else
        return __atomic_sub_fetch(arg, 1, __ATOMIC_RELEASE);
#endif
    }

    FORCEINLINE
    static int64_t GetCpuCycles()
    {
#if defined(TARGET_AMD64)
#ifdef _MSC_VER
        return __rdtsc();
#else
        ptrdiff_t cycles;
        ptrdiff_t cyclesHi;
        __asm__ __volatile__
        ("rdtsc":"=a" (cycles), "=d" (cyclesHi));
        return (cyclesHi << 32) | cycles;
#endif
#elif defined(TARGET_ARM64)
        // On arm64 just read timer register instead
#ifdef _MSC_VER
        return _ReadStatusReg(ARM64_CNTVCT_EL0);
#else
        int64_t timerTicks;
        asm volatile("mrs %0, cntvct_el0" : "=r"(timerTicks));
        return timerTicks;
#endif
#endif
    }

    static const uint16_t SpinCountNotInitialized = INT16_MIN;

    // While spinning is parameterized in terms of iterations,
    // the internal tuning operates with spin count at a finer scale.
    // One iteration is mapped to 64 spin count units.
    static const int SpinCountScaleShift = 6;

    static const uint16_t DefaultMaxSpinCount = 22 << SpinCountScaleShift;
    static const uint16_t DefaultMinSpinCount = 1 << SpinCountScaleShift;

    // We will use exponential backoff in rare cases when we need to change state atomically and cannot
    // make progress due to concurrent state changes by other threads.
    // While we cannot know the ideal amount of wait needed before making a successfull attempt,
    // the exponential backoff will generally be not more than 2X worse than the perfect guess and
    // will do a lot less attempts than an simple retry. On multiprocessor machine fruitless attempts
    // will cause unnecessary sharing of the contended state which may make modifying the state more expensive.
    // To protect against degenerate cases we will cap the per-iteration wait to 1024 spinwaits.
    static const uint32_t MaxExponentialBackoffBits = 10;

    // This lock is unfair and permits acquiring a contended lock by a nonwaiter in the presence of waiters.
    // It is possible for one thread to keep holding the lock long enough that waiters go to sleep and
    // then release and reacquire fast enough that waiters have no chance to get the lock.
    // In extreme cases one thread could keep retaking the lock starving everybody else.
    // If we see woken waiters not able to take the lock for too long we will ask nonwaiters to wait.
    static const uint32_t WaiterWatchdogTicks = 60;

public:
    void Initialize()
    {
        _state = 0;
        _spinCount = DefaultMinSpinCount;
        _wakeWatchDog = 0;
        _owningThreadId = 0;
        _gate = new SatoriGate();
    }

    FORCEINLINE
    bool TryEnter()
    {
        uint32_t origState = _state;
        if ((origState & (YieldToWaiters | Locked)) == 0)
        {
            uint32_t newState = origState + Locked;
            if (CompareExchangeAcq(&_state, newState, origState))
            {
                _ASSERTE(_owningThreadId == 0);
                _owningThreadId = SatoriUtil::GetCurrentThreadTag();
                return true;
            }
        }

        return false;
    }

    FORCEINLINE
    void Enter()
    {
        if (!TryEnter())
        {
            EnterSlow();
        }
    }

    bool IsLocked()
    {
        return (_state & Locked) != 0;
    }

    FORCEINLINE
    void Leave()
    {
        _ASSERTE(IsLocked());
        _ASSERTE(_owningThreadId == SatoriUtil::GetCurrentThreadTag());

        _owningThreadId = 0;
        uint32_t state = InterlockedDecRel(&_state);
        if ((int32_t)state < (int32_t)WaiterCountIncrement) // true if have no waiters or WaiterWoken is set
        {
            return;
        }

        //
        // We have waiters; take the slow path.
        //
        AwakeWaiterIfNeeded();
    }

private:

    static uint16_t GetTickCount()
    {
        return (uint16_t)GCToOSInterface::GetLowPrecisionTimeStamp();
    }

    static void CollisionBackoff(uint32_t collisions)
    {
        _ASSERTE(collisions > 0);

        // no need for much randomness here, we will just hash the stack location and a timestamp.
        uint32_t rand = ((uint32_t)(size_t)&collisions + (uint32_t)GetCpuCycles()) * 2654435769u;
        uint32_t spins = rand >> (uint8_t)((uint32_t)32 - min(collisions, MaxExponentialBackoffBits));
        for (int i = 0; i < (int)spins; i++)
        {
            YieldProcessor();
        }
    }

    // same idea as in CollisionBackoff, but with guaranteed minimum wait
    static void IterationBackoff(int iteration)
    {
        _ASSERTE(iteration > 0 && iteration < MaxExponentialBackoffBits);

        uint32_t rand = ((uint32_t)(size_t)&iteration + (uint32_t)GetCpuCycles()) * 2654435769u;
        // set the highmost bit to ensure minimum number of spins is exponentialy increasing
        // it basically gurantees that we spin at least 1, 2, 4, 8, 16, times, and so on
        rand |= (1u << 31);
        uint32_t spins = rand >> (uint8_t)(32 - iteration);
        for (int i = 0; i < (int)spins; i++)
        {
            YieldProcessor();
        }
    }

    NOINLINE
    void EnterSlow()
    {
        bool hasWaited = false;
        // we will retry after waking up
        while (true)
        {
            int iteration = 1;

            // We will count when we failed to change the state of the lock and increase pauses
            // so that bursts of activity are better tolerated. This should not happen often.
            int collisions = 0;

            // We will track the changes of ownership while we are trying to acquire the lock.
            size_t oldOwner = _owningThreadId;
            uint32_t ownerChanged = 0;

            int iterationLimit = _spinCount >> SpinCountScaleShift;
            // inner loop where we try acquiring the lock or registering as a waiter
            while (true)
            {
                //
                // Try to grab the lock.  We may take the lock here even if there are existing waiters.  This creates the possibility
                // of starvation of waiters, but it also prevents lock convoys and preempted waiters from destroying perf.
                // However, if we do not see _wakeWatchDog cleared for long enough, we go into YieldToWaiters mode to ensure some
                // waiter progress.
                //
                uint32_t oldState = _state;
                bool canAcquire = ((oldState & Locked) == Unlocked) &&
                    (hasWaited || ((oldState & YieldToWaiters) == 0));

                if (canAcquire)
                {
                    uint32_t newState = oldState | Locked;
                    if (hasWaited)
                        newState = (newState - WaiterCountIncrement) & ~(WaiterWoken | YieldToWaiters);

                    if (Interlocked::CompareExchange(&_state, newState, oldState) == oldState)
                    {
                        // GOT THE LOCK!!
                        _ASSERTE((_state | Locked) != 0);
                        _ASSERTE(_owningThreadId == 0);
                        _owningThreadId = SatoriUtil::GetCurrentThreadTag();

                        if (hasWaited)
                            _wakeWatchDog = 0;

                        // now we can estimate how busy the lock is and adjust spinning accordingly
                        uint16_t spinLimit = _spinCount;
                        if (ownerChanged != 0)
                        {
                            // The lock has changed ownership while we were trying to acquire it.
                            // It is a signal that we might want to spin less next time.
                            // Pursuing a lock that is being "stolen" by other threads is inefficient
                            // due to cache misses and unnecessary sharing of state that keeps invalidating.
                            if (spinLimit > DefaultMinSpinCount)
                            {
                                _spinCount = (uint16_t)(spinLimit - 1);
                            }
                        }
                        else if (spinLimit < DefaultMaxSpinCount &&
                            iteration >= (spinLimit >> SpinCountScaleShift))
                        {
                            // we used all of allowed iterations, but the lock does not look very contested,
                            // we can allow a bit more spinning.
                            //
                            // NB: if we acquired the lock while registering a waiter, and owner did not change it still counts.
                            //     (however iteration does not grow beyond the iterationLimit)
                            _spinCount = (uint16_t)(spinLimit + 1);
                        }

                        return;
                    }
                }

                size_t newOwner = _owningThreadId;
                if (newOwner != 0 && newOwner != oldOwner)
                {
                    if (oldOwner != 0)
                        ownerChanged++;

                    oldOwner = newOwner;
                }

                if (iteration < iterationLimit)
                {
                    // We failed to acquire the lock and want to retry after a pause.
                    // Ideally we will retry right when the lock becomes free, but we cannot know when that will happen.
                    // We will use a pause that doubles up on every iteration. It will not be more than 2x worse
                    // than the ideal guess, while minimizing the number of retries.
                    // We will allow pauses up to 64~128 spinwaits.
                    IterationBackoff(min(iteration, 6));
                    iteration++;
                    continue;
                }
                else if (!canAcquire)
                {
                    // We reached our spin limit, and need to wait.

                    // If waiter was awaken spuriously, it may acquire the lock before wake watchdog is set.
                    // If there are no more waiters for a long time, the watchdog could hang around for a while too.
                    // When a new waiter enters the system, it may look like we had no waiter progress for all that time.
                    // To avoid this, if it looks like we have no waiters and will be the first new one,
                    // clear the watchdog.
                    // It is ok to clear even if we will not end up the first one.
                    // We will self-correct on the next wake and reestablish a new watchdog.
                    if (oldState < WaiterCountIncrement && _wakeWatchDog !=0)
                        _wakeWatchDog = 0;
                    
                    // Increment the waiter count.
                    // Note that we do not do any overflow checking on this increment.  In order to overflow,
                    // we'd need to have about 1 billion waiting threads, which is inconceivable anytime in the
                    // forseeable future.
                    uint32_t newState = oldState + WaiterCountIncrement;
                    if (hasWaited)
                        newState = (newState - WaiterCountIncrement) & ~WaiterWoken;

                    if (Interlocked::CompareExchange(&_state, newState, oldState) == oldState)
                        break;
                }

                CollisionBackoff(++collisions);
            }

            //
            // Now we wait.
            //
            _ASSERTE(_state >= WaiterCountIncrement);
            _gate->Wait();
            _ASSERTE(_state >= WaiterCountIncrement);

            // this was either real or spurious wake.
            // either way try acquire again.
            hasWaited = true;
        }
    }

    NOINLINE
    void AwakeWaiterIfNeeded()
    {
        int collisions = 0;
        while (true)
        {
            uint32_t oldState = _state;
            if ((int32_t)oldState >= (int32_t)WaiterCountIncrement) // false if WaiterWoken is set
            {
                // there are waiters, and nobody has woken one.
                uint32_t newState = oldState | WaiterWoken;

                uint16_t lastWakeTicks = _wakeWatchDog;
                if (lastWakeTicks != 0)
                {
                    uint16_t currentTicks = GetTickCount();
                    if ((int16_t)currentTicks - (int16_t)lastWakeTicks > (int16_t)WaiterWatchdogTicks)
                    {
                        //printf("Last: %i ", (int)lastWakeTicks);
                        //printf("Current: %i \n", (int)currentTicks);
                        newState |= YieldToWaiters;
                    }
                }

                if (Interlocked::CompareExchange(&_state, newState, oldState) == oldState)
                {
                    if (lastWakeTicks == 0)
                    {
                        // Sometimes timestamp will be 0.
                        // It is harmless. We will try again on the next wake
                        _wakeWatchDog = GetTickCount();
                    }

                    _gate->WakeOne();
                    return;
                }
            }
            else
            {
                // no need to wake a waiter.
                return;
            }

            CollisionBackoff(++collisions);
        }
    }
};

class SatoriLockHolder : public Satori::StackOnly {
private:
    SatoriLock* const m_lock;

public:
    // Disallow copying
    SatoriLockHolder& operator=(const SatoriLockHolder&) = delete;
    SatoriLockHolder(const SatoriLockHolder&) = delete;

    SatoriLockHolder(SatoriLock* lock)
        : m_lock(lock)
    {
        m_lock->Enter();
    }

    SatoriLockHolder(SatoriLock* lock, bool isLocked)
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
