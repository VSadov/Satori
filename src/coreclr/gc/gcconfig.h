// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef __GCCONFIG_H__
#define __GCCONFIG_H__

// gcconfig.h - GC configuration management and retrieval.
//
// This file and the GCConfig class are designed to be the primary entry point
// for querying configuration information from within the GC.

// GCConfigStringHolder is a wrapper around a configuration string obtained
// from the EE. Such strings must be disposed using GCToEEInterface::FreeStringConfigValue,
// so this class ensures that is done correctly.
//
// The name is unfortunately a little long, but "ConfigStringHolder" is already taken by the
// EE's config mechanism.
class GCConfigStringHolder
{
private:
    const char* m_str;

public:
    // Constructs a new GCConfigStringHolder around a string obtained from
    // GCToEEInterface::GetStringConfigValue.
    explicit GCConfigStringHolder(const char* str)
      : m_str(str) {}

    // No copy operators - this type cannot be copied.
    GCConfigStringHolder(const GCConfigStringHolder&) = delete;
    GCConfigStringHolder& operator=(const GCConfigStringHolder&) = delete;

    // This type is returned by-value by string config functions, so it
    // requires a move constructor.
    GCConfigStringHolder(GCConfigStringHolder&&) = default;

    // Frees a string config value by delegating to GCToEEInterface::FreeStringConfigValue.
    ~GCConfigStringHolder()
    {
        if (m_str)
        {
            GCToEEInterface::FreeStringConfigValue(m_str);
        }

        m_str = nullptr;
    }

    // Retrieves the wrapped config string.
    const char* Get() const { return m_str; }
};

// Note that the configs starting BGCFLTuningEnabled ending BGCG2RatioStep are for BGC servo
// tuning which is currently an experimental feature which is why I'm not putting any of these
// in clrconfigvalues.h (yet).
// The only public facing configs are BGCFLTuningEnabled and BGCMemGoal.
// Currently the set point is the BGCMemGoal you specify, which indicates what physical memory
// load you'd like GC to maintain. And BGCFLTuningEnabled is to enable/disable this tuning.
//
// The value for BGCG2RatioStep, BGCFLkd and BGCFLSmoothFactor and BGCFLff will be divided by 100.
// The value for BGCMLkp and BGCMLki will be divided by 1000.
// The value for BGCFLkp and BGCFLki will be divided by 1000000.

