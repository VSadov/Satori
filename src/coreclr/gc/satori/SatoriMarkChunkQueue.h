// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRegionQueue.h
//

#ifndef __SATORI_MARK_CHUNK_QUEUE_H__
#define __SATORI_MARK_CHUNK_QUEUE_H__

#include "common.h"
#include "../gc.h"
#include "SatoriQueue.h"

class SatoriMarkChunk;

class SatoriMarkChunkQueue : public SatoriQueue<SatoriMarkChunk>
{
public:
    SatoriMarkChunkQueue() :
        SatoriQueue<SatoriMarkChunk>(QueueKind::MarkChunk) {}
};

#endif
