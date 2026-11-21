/* Linux msm DRM uapi structs (MIT license), kernel copy for s23gpu.sys */
#ifndef S23GPU_MSM_DRM_H
#define S23GPU_MSM_DRM_H

typedef signed char        __s8;
typedef unsigned char      __u8;
typedef short              __s16;
typedef unsigned short     __u16;
typedef int                __s32;
typedef unsigned int       __u32;
typedef long long          __s64;
typedef unsigned long long __u64;

#define MSM_PIPE_3D0 0x10

struct drm_gem_close {
    __u32 handle;
    __u32 pad;
};

struct drm_gem_flink {
    __u32 handle;
    __u32 name;
};

struct drm_gem_open {
    __u32 name;
    __u32 handle;
    __u64 size;
};

#define DRM_COMMAND_BASE 0x40

struct drm_msm_timespec {
    __s64 tv_sec;
    __s64 tv_nsec;
};

#define MSM_PARAM_GPU_ID            0x01
#define MSM_PARAM_GMEM_SIZE         0x02
#define MSM_PARAM_CHIP_ID           0x03
#define MSM_PARAM_MAX_FREQ          0x04
#define MSM_PARAM_TIMESTAMP         0x05
#define MSM_PARAM_GMEM_BASE         0x06
#define MSM_PARAM_PRIORITIES        0x07
#define MSM_PARAM_PP_PGTABLE        0x08
#define MSM_PARAM_FAULTS            0x09
#define MSM_PARAM_SUSPENDS          0x0a
#define MSM_PARAM_SYSPROF           0x0b
#define MSM_PARAM_COMM              0x0c
#define MSM_PARAM_CMDLINE           0x0d
#define MSM_PARAM_VA_START          0x0e
#define MSM_PARAM_VA_SIZE           0x0f
#define MSM_PARAM_HIGHEST_BANK_BIT  0x10
#define MSM_PARAM_RAYTRACING        0x11
#define MSM_PARAM_UBWC_SWIZZLE      0x12
#define MSM_PARAM_MACROTILE_MODE    0x13
#define MSM_PARAM_UCHE_TRAP_BASE    0x14

#define MSM_PARAM_NR_RINGS          MSM_PARAM_PRIORITIES

struct drm_msm_param {
    __u32 pipe;
    __u32 param;
    __u64 value;
    __u32 len;
    __u32 pad;
};

#define MSM_BO_SCANOUT         0x00000001
#define MSM_BO_GPU_READONLY    0x00000002
#define MSM_BO_CACHE_MASK      0x000f0000
#define MSM_BO_CACHED          0x00010000
#define MSM_BO_WC              0x00020000
#define MSM_BO_UNCACHED        0x00040000
#define MSM_BO_CACHED_COHERENT 0x00080000

#define MSM_BO_FLAGS (MSM_BO_SCANOUT | MSM_BO_GPU_READONLY | MSM_BO_CACHE_MASK)

struct drm_msm_gem_new {
    __u64 size;
    __u32 flags;
    __u32 handle;
};

#define MSM_INFO_GET_OFFSET   0x00
#define MSM_INFO_GET_IOVA     0x01
#define MSM_INFO_SET_NAME     0x02
#define MSM_INFO_GET_NAME     0x03
#define MSM_INFO_SET_IOVA     0x04
#define MSM_INFO_GET_FLAGS    0x05
#define MSM_INFO_SET_METADATA 0x06
#define MSM_INFO_GET_METADATA 0x07

struct drm_msm_gem_info {
    __u32 handle;
    __u32 info;
    __u64 value;
    __u32 len;
    __u32 pad;
};

#define MSM_PREP_READ   0x01
#define MSM_PREP_WRITE  0x02
#define MSM_PREP_NOSYNC 0x04
#define MSM_PREP_BOOST  0x08

struct drm_msm_gem_cpu_prep {
    __u32 handle;
    __u32 op;
    struct drm_msm_timespec timeout;
};

struct drm_msm_gem_cpu_fini {
    __u32 handle;
};

struct drm_msm_gem_submit_reloc {
    __u32 submit_offset;
#ifdef __cplusplus
    __u32 _or;
#else
    __u32 or;
#endif
    __s32 shift;
    __u32 reloc_idx;
    __u64 reloc_offset;
};

#define MSM_SUBMIT_CMD_BUF             0x0001
#define MSM_SUBMIT_CMD_IB_TARGET_BUF   0x0002
#define MSM_SUBMIT_CMD_CTX_RESTORE_BUF 0x0003

struct drm_msm_gem_submit_cmd {
    __u32 type;
    __u32 submit_idx;
    __u32 submit_offset;
    __u32 size;
    __u32 pad;
    __u32 nr_relocs;
    __u64 relocs;
};

#define MSM_SUBMIT_BO_READ        0x0001
#define MSM_SUBMIT_BO_WRITE       0x0002
#define MSM_SUBMIT_BO_DUMP        0x0004
#define MSM_SUBMIT_BO_NO_IMPLICIT 0x0008

struct drm_msm_gem_submit_bo {
    __u32 flags;
    __u32 handle;
    __u64 presumed;
};

#define MSM_SUBMIT_NO_IMPLICIT   0x80000000
#define MSM_SUBMIT_FENCE_FD_IN   0x40000000
#define MSM_SUBMIT_FENCE_FD_OUT  0x20000000
#define MSM_SUBMIT_SUDO          0x10000000
#define MSM_SUBMIT_SYNCOBJ_IN    0x08000000
#define MSM_SUBMIT_SYNCOBJ_OUT   0x04000000
#define MSM_SUBMIT_FENCE_SN_IN   0x02000000

struct drm_msm_gem_submit {
    __u32 flags;
    __u32 fence;
    __u32 nr_bos;
    __u32 nr_cmds;
    __u64 bos;
    __u64 cmds;
    __s32 fence_fd;
    __u32 queueid;
    __u64 in_syncobjs;
    __u64 out_syncobjs;
    __u32 nr_in_syncobjs;
    __u32 nr_out_syncobjs;
    __u32 syncobj_stride;
    __u32 pad;
};

#define MSM_WAIT_FENCE_BOOST 0x00000001

struct drm_msm_wait_fence {
    __u32 fence;
    __u32 flags;
    struct drm_msm_timespec timeout;
    __u32 queueid;
};

#define MSM_MADV_WILLNEED 0
#define MSM_MADV_DONTNEED 1
#define __MSM_MADV_PURGED 2

struct drm_msm_gem_madvise {
    __u32 handle;
    __u32 madv;
    __u32 retained;
};

struct drm_msm_submitqueue {
    __u32 flags;
    __u32 prio;
    __u32 id;
};

#define MSM_SUBMITQUEUE_PARAM_FAULTS 0

struct drm_msm_submitqueue_query {
    __u64 data;
    __u32 id;
    __u32 param;
    __u32 len;
    __u32 pad;
};

#define DRM_MSM_GET_PARAM         0x00
#define DRM_MSM_SET_PARAM         0x01
#define DRM_MSM_GEM_NEW           0x02
#define DRM_MSM_GEM_INFO          0x03
#define DRM_MSM_GEM_CPU_PREP      0x04
#define DRM_MSM_GEM_CPU_FINI      0x05
#define DRM_MSM_GEM_SUBMIT        0x06
#define DRM_MSM_WAIT_FENCE        0x07
#define DRM_MSM_GEM_MADVISE       0x08
#define DRM_MSM_SUBMITQUEUE_NEW   0x0A
#define DRM_MSM_SUBMITQUEUE_CLOSE 0x0B
#define DRM_MSM_SUBMITQUEUE_QUERY 0x0C

#endif