// Each one of these keys produces a method on GCConfig with the name "Get{name}", where {name}
// is the first parameter of the *_CONFIG macros below.
#define GC_CONFIGURATION_KEYS \
    BOOL_CONFIG  (ServerGC,                  "gcServer",                  "System.GC.Server",                  false,              "Whether we should be using Server GC")                                                   \
    BOOL_CONFIG  (ConcurrentGC,              "gcConcurrent",              "System.GC.Concurrent",              true,               "Whether we should be using Concurrent GC")                                                \
    BOOL_CONFIG  (ConservativeGC,            "gcConservative",            NULL,                                false,              "Enables/Disables conservative GC")                                                       \
    BOOL_CONFIG  (ForceCompact,              "gcForceCompact",            NULL,                                false,              "When set to true, always do compacting GC")                                              \
    BOOL_CONFIG  (RetainVM,                  "GCRetainVM",                "System.GC.RetainVM",                false,              "When set we put the segments that should be deleted on a standby list (instead of "       \
                                                                                                                                          "releasing them back to the OS) which will be considered to satisfy new segment requests" \
                                                                                                                                          " (note that the same thing can be specified via API which is the supported way)")        \
    BOOL_CONFIG  (BreakOnOOM,                "GCBreakOnOOM",              NULL,                                false,              "Does a DebugBreak at the soonest time we detect an OOM")                                 \
    BOOL_CONFIG  (NoAffinitize,              "GCNoAffinitize",            "System.GC.NoAffinitize",            false,              "If set, do not affinitize server GC threads")                                             \
    BOOL_CONFIG  (LogEnabled,                "GCLogEnabled",              NULL,                                false,              "Specifies if you want to turn on logging in GC")                                         \
    BOOL_CONFIG  (ConfigLogEnabled,          "GCConfigLogEnabled",        NULL,                                false,              "Specifies the name of the GC config log file")                                           \
    BOOL_CONFIG  (GCNumaAware,               "GCNumaAware",               NULL,                                true,               "Enables numa allocations in the GC")                                                     \
    BOOL_CONFIG  (GCCpuGroup,                "GCCpuGroup",                "System.GC.CpuGroup",                false,              "Enables CPU groups in the GC")                                                            \
    BOOL_CONFIG  (GCLargePages,              "GCLargePages",              "System.GC.LargePages",              false,              "Enables using Large Pages in the GC")                                                     \
    INT_CONFIG   (HeapVerifyLevel,           "HeapVerify",                NULL,                                HEAPVERIFY_NONE,    "When set verifies the integrity of the managed heap on entry and exit of each GC")       \
    INT_CONFIG   (LOHCompactionMode,         "GCLOHCompact",              NULL,                                0,                  "Specifies the LOH compaction mode")                                                      \
    INT_CONFIG   (LOHThreshold,              "GCLOHThreshold",            "System.GC.LOHThreshold",            LARGE_OBJECT_SIZE,  "Specifies the size that will make objects go on LOH")                                    \
    INT_CONFIG   (BGCSpinCount,              "BGCSpinCount",              NULL,                                140,                "Specifies the bgc spin count")                                                           \
    INT_CONFIG   (BGCSpin,                   "BGCSpin",                   NULL,                                2,                  "Specifies the bgc spin time")                                                            \
    INT_CONFIG   (HeapCount,                 "GCHeapCount",               "System.GC.HeapCount",               0,                  "Specifies the number of server GC heaps")                                                 \
    INT_CONFIG   (MaxHeapCount,              "GCMaxHeapCount",            "System.GC.MaxHeapCount",            0,                  "Specifies the max number of server GC heaps to adjust to")                                                 \
    INT_CONFIG   (Gen0Size,                  "GCgen0size",                NULL,                                0,                  "Specifies the smallest gen0 budget")                                                     \
    INT_CONFIG   (SegmentSize,               "GCSegmentSize",             NULL,                                0,                  "Specifies the managed heap segment size")                                                \
    INT_CONFIG   (LatencyMode,               "GCLatencyMode",             NULL,                                -1,                 "Specifies the GC latency mode - batch, interactive or low latency (note that the same "   \
                                                                                                                                           "thing can be specified via API which is the supported way")                             \
    INT_CONFIG   (LatencyLevel,              "GCLatencyLevel",            NULL,                                1,                  "Specifies the GC latency level that you want to optimize for. Must be a number from 0 "  \
                                                                                                                                          "to 3. See documentation for more details on each level.")                                \
    INT_CONFIG   (LogFileSize,               "GCLogFileSize",             NULL,                                0,                  "Specifies the GC log file size")                                                         \
    INT_CONFIG   (CompactRatio,              "GCCompactRatio",            NULL,                                0,                  "Specifies the ratio compacting GCs vs sweeping")                                         \
    INT_CONFIG   (GCHeapAffinitizeMask,      "GCHeapAffinitizeMask",      "System.GC.HeapAffinitizeMask",      0,                  "Specifies processor mask for Server GC threads")                                          \
    STRING_CONFIG(GCHeapAffinitizeRanges,    "GCHeapAffinitizeRanges",    "System.GC.HeapAffinitizeRanges",                        "Specifies list of processors for Server GC threads. The format is a comma separated "     \
                                                                                                                                          "list of processor numbers or ranges of processor numbers. On Windows, each entry is "    \
                                                                                                                                          "prefixed by the CPU group number. Example: Unix - 1,3,5,7-9,12, Windows - 0:1,1:7-9")    \
    INT_CONFIG   (GCHighMemPercent,          "GCHighMemPercent",          "System.GC.HighMemoryPercent",       0,                  "The percent for GC to consider as high memory")                                           \
    INT_CONFIG   (GCProvModeStress,          "GCProvModeStress",          NULL,                                0,                  "Stress the provisional modes")                                                           \
    INT_CONFIG   (GCGen0MaxBudget,           "GCGen0MaxBudget",           NULL,                                0,                  "Specifies the largest gen0 allocation budget")                                           \
    INT_CONFIG   (GCGen1MaxBudget,           "GCGen1MaxBudget",           NULL,                                0,                  "Specifies the largest gen1 allocation budget")                                           \
    INT_CONFIG   (GCLowSkipRatio,            "GCLowSkipRatio",            NULL,                                30,                 "Specifies the low generation skip ratio")                                                \
    INT_CONFIG   (GCHeapHardLimit,           "GCHeapHardLimit",           "System.GC.HeapHardLimit",           0,                  "Specifies a hard limit for the GC heap")                                                 \
    INT_CONFIG   (GCHeapHardLimitPercent,    "GCHeapHardLimitPercent",    "System.GC.HeapHardLimitPercent",    0,                  "Specifies the GC heap usage as a percentage of the total memory")                        \
    INT_CONFIG   (GCTotalPhysicalMemory,     "GCTotalPhysicalMemory",     NULL,                                0,                  "Specifies what the GC should consider to be total physical memory")                      \
    INT_CONFIG   (GCRegionRange,             "GCRegionRange",             NULL,                                0,                  "Specifies the range for the GC heap")                                                    \
    INT_CONFIG   (GCRegionSize,              "GCRegionSize",              NULL,                                0,                  "Specifies the size for a basic GC region")                                               \
    INT_CONFIG   (GCEnableSpecialRegions,    "GCEnableSpecialRegions",    NULL,                                0,                  "Specifies to enable special handling some regions like SIP")                             \
    STRING_CONFIG(LogFile,                   "GCLogFile",                 NULL,                                                    "Specifies the name of the GC log file")                                                  \
    STRING_CONFIG(ConfigLogFile,             "GCConfigLogFile",           NULL,                                                    "Specifies the name of the GC config log file")                                           \
    INT_CONFIG   (BGCFLTuningEnabled,        "BGCFLTuningEnabled",        NULL,                                0,                  "Enables FL tuning")                                                                      \
    INT_CONFIG   (BGCMemGoal,                "BGCMemGoal",                NULL,                                75,                 "Specifies the physical memory load goal")                                                \
    INT_CONFIG   (BGCMemGoalSlack,           "BGCMemGoalSlack",           NULL,                                10,                 "Specifies comfort zone of going above goal")                                             \
    INT_CONFIG   (BGCFLSweepGoal,            "BGCFLSweepGoal",            NULL,                                0,                  "Specifies the gen2 sweep FL ratio goal")                                                 \
    INT_CONFIG   (BGCFLSweepGoalLOH,         "BGCFLSweepGoalLOH",         NULL,                                0,                  "Specifies the LOH sweep FL ratio goal")                                                  \
    INT_CONFIG   (BGCFLkp,                   "BGCFLkp",                   NULL,                                6000,               "Specifies kp for above goal tuning")                                                     \
    INT_CONFIG   (BGCFLki,                   "BGCFLki",                   NULL,                                1000,               "Specifies ki for above goal tuning")                                                     \
    INT_CONFIG   (BGCFLkd,                   "BGCFLkd",                   NULL,                                11,                 "Specifies kd for above goal tuning")                                                     \
    INT_CONFIG   (BGCFLff,                   "BGCFLff",                   NULL,                                100,                "Specifies ff ratio")                                                                     \
    INT_CONFIG   (BGCFLSmoothFactor,         "BGCFLSmoothFactor",         NULL,                                150,                "Smoothing over these")                                                                   \
    INT_CONFIG   (BGCFLGradualD,             "BGCFLGradualD",             NULL,                                0,                  "Enable gradual D instead of cutting off at the value")                                   \
    INT_CONFIG   (BGCMLkp,                   "BGCMLkp",                   NULL,                                1000,               "Specifies kp for ML tuning")                                                             \
    INT_CONFIG   (BGCMLki,                   "BGCMLki",                   NULL,                                16,                 "Specifies ki for ML tuning")                                                             \
    INT_CONFIG   (BGCFLEnableKi,             "BGCFLEnableKi",             NULL,                                1,                  "Enables ki for above goal tuning")                                                       \
    INT_CONFIG   (BGCFLEnableKd,             "BGCFLEnableKd",             NULL,                                0,                  "Enables kd for above goal tuning")                                                       \
    INT_CONFIG   (BGCFLEnableSmooth,         "BGCFLEnableSmooth",         NULL,                                0,                  "Enables smoothing")                                                                      \
    INT_CONFIG   (BGCFLEnableTBH,            "BGCFLEnableTBH",            NULL,                                0,                  "Enables TBH")                                                                            \
    INT_CONFIG   (BGCFLEnableFF,             "BGCFLEnableFF",             NULL,                                0,                  "Enables FF")                                                                             \
    INT_CONFIG   (BGCG2RatioStep,            "BGCG2RatioStep",            NULL,                                5,                  "Ratio correction factor for ML loop")                                                    \
    INT_CONFIG   (GCHeapHardLimitSOH,        "GCHeapHardLimitSOH",        "System.GC.HeapHardLimitSOH",        0,                  "Specifies a hard limit for the GC heap SOH")                                             \
    INT_CONFIG   (GCHeapHardLimitLOH,        "GCHeapHardLimitLOH",        "System.GC.HeapHardLimitLOH",        0,                  "Specifies a hard limit for the GC heap LOH")                                             \
    INT_CONFIG   (GCHeapHardLimitPOH,        "GCHeapHardLimitPOH",        "System.GC.HeapHardLimitPOH",        0,                  "Specifies a hard limit for the GC heap POH")                                             \
    INT_CONFIG   (GCHeapHardLimitSOHPercent, "GCHeapHardLimitSOHPercent", "System.GC.HeapHardLimitSOHPercent", 0,                  "Specifies the GC heap SOH usage as a percentage of the total memory")                    \
    INT_CONFIG   (GCHeapHardLimitLOHPercent, "GCHeapHardLimitLOHPercent", "System.GC.HeapHardLimitLOHPercent", 0,                  "Specifies the GC heap LOH usage as a percentage of the total memory")                    \
    INT_CONFIG   (GCHeapHardLimitPOHPercent, "GCHeapHardLimitPOHPercent", "System.GC.HeapHardLimitPOHPercent", 0,                  "Specifies the GC heap POH usage as a percentage of the total memory")                    \
    INT_CONFIG   (GCEnabledInstructionSets,  "GCEnabledInstructionSets",  NULL,                                -1,                 "Specifies whether GC can use AVX2 or AVX512F - 0 for neither, 1 for AVX2, 3 for AVX512F")\
    INT_CONFIG   (GCConserveMem,             "GCConserveMemory",          "System.GC.ConserveMemory",          0,                  "Specifies how hard GC should try to conserve memory - values 0-9")                       \
    INT_CONFIG   (GCWriteBarrier,            "GCWriteBarrier",            NULL,                                0,                  "Specifies whether GC should use more precise but slower write barrier")                  \
    STRING_CONFIG(GCName,                    "GCName",                    "System.GC.Name",                                        "Specifies the name of the standalone GC implementation.")                                \
    STRING_CONFIG(GCPath,                    "GCPath",                    "System.GC.Path",                                        "Specifies the path of the standalone GC implementation.")                                \
    INT_CONFIG   (GCSpinCountUnit,           "GCSpinCountUnit",           NULL,                                0,                  "Specifies the spin count unit used by the GC.")                                          \
    INT_CONFIG   (GCDynamicAdaptationMode,   "GCDynamicAdaptationMode",   "System.GC.DynamicAdaptationMode",   1,                  "Enable the GC to dynamically adapt to application sizes.")                               \
    INT_CONFIG   (GCDTargetTCP,              "GCDTargetTCP",              "System.GC.DTargetTCP",              0,                  "Specifies the target tcp for DATAS")                                                     \
    INT_CONFIG   (GCDBGCRatio,              " GCDBGCRatio",               NULL,                                0,                  "Specifies the ratio of BGC to NGC2 for HC change")                                       \
    BOOL_CONFIG  (GCLogBGCThreadId,          "GCLogBGCThreadId",          NULL,                                false,              "Specifies if BGC ThreadId should be logged")                                             \
    BOOL_CONFIG  (GCCacheSizeFromSysConf,    "GCCacheSizeFromSysConf",    NULL,                                false,              "Specifies using sysconf to retrieve the last level cache size for Unix.")                \
