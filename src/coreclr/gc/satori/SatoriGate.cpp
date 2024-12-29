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
// SatoriGate.cpp
//

#ifdef TARGET_WINDOWS

#include "common.h"
#include "windows.h"
#include "synchapi.h"
#include "SatoriGate.h"

SatoriGate::SatoriGate()
{
    m_state = s_blocking;
}

// If this gate is in blocking state, the thread will block
// until woken up, possibly spuriously.
void SatoriGate::Wait()
{
    uint32_t blocking = s_blocking;
    BOOL result = WaitOnAddress(&m_state, &blocking, sizeof(uint32_t), INFINITE);
    _ASSERTE(result == TRUE);
    m_state = s_blocking;
}

// If this gate is in blocking state, the thread will block
// until woken up, possibly spuriously.
// or until the wait times out. (in a case of timeout returns false)
bool SatoriGate::TimedWait(int timeout)
{
    uint32_t blocking = s_blocking;
    BOOL result = WaitOnAddress(&m_state, &blocking, sizeof(uint32_t), timeout);
    _ASSERTE(result == TRUE || GetLastError() == ERROR_TIMEOUT);

    bool woken = result == TRUE;
    if (woken)
    {
        // consume the wake
        m_state = s_blocking;
    }

    return woken;
}

// After this call at least one thread will go through the gate, either by waking up,
// or by going through Wait without blocking.
// If there are several racing wakes, one or more may take effect,
// but all wakes will see at least one thread going through the gate.
void SatoriGate::WakeOne()
{
    m_state = s_open;
    WakeByAddressSingle((PVOID)&m_state);
}

// Same as WakeOne, but if there are multiple waiters sleeping,
// all will be woken up and go through the gate.
void SatoriGate::WakeAll()
{
    m_state = s_open;
    WakeByAddressAll((PVOID)&m_state);
}


#endif
