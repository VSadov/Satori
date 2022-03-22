// Copyright (c) 2022 Vladimir Sadov
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
// SatoriGC.h
//

#ifndef __SATORI_RECYCLER_QUEUS_H__
#define __SATORI_RECYCLER_QUEUS_H__

#include "common.h"
#include "../gc.h"
#include "SatoriUtil.h"

#include "SatoriAllocator.h"
#include "SatoriRecycler.h"

class SatoriRegion;

class SatoriRecyclerQueues
{
public:

    SatoriRecyclerQueues()
    {
        m_ephemeralRegions = new SatoriRegionQueue(QueueKind::RecyclerEphemeral);
        m_ephemeralFinalizationTrackingRegions = new SatoriRegionQueue(QueueKind::RecyclerEphemeralFinalizationTracking);
        m_tenuredRegions = new SatoriRegionQueue(QueueKind::RecyclerTenured);
        m_tenuredFinalizationTrackingRegions = new SatoriRegionQueue(QueueKind::RecyclerTenuredFinalizationTracking);

        m_stayingRegions = new SatoriRegionQueue(QueueKind::RecyclerStaying);
        m_relocationCandidates = new SatoriRegionQueue(QueueKind::RecyclerRelocationCandidates);

        for (int i = 0; i < Satori::FREELIST_COUNT; i++)
        {
            m_relocationTargets[i] = new SatoriRegionQueue(QueueKind::RecyclerRelocationTargets);
        }
    }

    SatoriRegionQueue* m_ephemeralFinalizationTrackingRegions;
    SatoriRegionQueue* m_tenuredFinalizationTrackingRegions;

    SatoriRegionQueue* m_stayingRegions;
    SatoriRegionQueue* m_relocationCandidates;

    SatoriRegionQueue* m_ephemeralRegions;
    SatoriRegionQueue* m_tenuredRegions;

    SatoriRegionQueue* m_relocationTargets[Satori::FREELIST_COUNT];

private:

};

#endif
