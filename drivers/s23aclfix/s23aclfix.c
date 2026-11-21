/*
 * s23aclfix.c - Native ARM64 WDM Start=1 one-shot NTFS ACL recovery driver for S23 (dm1q)
 *
 * Runs synchronously in IopInitializeSystemDrivers() BEFORE smss.exe and services.exe,
 * restoring Windows 11 NTFS DACLs wiped by chkdsk using pure kernel-mode ZwSetSecurityObject.
 * Disables itself (Start=4), writes a sentinel file, and returns STATUS_UNSUCCESSFUL
 * (ErrorControl=0) so Windows boots straight to desktop on the very same boot.
 */

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   UCHAR, BOOLEAN;
typedef uint16_t  USHORT, WCHAR;
typedef uint32_t  ULONG, ACCESS_MASK;
typedef int32_t   LONG, NTSTATUS;
typedef uint64_t  ULONGLONG, ULONG_PTR, SIZE_T;
typedef int64_t   LONGLONG;
typedef void     *PVOID, *HANDLE;
typedef WCHAR    *PWSTR;
typedef const WCHAR *PCWSTR;

#define TRUE  1
#define FALSE 0

#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#define STATUS_SUCCESS            ((NTSTATUS)0x00000000)
#define STATUS_UNSUCCESSFUL       ((NTSTATUS)0xC0000001)
#define STATUS_NO_MORE_FILES      ((NTSTATUS)0x80000006)

#define OBJ_CASE_INSENSITIVE      0x00000040U
#define OBJ_KERNEL_HANDLE         0x00000200U
#define OBJ_ATTR_K                (OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE)

#define SYMBOLIC_LINK_QUERY       0x00000001U
#define KEY_SET_VALUE             0x00020000U
#define KEY_WRITE_VAL             0x00000002U
#define REG_DWORD                 4U

#define SYNCHRONIZE               0x00100000U
#define WRITE_DAC                 0x00040000U
#define FILE_LIST_DIRECTORY       0x00000001U
#define FILE_WRITE_DATA           0x00000002U
#define FILE_READ_ATTRIBUTES      0x00000080U

#define FILE_SHARE_READ           0x00000001U
#define FILE_SHARE_WRITE          0x00000002U
#define FILE_SHARE_DELETE         0x00000004U
#define FILE_SHARE_ALL            0x00000007U

#define FILE_OPEN                 1U
#define FILE_OVERWRITE_IF         5U

#define FILE_DIRECTORY_FILE            0x00000001U
#define FILE_WRITE_THROUGH             0x00000002U
#define FILE_SYNCHRONOUS_IO_NONALERT   0x00000020U
#define FILE_NON_DIRECTORY_FILE        0x00000040U
#define FILE_OPEN_REPARSE_POINT        0x00200000U

#define FILE_ATTRIBUTE_DIRECTORY       0x00000010U
#define FILE_ATTRIBUTE_NORMAL          0x00000080U
#define FILE_ATTRIBUTE_REPARSE_POINT   0x00000400U

#define FileDirectoryInformation       1U
#define DACL_SECURITY_INFORMATION      0x00000004U
#define SECURITY_DESCRIPTOR_REVISION   1U
#define ACL_REVISION                   2U

#define OBJECT_INHERIT_ACE             0x01U
#define CONTAINER_INHERIT_ACE          0x02U
#define DIR_ACE_FLAGS                  (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)
#define FILE_ACE_FLAGS                 0x00U

/* Explicit NT File Access Masks */
#define MASK_F   0x001F01FFU
#define MASK_M   0x001301BFU
#define MASK_RX  0x001200A9U

#define POOL_FLAG_PAGED 0x0000000000000100ULL
#define POOL_TAG        0x66333253U /* 'S23f' */

#define DIR_BUF_SIZE    65536U
#define MAX_DEPTH       64U
#define MAX_TREE_ITEMS  350000U

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    ULONG  Pad;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
_Static_assert(sizeof(UNICODE_STRING) == 16, "UNICODE_STRING size");
_Static_assert(offsetof(UNICODE_STRING, Buffer) == 8, "UNICODE_STRING Buffer");

typedef struct _OBJECT_ATTRIBUTES {
    ULONG           Length;
    ULONG           Pad0;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG           Attributes;
    ULONG           Pad1;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
_Static_assert(sizeof(OBJECT_ATTRIBUTES) == 48, "OBJECT_ATTRIBUTES size");
_Static_assert(offsetof(OBJECT_ATTRIBUTES, RootDirectory) == 8, "OA RootDirectory");
_Static_assert(offsetof(OBJECT_ATTRIBUTES, ObjectName) == 16, "OA ObjectName");
_Static_assert(offsetof(OBJECT_ATTRIBUTES, Attributes) == 24, "OA Attributes");

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    };
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
_Static_assert(sizeof(IO_STATUS_BLOCK) == 16, "IO_STATUS_BLOCK size");
_Static_assert(offsetof(IO_STATUS_BLOCK, Information) == 8, "IOSB Information");

