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
// SatoriRecycler.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriUtil.h"
#include "SatoriTrimmer.h"
#include "SatoriHeap.h"
#include "SatoriPage.h"
#include "SatoriPage.inl"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"

SatoriTrimmer::SatoriTrimmer(SatoriHeap* heap)
{
    m_lastGen2Count = 0;
    m_heap  = heap;
    m_state = TRIMMER_STATE_STOPPED;

    m_gate = new (nothrow) GCEvent;
    m_gate->CreateAutoEventNoThrow(false);

    if (SatoriUtil::IsTrimmingEnabled())
    {
        GCToEEInterface::CreateThread(LoopFn, this, false, "Satori GC Trimmer Thread");
    }
}

void SatoriTrimmer::LoopFn(void* inst)
{
    SatoriTrimmer* ths = (SatoriTrimmer*)inst;
    ths->Loop();
}

void SatoriTrimmer::Loop()
{
    while (true)
    {
        int64_t curGen2 = m_heap->Recycler()->GetCollectionCount(2);

        // limit the trim rate to once per 1 sec + 1 gen2 gc.
        do
        {
            Interlocked::CompareExchange(&m_state, TRIMMER_STATE_STOPPED, TRIMMER_STATE_RUNNING);
            // we are not running here, so we can sleep a bit before continuing.
            GCToOSInterface::Sleep(1000);
            StopAndWait();
        } while (curGen2 == m_heap->Recycler()->GetCollectionCount(2));

        m_heap->ForEachPage(
            [&](SatoriPage* page)
            {
                // limit the rate of scanning to 1 page/msec.
                GCToOSInterface::Sleep(1);
                if (m_state != TRIMMER_STATE_RUNNING)
                {
                    StopAndWait();
                }

                page->ForEachRegion(
                    [&](SatoriRegion* region)
                    {
                        SatoriQueue<SatoriRegion>* queue = region->ContainingQueue();
                        if (queue && queue->Kind() == QueueKind::Allocator)
                        {
                            if (region->CanDecommit() || region->CanCoalesceWithNext())
                            {
                                if (queue->TryRemove(region))
                                {
                                    bool didSomeWork = region->TryCoalesceWithNext();
                                    if (region->TryDecommit())
                                    {
                                        m_heap->Allocator()->AddRegion(region);
                                        didSomeWork = true;
                                    }
                                    else
                                    {
                                        m_heap->Allocator()->ReturnRegion(region);
                                    }

                                    if (didSomeWork)
                                    {
                                        // limit the decommit/coalesce rate to 1 region/msec.
                                        GCToOSInterface::Sleep(1);
                                    }
                                }
                            }
                        }

                        if (m_state != TRIMMER_STATE_RUNNING)
                        {
                            StopAndWait();
                        }
                    }
                );
            }
        );
    }
}

void SatoriTrimmer::StopAndWait()
{
    while (true)
    {
        tryAgain:
        int state = m_state;
        switch (state)
        {
        case TRIMMER_STATE_STOP_SUGGESTED:
            Interlocked::CompareExchange(&m_state, TRIMMER_STATE_STOPPED, state);
            continue;
        case TRIMMER_STATE_OK_TO_RUN:
            Interlocked::CompareExchange(&m_state, TRIMMER_STATE_RUNNING, state);
            continue;
        case TRIMMER_STATE_STOPPED:
            for (int i = 0; i < 10; i++)
            {
                GCToOSInterface::Sleep(100);
                if (m_state != state)
                {
                    goto tryAgain;
                }
            }

            if (Interlocked::CompareExchange(&m_state, TRIMMER_STATE_BLOCKED, state) == state)
            {
                m_gate->Wait(INFINITE, false);
            }
            continue;
        case TRIMMER_STATE_RUNNING:
            return;
        default:
            __UNREACHABLE();
            return;
        }
    }
}

void SatoriTrimmer::SetOkToRun()
{
    int state = m_state;
    switch (state)
    {
    case TRIMMER_STATE_BLOCKED:
        // trimmer can't get out of BlOCKED by itself, ordinary assignment is ok
        m_state = TRIMMER_STATE_OK_TO_RUN;
        m_gate->Set();
        break;
    case TRIMMER_STATE_STOPPED:
        Interlocked::CompareExchange(&m_state, TRIMMER_STATE_OK_TO_RUN, state);
        break;
    case TRIMMER_STATE_STOP_SUGGESTED:
        Interlocked::CompareExchange(&m_state, TRIMMER_STATE_RUNNING, state);
        break;
    }
}

void SatoriTrimmer::SetStopSuggested()
{
    while (true)
    {
        int state = m_state;
        switch (state)
        {
        case TRIMMER_STATE_OK_TO_RUN:
            if (Interlocked::CompareExchange(&m_state, TRIMMER_STATE_STOPPED, state) == state)
            {
                return;
            }
            break;
        case TRIMMER_STATE_RUNNING:
            if (Interlocked::CompareExchange(&m_state, TRIMMER_STATE_STOP_SUGGESTED, state) == state)
            {
                return;
            }
            break;
        default:
            _ASSERTE(m_state <= TRIMMER_STATE_STOP_SUGGESTED);
            return;
        }
    }
}

void SatoriTrimmer::WaitForStop()
{
    _ASSERTE(m_state <= TRIMMER_STATE_STOP_SUGGESTED);

    int cycles = 0;
    while (m_state == TRIMMER_STATE_STOP_SUGGESTED)
    {
        YieldProcessor();
        if ((++cycles % 127) == 0)
        {
            GCToOSInterface::YieldThread(0);
        }
    }

    _ASSERTE(!IsActive());
}

bool SatoriTrimmer::IsActive()
{
    return m_state > TRIMMER_STATE_STOPPED;
}
