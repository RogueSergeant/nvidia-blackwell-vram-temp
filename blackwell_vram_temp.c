/*
 * blackwell_vram_temp.c — read RTX 5090 (GB202 / Blackwell) GDDR7 memory
 * temperature via the RM User Shared Data (RUSD) buffer. Open kernel module,
 * driver 610.43.02.
 *
 * SELF-CONTAINED build (no NVIDIA SDK, no -I, no /tmp/ogkm clone):
 *
 *     gcc -O2 -Wall blackwell_vram_temp.c -o blackwell_vram_temp
 *
 * This is a vendored single-file version of the reference gddr7_rusd.c (which
 * needed -I/tmp/ogkm/src/common/sdk/nvidia/inc). The minimal subset of the
 * NVIDIA 610.43.02 SDK headers that the reference pulled in has been inlined
 * below, VERBATIM where it affects ABI/layout:
 *
 *   - Nv base types (nvtypes.h): NvU8/16/32/64, NvS32, NvV32, NvHandle, NvBool,
 *     NvP64, plus NV_ALIGN_BYTES / NV_DECLARE_ALIGNED.
 *   - Fixed-point temperature type (nvfixedtypes.h): NvSFXP24_8 / NvTemp.
 *   - NVBIT (nvmisc.h).
 *   - The two NV2080 ECC ctrl param structs (ctrl2080ecc.h) that sit *before*
 *     temperatures[] in NV00DE_SHARED_DATA and therefore set its byte offset.
 *   - The full NV00DE_SHARED_DATA layout, RUSD_* sub-structs, RUSD_TEMPERATURE,
 *     the RUSD_TEMPERATURE_TYPE_* enum, the seqlock validity macro
 *     RUSD_SEQ_DATA_VALID, RUSD_TIMESTAMP_INVALID, the poll-mask bits, and
 *     NV00DE_ALLOC_PARAMETERS (class/cl00de.h).
 *
 * Because the full struct is reproduced (not hand-offset), the compiler still
 * computes the byte-exact offset of temperatures[RUSD_TEMPERATURE_TYPE_MEMORY]
 * for us. _Static_asserts below pin the offset (2336) and struct size (3152)
 * observed from the real 610.43.02 headers, so any accidental layout drift in
 * the vendored copy fails the build instead of reading the wrong bytes.
 *
 * Behavior is identical to the reference: alloc client->device->subdevice,
 * alloc NV00DE with POLL_THERMAL, REQUEST_DATA_POLL, MAP_MEMORY + mmap,
 * seqlock-read temperatures[MEMORY], convert raw NvSFXP24_8 / 256.0 -> Celsius.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* =====================================================================
 * Vendored NVIDIA SDK subset (610.43.02). 64-bit Linux / GCC variant.
 * ===================================================================== */

/* ---- nvtypes.h: base integer types (NV_64_BITS, uses C99 stdint) ---- */
typedef uint8_t            NvU8;     /* 0 to 255                         */
typedef uint16_t           NvU16;    /* 0 to 65535                       */
typedef int32_t            NvS32;    /* -2147483648 to 2147483647        */
typedef uint32_t           NvV32;    /* enumerated or multiple fields    */
typedef uint32_t           NvU32;    /* 0 to 4294967295                  */
typedef uint64_t           NvU64;    /* 0 to 18446744073709551615        */
typedef void*              NvP64;    /* 64 bit void pointer (NV_64_BITS) */
typedef NvU32              NvHandle;
typedef NvU8               NvBool;

/* ---- nvtypes.h: alignment macros (GNUC branch) ---- */
#define NV_ALIGN_BYTES(size) __attribute__ ((aligned (size)))
#define NV_DECLARE_ALIGNED(TYPE_VAR, ALIGN) TYPE_VAR __attribute__ ((aligned (ALIGN)))

/* ---- nvmisc.h ---- */
#define NVBIT(b)           (1U << (b))

/* ---- nvfixedtypes.h: signed fixed-point temperature type ---- */
typedef NvS32              NvSFXP32;
typedef NvSFXP32           NvSFXP24_8;
typedef NvSFXP24_8         NvTemp;

/* ---- nvtypes.h: 64-bit max ---- */
#define NV_U64_MAX         (~(0ULL))

/* =====================================================================
 * ctrl/ctrl2080/ctrl2080ecc.h — the two ECC ctrl param structs embedded
 * (via RUSD_MEM_ECC) ahead of temperatures[] in NV00DE_SHARED_DATA. Their
 * sizes contribute to the offset of temperatures[], so vendor verbatim.
 * ===================================================================== */
typedef struct NV2080_CTRL_ECC_GET_CLIENT_EXPOSED_COUNTERS_PARAMS {
    NV_DECLARE_ALIGNED(NvU64 sramParityUncorrectedUnique, 8);
    NV_DECLARE_ALIGNED(NvU64 sramSecDedUncorrectedUnique, 8);
    NV_DECLARE_ALIGNED(NvU64 sramCorrectedUnique, 8);
    NV_DECLARE_ALIGNED(NvU64 dramUncorrectedTotal, 8);
    NV_DECLARE_ALIGNED(NvU64 dramCorrectedTotal, 8);

    NvU32  lastClearedTimestamp;

    NV_DECLARE_ALIGNED(NvU64 sramBucketL2, 8);
    NV_DECLARE_ALIGNED(NvU64 sramBucketSM, 8);
    NV_DECLARE_ALIGNED(NvU64 sramBucketPcie, 8);
    NV_DECLARE_ALIGNED(NvU64 sramBucketFirmware, 8);
    NV_DECLARE_ALIGNED(NvU64 sramBucketOther, 8);

    NvBool sramErrorThresholdExceeded;
} NV2080_CTRL_ECC_GET_CLIENT_EXPOSED_COUNTERS_PARAMS;

