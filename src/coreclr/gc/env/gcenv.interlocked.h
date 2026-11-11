// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// Interlocked operations
//

#ifndef __GCENV_INTERLOCKED_H__
#define __GCENV_INTERLOCKED_H__

// Interlocked operations
class Interlocked
{
private:

#ifndef _MSC_VER
    static void InterlockedOperationBarrier();
#endif // !_MSC_VER

public:

    // Increment the value of the specified 32-bit variable as an atomic operation.
    // Parameters:
    //  addend - variable to be incremented
    // Return:
    //  The resulting incremented value
    template<typename T>
    static T Increment(T volatile *addend);

    // Decrement the value of the specified 32-bit variable as an atomic operation.
    // Parameters:
    //  addend - variable to be decremented
    // Return:
    //  The resulting decremented value
    template<typename T>
    static T Decrement(T volatile *addend);

    // Perform an atomic AND operation on the specified values values
    // Parameters:
    //  destination - the first operand and the destination
    //  value       - second operand
    template<typename T>
    static void And(T volatile *destination, T value);

    // Perform an atomic OR operation on the specified values values
    // Parameters:
    //  destination - the first operand and the destination
    //  value       - second operand
    template<typename T>
    static void Or(T volatile *destination, T value);

    // Set a 32-bit variable to the specified value as an atomic operation.
    // Parameters:
    //  destination - value to be exchanged
    //  value       - value to set the destination to
    // Return:
    //  The previous value of the destination
    template<typename T>
    static T Exchange(T volatile *destination, T value);

    // Set a pointer variable to the specified value as an atomic operation.
    // Parameters:
    //  destination - value to be exchanged
    //  value       - value to set the destination to
    // Return:
    //  The previous value of the destination
    template <typename T>
    static T ExchangePointer(T volatile * destination, T value);

    template <typename T>
    static T ExchangePointer(T volatile * destination, std::nullptr_t value);

    // Perform an atomic addition of two 32-bit values and return the original value of the addend.
    // Parameters:
    //  addend - variable to be added to
    //  value  - value to add
    // Return:
    //  The previous value of the addend
    template<typename T>
    static T ExchangeAdd(T volatile *addend, T value);

    template<typename T>
    static T ExchangeAdd64(T volatile* addend, T value);

    template<typename T>
    static T ExchangeAddPtr(T volatile* addend, T value);

    // Performs an atomic compare-and-exchange operation on the specified values.
    // Parameters:
    //  destination - value to be exchanged
    //  exchange    - value to set the destination to
    //  comparand   - value to compare the destination to before setting it to the exchange.
    //                The destination is set only if the destination is equal to the comparand.
    // Return:
    //  The original value of the destination
    template<typename T>
    static T CompareExchange(T volatile *destination, T exchange, T comparand);

    // Performs an atomic compare-and-exchange operation on the specified pointers.
    // Parameters:
    //  destination - value to be exchanged
    //  exchange    - value to set the destination to
    //  comparand   - value to compare the destination to before setting it to the exchange.
    //                The destination is set only if the destination is equal to the comparand.
    // Return:
    //  The original value of the destination
    template <typename T>
    static T CompareExchangePointer(T volatile *destination, T exchange, T comparand);

    template <typename T>
    static T CompareExchangePointer(T volatile *destination, T exchange, std::nullptr_t comparand);

    // The operations above order both ways: a locked instruction on x86 and x64; on ARM64 an atomic that is both an acquire and a release,
    // and with GCC and Clang a full barrier after it (see InterlockedOperationBarrier). The ones below order no more than their names say.
    //   Acq:       what the thread does after the operation is ordered after the read of the destination.
    //   Rel:       what the thread did before the operation is ordered before the write of the destination.
    //   Unordered: atomic, and nothing more.
    // On x86 and x64 they are the same locked instructions as the ones above.

    // Same as CompareExchange, but an acquire, whether the exchange happens or not.
    template<typename T>
    static T CompareExchangeAcq(T volatile *destination, T exchange, T comparand);

    // Same as CompareExchange, but unordered.
    template<typename T>
    static T CompareExchangeUnordered(T volatile *destination, T exchange, T comparand);

    // Same as Decrement, but a release.
    template<typename T>
    static T DecrementRel(T volatile *addend);

    // Same as And, but unordered.
    template<typename T>
    static void AndUnordered(T volatile *destination, T value);

    // Same as Or, but a release.
    template<typename T>
    static void OrRel(T volatile *destination, T value);

    // Same as Or, but unordered.
    template<typename T>
    static void OrUnordered(T volatile *destination, T value);
};

#endif // __GCENV_INTERLOCKED_H__
