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
// #define JUNK_FILL_FREE_SPACE

class SatoriObject : public Object
{
    friend class SatoriRegion;
    friend class SatoriGC;

public:
    SatoriObject() = delete;
    ~SatoriObject() = delete;

    static SatoriObject* At(size_t location);
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
    bool IsPinned();
    void SetPinned();
    void ClearPinnedAndMarked();
    bool IsEscaped();
    bool IsEscapedOrPinned();
    int GetMarkBitAndOffset(size_t* bitmapIndex);

    void SetPermanentlyPinned();
    bool IsPermanentlyPinned();

    void DirtyCardsForContent();

    void EscapeCheck();

    bool IsFinalizationSuppressed();

    int32_t GetNextInMarkStack();
    void SetNextInMarkStack(int32_t);
    void ClearNextInMarkStack();
    void ClearMarkCompactStateForRelocation();
    int32_t GetReloc();
    void SetReloc(int32_t);

    void CleanSyncBlock();

    void Validate();

    template<typename F>
    void ForEachObjectRef(F& lambda, bool includeCollectibleAllocator = false);

    template<typename F>
    void ForEachObjectRef(F& lambda, size_t start, size_t end);

private:
    static MethodTable* s_emptyObjectMt;
    static void Initialize();

    void SetBit(int offset);
    void ClearBit(int offset);
    bool CheckBit(int offset);
    void SetEscaped();
    void ClearPinned();
};

#endif