/* FEATURE_SATORI_GC */                                                                                                                                                                                                      \
    BOOL_CONFIG  (RelocatingInGen1,          "gcRelocatingGen1",          NULL,                                true,               "Specifies whether GC can relocate objects in Gen1 GC")                                   \
    BOOL_CONFIG  (RelocatingInGen2,          "gcRelocatingGen2",          NULL,                                true,               "Specifies whether GC can relocate objects in Gen2 GC")                                   \
    INT_CONFIG   (ParallelGC,                "gcParallel",                NULL,                                -1,                 "Specifies max number of addtional GC threads. 0 - no helpers, -1 - default")             \
    BOOL_CONFIG  (Gen0GC,                    "gcGen0",                    NULL,                                true,               "Specifies whether Gen0 GC can be performed")                                             \
    BOOL_CONFIG  (Gen1GC,                    "gcGen1",                    NULL,                                true,               "Specifies whether Gen1 GC can be performed")                                             \
    BOOL_CONFIG  (UseTHP,                    "gcTHP",                     NULL,                                true,               "Specifies whether Transparent Huge Pages can be used. (Linux only)")                     \
    BOOL_CONFIG  (TrimmigGC,                 "gcTrim",                    NULL,                                true,               "Specifies whether background trimming is enabled")                                       \
    BOOL_CONFIG  (PacingGC,                  "gcPace",                    NULL,                                true,               "Specifies whether allocation pacing is enabled")                                         \
    INT_CONFIG   (GCRate,                    "gcRate",                    NULL,                                -1,                 "Specifies soft min limit for time between GCs in milliseconds. -1 - default")            \
    INT_CONFIG   (GCSpin,                    "gcSpin",                    NULL,                                -1,                 "Spin")                                                                                   \
    INT_CONFIG   (Gen2Target,                "gcGen2Target",              NULL,                                -1,                 "Specifies target for Gen2 GC (in terms of % of the last known size)")                    \
    INT_CONFIG   (Gen1Target,                "gcGen1Target",              NULL,                                -1,                 "Specifies target for Gen1 GC (in terms of % of the last known size)")                    \