typedef union _LARGE_INTEGER {
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _FILE_DIRECTORY_INFORMATION {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    WCHAR         FileName[1];
} FILE_DIRECTORY_INFORMATION, *PFILE_DIRECTORY_INFORMATION;
_Static_assert(offsetof(FILE_DIRECTORY_INFORMATION, FileAttributes) == 0x38, "FDI FileAttributes");
_Static_assert(offsetof(FILE_DIRECTORY_INFORMATION, FileNameLength) == 0x3C, "FDI FileNameLength");
_Static_assert(offsetof(FILE_DIRECTORY_INFORMATION, FileName) == 0x40, "FDI FileName");

typedef struct _SID_IDENTIFIER_AUTHORITY {
    UCHAR Value[6];
} SID_IDENTIFIER_AUTHORITY, *PSID_IDENTIFIER_AUTHORITY;
_Static_assert(sizeof(SID_IDENTIFIER_AUTHORITY) == 6, "SID_IDENTIFIER_AUTHORITY size");

/* ntoskrnl.exe imports (all verified against Win11 24H2 ARM64 ntoskrnl.exe) */
__declspec(dllimport) void     RtlInitUnicodeString(PUNICODE_STRING, PCWSTR);
__declspec(dllimport) NTSTATUS RtlInitializeSid(PVOID Sid, PSID_IDENTIFIER_AUTHORITY Authority, UCHAR SubAuthorityCount);
__declspec(dllimport) ULONG   *RtlSubAuthoritySid(PVOID Sid, ULONG SubAuthority);
__declspec(dllimport) NTSTATUS RtlCreateAcl(PVOID Acl, ULONG AclLength, ULONG AclRevision);
__declspec(dllimport) NTSTATUS RtlAddAccessAllowedAceEx(PVOID Acl, ULONG AceRevision, ULONG AceFlags, ACCESS_MASK AccessMask, PVOID Sid);
__declspec(dllimport) NTSTATUS RtlCreateSecurityDescriptor(PVOID Sd, ULONG Revision);
__declspec(dllimport) NTSTATUS RtlSetDaclSecurityDescriptor(PVOID Sd, BOOLEAN DaclPresent, PVOID Dacl, BOOLEAN DaclDefaulted);
__declspec(dllimport) NTSTATUS RtlAbsoluteToSelfRelativeSD(PVOID AbsSd, PVOID RelSd, ULONG *BufLen);
__declspec(dllimport) BOOLEAN  RtlValidSecurityDescriptor(PVOID Sd);

__declspec(dllimport) NTSTATUS ZwOpenSymbolicLinkObject(HANDLE *LinkHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
__declspec(dllimport) NTSTATUS ZwQuerySymbolicLinkObject(HANDLE LinkHandle, PUNICODE_STRING LinkTarget, ULONG *ReturnedLength);
__declspec(dllimport) NTSTATUS ZwOpenFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions);
__declspec(dllimport) NTSTATUS ZwCreateFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
__declspec(dllimport) NTSTATUS ZwWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, ULONG *Key);
__declspec(dllimport) NTSTATUS ZwQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length, ULONG FileInformationClass, BOOLEAN ReturnSingleEntry, PUNICODE_STRING FileName, BOOLEAN RestartScan);
__declspec(dllimport) NTSTATUS ZwSetSecurityObject(HANDLE Handle, ULONG SecurityInformation, PVOID SecurityDescriptor);
__declspec(dllimport) NTSTATUS ZwFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock);
__declspec(dllimport) NTSTATUS ZwOpenKey(HANDLE *KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
__declspec(dllimport) NTSTATUS ZwSetValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize);
__declspec(dllimport) NTSTATUS ZwClose(HANDLE Handle);

__declspec(dllimport) PVOID    ExAllocatePool2(ULONGLONG Flags, SIZE_T NumberOfBytes, ULONG Tag);
__declspec(dllimport) void     ExFreePoolWithTag(PVOID P, ULONG Tag);
__declspec(dllimport) LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER PerformanceFrequency);

