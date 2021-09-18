// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriMarkChunk.h
//

#ifndef __SATORI_MARK_CHUNK_H__
#define __SATORI_MARK_CHUNK_H__

#include "common.h"
#include "../gc.h"
#include "SatoriUtil.h"
#include "SatoriQueue.h"

class SatoriMarkChunk
{
    friend class SatoriQueue<SatoriMarkChunk>;
    friend class SatoriObject;

public:
    SatoriMarkChunk() = delete;
    ~SatoriMarkChunk() = delete;

    static SatoriMarkChunk* InitializeAt(size_t address)
    {
        SatoriMarkChunk* self = (SatoriMarkChunk*)address;
        self->m_top = 0;
        self->m_prev = self->m_next = nullptr;
        self->m_containingQueue = nullptr;

        return self;
    }

    static size_t Capacity()
    {
        return (Satori::MARK_CHUNK_SIZE - sizeof(SatoriMarkChunk)) / sizeof(SatoriObject*);
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

    bool TryPush(SatoriObject* obj)
    {
        if (HasSpace())
        {
            m_data[m_top++] = obj;
            return true;
        }

        return false;
    }

    void SetNext(SatoriMarkChunk* next)
    {
        _ASSERTE(m_containingQueue == nullptr);
        m_next = next;
    }

    SatoriMarkChunk* Next()
    {
        _ASSERTE(m_containingQueue == nullptr);
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

private:
    size_t m_top;

    SatoriMarkChunk* m_prev;
    SatoriMarkChunk* m_next;
    SatoriQueue<SatoriMarkChunk>* m_containingQueue;

    SatoriObject* m_data[1];
};

#endif
