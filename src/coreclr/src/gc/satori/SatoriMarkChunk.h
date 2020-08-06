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

    bool TryPush(SatoriObject* obj)
    {
        if (m_top < (Satori::MARK_CHUNK_SIZE - sizeof(SatoriMarkChunk)) / sizeof(SatoriObject*))
        {
            m_data[m_top++] = obj;
            return true;
        }

        return false;
    }

    SatoriObject* TryPop()
    {
        return m_top ?
            m_data[--m_top] :
            nullptr;
    }

    size_t Count()
    {
        return m_top;
    }

private:
    size_t m_top;

    SatoriMarkChunk* m_prev;
    SatoriMarkChunk* m_next;
    SatoriQueue<SatoriMarkChunk>* m_containingQueue;

    SatoriObject* m_data[1];
};

#endif
