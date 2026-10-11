// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef FORCEDGCHOLDER_H
#define FORCEDGCHOLDER_H

extern bool s_forcedGCInProgress;

class ForcedGCHolder
{
public:
    ForcedGCHolder() { LIMITED_METHOD_CONTRACT; s_forcedGCInProgress = true; }
    ~ForcedGCHolder() { LIMITED_METHOD_CONTRACT; s_forcedGCInProgress = false; }
};

#endif // FORCEDGCHOLDER_H