typedef struct NV2080_CTRL_ECC_GET_VOLATILE_COUNTS_PARAMS {
    NV_DECLARE_ALIGNED(NvU64 sramCorUni, 8);
    NV_DECLARE_ALIGNED(NvU64 sramUncParityUni, 8);
    NV_DECLARE_ALIGNED(NvU64 sramUncSecDedUni, 8);
    NV_DECLARE_ALIGNED(NvU64 dramCorTot, 8);
    NV_DECLARE_ALIGNED(NvU64 dramUncTot, 8);
} NV2080_CTRL_ECC_GET_VOLATILE_COUNTS_PARAMS;

/* =====================================================================
 * class/cl00de.h — RM_USER_SHARED_DATA (RUSD). Vendored verbatim layout so
 * the compiler computes the byte-exact offset of temperatures[MEMORY].
 * ===================================================================== */

#define RM_USER_SHARED_DATA              (0x000000de)

#define RUSD_TIMESTAMP_WRITE_IN_PROGRESS (NV_U64_MAX)
#define RUSD_TIMESTAMP_INVALID           0

#define RUSD_SEQ_START (0xFF00000000000000LLU)

#define RUSD_SEQ_DATA_VALID(x) \
    ((((x) < RUSD_SEQ_START)  && ((x) != RUSD_TIMESTAMP_INVALID)) || \
     (((x) >= RUSD_SEQ_START) && (((x) & 0x1LLU) == 0x0LLU)))

enum {
    RUSD_CLK_THROTTLE_REASON_GPU_IDLE                         = NVBIT(0),
    RUSD_CLK_THROTTLE_REASON_APPLICATION_CLOCK_SETTING        = NVBIT(1),
    RUSD_CLK_THROTTLE_REASON_SW_POWER_CAP                     = NVBIT(2),
    RUSD_CLK_THROTTLE_REASON_HW_SLOWDOWN                      = NVBIT(3),
    RUSD_CLK_THROTTLE_REASON_SYNC_BOOST                       = NVBIT(4),
    RUSD_CLK_THROTTLE_REASON_SW_THERMAL_SLOWDOWN              = NVBIT(5),
    RUSD_CLK_THROTTLE_REASON_HW_THERMAL_SLOWDOWN              = NVBIT(6),
    RUSD_CLK_THROTTLE_REASON_HW_POWER_BRAKES_SLOWDOWN         = NVBIT(7),
    RUSD_CLK_THROTTLE_REASON_DISPLAY_CLOCK_SETTING            = NVBIT(8),
};

typedef struct RUSD_BAR1_MEMORY_INFO {
    volatile NvU64 lastModifiedTimestamp;
    NvU32 bar1Size;
    NvU32 bar1AvailSize;
} RUSD_BAR1_MEMORY_INFO;

typedef struct RUSD_PMA_MEMORY_INFO {
    volatile NvU64 lastModifiedTimestamp;
    NvU64 totalPmaMemory;
    NvU64 freePmaMemory;
} RUSD_PMA_MEMORY_INFO;

enum {
    RUSD_CLK_PUBLIC_DOMAIN_GRAPHICS = 0,
    RUSD_CLK_PUBLIC_DOMAIN_MEMORY,
    RUSD_CLK_PUBLIC_DOMAIN_VIDEO,
    RUSD_CLK_PUBLIC_DOMAIN_SM,
    RUSD_CLK_PUBLIC_DOMAIN_MAX_TYPE,
};

typedef struct RUSD_CLK_PUBLIC_DOMAIN_INFO {
    NvU32 targetClkMHz;
    NvU32 actualClkKHz;
} RUSD_CLK_PUBLIC_DOMAIN_INFO;

typedef struct RUSD_CLK_PUBLIC_DOMAIN_INFOS {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_CLK_PUBLIC_DOMAIN_INFO info[RUSD_CLK_PUBLIC_DOMAIN_MAX_TYPE];
} RUSD_CLK_PUBLIC_DOMAIN_INFOS;

typedef struct RUSD_ENG_UTILIZATION {
    NvU32 clkPercentBusy;
    NvU32 samplingPeriodUs;
} RUSD_ENG_UTILIZATION;

#define RUSD_ENG_UTILIZATION_VID_ENG_NVENC 0
#define RUSD_ENG_UTILIZATION_VID_ENG_NVDEC 1
#define RUSD_ENG_UTILIZATION_VID_ENG_NVJPG 2
#define RUSD_ENG_UTILIZATION_VID_ENG_NVOFA 3
#define RUSD_ENG_UTILIZATION_COUNT 4

typedef struct RUSD_PERF_DEVICE_UTILIZATION_INFO {
    NvU32 gpuPercentBusy;
    NvU32 memoryPercentBusy;
    RUSD_ENG_UTILIZATION engUtil[RUSD_ENG_UTILIZATION_COUNT];
} RUSD_PERF_DEVICE_UTILIZATION_INFO;

typedef struct RUSD_PERF_DEVICE_UTILIZATION {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_PERF_DEVICE_UTILIZATION_INFO info;
} RUSD_PERF_DEVICE_UTILIZATION;

