// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRecycler.h
//

#ifndef __SATORI_TRIMMER_H__
#define __SATORI_TRIMMER_H__

#include "common.h"
#include "../gc.h"
#include "SatoriRegionQueue.h"

class SatoriHeap;

class SatoriTrimmer
{
public:
    SatoriTrimmer(SatoriHeap* heap);

    void SetStopSuggested();
    void SetOkToRun();
    void WaitForStop();

private:
    static const int TRIMMER_STATE_BLOCKED = -1;
    static const int TRIMMER_STATE_STOPPED = 0;
    static const int TRIMMER_STATE_STOP_SUGGESTED = 1;
    static const int TRIMMER_STATE_OK_TO_RUN = 2;
    static const int TRIMMER_STATE_RUNNING = 3;

    SatoriHeap*  m_heap;
    GCEvent*     m_gate;
    size_t       m_lastGen2Count;
    volatile int m_state;

    static void LoopFn(void* inst);
    void Loop();
    void StopAndWait();
};

#endif
