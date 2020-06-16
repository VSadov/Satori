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

class SatoriGC;
class SatoriRegion;

class SatoriObject : public Object
{
    friend SatoriGC;

public:
    SatoriObject() = delete;
    ~SatoriObject() = delete;

    static SatoriObject* At(size_t location);
    static SatoriObject* FormatAsFree(size_t location, size_t size);

    SatoriRegion* ContainingRegion();
    SatoriObject* Next();

    size_t Size();
    bool IsFree();

    bool IsEscaped();
    void SetEscaped();
    bool IsMarked();
    void SetMarked();
    bool IsPinned();
    void SetPinned();

    void CleanSyncBlock();

    void Validate();

private:
    static MethodTable* s_emptyObjectMt;
    static void Initialize();
};

#endif