// This class is responsible for retreiving configuration information
// for how the GC should operate.
class GCConfig
{
#define BOOL_CONFIG(name, unused_private_key, unused_public_key, unused_default, unused_doc) \
  public: static bool Get##name();                                \
  public: static bool Get##name(bool defaultValue);               \
  public: static void Set##name(bool value);                      \
  private: static bool s_##name;                                  \
  private: static bool s_##name##Provided;                        \
  private: static bool s_Updated##name;
  
#define INT_CONFIG(name, unused_private_key, unused_public_key, unused_default, unused_doc) \
  public: static int64_t Get##name();                            \
  public: static int64_t Get##name(int64_t defaultValue);        \
  public: static void Set##name(int64_t value);                  \
  private: static int64_t s_##name;                              \
  private: static bool s_##name##Provided;                       \
  private: static int64_t s_Updated##name;                       \

#define STRING_CONFIG(name, unused_private_key, unused_public_key, unused_doc) \
  public: static GCConfigStringHolder Get##name();

GC_CONFIGURATION_KEYS

#undef BOOL_CONFIG
#undef INT_CONFIG
#undef STRING_CONFIG

public:

  static void RefreshHeapHardLimitSettings();

  static void EnumerateConfigurationValues(void* context, ConfigurationValueFunc configurationValueFunc);

// Flags that may inhabit the number returned for the HeapVerifyLevel config option.
// Keep this in sync with vm\eeconfig.h if this ever changes.
enum HeapVerifyFlags {
    HEAPVERIFY_NONE             = 0,
    HEAPVERIFY_GC               = 1,   // Verify the heap at beginning and end of GC
    HEAPVERIFY_BARRIERCHECK     = 2,   // Verify the assignment operations correctly went through write barrier code
    HEAPVERIFY_SYNCBLK          = 4,   // Verify sync block scanning