typedef struct RUSD_PERF_CURRENT_PSTATE {
    volatile NvU64 lastModifiedTimestamp;
    NvU32 currentPstate;
} RUSD_PERF_CURRENT_PSTATE;

#define RUSD_CLK_VIOLATION_NUM 32

#define RUSD_PERF_POINT_MAX_CLOCK                     0
#define RUSD_PERF_POINT_TURBO_BOOST                   1
#define RUSD_PERF_POINT_3D_BOOST                      2
#define RUSD_PERF_POINT_RATED_TDP                     3
#define RUSD_PERF_POINT_MAX_CUSTOMER_BOOST            4
#define RUSD_PERF_POINT_DISPLAY_CLOCK_INTERSECT       5
#define RUSD_PERF_POINT_NUM                           6

typedef struct RUSD_CLK_VIOLATION_STATUS {
    NvU32 perfPointMask;
    NvU64 timeNs[RUSD_PERF_POINT_NUM];
} RUSD_CLK_VIOLATION_STATUS;

typedef struct RUSD_CLK_THROTTLE_INFO {
    volatile NvU64 lastModifiedTimestamp;
    NvU32 reasonMask;

    NvU64 referenceTimeNs;
    NvU32 supportedViolationTimeMask;
    RUSD_CLK_VIOLATION_STATUS violation[RUSD_CLK_VIOLATION_NUM];
    RUSD_CLK_VIOLATION_STATUS globalViolation;
} RUSD_CLK_THROTTLE_INFO;

typedef struct RUSD_CLK_THROTTLE_INFO RUSD_CLK_THROTTLE_REASON;

typedef struct RUSD_MEM_ERROR_COUNTS {
    NvU64 correctedVolatile;
    NvU64 correctedAggregate;
    NvU64 uncorrectedVolatile;
    NvU64 uncorrectedAggregate;
} RUSD_MEM_ERROR_COUNTS;

#define RUSD_MEMORY_ERROR_TYPE_TOTAL 0
#define RUSD_MEMORY_ERROR_TYPE_DRAM  1
#define RUSD_MEMORY_ERROR_TYPE_SRAM  2
#define RUSD_MEMORY_ERROR_TYPE_COUNT 3

typedef struct RUSD_ECC_COUNTS {
    NV2080_CTRL_ECC_GET_VOLATILE_COUNTS_PARAMS volatileCounts;
    NV2080_CTRL_ECC_GET_CLIENT_EXPOSED_COUNTERS_PARAMS clientExposedCounts;
} RUSD_ECC_INFO;

typedef struct RUSD_MEM_ECC {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_MEM_ERROR_COUNTS count[RUSD_MEMORY_ERROR_TYPE_COUNT];
    RUSD_ECC_INFO info;
} RUSD_MEM_ECC;

typedef struct RUSD_POWER_LIMIT_INFO {
    NvU32 requestedmW;
    NvU32 enforcedmW;
} RUSD_POWER_LIMIT_INFO;

typedef struct RUSD_POWER_LIMITS {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_POWER_LIMIT_INFO info;
} RUSD_POWER_LIMITS;

typedef enum RUSD_TEMPERATURE_SENSOR {
    RUSD_TEMPERATURE_SENSOR_GPU,
    RUSD_TEMPERATURE_SENSOR_MEMORY,
    RUSD_TEMPERATURE_SENSOR_BOARD,
    RUSD_TEMPERATURE_SENSOR_POWER_SUPPLY,
    RUSD_TEMPERATURE_SENSOR_MAX
} RUSD_TEMPERATURE_SENSOR;

typedef enum RUSD_TEMPERATURE_TYPE {
    RUSD_TEMPERATURE_TYPE_GPU,
    RUSD_TEMPERATURE_TYPE_MEMORY,
    RUSD_TEMPERATURE_TYPE_BOARD,
    RUSD_TEMPERATURE_TYPE_POWER_SUPPLY,
    RUSD_TEMPERATURE_TYPE_HBM,
    RUSD_TEMPERATURE_TYPE_MAX
} RUSD_TEMPERATURE_TYPE;

typedef struct RUSD_TEMPERATURE {
    volatile NvU64 lastModifiedTimestamp;
    NvTemp temperature;
} RUSD_TEMPERATURE;

typedef struct RUSD_MEM_ROW_REMAP_INFO {
    NvU32 histogramMax;
    NvU32 histogramHigh;
    NvU32 histogramPartial;
    NvU32 histogramLow;
    NvU32 histogramNone;

    NvU32 correctableRows;
    NvU32 uncorrectableRows;
    NvU32 correctableRowsInactive;
    NvU32 uncorrectableRowsInactive;
    NvBool isPending;
    NvBool hasFailureOccurred;
} RUSD_MEM_ROW_REMAP_INFO;

typedef struct RUSD_MEM_ROW_REMAP {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_MEM_ROW_REMAP_INFO info;
} RUSD_MEM_ROW_REMAP;

typedef struct RUSD_AVG_POWER_INFO {
    NvU32 averageGpuPower;
    NvU32 averageModulePower;
    NvU32 averageMemoryPower;
} RUSD_AVG_POWER_INFO;

typedef struct RUSD_AVG_POWER_USAGE {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_AVG_POWER_INFO info;
} RUSD_AVG_POWER_USAGE;

typedef struct RUSD_INST_POWER_INFO {
    NvU32 instGpuPower;
    NvU32 instModulePower;
    NvU32 instCpuPower;
} RUSD_INST_POWER_INFO;

