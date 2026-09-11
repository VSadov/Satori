// Copyright (c) 2025 Vladimir Sadov
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//
// SatoriUtil.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "SatoriUtil.h"

#if defined(HOST_X86) || defined(HOST_AMD64)
#include <minipal/cpuid.h>
#endif

int64_t SatoriUtil::s_timeStampFrequency = 0;
int64_t SatoriUtil::s_osTimeStampFrequency = 0;

// a counter that ticks slower than this is too coarse for the intervals that we measure.
static const int64_t MIN_USABLE_FREQUENCY = 1000 * 1000;

// a HW read that costs more than this is trapped, emulated or otherwise not what we are after.
static const int64_t MAX_READ_NSEC = 200;

// calibration reads per one check of the OS timer, so that its cost does not hide the cost of ours.
static const int READ_BATCH = 64;

// a measurement is inconclusive only if something interfered, so a few tries are plenty.
static const int MAX_CALIBRATION_ATTEMPTS = 4;

#if defined(HOST_X86) || defined(HOST_AMD64)

// The TSC is only useful to us if it ticks at a constant rate regardless of the
// core frequency and power state.
static bool HasConstantRateTimeStamp()
{
    int regs[4];

    __cpuid(regs, (int)0x80000000);
    if ((uint32_t)regs[0] < 0x80000007)
    {
        return false;
    }

    __cpuid(regs, (int)0x80000007);
    const int INVARIANT_TSC = 1 << 8;
    return (regs[3] & INVARIANT_TSC) != 0;
}

#elif defined(HOST_ARM64)

static bool HasConstantRateTimeStamp()
{
    // the counter is required to run at a constant rate.
    return true;
}

#else

static bool HasConstantRateTimeStamp()
{
    return false;
}

#endif

// keeps the calibration reads from being optimized away.
static volatile int64_t s_readSink;

//
// TEMPORARY INSTRUMENTATION - remove before merging.
//
// Records how calibration went so that a full test pass can be surveyed for how often
// the hardware counter gets picked, on which attempt, and how much cheaper reading it
// actually is. Writing is opt-in via the SATORI_TIMER_LOG environment variable, so an
// uninstrumented run behaves exactly as before.
//
#define SATORI_TIMER_INSTRUMENTATION 1

#ifdef SATORI_TIMER_INSTRUMENTATION
#include <stdio.h>
#include <stdlib.h>

// why a calibration attempt did not produce a usable frequency
enum CalibrationOutcome
{
    CALIB_OK = 0,
    CALIB_INTERRUPTED = 1,   // overshot the window, something descheduled us
    CALIB_READ_TOO_SLOW = 2, // the read is trapped or emulated
    CALIB_FREQ_TOO_LOW = 3,  // counter too coarse for the intervals we measure
};

static const char* const s_outcomeNames[] = { "ok", "interrupted", "read_too_slow", "freq_too_low" };

static int s_calibOutcomes[MAX_CALIBRATION_ATTEMPTS];
static int s_calibAttemptsMade;
static int s_calibSucceededOnAttempt = -1; // 1-based, -1 if never
static int64_t s_hwReadPicos;              // cost of one inline counter read
static int64_t s_osReadPicos;              // cost of one OS timer read
#endif // SATORI_TIMER_INSTRUMENTATION

// Measures the counter rate and checks that reading it is actually cheap.
// Returns 0 if the measurement was inconclusive.
int64_t SatoriUtil::MeasureTimeStampFrequency()
{
    // ~100 usec gets the rate within ~0.05%, which is far more than deadlines need.
    int64_t window = s_osTimeStampFrequency / 10000;

    // Read the OS timer on the outside of the counter at both ends, so that the interval
    // attributed to the counter is contained in the interval measured with the OS timer.
    // Bracketing it the other way round makes the counter's interval the larger of the
    // two and systematically over-reports its rate - measured at +0.09% typical and up
    // to +5% when the calibration is preempted, always in the same direction.
    int64_t osStart = minipal_hires_ticks();
    int64_t hwStart = ReadHwTimeStamp();
    int64_t reads = 0;
    int64_t sink = 0;
    do
    {
        for (int i = 0; i < READ_BATCH; i++)
        {
            sink += ReadHwTimeStamp();
        }
        reads += READ_BATCH;
    } while (minipal_hires_ticks() - osStart < window);

    int64_t hwEnd = ReadHwTimeStamp();
    int64_t osNow = minipal_hires_ticks();
    s_readSink = sink;

    int64_t hwElapsed = hwEnd - hwStart;
    int64_t osElapsed = osNow - osStart;

#ifdef SATORI_TIMER_INSTRUMENTATION
    // picoseconds per read, so that sub-nanosecond reads are still distinguishable
    s_hwReadPicos = osElapsed * 1000000000 / s_osTimeStampFrequency * 1000 / reads;
    int outcomeIndex = s_calibAttemptsMade < MAX_CALIBRATION_ATTEMPTS ? s_calibAttemptsMade : MAX_CALIBRATION_ATTEMPTS - 1;
    s_calibAttemptsMade++;
#define RECORD_OUTCOME(o) (s_calibOutcomes[outcomeIndex] = (o))
#else
#define RECORD_OUTCOME(o) ((void)0)
#endif

    // Overshooting the window by this much means we were interrupted, so the numbers
    // do not describe this machine. Bounding it also keeps the math below in range.
    if (osElapsed > window * 4)
    {
        RECORD_OUTCOME(CALIB_INTERRUPTED);
        return 0;
    }

    if (osElapsed * 1000000000 / s_osTimeStampFrequency / reads > MAX_READ_NSEC)
    {
        RECORD_OUTCOME(CALIB_READ_TOO_SLOW);
        return 0;
    }

    int64_t frequency = hwElapsed * s_osTimeStampFrequency / osElapsed;
    if (frequency < MIN_USABLE_FREQUENCY)
    {
        RECORD_OUTCOME(CALIB_FREQ_TOO_LOW);
        return 0;
    }

    RECORD_OUTCOME(CALIB_OK);
    return frequency;
#undef RECORD_OUTCOME
}

