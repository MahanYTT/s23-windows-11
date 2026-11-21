/*
 * Escape protocol between Mesa Freedreno/Turnip WDDM shim and s23gpu.sys.
 * Wire-compatible with topazgpu_escape.h.
 */
#ifndef TOPAZGPU_ESCAPE_H
#define TOPAZGPU_ESCAPE_H

#define TOPAZGPU_ESC_MAGIC    0x55504754u /* 'TGPU' */
#define TOPAZGPU_ESC_MAX_DATA 1024

#define TOPAZGPU_NR_GEM_CLOSE 0x09
#define TOPAZGPU_NR_GEM_FLINK 0x0a
#define TOPAZGPU_NR_GEM_OPEN  0x0b
#define TOPAZGPU_NR_VERSION   0x1000
#define TOPAZGPU_NR_MMAP      0x1001
#define TOPAZGPU_NR_MUNMAP    0x1002

struct topazgpu_version {
    int major, minor, patch;
    char name[16];
};

struct topazgpu_mmap {
    unsigned long long offset;
    unsigned long long size;
    unsigned long long addr;
};

struct topazgpu_escape {
    unsigned magic;
    unsigned nr;
    unsigned size;
    int ret;
    unsigned char data[TOPAZGPU_ESC_MAX_DATA];
};

#define TOPAZGPU_ALLOC_MAGIC         0x41475054u /* 'TPGA' */
#define TOPAZGPU_ALLOC_UMD           0
#define TOPAZGPU_ALLOC_SHAREDPRIMARY 1
#define TOPAZGPU_ALLOC_SHADOW        2
#define TOPAZGPU_ALLOC_STAGING       3
#define TOPAZGPU_ALLOC_GDI           4
#define TOPAZGPU_ALLOC_F_PRIMARY     0x1

struct topazgpu_alloc {
    unsigned magic;
    unsigned kind;
    unsigned width, height, pitch;
    unsigned format;
    unsigned bo;
    unsigned bo_offset;
    unsigned size;
    unsigned vidpn;
    unsigned flags;
    unsigned reserved[5];
};

#endif