    // the following options can be used to mitigate some of the overhead introduced
    // by heap verification.  some options might cause heap verifiction to be less
    // effective depending on the scenario.

    HEAPVERIFY_NO_RANGE_CHECKS  = 0x10,   // Excludes checking if an OBJECTREF is within the bounds of the managed heap
    HEAPVERIFY_NO_MEM_FILL      = 0x20,   // Excludes filling unused segment portions with fill pattern
    HEAPVERIFY_POST_GC_ONLY     = 0x40,   // Performs heap verification post-GCs only (instead of before and after each GC)
    HEAPVERIFY_DEEP_ON_COMPACT  = 0x80    // Performs deep object verfication only on compacting GCs.
};

enum WriteBarrierFlavor
{
    WRITE_BARRIER_DEFAULT = 0,
    WRITE_BARRIER_REGION_BIT = 1,
    WRITE_BARRIER_REGION_BYTE = 2,
    WRITE_BARRIER_SERVER = 3,
};

// Initializes the GCConfig subsystem. Must be called before accessing any
// configuration information.
static void Initialize();

};

bool ParseGCHeapAffinitizeRanges(const char* cpu_index_ranges, AffinitySet* config_affinity_set, uintptr_t& config_affinity_mask);

#endif // __GCCONFIG_H__