typedef struct RUSD_INST_POWER_USAGE {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_INST_POWER_INFO info;
} RUSD_INST_POWER_USAGE;

typedef struct RUSD_POWER_POLICY_STATUS_INFO {
    NvU32 tgpmW;
} RUSD_POWER_POLICY_STATUS_INFO;

typedef struct RUSD_POWER_POLICY_STATUS {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_POWER_POLICY_STATUS_INFO info;
} RUSD_POWER_POLICY_STATUS;

#define RUSD_FAN_COOLER_MAX_COOLERS 16U

typedef struct RUSD_FAN_COOLER_INFO {
    NvU32 rpmCurr[RUSD_FAN_COOLER_MAX_COOLERS];
} RUSD_FAN_COOLER_INFO;

typedef struct RUSD_FAN_COOLER_STATUS {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_FAN_COOLER_INFO info;
} RUSD_FAN_COOLER_STATUS;

typedef struct RUSD_SHADOW_ERR_CONT {
    volatile NvU64 lastModifiedTimestamp;
    NvU32 shadowErrContVal;
} RUSD_SHADOW_ERR_CONT;

#define RUSD_BUS_DATA_PCIE_GEN_INFO                              0
#define RUSD_BUS_DATA_PCIE_GPU_LINK_LINECODE_ERRORS              1
#define RUSD_BUS_DATA_PCIE_GPU_LINK_CRC_ERRORS                   2
#define RUSD_BUS_DATA_PCIE_GPU_LINK_NAKS_RECEIVED                3
#define RUSD_BUS_DATA_PCIE_GPU_LINK_FAILED_L0S_EXITS             4
#define RUSD_BUS_DATA_PCIE_GPU_LINK_CORRECTABLE_ERRORS           5
#define RUSD_BUS_DATA_PCIE_GPU_LINK_NONFATAL_ERRORS              6
#define RUSD_BUS_DATA_PCIE_GPU_LINK_FATAL_ERRORS                 7
#define RUSD_BUS_DATA_PCIE_GPU_LINK_UNSUPPORTED_REQUESTS         8
#define RUSD_BUS_DATA_COUNT                                      9

typedef struct RUSD_PCIE_DATA_INFO {
    NvU32 data[RUSD_BUS_DATA_COUNT];
} RUSD_PCIE_DATA_INFO;

typedef struct RUSD_PCIE_DATA {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_PCIE_DATA_INFO info;
} RUSD_PCIE_DATA;

typedef struct RUSD_GR_INFO
{
    volatile NvU64 lastModifiedTimestamp;
    NvBool bCtxswLoggingEnabled;
} RUSD_GR_INFO;

#define RUSD_PROC_UTIL_SAMPLE_COUNT 5

typedef struct {
    NvU32 util;
    NvU32 vgpuScale;
    NvU32 procId;
    NvU32 subProcessID;
} RUSD_CTRL_PERF_GPUMON_ENGINE_UTIL_SAMPLE;

typedef struct  {
    NvU64 timeStamp;
    RUSD_CTRL_PERF_GPUMON_ENGINE_UTIL_SAMPLE fb;
    RUSD_CTRL_PERF_GPUMON_ENGINE_UTIL_SAMPLE gr;
    RUSD_CTRL_PERF_GPUMON_ENGINE_UTIL_SAMPLE nvenc;
    RUSD_CTRL_PERF_GPUMON_ENGINE_UTIL_SAMPLE nvdec;
    RUSD_CTRL_PERF_GPUMON_ENGINE_UTIL_SAMPLE nvjpg;
    RUSD_CTRL_PERF_GPUMON_ENGINE_UTIL_SAMPLE nvofa;
} RUSD_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE;

typedef struct {
    RUSD_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE samples[RUSD_PROC_UTIL_SAMPLE_COUNT];
} RUSD_PROC_UTIL_INFO;

typedef struct {
    volatile NvU64 lastModifiedTimestamp;
    RUSD_PROC_UTIL_INFO info;
} RUSD_PROC_UTIL;

typedef struct NV00DE_SHARED_DATA {
    NV_DECLARE_ALIGNED(RUSD_BAR1_MEMORY_INFO bar1MemoryInfo, 8);

    NV_DECLARE_ALIGNED(RUSD_PMA_MEMORY_INFO pmaMemoryInfo, 8);

    NV_DECLARE_ALIGNED(RUSD_SHADOW_ERR_CONT shadowErrCont, 8);

    NV_DECLARE_ALIGNED(RUSD_GR_INFO grInfo, 8);

    // gpuUpdateUserSharedData is sensitive to these two sections being contiguous

    // POLL_CLOCK
    NV_DECLARE_ALIGNED(RUSD_CLK_PUBLIC_DOMAIN_INFOS clkPublicDomainInfos, 8);

    // POLL_PERF
    NV_DECLARE_ALIGNED(RUSD_CLK_THROTTLE_INFO clkThrottleInfo, 8);

    // POLL_PERF
    NV_DECLARE_ALIGNED(RUSD_PERF_DEVICE_UTILIZATION perfDevUtil, 8);

    // POLL_MEMORY
    NV_DECLARE_ALIGNED(RUSD_MEM_ECC memEcc, 8);

    // POLL_PERF
    NV_DECLARE_ALIGNED(RUSD_PERF_CURRENT_PSTATE perfCurrentPstate, 8);

    // POLL_POWER
    NV_DECLARE_ALIGNED(RUSD_POWER_LIMITS powerLimitGpu, 8);

    // POLL_THERMAL
    NV_DECLARE_ALIGNED(RUSD_TEMPERATURE temperatures[RUSD_TEMPERATURE_TYPE_MAX], 8);

    // POLL_MEMORY
    NV_DECLARE_ALIGNED(RUSD_MEM_ROW_REMAP memRowRemap, 8);

    // POLL_POWER
    NV_DECLARE_ALIGNED(RUSD_AVG_POWER_USAGE avgPowerUsage, 8);

    // POLL_POWER
    NV_DECLARE_ALIGNED(RUSD_INST_POWER_USAGE instPowerUsage, 8);

    // POLL_POWER
    NV_DECLARE_ALIGNED(RUSD_POWER_POLICY_STATUS powerPolicyStatus, 8);

    // POLL_PCI
    NV_DECLARE_ALIGNED(RUSD_PCIE_DATA pciBusData, 8);

    // POLL_FAN
    NV_DECLARE_ALIGNED(RUSD_FAN_COOLER_STATUS fanCoolerStatus, 8);

    // POLL_PROC_UTIL
    NV_DECLARE_ALIGNED(RUSD_PROC_UTIL procUtil, 8);

} NV00DE_SHARED_DATA;

