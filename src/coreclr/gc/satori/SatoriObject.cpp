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

NOINLINE void SatoriObject::ClearPinned()
{
    if (!IsEscaped())
    {
        ClearBit(2);
    }
}

void SatoriObject::EscapeCheck()
{
    SatoriRegion* region = ContainingRegion();
    if (region->OwnedByCurrentThread())
    {
        region->EscapeRecursively(this);
    }
}

SatoriObject* SatoriObject::FormatAsFree(size_t location, size_t size)
{
    _ASSERTE(location == ALIGN_UP(location, Satori::OBJECT_ALIGNMENT));
    _ASSERTE(size >= sizeof(Object) + sizeof(size_t));
    _ASSERTE(size < Satori::REGION_SIZE_GRANULARITY);

    SatoriObject* obj = SatoriObject::At(location);
    _ASSERTE(obj->ContainingRegion()->m_used > location + ArrayBase::GetOffsetOfNumComponents() + sizeof(size_t));

#ifdef JUNK_FILL_FREE_SPACE
    size_t dirtySize = min(size, obj->ContainingRegion()->m_used - location);
    memset((void*)(location - sizeof(size_t)), 0xAC, dirtySize);
#endif

    obj->CleanSyncBlock();
    obj->RawSetMethodTable(s_emptyObjectMt);

    // deduct the size of Array header + syncblock and set the size of the free bytes.
    ((DWORD*)obj)[ArrayBase::GetOffsetOfNumComponents() / sizeof(DWORD)] = (DWORD)(size - (sizeof(ArrayBase) + sizeof(size_t)));
    return obj;
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

    if (ContainingRegion()->OwnedByCurrentThread())
    {
        if (IsEscaped())
        {
            _ASSERTE(!IsFree());

            ForEachObjectRef(
                [this](SatoriObject** ref)
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
