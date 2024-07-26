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

#include "windows.h"
#include "synchapi.h"
#include "SatoriGate.h"

// static
uint32_t SatoriGate::s_open = 1;
uint32_t SatoriGate::s_blocking = 0;

SatoriGate::SatoriGate()
{
    m_state = s_blocking;
}

// returns true if was woken up,
// false if timed out.
void SatoriGate::Wait()
{
    // TODO: VS handle errors? at least asserts
    BOOL result = WaitOnAddress(&m_state, &s_blocking, sizeof(uint32_t), INFINITE);
}

// returns true if was woken up,
// false if timed out.
bool SatoriGate::TimedWait(int timeout)
{
    // TODO: VS handle errors? at least asserts
    BOOL result = WaitOnAddress(&m_state, &s_blocking, sizeof(uint32_t), timeout);
    m_state = s_blocking;
    return result == TRUE;
}

void SatoriGate::WakeAll()
{
    m_state = s_open;
    WakeByAddressAll(&m_state);
}

void SatoriGate::WakeOne()
{
    m_state = s_open;
    WakeByAddressSingle(&m_state);
}

#endif
