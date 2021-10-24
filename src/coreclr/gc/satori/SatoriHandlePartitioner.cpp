// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriHandlePartitioner.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriHandlePartitioner.h"

// static
int SatoriHandlePartitioner::s_partitionCount;
// static
uint8_t* SatoriHandlePartitioner::s_scanTickets;
//static
uint8_t SatoriHandlePartitioner::s_currentTicket;
//static
uint8_t SatoriHandlePartitioner::s_completedTicket;