static void ZeroMem(void *p, size_t n) {
    volatile UCHAR *b = (volatile UCHAR *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

static void CopyMem(void *dst, const void *src, size_t n) {
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void InitOA(POBJECT_ATTRIBUTES oa, HANDLE root, PUNICODE_STRING name) {
    oa->Length = sizeof(OBJECT_ATTRIBUTES);
    oa->Pad0 = 0;
    oa->RootDirectory = root;
    oa->ObjectName = name;
    oa->Attributes = OBJ_ATTR_K;
    oa->Pad1 = 0;
    oa->SecurityDescriptor = NULL;
    oa->SecurityQualityOfService = NULL;
}

/* Write-through log handle */
static HANDLE g_log = NULL;
static LONGLONG g_logOff = 0;

static void LogStr(const char *s) {
    if (!g_log) return;
    ULONG len = 0;
    while (s[len]) len++;
    if (!len) return;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    off.QuadPart = g_logOff;
    if (NT_SUCCESS(ZwWriteFile(g_log, NULL, NULL, NULL, &iosb, (PVOID)s, len, &off, NULL))) {
        g_logOff += (LONGLONG)iosb.Information;
    }
}

static void LogU32(ULONG v) {
    char buf[16];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[16];
        int t = 0;
        while (v > 0) { tmp[t++] = (char)('0' + (v % 10U)); v /= 10U; }
        while (t > 0) buf[n++] = tmp[--t];
    }
    buf[n] = 0;
    LogStr(buf);
}

static void LogHex(ULONG v) {
    static const char hx[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = hx[(v >> ((7 - i) * 4)) & 0xFU];
    buf[10] = 0;
    LogStr(buf);
}

/* Pre-built SIDs (64 bytes each, 8-byte aligned) */
typedef struct _SID_BUF { uint64_t raw[8]; } SID_BUF;

static SID_BUF g_sidSystem;
static SID_BUF g_sidAdmins;
static SID_BUF g_sidUsers;
static SID_BUF g_sidAuthUsers;
static SID_BUF g_sidLocalSvc;
static SID_BUF g_sidNetSvc;
static SID_BUF g_sidEveryone;
static SID_BUF g_sidAppPkg;
static SID_BUF g_sidRAppPkg;
static SID_BUF g_sidTrustedInst;
static SID_BUF g_sidUserS23;

static BOOLEAN InitSidN(SID_BUF *buf, UCHAR auth5, UCHAR count, const ULONG *subs) {
    ZeroMem(buf, sizeof(*buf));
    SID_IDENTIFIER_AUTHORITY ia = {{ 0, 0, 0, 0, 0, auth5 }};
    if (!NT_SUCCESS(RtlInitializeSid(buf, &ia, count))) return FALSE;
    for (UCHAR i = 0; i < count; i++) {
        *RtlSubAuthoritySid(buf, i) = subs[i];
    }
    return TRUE;
}

static BOOLEAN InitAllSids(void) {
    const ULONG s_sys[1]  = { 18U };
    const ULONG s_adm[2]  = { 32U, 544U };
    const ULONG s_usr[2]  = { 32U, 545U };
    const ULONG s_au[1]   = { 11U };
    const ULONG s_ls[1]   = { 19U };
    const ULONG s_ns[1]   = { 20U };
    const ULONG s_ev[1]   = { 0U };
    const ULONG s_ap[2]   = { 2U, 1U };
    const ULONG s_rap[2]  = { 2U, 2U };
    const ULONG s_ti[6]   = { 80U, 956008885U, 3418522649U, 1831038044U, 1853292631U, 2271478464U };
    const ULONG s_s23[5]  = { 21U, 90484302U, 2165722077U, 966411361U, 1000U };

    return InitSidN(&g_sidSystem,      5,  1, s_sys) &&
           InitSidN(&g_sidAdmins,      5,  2, s_adm) &&
           InitSidN(&g_sidUsers,       5,  2, s_usr) &&
           InitSidN(&g_sidAuthUsers,   5,  1, s_au)  &&
           InitSidN(&g_sidLocalSvc,    5,  1, s_ls)  &&
           InitSidN(&g_sidNetSvc,      5,  1, s_ns)  &&
           InitSidN(&g_sidEveryone,    1,  1, s_ev)  &&
           InitSidN(&g_sidAppPkg,      15, 2, s_ap)  &&
           InitSidN(&g_sidRAppPkg,     15, 2, s_rap) &&
           InitSidN(&g_sidTrustedInst, 5,  6, s_ti)  &&
           InitSidN(&g_sidUserS23,     5,  5, s_s23);
}

typedef struct _ACE_SPEC {
    SID_BUF    *sid;
    ACCESS_MASK mask;
} ACE_SPEC;

typedef struct _SD_PROFILE {
    uint64_t dirSd[128];  /* 1024 bytes self-relative SD with OI|CI */
    uint64_t fileSd[128]; /* 1024 bytes self-relative SD with 0x00 */
} SD_PROFILE;

static BOOLEAN BuildOneSd(const char *name, PVOID outSr, ULONG outSrCap, const ACE_SPEC *aces, size_t nAces, ULONG aceFlags) {
    uint64_t absSd[8];
    uint64_t aclBuf[64]; /* 512 bytes max; exact ACL size computed below (~100-260 bytes) */
    ZeroMem(absSd, sizeof(absSd));
    ZeroMem(aclBuf, sizeof(aclBuf));
    ZeroMem(outSr, outSrCap);

    /* Compute tight ACL size: 8-byte ACL header + sum(8-byte ACE header + SID length), 4-byte aligned */
    ULONG aclLen = 8U;
    for (size_t i = 0; i < nAces; i++) {
        UCHAR subCnt = ((const UCHAR *)aces[i].sid)[1];
        aclLen += 8U + (8U + 4U * (ULONG)subCnt);
    }
    aclLen = ((aclLen + 16U) + 3U) & ~3U;
    if (aclLen > sizeof(aclBuf)) {
        LogStr("[sd] "); LogStr(name); LogStr(" aclLen overflow="); LogU32(aclLen); LogStr("\r\n");
        return FALSE;
    }

    NTSTATUS st = RtlCreateSecurityDescriptor(absSd, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(st)) { LogStr("[sd] CreateSD "); LogHex((ULONG)st); LogStr("\r\n"); return FALSE; }

    st = RtlCreateAcl(aclBuf, aclLen, ACL_REVISION);
    if (!NT_SUCCESS(st)) { LogStr("[sd] CreateAcl "); LogHex((ULONG)st); LogStr("\r\n"); return FALSE; }

    for (size_t i = 0; i < nAces; i++) {
        st = RtlAddAccessAllowedAceEx(aclBuf, ACL_REVISION, aceFlags, aces[i].mask, aces[i].sid);
        if (!NT_SUCCESS(st)) {
            LogStr("[sd] "); LogStr(name); LogStr(" AddAce["); LogU32((ULONG)i); LogStr("]="); LogHex((ULONG)st); LogStr("\r\n");
            return FALSE;
        }
    }
    st = RtlSetDaclSecurityDescriptor(absSd, TRUE, aclBuf, FALSE);
    if (!NT_SUCCESS(st)) { LogStr("[sd] SetDacl "); LogHex((ULONG)st); LogStr("\r\n"); return FALSE; }

    ULONG req = outSrCap;
    st = RtlAbsoluteToSelfRelativeSD(absSd, outSr, &req);
    if (!NT_SUCCESS(st)) {
        LogStr("[sd] "); LogStr(name); LogStr(" AbsToRel st="); LogHex((ULONG)st); LogStr(" req="); LogU32(req); LogStr("\r\n");
        return FALSE;
    }
    if (!RtlValidSecurityDescriptor(outSr)) {
        LogStr("[sd] "); LogStr(name); LogStr(" ValidSD=FALSE\r\n");
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN BuildProfile(const char *name, SD_PROFILE *p, const ACE_SPEC *aces, size_t nAces) {
    return BuildOneSd(name, p->dirSd,  sizeof(p->dirSd),  aces, nAces, DIR_ACE_FLAGS) &&
           BuildOneSd(name, p->fileSd, sizeof(p->fileSd), aces, nAces, FILE_ACE_FLAGS);
}

static SD_PROFILE g_profRx;           /* Windows / System32 / WinSxS / Program Files / Default */
static SD_PROFILE g_profLocalSvc;     /* ServiceProfiles\LocalService */
static SD_PROFILE g_profNetSvc;       /* ServiceProfiles\NetworkService */
static SD_PROFILE g_profSvcRw;        /* winevt / LogFiles / Tasks / catroot2 / spool / sru / wdi / ServiceState */
static SD_PROFILE g_profTemp;         /* Windows\Temp */
static SD_PROFILE g_profProgramData;  /* ProgramData */
static SD_PROFILE g_profPublic;       /* Users\Public */
static SD_PROFILE g_profUserS23;      /* Users\s23 */
static SD_PROFILE g_profUserPkgs;     /* Users\s23\AppData\Local\Packages & Temp */

static BOOLEAN InitAllProfiles(void) {
    const ACE_SPEC rx[] = {
        { &g_sidSystem,      MASK_F  },
        { &g_sidAdmins,      MASK_F  },
        { &g_sidTrustedInst, MASK_F  },
        { &g_sidUsers,       MASK_RX },
        { &g_sidAuthUsers,   MASK_RX },
        { &g_sidLocalSvc,    MASK_RX },
        { &g_sidNetSvc,      MASK_RX },
        { &g_sidEveryone,    MASK_RX },
        { &g_sidAppPkg,      MASK_RX },
        { &g_sidRAppPkg,     MASK_RX },
    };
    const ACE_SPEC lsvc[] = {
        { &g_sidSystem,      MASK_F  },
        { &g_sidAdmins,      MASK_F  },
        { &g_sidLocalSvc,    MASK_F  },
    };
    const ACE_SPEC nsvc[] = {
        { &g_sidSystem,      MASK_F  },
        { &g_sidAdmins,      MASK_F  },
        { &g_sidNetSvc,      MASK_F  },
    };
    const ACE_SPEC svcrw[] = {
        { &g_sidSystem,      MASK_F  },
        { &g_sidAdmins,      MASK_F  },
        { &g_sidTrustedInst, MASK_F  },
        { &g_sidLocalSvc,    MASK_F  },
        { &g_sidNetSvc,      MASK_F  },
        { &g_sidUsers,       MASK_RX },
        { &g_sidAuthUsers,   MASK_RX },
        { &g_sidEveryone,    MASK_RX },
        { &g_sidAppPkg,      MASK_RX },
        { &g_sidRAppPkg,     MASK_RX },
    };
    const ACE_SPEC wtemp[] = {
        { &g_sidSystem,      MASK_F  },
        { &g_sidAdmins,      MASK_F  },
        { &g_sidLocalSvc,    MASK_F  },
        { &g_sidNetSvc,      MASK_F  },
        { &g_sidUsers,       MASK_M  },
        { &g_sidAuthUsers,   MASK_M  },
        { &g_sidEveryone,    MASK_M  },
        { &g_sidAppPkg,      MASK_RX },
        { &g_sidRAppPkg,     MASK_RX },
    };
    const ACE_SPEC pdata[] = {
        { &g_sidSystem,      MASK_F  },
        { &g_sidAdmins,      MASK_F  },
        { &g_sidLocalSvc,    MASK_F  },
        { &g_sidNetSvc,      MASK_F  },
        { &g_sidUsers,       MASK_M  },
        { &g_sidAuthUsers,   MASK_M  },
        { &g_sidEveryone,    MASK_RX },
        { &g_sidAppPkg,      MASK_RX },
        { &g_sidRAppPkg,     MASK_RX },
    };
    const ACE_SPEC pub[] = {
        { &g_sidSystem,      MASK_F  },
        { &g_sidAdmins,      MASK_F  },
        { &g_sidLocalSvc,    MASK_F  },
        { &g_sidNetSvc,      MASK_F  },
        { &g_sidUsers,       MASK_F  },
        { &g_sidAuthUsers,   MASK_F  },
        { &g_sidEveryone,    MASK_F  },
        { &g_sidAppPkg,      MASK_RX },
        { &g_sidRAppPkg,     MASK_RX },
    };
    const ACE_SPEC us23[] = {
        { &g_sidSystem,      MASK_F  },
        { &g_sidAdmins,      MASK_F  },
        { &g_sidUserS23,     MASK_F  },
        { &g_sidAppPkg,      MASK_RX },
        { &g_sidRAppPkg,     MASK_RX },
    };
    const ACE_SPEC upkgs[] = {
        { &g_sidSystem,      MASK_F  },
        { &g_sidAdmins,      MASK_F  },
        { &g_sidUserS23,     MASK_F  },
        { &g_sidAppPkg,      MASK_F  },
        { &g_sidRAppPkg,     MASK_F  },
    };

    return BuildProfile("rx",          &g_profRx,          rx,    sizeof(rx)/sizeof(rx[0]))       &&
           BuildProfile("lsvc",        &g_profLocalSvc,    lsvc,  sizeof(lsvc)/sizeof(lsvc[0]))   &&
           BuildProfile("nsvc",        &g_profNetSvc,      nsvc,  sizeof(nsvc)/sizeof(nsvc[0]))   &&
           BuildProfile("svcrw",       &g_profSvcRw,       svcrw, sizeof(svcrw)/sizeof(svcrw[0])) &&
           BuildProfile("wtemp",       &g_profTemp,        wtemp, sizeof(wtemp)/sizeof(wtemp[0])) &&
           BuildProfile("pdata",       &g_profProgramData, pdata, sizeof(pdata)/sizeof(pdata[0])) &&
           BuildProfile("pub",         &g_profPublic,      pub,   sizeof(pub)/sizeof(pub[0]))     &&
           BuildProfile("us23",        &g_profUserS23,     us23,  sizeof(us23)/sizeof(us23[0]))   &&
           BuildProfile("upkgs",       &g_profUserPkgs,    upkgs, sizeof(upkgs)/sizeof(upkgs[0]));
}

static BOOLEAN WstrEqInsensitive(const WCHAR *a, ULONG aBytes, const WCHAR *bLit) {
    ULONG bChars = 0;
    while (bLit[bChars]) bChars++;
    if (aBytes != bChars * 2U) return FALSE;
    for (ULONG i = 0; i < bChars; i++) {
        WCHAR ca = a[i], cb = bLit[i];
        if (ca >= L'A' && ca <= L'Z') ca = (WCHAR)(ca + 32);
        if (cb >= L'A' && cb <= L'Z') cb = (WCHAR)(cb + 32);
        if (ca != cb) return FALSE;
    }
    return TRUE;
}

/* Iterative BFS directory node */
typedef struct _DIR_NODE {
    struct _DIR_NODE *next;
    USHORT depth;
    USHORT byteLen;
    WCHAR  path[1];
} DIR_NODE;

static DIR_NODE *AllocDirNode(const WCHAR *parent, USHORT parentBytes, const WCHAR *child, USHORT childBytes, USHORT depth) {
    ULONG totalBytes = (ULONG)parentBytes + (parentBytes ? 2U : 0U) + (ULONG)childBytes;
    if (totalBytes == 0 || totalBytes > 32700U) return NULL;
    DIR_NODE *n = (DIR_NODE *)ExAllocatePool2(POOL_FLAG_PAGED, sizeof(DIR_NODE) + totalBytes + 2U, POOL_TAG);
    if (!n) return NULL;
    n->next = NULL;
    n->depth = depth;
    n->byteLen = (USHORT)totalBytes;
    UCHAR *dst = (UCHAR *)n->path;
    if (parentBytes) {
        CopyMem(dst, parent, parentBytes);
        dst += parentBytes;
        WCHAR sep = L'\\';
        CopyMem(dst, &sep, 2);
        dst += 2;
    }
    CopyMem(dst, child, childBytes);
    dst[childBytes] = 0;
    dst[childBytes + 1] = 0;
    return n;
}

static void ApplySinglePath(HANDLE hRoot, const char *tag, PCWSTR relPath, BOOLEAN isDir, const SD_PROFILE *prof) {
    UNICODE_STRING us;
    RtlInitUnicodeString(&us, relPath);
    OBJECT_ATTRIBUTES oa;
    InitOA(&oa, hRoot, &us);
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    ULONG opts = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | (isDir ? FILE_DIRECTORY_FILE : 0U);
    NTSTATUS st = ZwOpenFile(&h, WRITE_DAC | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_ALL, opts);
    if (!NT_SUCCESS(st)) {
        LogStr("[one] "); LogStr(tag); LogStr(" open="); LogHex((ULONG)st); LogStr("\r\n");
        return;
    }
    st = ZwSetSecurityObject(h, DACL_SECURITY_INFORMATION, (PVOID)(isDir ? prof->dirSd : prof->fileSd));
    ZwClose(h);
    LogStr("[one] "); LogStr(tag); LogStr(" set="); LogHex((ULONG)st); LogStr("\r\n");
}

static void WalkTree(HANDLE hRoot, PVOID dirBuf, const char *tag, PCWSTR startRel, const SD_PROFILE *prof, BOOLEAN skipConfigDir, LONGLONG qpf) {
    LARGE_INTEGER t0 = KeQueryPerformanceCounter(NULL);
    UNICODE_STRING startUs;
    RtlInitUnicodeString(&startUs, startRel);

    DIR_NODE *head = AllocDirNode(NULL, 0, startUs.Buffer, startUs.Length, 0);
    DIR_NODE *tail = head;
    if (!head) {
        LogStr("[tree] "); LogStr(tag); LogStr(" OOM\r\n");
        return;
    }

    ULONG dirsOk = 0, filesOk = 0, errs = 0, repSkip = 0, totalSeen = 0;

    while (head && totalSeen < MAX_TREE_ITEMS) {
        DIR_NODE *cur = head;
        head = cur->next;
        if (!head) tail = NULL;

        UNICODE_STRING dirUs;
        dirUs.Length = cur->byteLen;
        dirUs.MaximumLength = (USHORT)(cur->byteLen + 2U);
        dirUs.Pad = 0;
        dirUs.Buffer = cur->path;

        OBJECT_ATTRIBUTES oa;
        InitOA(&oa, hRoot, &dirUs);
        IO_STATUS_BLOCK iosb;
        HANDLE hDir = NULL;
        BOOLEAN canWriteDac = TRUE;

        NTSTATUS st = ZwOpenFile(&hDir, FILE_LIST_DIRECTORY | WRITE_DAC | SYNCHRONIZE,
                                 &oa, &iosb, FILE_SHARE_ALL,
                                 FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
        if (!NT_SUCCESS(st)) {
            canWriteDac = FALSE;
            st = ZwOpenFile(&hDir, FILE_LIST_DIRECTORY | SYNCHRONIZE,
                            &oa, &iosb, FILE_SHARE_ALL,
                            FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
        }
        if (!NT_SUCCESS(st)) {
            errs++;
            ExFreePoolWithTag(cur, POOL_TAG);
            continue;
        }

        if (canWriteDac) {
            NTSTATUS sst = ZwSetSecurityObject(hDir, DACL_SECURITY_INFORMATION, (PVOID)prof->dirSd);
            if (NT_SUCCESS(sst)) dirsOk++; else errs++;
        } else {
            errs++;
        }

        BOOLEAN restart = TRUE;
        while (totalSeen < MAX_TREE_ITEMS) {
            iosb.Status = 0;
            iosb.Information = 0;
            NTSTATUS qst = ZwQueryDirectoryFile(hDir, NULL, NULL, NULL, &iosb,
                                                dirBuf, DIR_BUF_SIZE, FileDirectoryInformation,
                                                FALSE, NULL, restart);
            restart = FALSE;
            if (qst == STATUS_NO_MORE_FILES || !NT_SUCCESS(qst)) break;

            ULONG bufLen = (ULONG)iosb.Information;
            if (bufLen < 0x40U || bufLen > DIR_BUF_SIZE) break;

            ULONG off = 0;
            for (;;) {
                PFILE_DIRECTORY_INFORMATION fdi = (PFILE_DIRECTORY_INFORMATION)((UCHAR *)dirBuf + off);
                ULONG fnLen = fdi->FileNameLength;
                if (fnLen >= 2U && (fnLen & 1U) == 0U && fnLen <= 512U && (0x40U + fnLen) <= (bufLen - off)) {
                    BOOLEAN isDot = (fnLen == 2U && fdi->FileName[0] == L'.') ||
                                    (fnLen == 4U && fdi->FileName[0] == L'.' && fdi->FileName[1] == L'.');
                    if (!isDot) {
                        totalSeen++;
                        BOOLEAN isDir = (fdi->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                        BOOLEAN isRep = (fdi->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

                        if (isDir) {
                            if (isRep) {
                                /* Never recurse or overwrite DACL on directory junctions/symlinks */
                                repSkip++;
                            } else if (skipConfigDir && cur->depth == 0 && WstrEqInsensitive(fdi->FileName, fnLen, L"config")) {
                                repSkip++;
                            } else if (cur->depth + 1U < MAX_DEPTH) {
                                DIR_NODE *child = AllocDirNode(cur->path, cur->byteLen, fdi->FileName, (USHORT)fnLen, (USHORT)(cur->depth + 1U));
                                if (child) {
                                    if (tail) { tail->next = child; tail = child; }
                                    else      { head = tail = child; }
                                }
                            }
                        } else {
                            /* Regular file (or WOF reparse file): handle-relative open with WRITE_DAC only */
                            UNICODE_STRING cus;
                            cus.Length = (USHORT)fnLen;
                            cus.MaximumLength = (USHORT)fnLen;
                            cus.Pad = 0;
                            cus.Buffer = fdi->FileName;
                            OBJECT_ATTRIBUTES coa;
                            InitOA(&coa, hDir, &cus);
                            IO_STATUS_BLOCK ciosb;
                            HANDLE hFile = NULL;
                            NTSTATUS ost = ZwOpenFile(&hFile, WRITE_DAC | SYNCHRONIZE, &coa, &ciosb,
                                                      FILE_SHARE_ALL, FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
                            if (NT_SUCCESS(ost)) {
                                NTSTATUS sst = ZwSetSecurityObject(hFile, DACL_SECURITY_INFORMATION, (PVOID)prof->fileSd);
                                ZwClose(hFile);
                                if (NT_SUCCESS(sst)) filesOk++; else errs++;
                            } else {
                                errs++;
                            }
                        }
                    }
                }

                ULONG next = fdi->NextEntryOffset;
                if (next == 0U || next > (bufLen - off) || (next & 7U) != 0U) break;
                off += next;
                if (off + 0x40U > bufLen) break;
            }
        }

        ZwClose(hDir);
        ExFreePoolWithTag(cur, POOL_TAG);
    }

    while (head) {
        DIR_NODE *next = head->next;
        ExFreePoolWithTag(head, POOL_TAG);
        head = next;
    }

    LARGE_INTEGER t1 = KeQueryPerformanceCounter(NULL);
    ULONG ms = (qpf > 0) ? (ULONG)(((t1.QuadPart - t0.QuadPart) * 1000LL) / qpf) : 0U;
    LogStr("[tree] "); LogStr(tag);
    LogStr(" dirs="); LogU32(dirsOk);
    LogStr(" files="); LogU32(filesOk);
    LogStr(" repSkip="); LogU32(repSkip);
    LogStr(" errs="); LogU32(errs);
    LogStr(" ms="); LogU32(ms);
    LogStr("\r\n");
}

static HANDLE OpenOsVolumeRoot(void) {
    /* Try \??\C:\ first; fallback to resolving \SystemRoot -> \Device\HarddiskVolumeX\ */
    UNICODE_STRING cUs;
    RtlInitUnicodeString(&cUs, L"\\??\\C:\\");
    OBJECT_ATTRIBUTES oa;
    InitOA(&oa, NULL, &cUs);
    IO_STATUS_BLOCK iosb;
    HANDLE hRoot = NULL;
    if (NT_SUCCESS(ZwOpenFile(&hRoot, FILE_LIST_DIRECTORY | WRITE_DAC | SYNCHRONIZE,
                              &oa, &iosb, FILE_SHARE_ALL,
                              FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT))) {
        return hRoot;
    }

    UNICODE_STRING srUs;
    RtlInitUnicodeString(&srUs, L"\\SystemRoot");
    InitOA(&oa, NULL, &srUs);
    HANDLE hLink = NULL;
    if (!NT_SUCCESS(ZwOpenSymbolicLinkObject(&hLink, SYMBOLIC_LINK_QUERY, &oa))) return NULL;

    WCHAR *buf = (WCHAR *)ExAllocatePool2(POOL_FLAG_PAGED, 1024, POOL_TAG);
    if (!buf) { ZwClose(hLink); return NULL; }

    UNICODE_STRING target;
    target.Length = 0;
    target.MaximumLength = 1020;
    target.Pad = 0;
    target.Buffer = buf;
    ULONG retLen = 0;
    NTSTATUS st = ZwQuerySymbolicLinkObject(hLink, &target, &retLen);
    ZwClose(hLink);

    if (NT_SUCCESS(st) && target.Length >= 4U) {
        /* Strip trailing \Windows -> keep trailing '\' on \Device\HarddiskVolumeX\ */
        ULONG chars = target.Length / 2U;
        while (chars > 1U && buf[chars - 1U] == L'\\') chars--;
        while (chars > 1U && buf[chars - 1U] != L'\\') chars--;
        if (chars > 1U && buf[chars - 1U] == L'\\') {
            target.Length = (USHORT)(chars * 2U);
            buf[chars] = 0;
            InitOA(&oa, NULL, &target);
            ZwOpenFile(&hRoot, FILE_LIST_DIRECTORY | WRITE_DAC | SYNCHRONIZE,
                       &oa, &iosb, FILE_SHARE_ALL,
                       FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
        }
    }
    ExFreePoolWithTag(buf, POOL_TAG);
    return hRoot;
}

static void DisableSelfInRegistry(PUNICODE_STRING regPath) {
    if (!regPath || !regPath->Buffer || !regPath->Length) return;
    OBJECT_ATTRIBUTES oa;
    InitOA(&oa, NULL, regPath);
    HANDLE hKey = NULL;
    NTSTATUS st = ZwOpenKey(&hKey, KEY_WRITE_VAL, &oa);
    if (NT_SUCCESS(st)) {
        UNICODE_STRING valName;
        RtlInitUnicodeString(&valName, L"Start");
        ULONG start4 = 4U;
        st = ZwSetValueKey(hKey, &valName, 0, REG_DWORD, &start4, sizeof(start4));
        ZwClose(hKey);
    }
    LogStr("[reg] Disable self Start=4 st="); LogHex((ULONG)st); LogStr("\r\n");
}

static BOOLEAN SentinelExists(HANDLE hRoot) {
    UNICODE_STRING us;
    RtlInitUnicodeString(&us, L"Users\\Public\\s23_aclfix.done");
    OBJECT_ATTRIBUTES oa;
    InitOA(&oa, hRoot, &us);
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    if (NT_SUCCESS(ZwOpenFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb,
                              FILE_SHARE_ALL, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE))) {
        ZwClose(h);
        return TRUE;
    }
    return FALSE;
}

static void WriteSentinel(HANDLE hRoot) {
    UNICODE_STRING us;
    RtlInitUnicodeString(&us, L"Users\\Public\\s23_aclfix.done");
    OBJECT_ATTRIBUTES oa;
    InitOA(&oa, hRoot, &us);
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    if (NT_SUCCESS(ZwCreateFile(&h, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                                FILE_ATTRIBUTE_NORMAL, FILE_SHARE_ALL, FILE_OVERWRITE_IF,
                                FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH | FILE_NON_DIRECTORY_FILE,
                                NULL, 0))) {
        static const char msg[] = "OK\r\n";
        LARGE_INTEGER off = { 0 };
        ZwWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)msg, 4, &off, NULL);
        ZwFlushBuffersFile(h, &iosb);
        ZwClose(h);
    }
}

/* Force non-empty .reloc for ARM64 /DYNAMICBASE */
static const void * volatile gRelocAnchors[2] = {
    (const void *)WalkTree,
    (const void *)OpenOsVolumeRoot,
};

NTSTATUS DriverEntry(PVOID DriverObject, PUNICODE_STRING RegistryPath) {
    (void)DriverObject;
    if (!gRelocAnchors[0] || !gRelocAnchors[1]) return STATUS_UNSUCCESSFUL;

    HANDLE hRoot = OpenOsVolumeRoot();
    if (!hRoot) return STATUS_UNSUCCESSFUL;

    /* Power-loss idempotency guard */
    if (SentinelExists(hRoot)) {
        DisableSelfInRegistry(RegistryPath);
        ZwClose(hRoot);
        return STATUS_UNSUCCESSFUL;
    }

    /* Open write-through log relative to volume root */
    UNICODE_STRING logUs;
    RtlInitUnicodeString(&logUs, L"Users\\Public\\s23_aclfix.log");
    OBJECT_ATTRIBUTES logOa;
    InitOA(&logOa, hRoot, &logUs);
    IO_STATUS_BLOCK iosb;
    g_logOff = 0;
    if (!NT_SUCCESS(ZwCreateFile(&g_log, FILE_WRITE_DATA | SYNCHRONIZE, &logOa, &iosb, NULL,
                                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_ALL, FILE_OVERWRITE_IF,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH | FILE_NON_DIRECTORY_FILE,
                                 NULL, 0))) {
        g_log = NULL;
    }

    LARGE_INTEGER freq = { 0 };
    LARGE_INTEGER tStart = KeQueryPerformanceCounter(&freq);
    LogStr("=== s23aclfix.sys v1.0.0.2 Start=1 DriverEntry ===\r\n");

    if (!InitAllSids() || !InitAllProfiles()) {
        LogStr("FATAL: InitAllSids/InitAllProfiles failed — aborting safely!\r\n");
        DisableSelfInRegistry(RegistryPath);
        if (g_log) { ZwFlushBuffersFile(g_log, &iosb); ZwClose(g_log); g_log = NULL; }
        ZwClose(hRoot);
        return STATUS_UNSUCCESSFUL;
    }
    LogStr("[init] All 11 SIDs and 9 self-relative SD profiles validated OK\r\n");

    PVOID dirBuf = ExAllocatePool2(POOL_FLAG_PAGED, DIR_BUF_SIZE, POOL_TAG);
    if (!dirBuf) {
        LogStr("FATAL: Failed to allocate 64KB dirBuf\r\n");
        DisableSelfInRegistry(RegistryPath);
        if (g_log) { ZwFlushBuffersFile(g_log, &iosb); ZwClose(g_log); g_log = NULL; }
        ZwClose(hRoot);
        return STATUS_UNSUCCESSFUL;
    }

    /* Phase 1: Root & non-recursive top-level directories + root S23 scripts */
    NTSTATUS rootSt = ZwSetSecurityObject(hRoot, DACL_SECURITY_INFORMATION, (PVOID)g_profRx.dirSd);
    LogStr("[one] VolumeRoot set="); LogHex((ULONG)rootSt); LogStr("\r\n");
    ApplySinglePath(hRoot, "Windows",         L"Windows",                 TRUE,  &g_profRx);
    ApplySinglePath(hRoot, "ServiceProfiles", L"Windows\\ServiceProfiles",TRUE,  &g_profRx);
    ApplySinglePath(hRoot, "Users",           L"Users",                   TRUE,  &g_profRx);
    ApplySinglePath(hRoot, "s23_diag.ps1",    L"s23_diag.ps1",            FALSE, &g_profRx);
    ApplySinglePath(hRoot, "s23_sys.ps1",     L"s23_sys_install.ps1",     FALSE, &g_profRx);
    ApplySinglePath(hRoot, "s23_agent.ps1",   L"s23_usb_agent.ps1",       FALSE, &g_profRx);

    /* Phase 2: Broad RX subtrees FIRST (before more-specific writable overlays!) */
    WalkTree(hRoot, dirBuf, "Windows\\System32",           L"Windows\\System32",           &g_profRx, TRUE,  freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\SysArm32",           L"Windows\\SysArm32",           &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\SysWOW64",           L"Windows\\SysWOW64",           &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\SystemApps",         L"Windows\\SystemApps",         &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\ImmersiveCP",        L"Windows\\ImmersiveControlPanel", &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\ShellExperiences",   L"Windows\\ShellExperiences",   &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\ShellComponents",    L"Windows\\ShellComponents",    &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\SystemResources",    L"Windows\\SystemResources",    &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\Resources",          L"Windows\\Resources",          &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\Fonts",              L"Windows\\Fonts",              &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\Globalization",      L"Windows\\Globalization",      &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\INF",                L"Windows\\INF",                &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\Registration",       L"Windows\\Registration",       &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\PolicyDefinitions",  L"Windows\\PolicyDefinitions",  &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\Security",           L"Windows\\Security",           &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\Web",                L"Windows\\Web",                &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\Microsoft.NET",      L"Windows\\Microsoft.NET",      &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\assembly",           L"Windows\\assembly",           &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\WinSxS",             L"Windows\\WinSxS",             &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Program Files",               L"Program Files",               &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Program Files (x86)",         L"Program Files (x86)",         &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "s23_touch_pkg",               L"s23_touch_pkg",               &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "s23_batt_pkg",                L"s23_batt_pkg",                &g_profRx, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "s23_wifi_pkg",                L"s23_wifi_pkg",                &g_profRx, FALSE, freq.QuadPart);

    /* Phase 3: Specific writable service & Windows runtime overlays AFTER System32 */
    WalkTree(hRoot, dirBuf, "LocalService",                L"Windows\\ServiceProfiles\\LocalService",   &g_profLocalSvc, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "NetworkService",              L"Windows\\ServiceProfiles\\NetworkService", &g_profNetSvc,   FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\Temp",               L"Windows\\Temp",                            &g_profTemp,     FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "System32\\winevt",            L"Windows\\System32\\winevt",                &g_profSvcRw,    FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "System32\\LogFiles",          L"Windows\\System32\\LogFiles",              &g_profSvcRw,    FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "System32\\Tasks",             L"Windows\\System32\\Tasks",                 &g_profSvcRw,    FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "System32\\catroot2",          L"Windows\\System32\\catroot2",              &g_profSvcRw,    FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "System32\\spool",             L"Windows\\System32\\spool",                 &g_profSvcRw,    FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "System32\\sru",               L"Windows\\System32\\sru",                   &g_profSvcRw,    FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "System32\\wdi",               L"Windows\\System32\\wdi",                   &g_profSvcRw,    FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "System32\\config\\sysprof",   L"Windows\\System32\\config\\systemprofile", &g_profSvcRw,    FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\ServiceState",       L"Windows\\ServiceState",                    &g_profSvcRw,    FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Windows\\Logs",               L"Windows\\Logs",                            &g_profSvcRw,    FALSE, freq.QuadPart);

    /* Phase 4: ProgramData & Users (broad -> specific) */
    WalkTree(hRoot, dirBuf, "ProgramData",                 L"ProgramData",                              &g_profProgramData, FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Users\\Default",              L"Users\\Default",                           &g_profRx,          FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Users\\Public",               L"Users\\Public",                            &g_profPublic,      FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Users\\s23",                  L"Users\\s23",                               &g_profUserS23,     FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Users\\s23\\Packages",        L"Users\\s23\\AppData\\Local\\Packages",     &g_profUserPkgs,    FALSE, freq.QuadPart);
    WalkTree(hRoot, dirBuf, "Users\\s23\\Temp",            L"Users\\s23\\AppData\\Local\\Temp",         &g_profUserPkgs,    FALSE, freq.QuadPart);

    ExFreePoolWithTag(dirBuf, POOL_TAG);

    DisableSelfInRegistry(RegistryPath);
    WriteSentinel(hRoot);

    LARGE_INTEGER tEnd = KeQueryPerformanceCounter(NULL);
    ULONG totalMs = (freq.QuadPart > 0) ? (ULONG)(((tEnd.QuadPart - tStart.QuadPart) * 1000LL) / freq.QuadPart) : 0U;
    LogStr("=== s23aclfix.sys COMPLETE in "); LogU32(totalMs); LogStr(" ms — continuing normal boot! ===\r\n");

    if (g_log) {
        ZwFlushBuffersFile(g_log, &iosb);
        ZwClose(g_log);
        g_log = NULL;
    }
    ZwClose(hRoot);

    /* Return STATUS_UNSUCCESSFUL (ErrorControl=0) so we unload cleanly and boot continues to smss/services/desktop! */
    return STATUS_UNSUCCESSFUL;
}