#define NV00DE_RUSD_POLL_CLOCK     0x1
#define NV00DE_RUSD_POLL_PERF      0x2
#define NV00DE_RUSD_POLL_MEMORY    0x4
#define NV00DE_RUSD_POLL_POWER     0x8
#define NV00DE_RUSD_POLL_THERMAL   0x10
#define NV00DE_RUSD_POLL_PCI       0x20
#define NV00DE_RUSD_POLL_FAN       0x40
#define NV00DE_RUSD_POLL_PROC_UTIL 0x80

typedef struct NV00DE_ALLOC_PARAMETERS {
    NvU64 polledDataMask;
} NV00DE_ALLOC_PARAMETERS;

/* Pin the vendored layout to the byte-exact values observed from the real
 * 610.43.02 SDK headers. If the inlined struct ever drifts, the build fails
 * here rather than silently reading the wrong offset. */
_Static_assert(sizeof(NV00DE_SHARED_DATA) == 3152, "NV00DE_SHARED_DATA size drift");
_Static_assert(offsetof(NV00DE_SHARED_DATA,
                        temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].temperature) == 2336,
               "temperatures[MEMORY].temperature offset drift");
_Static_assert(sizeof(RUSD_TEMPERATURE) == 16, "RUSD_TEMPERATURE size drift");
_Static_assert(sizeof(RUSD_MEM_ECC) == 240, "RUSD_MEM_ECC size drift");

/* =====================================================================
 * End vendored SDK subset. The rest matches the reference verbatim.
 * ===================================================================== */

/* ---- ioctl request codes (verbatim from ABI notes) ---- */
#define NV_IOCTL_MAGIC      'F'   /* 0x46 */
#define NV_ESC_RM_FREE          0x29
#define NV_ESC_RM_CONTROL       0x2A
#define NV_ESC_RM_ALLOC         0x2B
#define NV_ESC_RM_MAP_MEMORY    0x4E

/* ---- class ids ---- */
#define NV01_ROOT_CLIENT    0x41
#define NV01_DEVICE_0       0x80
#define NV20_SUBDEVICE_0    0x2080
/* RM_USER_SHARED_DATA (0x00de) defined above with the vendored cl00de subset */

/* ---- status ---- */
#define NV_OK               0x00000000

/* ---- NV00DE request-data-poll control (ctrl00de.h) ---- */
#define NV00DE_CTRL_CMD_REQUEST_DATA_POLL   0xde0001U
typedef struct {
    NvU64 polledDataMask __attribute__((aligned(8)));
} RUSD_REQUEST_DATA_POLL_PARAMS;

/* ---- NVOS33 map flags (nvos.h) ---- */
#define NVOS33_FLAGS_ACCESS_READ_ONLY   0x00000001u         /* bits 1:0  */
#define NVOS33_FLAGS_MEM_SPACE_USER     (0x00000001u << 14)  /* bit 14:14 */

/* ---- escape ioctl param structs ---- */
typedef struct {
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectNew;
    NvV32    hClass;
    NvP64    pAllocParms __attribute__((aligned(8)));
    NvU32    paramsSize;
    NvV32    status;
} NVOS21_PARAMETERS;

typedef struct {
    NvHandle hClient;
    NvHandle hObject;
    NvV32    cmd;
    NvU32    flags;
    NvP64    params __attribute__((aligned(8)));
    NvU32    paramsSize;
    NvV32    status;
} NVOS54_PARAMETERS;

typedef struct {
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectOld;
    NvV32    status;
} NVOS00_PARAMETERS;

/* RM_MAP_MEMORY param struct (nvos.h:1845) */
typedef struct {
    NvHandle hClient;
    NvHandle hDevice;            /* device or sub-device handle */
    NvHandle hMemory;            /* handle to memory object */
    NvU64    offset NV_ALIGN_BYTES(8);
    NvU64    length NV_ALIGN_BYTES(8);
    NvP64    pLinearAddress NV_ALIGN_BYTES(8);
    NvU32    status;
    NvU32    flags;
} NVOS33_PARAMETERS;

