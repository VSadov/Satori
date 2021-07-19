// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriHandlePartitioner.h
//

#ifndef __SATORI_HANDLE_PARTITIONER_H__
#define __SATORI_HANDLE_PARTITIONER_H__

#include "common.h"
#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriUtil.h"

class SatoriHandlePartitioner
{
public:
    static void Initialize()
    {
        s_partitionCount = (int)GCToOSInterface::GetTotalProcessorCount();
        s_scanTickets = new uint8_t[s_partitionCount]{};
    }

    static void StartNextScan()
    {
        s_currentTicket++;
    }

    static int PartitionCount()
    {
        return s_partitionCount;
    }

    static int CurrentThreadPartition()
    {
        if (!s_partitionCount)
        {
            return 0;
        }

        uint32_t value;
        if (GCToOSInterface::CanGetCurrentProcessorNumber())
        {
            //TODO: VS GetCurrentProcessorNumber could be slow (can it actually? on platformas that matter?).
            //      We could cache this for handle creation.
            // 
            //      this is for:
            //      - balance handle tables
            //      - affinity when scanning handles
            // 
            //      it must be CPU related, since scan could be on a different thread.
            //      it does not need to be very up to date though, just statistically close would be enough.
            //      we just want to improve the likelyhood of finding it in cur proc table when scanning.
            //
            //      just invalidate the cache whe scan num moves forward? (it means the scan will get uncached value, which is ok)
            value = GCToOSInterface::GetCurrentProcessorNumber();
        }
        else
        {
            size_t thread = SatoriUtil::GetCurrentThreadTag();
            value = (uint32_t)((thread * 11400714819323198485llu) >> 32);
        }

        return (int)(value % (uint32_t)s_partitionCount);
    }

    template<typename F>
    static bool ForEachUnscannedPartition(F lambda, int64_t deadline = 0)
    {
        int startPartition = CurrentThreadPartition();
        //TODO: VS partition walk should be NUMA-aware, if possible
        for (int i = 0; i < s_partitionCount; i++)
        {
            int partition = (i + startPartition) % s_partitionCount;
            uint8_t partitionTicket = VolatileLoadWithoutBarrier(&s_scanTickets[partition]);
            if (partitionTicket != s_currentTicket)
            {
                if (Interlocked::CompareExchange(&s_scanTickets[partition], s_currentTicket, partitionTicket) == partitionTicket)
                {
                    lambda(partition);

                    if (deadline && (GCToOSInterface::QueryPerformanceCounter() - deadline > 0))
                    {
                        // timed out, there could be more work
                        return true;
                    }
                }
            }
        }

        // done
        return false;
    }

private:
    static int s_partitionCount;
    static uint8_t* s_scanTickets;
    static uint8_t s_currentTicket;
};

#endif
