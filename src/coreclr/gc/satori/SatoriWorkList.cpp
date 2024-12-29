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
// SatoriWorkList.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "SatoriWorkList.h"

NOINLINE
void SatoriWorkList::PushSlow(SatoriWorkChunk* item)
{
    uint32_t collisions = 1;
    while (true)
    {
        SatoriWorkList orig = *this;
        item->m_next = orig.m_head;
        if (Cas128((int64_t*)this, orig.m_aba + 1, (int64_t)item, (int64_t*)&orig))
            break;

        SatoriLock::CollisionBackoff(collisions++);
    }

#ifdef _DEBUG
    Interlocked::Increment(&m_count);
#endif
}

NOINLINE
SatoriWorkChunk* SatoriWorkList::TryPopSlow()
{
    uint32_t collisions = 1;
    SatoriWorkList orig;
    while (true)
    {
        orig = *this;
        if (orig.m_head == nullptr)
        {
            return nullptr;
        }

        if (Cas128((int64_t*)this, orig.m_aba + 1, (int64_t)orig.m_head->m_next, (int64_t*)&orig))
            break;

        SatoriLock::CollisionBackoff(collisions++);
    }

#ifdef _DEBUG
    Interlocked::Decrement(&m_count);
#endif

    SatoriWorkChunk* result = orig.m_head;
    result->m_next = nullptr;
    return result;
}