/* unix escape wrapper (nv-unix-nvos-params-wrappers.h): the kernel handler for
 * NV_ESC_RM_MAP_MEMORY expects this struct (NVOS33 + the fd whose per-file mmap
 * context we then mmap()). */
typedef struct {
    NVOS33_PARAMETERS params;
    int fd;
} nv_ioctl_nvos33_parameters_with_fd;

/* ---- class alloc param structs ---- */
typedef struct {
    NvHandle hClient;
    NvU32    processID;
    char     processName[100];   /* NV_PROC_NAME_MAX_LENGTH */
    NvP64    pOsPidInfo __attribute__((aligned(8)));
} NV0000_ALLOC_PARAMETERS;

typedef struct {
    NvU32    deviceId;
    NvHandle hClientShare;
    NvHandle hTargetClient;
    NvHandle hTargetDevice;
    NvV32    flags;
    NvU64    vaSpaceSize     __attribute__((aligned(8)));
    NvU64    vaStartInternal __attribute__((aligned(8)));
    NvU64    vaLimitInternal __attribute__((aligned(8)));
    NvV32    vaMode;
} NV0080_ALLOC_PARAMETERS;

typedef struct {
    NvU32 subDeviceId;
} NV2080_ALLOC_PARAMETERS;

/* ---- compile-time ABI checks ---- */
_Static_assert(sizeof(NVOS21_PARAMETERS) == 32, "NVOS21 size");
_Static_assert(sizeof(NVOS54_PARAMETERS) == 32, "NVOS54 size");
_Static_assert(sizeof(NVOS00_PARAMETERS) == 16, "NVOS00 size");
_Static_assert(sizeof(NVOS33_PARAMETERS) == 48, "NVOS33 size");

/* request code encoding: _IOC(dir=3,type='F',nr,size) */
#define NV_IOWR(nr, type) \
    ((3U << 30) | ((unsigned)sizeof(type) << 16) | ((unsigned)NV_IOCTL_MAGIC << 8) | (unsigned)(nr))

/* ===== ioctl helpers (reused verbatim from gddr7_temp.c) ===== */

/* hNew is in/out: pass 0 to auto-generate; the generated handle is written back. */
static int rm_alloc(int fd, NvHandle hRoot, NvHandle hParent, NvHandle *hNew,
                    NvV32 hClass, void *parms, NvU32 parmsSize, const char *name)
{
    NVOS21_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot         = hRoot;
    p.hObjectParent = hParent;
    p.hObjectNew    = *hNew;
    p.hClass        = hClass;
    p.pAllocParms   = (NvP64)(uintptr_t)parms;
    p.paramsSize    = parmsSize;
    p.status        = 0;

    if (ioctl(fd, NV_IOWR(NV_ESC_RM_ALLOC, NVOS21_PARAMETERS), &p) < 0) {
        fprintf(stderr, "ALLOC(%s): ioctl failed: %s\n", name, strerror(errno));
        return -1;
    }
    if (p.status != NV_OK) {
        fprintf(stderr, "ALLOC(%s): NV status 0x%08x\n", name, p.status);
        return -1;
    }
    *hNew = p.hObjectNew;   /* RM may have generated the handle */
    return 0;
}

static int rm_control(int fd, NvHandle hClient, NvHandle hObject, NvV32 cmd,
                      void *params, NvU32 paramsSize, const char *name)
{
    NVOS54_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hClient    = hClient;
    p.hObject    = hObject;
    p.cmd        = cmd;
    p.flags      = 0;
    p.params     = (NvP64)(uintptr_t)params;
    p.paramsSize = paramsSize;
    p.status     = 0;

    if (ioctl(fd, NV_IOWR(NV_ESC_RM_CONTROL, NVOS54_PARAMETERS), &p) < 0) {
        fprintf(stderr, "CONTROL(%s): ioctl failed: %s\n", name, strerror(errno));
        return -1;
    }
    if (p.status != NV_OK) {
        fprintf(stderr, "CONTROL(%s): NV status 0x%08x\n", name, p.status);
        return -1;
    }
    return 0;
}

static void rm_free(int fd, NvHandle hRoot, NvHandle hParent, NvHandle hOld)
{
    NVOS00_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot         = hRoot;
    p.hObjectParent = hParent;
    p.hObjectOld    = hOld;
    ioctl(fd, NV_IOWR(NV_ESC_RM_FREE, NVOS00_PARAMETERS), &p);
}

/* ===== RUSD seqlock read (userspace equivalent of RUSD_READ_DATA) ===== */
/* Returns 1 if a stable, valid sample was captured; 0 otherwise.
 * Mirrors subdevice_ctrl_gpu_kernel.c: up to 10 attempts, load-acquire fences
 * around the copy, validity gated by RUSD_SEQ_DATA_VALID + a stable timestamp. */
static int rusd_read_temp(volatile NV00DE_SHARED_DATA *sd, int typeIdx,
                          RUSD_TEMPERATURE *out)
{
    volatile RUSD_TEMPERATURE *slot = &sd->temperatures[typeIdx];
    memset(out, 0, sizeof(*out));
    for (int attempt = 0; attempt < 10; ++attempt) {
        NvU64 seq = slot->lastModifiedTimestamp;          /* volatile load */
        atomic_thread_fence(memory_order_acquire);        /* portAtomicMemoryFenceLoad */
        if (!RUSD_SEQ_DATA_VALID(seq))
            continue;
        memcpy(out, (const void *)slot, sizeof(*out));    /* copy {ts, temperature} */
        atomic_thread_fence(memory_order_acquire);
        if (seq == slot->lastModifiedTimestamp)
            return 1;                                     /* stable -> success */
        out->lastModifiedTimestamp = RUSD_TIMESTAMP_INVALID; /* torn; retry */
    }
    return 0;
}

