// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriObject.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "SatoriObject.h"
#include "SatoriObject.inl"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"
#include "SatoriPage.h"
#include "SatoriPage.inl"

MethodTable* SatoriObject::s_emptyObjectMt;

void SatoriObject::Initialize()
{
    s_emptyObjectMt = GCToEEInterface::GetFreeObjectMethodTable();
}

void SatoriObject::EscapeCheckOnHandleCreation()
{
    SatoriRegion* region = ContainingRegion();
    if (region->IsEscapeTrackedByCurrentThread())
    {
        region->EscapeRecursively(this);

        // make sure the object is published after we are done marking escapes
        VolatileStoreBarrier();
    }
}

SatoriObject* SatoriObject::FormatAsFree(size_t location, size_t size)
{
    _ASSERTE(location == ALIGN_UP(location, Satori::OBJECT_ALIGNMENT));
    _ASSERTE(size == ALIGN_UP(size, Satori::OBJECT_ALIGNMENT));
    _ASSERTE(size >= Satori::MIN_FREE_SIZE);
    _ASSERTE(size < Satori::REGION_SIZE_GRANULARITY);

    SatoriObject* o = (SatoriObject*)location;
    _ASSERTE(o->ContainingRegion()->m_used > location + 2 * sizeof(size_t));

#ifdef JUNK_FILL_FREE_SPACE
    size_t dirtySize = min(size, o->ContainingRegion()->m_used - location);
    memset((void*)(location - sizeof(size_t)), 0xAC, dirtySize);
#endif

    o->CleanSyncBlock();
    o->RawSetMethodTable(s_emptyObjectMt);
    _ASSERTE(!o->IsMarked());

    // deduct the size of Array header + syncblock and set the size of the free bytes.
    ((size_t*)o)[1] = size - Satori::MIN_FREE_SIZE;
    return o;
}

void SatoriObject::DirtyCardsForContent()
{
    _ASSERTE(IsMarked());
    MethodTable* mt = RawGetMethodTable();
    if (mt->ContainsPointersOrCollectible())
    {
        SatoriPage* page = ContainingRegion()->m_containingPage;
        // if dealing with a collectible type, include MT in the dirty range
        // to be treated as a "reference" to the collectible allocator.
        size_t rangeStart = mt->Collectible() ? Start() : Start() + sizeof(size_t);
        page->DirtyCardsForRange(rangeStart, End());
    }
}

void SatoriObject::Validate()
{
#ifdef _DEBUG
    // _ASSERTE(this->GetReloc() == 0);
    _ASSERTE(this->Size() >= Satori::MIN_FREE_SIZE);

    if (ContainingRegion()->IsEscapeTrackedByCurrentThread())
    {
        if (IsEscaped())
        {
            _ASSERTE(!IsFree());

            ForEachObjectRef(
                [&](SatoriObject** ref)
                {
                    _ASSERTE(ContainingRegion()->IsExposed(ref));
                    SatoriObject* child = *ref;
                    if (child->ContainingRegion() == ContainingRegion())
                    {
                        _ASSERTE(child->IsEscaped());
                    }
                }
            );
        }
        else
        {
            size_t size = Size();
            for (size_t i = Start(); i < size; i++)
            {
                _ASSERTE(!((SatoriObject*)i)->IsMarked());
            }
        }
    }
#endif
}
