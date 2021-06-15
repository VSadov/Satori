// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriFinalizationQueue.h
//

#ifndef __SATORI_FINALIZATION_QUEUE_H__
#define __SATORI_FINALIZATION_QUEUE_H__

#include "common.h"
#include "../gc.h"

class SatoriObject;

class SatoriFinalizationQueue
{
public:
    void Initialize();
    bool TryUpdateScanTicket(int currentScanTicket);
    bool TryScheduleForFinalization(SatoriObject* finalizable);
    bool TryScheduleForFinalizationSTW(SatoriObject* finalizable);
    SatoriObject* TryGetNextItem();
    bool HasItems();

    template<typename F>
    void ForEachObjectRef(F& lambda)
    {
        for (int i = m_dequeue; i != m_enqueue; i++)
        {
            lambda(&m_data[i & m_sizeMask].value);
        }
    }

private:
    struct Entry
    {
        SatoriObject* value;
        int           version;
    };

    int m_enqueue;
    int m_dequeue;
    int m_sizeMask;
    int m_scanTicket;
    Entry* m_data;
};

#endif
