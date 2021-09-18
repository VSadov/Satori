// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriObject.h
//

#ifndef __SATORI_OBJECT_H__
#define __SATORI_OBJECT_H__

#include "common.h"
#include "../gc.h"

// overwrite freed space with junk for debugging purposes.
#if _DEBUG
#define JUNK_FILL_FREE_SPACE
#endif

class SatoriRegion;

class SatoriObject : public Object
{
    friend class SatoriRegion;
    friend class SatoriGC;

public:
    SatoriObject() = delete;
    ~SatoriObject() = delete;

    static SatoriObject* FormatAsFree(size_t location, size_t size);

    SatoriRegion* ContainingRegion();
    size_t Start();
    size_t End();
    SatoriObject* Next();

    size_t Size();
    bool IsFree();

    bool IsMarked();
    bool IsMarkedOrOlderThan(int generation);
    void SetMarked();
    void ClearMarked();
    void SetMarkedAtomic();
    bool IsPinned();
    void SetPinned();
    void ClearPinnedAndMarked();
    bool IsEscaped();
    bool IsEscapedOrPinned();
    int GetMarkBitAndWord(size_t* bitmapIndex);

    void SetUnmovable();
    bool IsUnmovable();

    void DirtyCardsForContent();

    void EscapeCheckOnHandleCreation();

    bool IsFinalizationSuppressed();
    void SuppressFinalization();
    void UnSuppressFinalization();

    int32_t GetNextInLocalMarkStack();
    void SetNextInLocalMarkStack(int32_t);
    void ClearNextInLocalMarkStack();
    void ClearMarkCompactStateForRelocation();
    int32_t GetLocalReloc();
    void SetLocalReloc(int32_t);

    void CleanSyncBlock();

    void Validate();

    template<typename F>
    void ForEachObjectRef(F lambda, bool includeCollectibleAllocator = false);

    template<typename F>
    void ForEachObjectRef(F lambda, size_t start, size_t end);

private:
    static MethodTable* s_emptyObjectMt;
    static void Initialize();

    void SetBit(int offset);
    void SetBitAtomic(int offset);
    void ClearBit(int offset);
    bool CheckBit(int offset);
    void SetEscaped();
    void ClearPinned();
};

#endif
