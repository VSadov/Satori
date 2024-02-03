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
// SatoriWorkList.h
//

#ifndef __SATORI_WORK_LIST_H__
#define __SATORI_WORK_LIST_H__

#include "common.h"
#include "../gc.h"
#include "SatoriWorkChunk.h"

class SatoriWorkList
{
public:
    SatoriWorkList() :
        m_lock(), m_head()
#ifdef _DEBUG
        , m_count()
#endif
    {
        m_lock.Initialize();
    }

    bool IsEmpty()
    {
        return m_head == nullptr;
    }

    void Push(SatoriWorkChunk* item)
    {
        _ASSERTE(item->m_next == nullptr);

        SatoriLockHolder<SatoriSpinLock> holder(&m_lock);
        item->m_next = m_head;
        m_head = item;
#ifdef _DEBUG
        m_count++;
#endif
    }

    SatoriWorkChunk* TryPop()
    {
        if (IsEmpty())
        {
            return nullptr;
        }

        SatoriWorkChunk* result;
        {
            SatoriLockHolder<SatoriSpinLock> holder(&m_lock);
            result = m_head;
            if (result == nullptr)
            {
                return result;
            }

            m_head = result->m_next;
#ifdef _DEBUG
            m_count--;
#endif
        }

        result->m_next = nullptr;
        return result;
    }

#ifdef _DEBUG
    size_t Count()
    {
        return m_count;
    }
#endif

private:
    SatoriSpinLock m_lock;
    SatoriWorkChunk* m_head;
#ifdef _DEBUG
    size_t m_count;
#endif
};

#endif
