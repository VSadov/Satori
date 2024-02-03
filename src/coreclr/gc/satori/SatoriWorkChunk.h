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
// SatoriWorkChunk.h
//

#ifndef __SATORI_WORK_CHUNK_H__
#define __SATORI_WORK_CHUNK_H__

#include "common.h"
#include "../gc.h"
#include "SatoriUtil.h"
#include "SatoriQueue.h"

class SatoriWorkChunk
{
    friend class SatoriWorkList;

public:
    SatoriWorkChunk() = delete;
    ~SatoriWorkChunk() = delete;

    static SatoriWorkChunk* InitializeAt(size_t address)
    {
        SatoriWorkChunk* self = (SatoriWorkChunk*)address;
        self->m_top = 0;
        self->m_next = nullptr;

        return self;
    }

    static size_t Capacity()
    {
        return (Satori::MARK_CHUNK_SIZE - sizeof(SatoriWorkChunk)) / sizeof(SatoriObject*);
    }

    size_t Count()
    {
        return m_top;
    }

    void Clear()
    {
        m_top = 0;
    }

    size_t FreeSpace()
    {
        return Capacity() - m_top;
    }

    bool HasSpace()
    {
        return m_top < Capacity();
    }

    void Push(SatoriObject* obj)
    {
        _ASSERTE(HasSpace());
        m_data[m_top++] = obj;
    }

    SatoriObject* Pop()
    {
        _ASSERTE(Count() > 0);
        return m_data[--m_top];
    }

    SatoriObject* Peek()
    {
        return m_data[m_top - 1];
    }

    bool TryPush(SatoriObject* obj)
    {
        if (HasSpace())
        {
            m_data[m_top++] = obj;
            return true;
        }

        return false;
    }

    void SetNext(SatoriWorkChunk* next)
    {
        m_next = next;
    }

    SatoriWorkChunk* Next()
    {
        return m_next;
    }

    SatoriObject* &Item(size_t i)
    {
        _ASSERTE(i < m_top);
        return m_data[i];
    }

    size_t Compact()
    {
        size_t to = 0;
        while (to < m_top && m_data[to])
        {
            to++;
        }

        size_t from = to + 1;
        while (from < m_top)
        {
            SatoriObject* o = m_data[from++];
            if (o)
            {
                m_data[to++] = o;
            }
        }

        m_top = to;
        return to;
    }

    bool IsRange()
    {
        return m_top == (size_t)-1;
    }

    void SetRange(SatoriObject* obj, size_t start, size_t end)
    {
        m_top = (size_t)-1;
        m_data[0] = obj;
        m_start = start;
        m_end = end;
    }

    void GetRange(SatoriObject* &obj, size_t &start, size_t &end)
    {
        _ASSERTE(m_top == (size_t)-1);

        obj = m_data[0];
        start = m_start;
        end = m_end;
    }

private:
    size_t m_top;
    SatoriWorkChunk* m_next;

    SatoriObject* m_data[1];
    size_t m_start;
    size_t m_end;
};

#endif