#ifdef SATORI_TIMER_INSTRUMENTATION

// Same shape as the loop above, but reading the OS timer, so the two costs are
// measured the same way and can be compared directly.
static int64_t MeasureOsReadPicos(int64_t osFrequency)
{
    int64_t window = osFrequency / 10000;
    int64_t osStart = minipal_hires_ticks();
    int64_t osNow;
    int64_t reads = 0;
    int64_t sink = 0;
    do
    {
        for (int i = 0; i < READ_BATCH; i++)
        {
            sink += minipal_hires_ticks();
        }
        reads += READ_BATCH;
    } while ((osNow = minipal_hires_ticks()) - osStart < window);

    s_readSink = sink;
    return (osNow - osStart) * 1000000000 / osFrequency * 1000 / reads;
}

static void LogCalibration(int64_t chosenFrequency, int64_t osFrequency, bool constantRate)
{
    const char* path = getenv("SATORI_TIMER_LOG");
    if (path == nullptr || path[0] == '\0')
    {
        return;
    }

    s_osReadPicos = MeasureOsReadPicos(osFrequency);

    char attempts[128];
    int n = 0;
    attempts[0] = '\0';
    for (int i = 0; i < s_calibAttemptsMade && i < MAX_CALIBRATION_ATTEMPTS; i++)
    {
        n += snprintf(attempts + n, sizeof(attempts) - n, "%s%s",
            i == 0 ? "" : ",", s_outcomeNames[s_calibOutcomes[i]]);
    }

    // one short line, written in a single call so concurrent processes do not interleave
    FILE* f = fopen(path, "a");
    if (f != nullptr)
    {
        fprintf(f,
            "hw=%d attempt=%d attempts_made=%d outcomes=%s hw_ps=%lld os_ps=%lld "
            "speedup=%.2f hw_freq=%lld os_freq=%lld constant_rate=%d\n",
            chosenFrequency != 0 ? 1 : 0,
            s_calibSucceededOnAttempt,
            s_calibAttemptsMade,
            attempts[0] ? attempts : "none",
            (long long)s_hwReadPicos,
            (long long)s_osReadPicos,
            s_hwReadPicos > 0 ? (double)s_osReadPicos / (double)s_hwReadPicos : 0.0,
            (long long)chosenFrequency,
            (long long)osFrequency,
            constantRate ? 1 : 0);
        fclose(f);
    }
}

#endif // SATORI_TIMER_INSTRUMENTATION

void SatoriUtil::Initialize()
{
    s_osTimeStampFrequency = minipal_hires_tick_frequency();

    // The choice is made once and for all - switching timers later would leave
    // deadlines that were taken on one of them to be checked against the other.
    bool constantRate = HasConstantRateTimeStamp();
    if (constantRate)
    {
        for (int i = 0; i < MAX_CALIBRATION_ATTEMPTS; i++)
        {
            s_timeStampFrequency = MeasureTimeStampFrequency();
            if (s_timeStampFrequency != 0)
            {
#ifdef SATORI_TIMER_INSTRUMENTATION
                s_calibSucceededOnAttempt = i + 1;
#endif
                break;
            }
        }
    }

#ifdef SATORI_TIMER_INSTRUMENTATION
    LogCalibration(s_timeStampFrequency, s_osTimeStampFrequency, constantRate);
#endif
}

int64_t SatoriUtil::GetTimeStampSlow()
{
    return minipal_hires_ticks();
}