static const char *type_name(int t)
{
    switch (t) {
        case RUSD_TEMPERATURE_TYPE_GPU:          return "GPU";
        case RUSD_TEMPERATURE_TYPE_MEMORY:       return "MEMORY(GDDR7)";
        case RUSD_TEMPERATURE_TYPE_BOARD:        return "BOARD";
        case RUSD_TEMPERATURE_TYPE_POWER_SUPPLY: return "POWER_SUPPLY";
        case RUSD_TEMPERATURE_TYPE_HBM:          return "HBM";
        default:                                 return "?";
    }
}

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(void)
{
    const char *dev = "/dev/nvidiactl";
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", dev, strerror(errno));
        return 2;
    }

    /* Hold /dev/nvidia0 open so the GPU is "accessible" to this process for a
     * non-kernel client to allocate a Device (osIsGpuAccessible scans the fd
     * table). All RM escapes still go on the /dev/nvidiactl fd. */
    const char *gpudev = "/dev/nvidia0";
    int gfd = open(gpudev, O_RDWR);
    if (gfd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", gpudev, strerror(errno));
        close(fd);
        return 2;
    }

    NvHandle hClient    = 0;            /* auto-generated by RM */
    NvHandle hDevice    = 0xD0D00001;
    NvHandle hSubdevice = 0x5D000001;
    NvHandle hRusd      = 0x00DE0001;
    int rc = 3;
    void *map = MAP_FAILED;
    size_t maplen = 0;
    int mfd = -1;

    /* Step 1: root client (handle auto-generated; read back) */
    NV0000_ALLOC_PARAMETERS clientParms;
    memset(&clientParms, 0, sizeof(clientParms));
    if (rm_alloc(fd, 0, 0, &hClient, NV01_ROOT_CLIENT,
                 &clientParms, sizeof(clientParms), "client") != 0)
        goto out_close;
    printf("Allocated client handle:    0x%08x\n", hClient);

    /* Step 2: device */
    NV0080_ALLOC_PARAMETERS devParms;
    memset(&devParms, 0, sizeof(devParms));
    devParms.deviceId = 0;
    if (rm_alloc(fd, hClient, hClient, &hDevice, NV01_DEVICE_0,
                 &devParms, sizeof(devParms), "device") != 0)
        goto out_free_client;

    /* Step 3: subdevice */
    NV2080_ALLOC_PARAMETERS subParms;
    memset(&subParms, 0, sizeof(subParms));
    subParms.subDeviceId = 0;
    if (rm_alloc(fd, hClient, hDevice, &hSubdevice, NV20_SUBDEVICE_0,
                 &subParms, sizeof(subParms), "subdevice") != 0)
        goto out_free_device;

    /* Step 4: alloc NV00DE (RM_USER_SHARED_DATA) under the subdevice, asking
     * for thermal polling up front. */
    NV00DE_ALLOC_PARAMETERS rusdParms;
    memset(&rusdParms, 0, sizeof(rusdParms));
    rusdParms.polledDataMask = NV00DE_RUSD_POLL_THERMAL;   /* 0x10 */
    if (rm_alloc(fd, hClient, hSubdevice, &hRusd, RM_USER_SHARED_DATA,
                 &rusdParms, sizeof(rusdParms), "rusd(NV00DE)") != 0)
        goto out_free_sub;
    printf("Allocated NV00DE handle:     0x%08x (class 0x%02x)\n",
           hRusd, RM_USER_SHARED_DATA);

    /* Step 5: (re)request the thermal poll feature. */
    RUSD_REQUEST_DATA_POLL_PARAMS poll;
    memset(&poll, 0, sizeof(poll));
    poll.polledDataMask = NV00DE_RUSD_POLL_THERMAL;
    if (rm_control(fd, hClient, hRusd, NV00DE_CTRL_CMD_REQUEST_DATA_POLL,
                   &poll, sizeof(poll), "REQUEST_DATA_POLL") != 0)
        goto out_free_rusd;
    printf("Requested POLL_THERMAL (0x%x) on NV00DE\n", NV00DE_RUSD_POLL_THERMAL);

    /* Step 6: map the shared buffer into this process. System-memory mappings
     * are associated with the control node, so the fd handed to RM (and then
     * mmap'd) must be a /dev/nvidiactl fd; use a dedicated one. */
    mfd = open(dev, O_RDWR);
    if (mfd < 0) {
        fprintf(stderr, "open(%s) for map failed: %s\n", dev, strerror(errno));
        goto out_free_rusd;
    }

    nv_ioctl_nvos33_parameters_with_fd m;
    memset(&m, 0, sizeof(m));
    m.params.hClient        = hClient;
    m.params.hDevice        = hSubdevice;     /* parent of the NV00DE object */
    m.params.hMemory        = hRusd;
    m.params.offset         = 0;
    m.params.length         = sizeof(NV00DE_SHARED_DATA);
    m.params.pLinearAddress = 0;
    /* User (non-kernel) RM clients must NOT set MEM_SPACE_USER (kernel-only;
     * rejected with NV_ERR_INVALID_FLAGS). The default MEM_SPACE_CLIENT already
     * yields a user-space mapping for a user client. */
    m.params.flags          = NVOS33_FLAGS_ACCESS_READ_ONLY;
    m.fd                    = mfd;

    if (ioctl(fd, NV_IOWR(NV_ESC_RM_MAP_MEMORY, nv_ioctl_nvos33_parameters_with_fd), &m) < 0) {
        fprintf(stderr, "MAP_MEMORY: ioctl failed: %s\n", strerror(errno));
        goto out_free_rusd;
    }
    if (m.params.status != NV_OK) {
        fprintf(stderr, "MAP_MEMORY: NV status 0x%08x\n", m.params.status);
        goto out_free_rusd;
    }

    /* Page-align the mmap length (kernel maps whole pages of the sysmem alloc). */
    long pg = sysconf(_SC_PAGESIZE);
    maplen = ((size_t)sizeof(NV00DE_SHARED_DATA) + (pg - 1)) & ~((size_t)pg - 1);
    map = mmap(NULL, maplen, PROT_READ, MAP_SHARED, mfd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        goto out_free_rusd;
    }
    printf("Mapped NV00DE_SHARED_DATA: %p (maplen %zu, sizeof struct %zu)\n",
           map, maplen, (size_t)sizeof(NV00DE_SHARED_DATA));
    printf("offsetof(temperatures[MEMORY].temperature) = %zu bytes\n",
           offsetof(NV00DE_SHARED_DATA, temperatures[RUSD_TEMPERATURE_TYPE_MEMORY].temperature));

    volatile NV00DE_SHARED_DATA *sd = (volatile NV00DE_SHARED_DATA *)map;

    /* Step 7: poll the buffer. RM's poller updates it in place on an interval,
     * so the MEMORY slot may be INVALID for the first few hundred ms. Re-read
     * (and occasionally re-request) for up to ~6 s. */
    RUSD_TEMPERATURE memT, gpuT;
    int memValid = 0, gpuValid = 0;
    const int max_iter = 30;
    for (int i = 0; i < max_iter; ++i) {
        gpuValid = rusd_read_temp(sd, RUSD_TEMPERATURE_TYPE_GPU,    &gpuT);
        memValid = rusd_read_temp(sd, RUSD_TEMPERATURE_TYPE_MEMORY, &memT);
        if (memValid)
            break;
        if ((i % 5) == 4) {
            /* keep the poll request fresh */
            poll.polledDataMask = NV00DE_RUSD_POLL_THERMAL;
            rm_control(fd, hClient, hRusd, NV00DE_CTRL_CMD_REQUEST_DATA_POLL,
                       &poll, sizeof(poll), "REQUEST_DATA_POLL(refresh)");
        }
        msleep(200);
    }

    /* Sanity: GPU-target temperature from the same buffer. */
    if (gpuValid) {
        NvS32 graw = (NvS32)gpuT.temperature;
        printf("GPU   temp [type %d %-13s]: raw=%d (0x%08x)  %.3f C  (ts=0x%016llx)\n",
               RUSD_TEMPERATURE_TYPE_GPU, type_name(RUSD_TEMPERATURE_TYPE_GPU),
               graw, (NvU32)graw, (double)graw / 256.0,
               (unsigned long long)gpuT.lastModifiedTimestamp);
    } else {
        printf("GPU   temp [type %d %-13s]: <no valid sample>\n",
               RUSD_TEMPERATURE_TYPE_GPU, type_name(RUSD_TEMPERATURE_TYPE_GPU));
    }

    if (memValid) {
        NvS32 raw = (NvS32)memT.temperature;
        double celsius = (double)raw / 256.0;
        printf("GDDR7 temp [type %d %-13s]: raw=%d (0x%08x)  %.3f C  (ts=0x%016llx)\n",
               RUSD_TEMPERATURE_TYPE_MEMORY, type_name(RUSD_TEMPERATURE_TYPE_MEMORY),
               raw, (NvU32)raw, celsius,
               (unsigned long long)memT.lastModifiedTimestamp);
        printf("\nRESULT GDDR7_MEMORY_TEMP_C = %.3f  (raw NvSFXP24_8 = %d)\n",
               celsius, raw);
        rc = 0;
    } else {
        /* Buffer mapped but the MEMORY slot never populated. Report the raw
         * sentinel/zero we saw (per guidance: this is the residual GSP risk on
         * a GeForce SKU). */
        NvS32 raw = (NvS32)memT.temperature;
        printf("GDDR7 temp [type %d %-13s]: <no valid sample after poll>"
               "  raw=%d (0x%08x) ts=0x%016llx\n",
               RUSD_TEMPERATURE_TYPE_MEMORY, type_name(RUSD_TEMPERATURE_TYPE_MEMORY),
               raw, (NvU32)raw,
               (unsigned long long)memT.lastModifiedTimestamp);
        printf("\nRESULT GDDR7_MEMORY_TEMP not populated (RUSD MEMORY slot stayed INVALID)\n");
        rc = 10;   /* signal not_populated */
    }

    if (map != MAP_FAILED)
        munmap(map, maplen);
out_free_rusd:
    rm_free(fd, hClient, hSubdevice, hRusd);
out_free_sub:
    rm_free(fd, hClient, hDevice, hSubdevice);
out_free_device:
    rm_free(fd, hClient, hClient, hDevice);
out_free_client:
    rm_free(fd, hClient, hClient, hClient);
out_close:
    if (mfd >= 0) close(mfd);
    close(gfd);
    close(fd);
    return rc;
}
