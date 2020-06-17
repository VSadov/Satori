// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRegion.inl
//

#ifndef __SATORI_REGION_INL__
#define __SATORI_REGION_INL__

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "../env/gcenv.ee.h"
#include "SatoriRegion.h"

inline bool SatoriRegion::IsAllocating()
{
    return m_allocEnd != 0;
}

inline size_t SatoriRegion::Start()
{
    return (size_t)&m_state;
}

inline size_t SatoriRegion::End()
{
    return m_end;
}

inline size_t SatoriRegion::Size()
{
    return End() - Start();
}

inline size_t SatoriRegion::AllocStart()
{
    return m_allocStart;
}

inline size_t SatoriRegion::AllocEnd()
{
    return m_allocEnd;
}

inline size_t SatoriRegion::AllocSize()
{
    return m_allocEnd - m_allocStart;
}

inline SatoriObject* SatoriRegion::FirstObject()
{
    return &m_firstObject;
}

inline void SatoriRegion::Publish()
{
    _ASSERTE(m_state == SatoriRegionState::allocating);
    m_state = SatoriRegionState::shared;
}

#endif
