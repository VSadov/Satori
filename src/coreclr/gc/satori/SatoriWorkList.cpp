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
    SatoriWorkChunk* head;
    while (true)
    {
        head = this->m_head;
        size_t aba = this->m_aba;

        item->m_next = head;

        SatoriWorkList orig(head, aba);
        if (Cas128((int64_t*)this, aba + 1, (int64_t)item, (int64_t*)&orig))
            break;

        SatoriLock::CollisionBackoff(collisions++);
    }

#ifdef COUNT_CHUNKS
    Interlocked::Increment(&m_count);
#endif
}

NOINLINE
SatoriWorkChunk* SatoriWorkList::TryPopSlow()
{
    uint32_t collisions = 1;
    SatoriWorkChunk* head;
    while (true)
    {
        head = this->m_head;
        size_t aba = this->m_aba;

        if (head == nullptr)
        {
            return nullptr;
        }

        SatoriWorkList orig(head, aba);
        if (Cas128((int64_t*)this, aba + 1, (int64_t)head->m_next, (int64_t*)&orig))
            break;

        SatoriLock::CollisionBackoff(collisions++);
    }

#ifdef COUNT_CHUNKS
    Interlocked::Decrement(&m_count);
#endif

    head->m_next = nullptr;
    return head;
}

// Takes all the chunks at once. They stay linked through Next(), the first one is returned.
NOINLINE
SatoriWorkChunk* SatoriWorkList::TakeAll()
{
    uint32_t collisions = 1;
    SatoriWorkChunk* head;
    while (true)
    {
        head = this->m_head;
        size_t aba = this->m_aba;

        if (head == nullptr)
        {
            return nullptr;
        }

        SatoriWorkList orig(head, aba);
        if (Cas128((int64_t*)this, aba + 1, (int64_t)nullptr, (int64_t*)&orig))
            break;

        SatoriLock::CollisionBackoff(collisions++);
    }

#ifdef COUNT_CHUNKS
    for (SatoriWorkChunk* chunk = head; chunk; chunk = chunk->m_next)
    {
        Interlocked::Decrement(&m_count);
    }
#endif

    return head;
}

// Pushes a chain of chunks at once. The chunks are linked through Next(), from the first to the last.
NOINLINE
void SatoriWorkList::PushChain(SatoriWorkChunk* first, SatoriWorkChunk* last)
{
    _ASSERTE(last->m_next == nullptr);

#ifdef COUNT_CHUNKS
    for (SatoriWorkChunk* chunk = first; ; chunk = chunk->m_next)
    {
        _ASSERTE(chunk != nullptr);
        Interlocked::Increment(&m_count);
        if (chunk == last)
        {
            break;
        }
    }
#endif

    uint32_t collisions = 1;
    while (true)
    {
        SatoriWorkChunk* head = this->m_head;
        size_t aba = this->m_aba;

        last->m_next = head;

        SatoriWorkList orig(head, aba);
        if (Cas128((int64_t*)this, aba + 1, (int64_t)first, (int64_t*)&orig))
            break;

        SatoriLock::CollisionBackoff(collisions++);
    }
}
