//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
//
// This source code is licensed under Microsoft Shared Source License
// Version 1.0 for Windows CE.
// For a copy of the license visit http://go.microsoft.com/fwlink/?LinkId=3223.
//
//------------------------------------------------------------------------------
// 
//      NK Kernel loader code
// 
// 
// Module Name:
// 
//      loader.c
// 
// Abstract:
// 
//      This file implements the NK kernel loader for EXE/DLL
// 
// 
//------------------------------------------------------------------------------
//
// The loader is derived from the Win32 executable file format spec.  The only
// interesting thing is how we load a process.  When we load a process, we
// simply schedule a thread in a new process context.  That thread (in coredll)
// knows how to read in an executable, and does so.  Then it traps into the kernel
// and starts running in the new process.  Most of the code below is simply a direct
// implimentation of the executable file format specification
//
//------------------------------------------------------------------------------

#include "kernel.h"
#include "altimports.h"
#include "bldver.h"

#define SYSTEMDIR L"\\Windows\\"
#define SYSTEMDIRLEN 9

#define LOAD_LIBRARY_IN_KERNEL  0x8000

struct KDataStruct *PtrKData;

fslog_t *LogPtr;
BOOL fForceCleanBoot;
BOOL fNoDebugger;

FREEINFO FreeInfo[MAX_MEMORY_SECTIONS];
MEMORYINFO MemoryInfo;


ROMChain_t FirstROM;
ROMChain_t *ROMChain, *OEMRomChain;

PROMINFO g_pROMInfo;

extern CRITICAL_SECTION VAcs, DbgApiCS, LLcs, ModListcs, PagerCS;
extern BOOL IsCommittedSecureSlot (DWORD addr);
extern PMODULE LoadMUI (HANDLE hModule, LPBYTE BasePtr, e32_lite* eptr);

DWORD   ROMDllLoadBase;     // this is the low water mark for DLL loaded into per-process's slot
DWORD   SharedDllBase;      // base of dlls loaded in slot 1

extern BOOL (*g_pfnInitLoaderHook)(DWORD, LPCTSTR);
extern PMODULE (*g_pfnWhichMod)(PMODULE, LPCTSTR, LPCTSTR, DWORD, DWORD);
extern BOOL (*g_pfnUndoDepends)(e32_lite *eptr, DWORD BaseAddr);

DWORD MainMemoryEndAddress, PagedInCount;
DWORD (*pNKEnumExtensionDRAM)(PMEMORY_SECTION pMemSections, DWORD cMemSections);
DWORD (*pOEMCalcFSPages)(DWORD dwMemPages, DWORD dwDefaultFSPages);
DWORD dwOEMCleanPages;

typedef void (* NFC_t)(DWORD, DWORD, DWORD, DWORD);
NFC_t pNotifyForceCleanboot;
extern Name *pPath;
extern Name *pInjectDLLs;
extern HANDLE hCoreDll;

BOOL IsPreloadedDlls (PMODULE pMod)
{
    if (((HANDLE) pMod == hCoreDll) || !strcmpW (L"mscoree.dll", pMod->lpszModName))
        return TRUE;
    if (pInjectDLLs) {
        LPTSTR p = (LPTSTR)(pInjectDLLs->name);
        while(*p) {
            if (!strcmpW (pMod->lpszModName, p))
                return TRUE;
            p += (strlenW(p)+1);
        }
    }
    return FALSE;
}

#ifdef SH3
extern unsigned int SH3DSP;
#endif

ROMHDR *const volatile pTOC = (ROMHDR *)-1;     // Gets replaced by RomLoader with real address

// ROM Header extension.  The ROM loader (RomImage) will set the pExtensions field of the table
// of contents to point to this structure.  This structure contains the PID and a extra field to point
// to further extensions.
const ROMPID RomExt = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    NULL
};

DWORD DecompressROM(LPBYTE BufIn, DWORD InSize, LPBYTE BufOut, DWORD OutSize, DWORD skip);
void GoToUserTime(void);
void GoToKernTime(void);

WCHAR lowerW(WCHAR ch) {
    return ((ch >= 'A') && (ch <= 'Z')) ? (ch - 'A' + 'a') : ch;
}

int strcmponeiW(const wchar_t *pwc1, const wchar_t *pwc2) {
    while (*pwc1 && (lowerW(*pwc1) == *pwc2)) {
        pwc1++;
        pwc2++;
    }
    return (*pwc1 ? 1 : 0);
}

int strcmpdllnameW(LPCWSTR src, LPCWSTR tgt) {
    while (*src && (*src == lowerW(*tgt))) {
        src++;
        tgt++;
    }
    return ((*tgt && strcmponeiW(tgt,L".dll") && strcmponeiW(tgt,L".cpl")) || (*src && memcmp(src,L".dll",10) && memcmp(src,L".cpl",10))) ? 1 : 0;
}

int strcmpiAandW(LPCHAR lpa, LPWSTR lpu) {
    while (*lpa && (lowerW((WCHAR)*lpa) == lowerW(*lpu))) {
        lpa++;
        lpu++;
    }
    return ((*lpa || *lpu) ? 1 : 0);
}

int kstrncmpi(LPWSTR str1, LPWSTR str2, int count) {
    wchar_t f,l;
    if (!count)
        return 0;
    do {
        f = lowerW(*str1++);
        l = lowerW(*str2++);
    } while (--count && f && (f == l));
    return (int)(f - l);
}

int kstrcmpi(LPWSTR str1, LPWSTR str2) {
    wchar_t f,l;
    do  {
        f = lowerW(*str1++);
        l = lowerW(*str2++);
    } while (f && (f == l));
    return (int)(f - l);
}

void kstrcpyW(LPWSTR p1, LPCWSTR p2) {
    while (*p2)
        *p1++ = *p2++;
    *p1 = 0;
}

o32_lite *FindOptr (o32_lite *optr, int nCnt, DWORD dwAddr)
{
    for (dwAddr = ZeroPtr (dwAddr) ; nCnt; nCnt --, optr ++) {
        if ((DWORD) (dwAddr - ZeroPtr(optr->o32_realaddr)) < optr->o32_vsize) {
            return optr;
        }
    }

    return NULL;
}

typedef BOOL (* OEMLoadInit_t)(LPWSTR lpszName);
typedef BOOL (* OEMLoadModule_t)(LPBYTE lpData, DWORD cbData);

OEMLoadInit_t pOEMLoadInit;
OEMLoadModule_t pOEMLoadModule;

ERRFALSE(!OEM_CERTIFY_FALSE);

ULONG FakeKDTrap (PEXCEPTION_RECORD ExceptionRecord, CONTEXT *ContextRecord, BOOLEAN SecondChance);

//------------------------------------------------------------------------------
// check if a particular module can be debugged
//------------------------------------------------------------------------------
BOOL ChkDebug (openexe_t *oeptr)
{
    BOOL fRet = TRUE, dwAttrib;

    DEBUGCHK (FT_OBJSTORE != oeptr->filetype);

    dwAttrib = (FT_ROMIMAGE == oeptr->filetype)? oeptr->tocptr->dwFileAttributes : oeptr->dwExtRomAttrib;

    if (dwAttrib & MODULE_ATTR_NODEBUG) {
        if (pCurProc->hDbgrThrd || pCurProc->DbgActive || (FakeKDTrap != KDTrap)) {
            fRet = FALSE;
        } else {
            // set both the process flag and the global flag to prevent loading KD 
            // or attaching debugger to this process
            pCurProc->fNoDebug = fNoDebugger = TRUE;
        }
    }
    DEBUGMSG(ZONE_LOADER1,(TEXT("ChkDebug returns %d\r\n"), fRet));

    return fRet;
}

//------------------------------------------------------------------------------
// read O32/E32 from filesys for FT_EXTIMAGE
//------------------------------------------------------------------------------
BOOL ReadExtImageInfo (HANDLE hf, DWORD dwCode, DWORD cbSize, LPVOID pBuf)
{
    DWORD cbRead;
    return SC_DeviceIoControl (hf, dwCode, NULL, 0, pBuf, cbSize, &cbRead, NULL)
        && (cbSize == cbRead);
}


//------------------------------------------------------------------------------
// returns getlasterror code, or 0 for success
//------------------------------------------------------------------------------
DWORD 
VerifyBinary(
    openexe_t *oeptr, 
    LPWSTR lpszName, 
    LPBYTE pbTrustLevel
    ) 
{
#define VBSIZE 1024
    BYTE Buf[VBSIZE];
    int iTrust;
    DWORD len, pos, size, bytesread;

    // for files in ROM -- fully trusted unless specified otherwise in the flag
    // NOTE: we perform the test before testing pOEMLoadInit/pOEMLoadModule so
    //       OEM can have a close system with trusted model without implementing
    //       pOEMLoadInit/pOEMLoadModule. However, if an OEM has RAM filesys, he
    //       must implement the functions or files in RAM will be fully trusted.
    if (FT_OBJSTORE != oeptr->filetype) {
        DWORD dwAttrib = (FT_ROMIMAGE == oeptr->filetype)? oeptr->tocptr->dwFileAttributes : oeptr->dwExtRomAttrib;

        if (!ChkDebug (oeptr)) {
            return (DWORD)NTE_BAD_SIGNATURE;
        }
        if (dwAttrib & MODULE_ATTR_NOT_TRUSTED) {
            *pbTrustLevel = KERN_TRUST_RUN;
        }
        return 0;
    }

    // ALWAYS FULLY TRUSTED if ALLKMODE
    if (bAllKMode) {
        return 0;
    }

    // only files in ROM trusted?
    if (pTOC->ulKernelFlags & KFLAG_TRUSTROMONLY) {
        *pbTrustLevel = KERN_TRUST_RUN;
        return 0;
    }

    // if the image isn't built with TRUST model, always fully trusted
    if (!pOEMLoadInit || !pOEMLoadModule) {
        return 0;
    }
    if (!pOEMLoadInit(lpszName)) {
        return (DWORD)NTE_BAD_SIGNATURE;
    }

    len = SetFilePointer(oeptr->hf,0,0,2);

    for (pos = 0; pos < len; pos += size) {
        size = ((len - pos) > VBSIZE ? VBSIZE : (len - pos));
        if (!ReadFileWithSeek(oeptr->hf,Buf,size,&bytesread,0,pos,0) || (bytesread != size)) {
            return (DWORD)NTE_BAD_SIGNATURE;
        }    
        if (!pOEMLoadModule(Buf,size)) {
            return (DWORD)NTE_BAD_SIGNATURE;
        }
    }

    if (!(iTrust = pOEMLoadModule(0,0))) {
        return (DWORD)NTE_BAD_SIGNATURE;
    }
    if (iTrust != OEM_CERTIFY_TRUST) {
        *pbTrustLevel = KERN_TRUST_RUN;
    }
    return 0;
}


//------------------------------------------------------------------------------
// change DllLoadBase (can only be called from filesys
//------------------------------------------------------------------------------
BOOL SetROMDllBase (DWORD cbSize, PROMINFO pInfo)
{
    LPVOID pSlot0Base = NULL;
    BOOL   fRet = FALSE;

    if (!pInfo) {
        DEBUGMSG (1, (L"SetROMDllBase: Failed! (NULL ROMINFO)\r\n"));
        return FALSE;
    }

    DEBUGCHK (cbSize == (sizeof(ROMINFO) + pInfo->nROMs * sizeof (LARGE_INTEGER)));

    // grab loader CS 
    EnterCriticalSection (&LLcs);

    // check parameters
    if ((pCurProc->procnum != 1)                        // only filesys can make this call
        || g_pROMInfo                                   // ROMInfo already set
        || (ROMDllLoadBase != (DWORD) DllLoadBase)      // any non-ROM dll has been loaded
        || (pInfo->nROMs < 1)                           // at least one ROM
        || (cbSize > HEAP_SIZE1)                        // too many ROMs
        ) {
        DEBUGMSG (1, (L"SetROMDllBase faild, (slot0 = %8.8lx, slot1 = %8.8lx, Cnt = %d, cbSize = %d)\r\n", 
                    pInfo->dwSlot_0_DllBase, pInfo->dwSlot_1_DllBase, pInfo->nROMs, cbSize));

    // allocate memory for ROMINFO
    } else if (!(g_pROMInfo = (PROMINFO) AllocMem (HEAP_ROMINFO))) {
        DEBUGMSG (1, (L"SetROMDllBase: AllocMem failed!\r\n"));

    // reserve memory in the secure section
    } else if ((pInfo->dwSlot_0_DllBase < ROMDllLoadBase)
        && !(pSlot0Base = VirtualAlloc ((LPVOID)(ProcArray[0].dwVMBase+pInfo->dwSlot_0_DllBase), ROMDllLoadBase - pInfo->dwSlot_0_DllBase,
                            MEM_RESERVE|MEM_IMAGE, PAGE_NOACCESS))) {
        DEBUGMSG (1, (L"SetROMDllBase: VirtualAlloc Failed (%8.8lx -> %8.8lx)\r\n", pInfo->dwSlot_0_DllBase, ROMDllLoadBase));

    // reserve memory in slot 1
    } else if ((pInfo->dwSlot_1_DllBase < SharedDllBase)
        && !VirtualAlloc ((LPVOID)pInfo->dwSlot_1_DllBase, SharedDllBase - pInfo->dwSlot_1_DllBase,
                            MEM_RESERVE|MEM_IMAGE, PAGE_NOACCESS)) {
        DEBUGMSG (1, (L"SetROMDllBase: VirtualAlloc Failed (%8.8lx -> %8.8lx)\r\n", pInfo->dwSlot_1_DllBase, SharedDllBase));

    // succeed, save ROMINFO
    } else {
        int i, j;
        // reserve the R/W section for all existing processes
        for (i = 0; i < MAX_PROCESSES; i ++) {
            if (ProcArray[i].dwVMBase) {
                LARGE_INTEGER *pLimits = (LARGE_INTEGER *) (pInfo + 1);
                for (j = 0; j < pInfo->nROMs; j ++, pLimits ++) {
                    VirtualAlloc((LPVOID)(pLimits->LowPart + ProcArray[i].dwVMBase), pLimits->HighPart - pLimits->LowPart, MEM_RESERVE|MEM_IMAGE, PAGE_NOACCESS);
                }
            }
        }
    
        memcpy (g_pROMInfo, pInfo, cbSize);
        if (pInfo->dwSlot_0_DllBase < ROMDllLoadBase)
            ROMDllLoadBase = DllLoadBase = pInfo->dwSlot_0_DllBase;
        if (pInfo->dwSlot_1_DllBase < SharedDllBase)
            SharedDllBase = pInfo->dwSlot_1_DllBase;
        fRet = TRUE;
        DEBUGMSG (1, (L"SetROMDllBase: Change DllLoadBase to %8.8lx, %8.8lx\r\n", 
                    ROMDllLoadBase, SharedDllBase));
    }

    // free memory on error
    if (!fRet) {
        if (g_pROMInfo) {
            FreeMem (g_pROMInfo, HEAP_ROMINFO);
            g_pROMInfo = NULL;
        }
        if (pSlot0Base) {
            VirtualFree (pSlot0Base, 0, MEM_RELEASE);
        }
    }

    // release Loader CS
    LeaveCriticalSection (&LLcs);
    return fRet;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
BOOL 
OpenExe(
    LPWSTR lpszName, 
    openexe_t *oeptr, 
    BOOL bIsDLL, 
    BOOL bAllowPaging
    ) 
{
    static OEinfo_t OEinfo;
    return SafeOpenExe(lpszName, oeptr, bIsDLL, bAllowPaging, &OEinfo);
}

//------------------------------------------------------------------------------
//  OpenFileFromFilesys: call into filesys to open a file
//
//  return: 0                       -- success
//          ERROR_FILE_NOT_FOUND    -- file doesn't exist
//          ERROR_FILE_CORRUPT      -- file is corrupted
//------------------------------------------------------------------------------

DWORD OpenFileFromFilesys (
    LPWSTR lpszName, 
    openexe_t *oeptr, 
    BOOL bAllowPaging
    )
{
    DWORD bytesread;
    BY_HANDLE_FILE_INFORMATION bhfi;

    // try open the file
    if ((oeptr->hf = CreateFileW (lpszName, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0)) == INVALID_HANDLE_VALUE)
        return ERROR_FILE_NOT_FOUND;

    // get the information of the file
    if (GetFileInformationByHandle (oeptr->hf, &bhfi) && ((ULONG)INVALID_HANDLE_VALUE != bhfi.dwOID)) {
        FreeName(oeptr->lpName);
        // is this an extern ROM MODULE?
        if (bhfi.dwFileAttributes & FILE_ATTRIBUTE_ROMMODULE) {
            oeptr->pagemode = (bAllowPaging && !(pTOC->ulKernelFlags & KFLAG_DISALLOW_PAGING))? PM_FULLPAGING : PM_NOPAGING;
            oeptr->filetype = FT_EXTIMAGE;
            oeptr->dwExtRomAttrib = bhfi.dwFileAttributes;
            oeptr->bIsOID = 0;
            if (oeptr->lpName = AllocName ((strlenW (lpszName)+1)*2)) {
                kstrcpyW (oeptr->lpName->name, lpszName);
            }
            return 0;   // succeed
        } else {
            oeptr->ceOid = bhfi.dwOID;
        }
    } else {

        // no information available, keep the name
        // NOTE: extern ROM MODULE not supported if can't get information
        LPName pName;
        oeptr->bIsOID = 0;
        if (pName = AllocName ((strlenW (lpszName)+1)*2)) {
            FreeName (oeptr->lpName);
            oeptr->lpName = pName;
        }
        kstrcpyW (oeptr->lpName->name, lpszName);
    }
    
    oeptr->filetype = FT_OBJSTORE;
    oeptr->pagemode = (bAllowPaging
        && !(pTOC->ulKernelFlags & KFLAG_DISALLOW_PAGING)
        && ReadFileWithSeek(oeptr->hf,0,0,0,0,0,0))? PM_FULLPAGING : PM_NOPAGING;

    // read the offset information from PE header
    if ((SetFilePointer(oeptr->hf,0x3c,0,FILE_BEGIN) != 0x3c) ||
        !ReadFile(oeptr->hf,(LPBYTE)&oeptr->offset,4,&bytesread,0) || (bytesread != 4)) {
        CloseExe(oeptr);
        oeptr->hf = INVALID_HANDLE_VALUE;
        return ERROR_FILE_CORRUPT;
    }
    DEBUGMSG(ZONE_OPENEXE,(TEXT("OpenExe %s: OSTORE: %8.8lx\r\n"),lpszName,oeptr->hf));
    
    return 0;   // succeeded
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
BOOL 
SafeOpenExe(
    LPWSTR lpszName, 
    openexe_t *oeptr, 
    BOOL bIsDLL, 
    BOOL bAllowPaging, 
    OEinfo_t *poeinfo
    ) 
{
    CEGUID sysguid;
    TOCentry *tocptr;
    int loop, plainlen, totlen, copylen;
    LPWSTR nameptr, p2;
    BOOL inWin, isAbs;
    DWORD dwLastError;
    HANDLE hFind;
    DWORD  dwErr;

    DEBUGMSG (ZONE_OPENEXE,(TEXT("OpenExe: %s\r\n"),lpszName));

    dwLastError = KGetLastError(pCurThread);
    isAbs = ((*lpszName == '\\') || (*lpszName == '/'));
    totlen = strlenW(lpszName);
    nameptr = lpszName + totlen;
    while ((nameptr != lpszName) && (*nameptr != '\\') && (*nameptr != '/'))
        nameptr--;
    if ((*nameptr == '\\') || (*nameptr == '/'))
        nameptr++;
    inWin = 0;
    if ((nameptr != lpszName) && (nameptr != lpszName+1)) {
        if (((nameptr == lpszName + SYSTEMDIRLEN) && !kstrncmpi(lpszName,SYSTEMDIR,SYSTEMDIRLEN)) ||
            ((nameptr == lpszName + SYSTEMDIRLEN - 1) && !kstrncmpi(lpszName,SYSTEMDIR+1,SYSTEMDIRLEN-1)))
            inWin = 1;
        else {
            memcpy(poeinfo->tmpname,lpszName,(nameptr-lpszName-1)*sizeof(WCHAR));
            poeinfo->tmpname[nameptr-lpszName-1] = 0;
            hFind = FindFirstFile(poeinfo->tmpname,&poeinfo->wfd);
            if (hFind != INVALID_HANDLE_VALUE) {
                CloseHandle(hFind);
                if (poeinfo->wfd.dwOID == 1)
                    inWin = 1;
            }
        }
    }
    p2 = (LPWSTR)poeinfo->plainname;
    while (*nameptr && (p2 - poeinfo->plainname < MAX_PATH-1))
        *p2++ = *nameptr++;
    *p2 = 0;
    plainlen = p2 - poeinfo->plainname;
    DEBUGMSG(ZONE_OPENEXE,(TEXT("OpenExe: plain name %s\r\n"),poeinfo->plainname));
    oeptr->bIsOID = 1;
    oeptr->lpName = 0;
    if (!(oeptr->lpName = AllocName(MAX_PATH*2))) {
        KSetLastError(pCurThread,dwLastError);
        return 0;
    }

    // check the file system
    if (SystemAPISets[SH_FILESYS_APIS]) {
        if (!isAbs) {
            // if dll, check in directory exe launched from
            if (bIsDLL) {
                PPROCESS pProc;
                pProc = (pCurThread->pcstkTop ? pCurThread->pcstkTop->pprcLast : pCurProc);
                if (pProc->oe.filetype != FT_ROMIMAGE) {
                    if (pProc->oe.bIsOID) {
                        CREATE_SYSTEMGUID(&sysguid);
                        poeinfo->ceoi.wVersion = CEOIDINFOEX_VERSION;
                        if (!(CeOidGetInfoEx2(&sysguid, pProc->oe.ceOid, &poeinfo->ceoi)))
                            poeinfo->ceoi.wObjType = OBJTYPE_INVALID;
                    } else {
                        kstrcpyW(poeinfo->ceoi.infFile.szFileName,pProc->oe.lpName->name);
                        poeinfo->ceoi.wObjType = OBJTYPE_INVALID;
                        if (FT_EXTIMAGE == pProc->oe.filetype)
                            poeinfo->ceoi.wObjType = OBJTYPE_INVALID;
                        else 
                            poeinfo->ceoi.wObjType = OBJTYPE_FILE;
                    }

                    if (OBJTYPE_FILE == poeinfo->ceoi.wObjType) {
                        nameptr = poeinfo->ceoi.infFile.szFileName + strlenW(poeinfo->ceoi.infFile.szFileName);
                        while ((*nameptr != '\\') && (*nameptr != '/') && (nameptr != poeinfo->ceoi.infFile.szFileName))
                            nameptr--;
                        if ((nameptr != poeinfo->ceoi.infFile.szFileName) && (MAX_PATH - 1 - (nameptr + 1 - poeinfo->ceoi.infFile.szFileName) >= plainlen)) {
                            memcpy(nameptr+1,poeinfo->plainname,plainlen*sizeof(WCHAR));
                            nameptr[plainlen+1] = 0;

                            dwErr = OpenFileFromFilesys (poeinfo->ceoi.infFile.szFileName, oeptr, bAllowPaging);

                            if (ERROR_FILE_NOT_FOUND != dwErr) {
                                if (!dwErr)
                                    // restore dwLastError when successful
                                    KSetLastError(pCurThread,dwLastError);
                                return !dwErr;
                            }
                        }
                    }
                }
            }
            oeptr->hf = INVALID_HANDLE_VALUE;
            dwErr = ERROR_FILE_NOT_FOUND;
            // check for file as path from root
            if (plainlen != totlen) {
                poeinfo->tmpname[0] = '\\';
                copylen = (totlen < MAX_PATH - 2 ? totlen : MAX_PATH - 2);
                memcpy((LPWSTR)poeinfo->tmpname+1,lpszName,totlen*sizeof(WCHAR));
                poeinfo->tmpname[1+copylen] = 0;
                dwErr = OpenFileFromFilesys ((LPWSTR)poeinfo->tmpname, oeptr, bAllowPaging);
            }
            if (ERROR_FILE_NOT_FOUND == dwErr) {
                // check for file in windows directory
                memcpy(poeinfo->tmpname,SYSTEMDIR,SYSTEMDIRLEN*sizeof(WCHAR));
                copylen = (plainlen + SYSTEMDIRLEN < MAX_PATH-1) ? plainlen : MAX_PATH - 2 - SYSTEMDIRLEN;
                memcpy((LPWSTR)poeinfo->tmpname+SYSTEMDIRLEN,poeinfo->plainname,copylen*sizeof(WCHAR));
                poeinfo->tmpname[SYSTEMDIRLEN+copylen] = 0;
                dwErr = OpenFileFromFilesys ((LPWSTR)poeinfo->tmpname, oeptr, bAllowPaging);
            }
            if (ERROR_FILE_NOT_FOUND == dwErr) {
                // check for file in root directory
                poeinfo->tmpname[0] = '\\';
                copylen = (plainlen + 1 < MAX_PATH-1) ? plainlen : MAX_PATH - 3;
                memcpy((LPWSTR)poeinfo->tmpname+1,poeinfo->plainname,copylen*sizeof(WCHAR));
                poeinfo->tmpname[1+copylen] = 0;
                dwErr = OpenFileFromFilesys ((LPWSTR)poeinfo->tmpname, oeptr, bAllowPaging);
            }
        } else {
            // check in main file system (will route to mounted if part of path)
            dwErr = OpenFileFromFilesys ((LPWSTR)lpszName, oeptr, bAllowPaging);
        }
        if (ERROR_FILE_NOT_FOUND != dwErr) {
            if (!dwErr)
                // restore dwLastError when successful
                KSetLastError(pCurThread,dwLastError);
            return !dwErr;
        }
    }
    if ((plainlen == totlen) || inWin) {
        ROMChain_t *pROM;
        o32_rom *o32rp;

        for (pROM = ROMChain; pROM; pROM = pROM->pNext) {
            // check ROM for any copy
            tocptr = (TOCentry *)(pROM->pTOC+1);  // toc entries follow the header
            for (loop = 0; loop < (int)pROM->pTOC->nummods; loop++) {
                if (!strcmpiAandW(tocptr->lpszFileName,poeinfo->plainname)) {
                    DEBUGMSG(ZONE_OPENEXE,(TEXT("OpenExe %s: ROM: %8.8lx\r\n"),lpszName,tocptr));
                    o32rp = (o32_rom *)(tocptr->ulO32Offset);
                    DEBUGMSG(ZONE_LOADER1,(TEXT("(10) o32rp->o32_realaddr = %8.8lx\r\n"), o32rp->o32_realaddr));
                    oeptr->tocptr = tocptr;
                    oeptr->filetype = FT_ROMIMAGE;
                    oeptr->pagemode = (bAllowPaging && !(pTOC->ulKernelFlags & KFLAG_DISALLOW_PAGING)) ? PM_FULLPAGING : PM_NOPAGING;
                    KSetLastError(pCurThread,dwLastError);
                    if (oeptr->lpName)
                        FreeName(oeptr->lpName,);
                    return 1;
                }
                tocptr++;
            }
        }
    }
    if (!isAbs && pPath) {
        // check the alternative search path
        LPWSTR pTrav = pPath->name;
        int len;
        while (*pTrav) {
            len = strlenW(pTrav);
            memcpy(poeinfo->tmpname,pTrav,len*sizeof(WCHAR));
            copylen = (totlen + len < MAX_PATH-1) ? totlen : MAX_PATH - 2 - len;
            memcpy((LPWSTR)poeinfo->tmpname+len,lpszName,copylen*sizeof(WCHAR));
            poeinfo->tmpname[len+copylen] = 0;

            dwErr = OpenFileFromFilesys ((LPWSTR)poeinfo->tmpname, oeptr, bAllowPaging);

            if (ERROR_FILE_NOT_FOUND != dwErr) {
                if (!dwErr)
                    // restore dwLastError when successful
                    KSetLastError(pCurThread,dwLastError);
                return !dwErr;
            }
            
            pTrav += len + 1;
        }
    }

    DEBUGMSG (ZONE_OPENEXE,(TEXT("OpenExe %s: failed!\r\n"),lpszName));
    // If not, it failed!
    if (oeptr->lpName)
        FreeName(oeptr->lpName);
    return 0;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
CloseExe(
    openexe_t *oeptr
    ) 
{
    CALLBACKINFO cbi;

    if (oeptr && oeptr->filetype) {
        cbi.hProc = ProcArray[0].hProc;

        if (oeptr->filetype != FT_ROMIMAGE) {
            cbi.pfn = (FARPROC)SC_CloseHandle;
            cbi.pvArg0 = (LPVOID)oeptr->hf;
            PerformCallBack4Int(&cbi);
        }

        if (!oeptr->bIsOID && oeptr->lpName)
            FreeName(oeptr->lpName);
        oeptr->lpName = 0;
        oeptr->filetype = 0;
    }
}



//------------------------------------------------------------------------------
//  @doc INTERNAL
//  @func void | CloseProcOE | Closes down the internal file handle for the process
//  @comm Used by apis.c in final process termination before doing final kernel syscall
//------------------------------------------------------------------------------
void 
SC_CloseProcOE(
    DWORD bCloseFile
    ) 
{
    THREAD *pdbgr, *ptrav;

    DEBUGMSG(ZONE_ENTRY,(L"SC_CloseProcOE entry\r\n"));
    if (bCloseFile == 2) {
        if (!GET_DYING(pCurThread))
            SetUserInfo(hCurThread,KGetLastError(pCurThread));
        SET_DYING(pCurThread);
        SET_DEAD(pCurThread);
        DEBUGMSG(ZONE_ENTRY,(L"SC_CloseProcOE exit (2)\r\n"));
        return;
    }
    EnterCriticalSection(&DbgApiCS);
    if (pCurThread->pThrdDbg && pCurThread->pThrdDbg->hEvent) {
        if (pCurProc->hDbgrThrd) {
            pdbgr = HandleToThread(pCurProc->hDbgrThrd);
            if (pdbgr->pThrdDbg->hFirstThrd == hCurThread)
                pdbgr->pThrdDbg->hFirstThrd = pCurThread->pThrdDbg->hNextThrd;
            else {
                ptrav = HandleToThread(pdbgr->pThrdDbg->hFirstThrd);
                while (ptrav->pThrdDbg->hNextThrd != hCurThread)
                    ptrav = HandleToThread(ptrav->pThrdDbg->hNextThrd);
                    ptrav->pThrdDbg->hNextThrd = pCurThread->pThrdDbg->hNextThrd;
            }
        }
        pCurThread->pThrdDbg->dbginfo.dwDebugEventCode = 0;
        SetEvent(pCurThread->pThrdDbg->hEvent);
        CloseHandle(pCurThread->pThrdDbg->hEvent);
        pCurThread->pThrdDbg->hEvent = 0;
        pCurThread->pThrdDbg->dbginfo.dwDebugEventCode = 0;
        if (pCurThread->pThrdDbg->hBlockEvent) {
            CloseHandle(pCurThread->pThrdDbg->hBlockEvent);
            pCurThread->pThrdDbg->hBlockEvent = 0;
        }
    }
    LeaveCriticalSection(&DbgApiCS);
    if (bCloseFile) {
        KDUpdateSymbols(((DWORD)pCurProc->BasePtr)+1, TRUE);
        CloseExe(&pCurProc->oe);
        CloseAllCrits();
    }
    DEBUGMSG(ZONE_ENTRY,(L"SC_CloseProcOE exit\r\n"));
}




#ifdef DEBUG

LPWSTR e32str[STD_EXTRA] = {
    TEXT("EXP"),TEXT("IMP"),TEXT("RES"),TEXT("EXC"),TEXT("SEC"),TEXT("FIX"),TEXT("DEB"),TEXT("IMD"),
    TEXT("MSP"),TEXT("TLS"),TEXT("CBK"),TEXT("RS1"),TEXT("RS2"),TEXT("RS3"),TEXT("RS4"),TEXT("RS5"),
};


//------------------------------------------------------------------------------
// Dump e32 header
//------------------------------------------------------------------------------
void 
DumpHeader(
    e32_exe *e32
    ) 
{
    int loop;

    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_magic      : %8.8lx\r\n"),*(LPDWORD)&e32->e32_magic));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_cpu        : %8.8lx\r\n"),e32->e32_cpu));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_objcnt     : %8.8lx\r\n"),e32->e32_objcnt));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_symtaboff  : %8.8lx\r\n"),e32->e32_symtaboff));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_symcount   : %8.8lx\r\n"),e32->e32_symcount));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_timestamp  : %8.8lx\r\n"),e32->e32_timestamp));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_opthdrsize : %8.8lx\r\n"),e32->e32_opthdrsize));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_imageflags : %8.8lx\r\n"),e32->e32_imageflags));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_coffmagic  : %8.8lx\r\n"),e32->e32_coffmagic));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_linkmajor  : %8.8lx\r\n"),e32->e32_linkmajor));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_linkminor  : %8.8lx\r\n"),e32->e32_linkminor));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_codesize   : %8.8lx\r\n"),e32->e32_codesize));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_initdsize  : %8.8lx\r\n"),e32->e32_initdsize));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_uninitdsize: %8.8lx\r\n"),e32->e32_uninitdsize));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_entryrva   : %8.8lx\r\n"),e32->e32_entryrva));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_codebase   : %8.8lx\r\n"),e32->e32_codebase));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_database   : %8.8lx\r\n"),e32->e32_database));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_vbase      : %8.8lx\r\n"),e32->e32_vbase));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_objalign   : %8.8lx\r\n"),e32->e32_objalign));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_filealign  : %8.8lx\r\n"),e32->e32_filealign));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_osmajor    : %8.8lx\r\n"),e32->e32_osmajor));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_osminor    : %8.8lx\r\n"),e32->e32_osminor));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_usermajor  : %8.8lx\r\n"),e32->e32_usermajor));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_userminor  : %8.8lx\r\n"),e32->e32_userminor));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_subsysmajor: %8.8lx\r\n"),e32->e32_subsysmajor));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_subsysminor: %8.8lx\r\n"),e32->e32_subsysminor));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_vsize      : %8.8lx\r\n"),e32->e32_vsize));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_hdrsize    : %8.8lx\r\n"),e32->e32_hdrsize));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_filechksum : %8.8lx\r\n"),e32->e32_filechksum));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_subsys     : %8.8lx\r\n"),e32->e32_subsys));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_dllflags   : %8.8lx\r\n"),e32->e32_dllflags));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_stackmax   : %8.8lx\r\n"),e32->e32_stackmax));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_stackinit  : %8.8lx\r\n"),e32->e32_stackinit));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_heapmax    : %8.8lx\r\n"),e32->e32_heapmax));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_heapinit   : %8.8lx\r\n"),e32->e32_heapinit));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_hdrextra   : %8.8lx\r\n"),e32->e32_hdrextra));
    for (loop = 0; loop < STD_EXTRA; loop++)
        DEBUGMSG(ZONE_LOADER1,(TEXT("  e32_unit:%3.3s    : %8.8lx %8.8lx\r\n"),
            e32str[loop],e32->e32_unit[loop].rva,e32->e32_unit[loop].size));
}



//------------------------------------------------------------------------------
// Dump o32 header
//------------------------------------------------------------------------------
void 
DumpSection(
    o32_obj *o32, 
    o32_lite *o32l
    ) 
{
    ulong loop;

    DEBUGMSG(ZONE_LOADER1,(TEXT("Section %a:\r\n"),o32->o32_name));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  o32_vsize      : %8.8lx\r\n"),o32->o32_vsize));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  o32_rva        : %8.8lx\r\n"),o32->o32_rva));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  o32_psize      : %8.8lx\r\n"),o32->o32_psize));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  o32_dataptr    : %8.8lx\r\n"),o32->o32_dataptr));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  o32_realaddr   : %8.8lx\r\n"),o32->o32_realaddr));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  o32_flags      : %8.8lx\r\n"),o32->o32_flags));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  o32l_vsize     : %8.8lx\r\n"),o32l->o32_vsize));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  o32l_rva       : %8.8lx\r\n"),o32l->o32_rva));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  o32l_realaddr  : %8.8lx\r\n"),o32l->o32_realaddr));
    DEBUGMSG(ZONE_LOADER1,(TEXT("  o32l_flags     : %8.8lx\r\n"),o32l->o32_flags));
    for (loop = 0; loop+16 <= o32l->o32_vsize; loop+=16) {
        DEBUGMSG(ZONE_LOADER2,(TEXT(" %8.8lx: %8.8lx %8.8lx %8.8lx %8.8lx\r\n"),
            loop,
            *(LPDWORD)(o32l->o32_realaddr+loop),
            *(LPDWORD)(o32l->o32_realaddr+loop+4),
            *(LPDWORD)(o32l->o32_realaddr+loop+8),
            *(LPDWORD)(o32l->o32_realaddr+loop+12)));
        }
    if (loop != o32l->o32_vsize) {
        DEBUGMSG(ZONE_LOADER2,(TEXT(" %8.8lx:"),loop));
        while (loop+4 <= o32l->o32_vsize) {
            DEBUGMSG(ZONE_LOADER2,(TEXT(" %8.8lx"),*(LPDWORD)(o32l->o32_realaddr+loop)));
            loop += 4;
        }
        if (loop+2 <= o32l->o32_vsize)
            DEBUGMSG(ZONE_LOADER2,(TEXT(" %4.4lx"),(DWORD)*(LPWORD)(o32l->o32_realaddr+loop)));
        DEBUGMSG(ZONE_LOADER2,(TEXT("\r\n")));
    }
}

#endif

//
// OAL can call this function to force a clean boot
//
void NKForceCleanBoot (void)
{
    fForceCleanBoot = TRUE;
    if (LogPtr) 
        LogPtr->magic1 = 0;
}


//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
SC_SetCleanRebootFlag(void) 
{
    TRUSTED_API_VOID (L"SC_SetCleanRebootFlag");

    DEBUGMSG(ZONE_ENTRY,(L"SC_SetCleanRebootFlag entry\r\n"));

    NKForceCleanBoot ();

    DEBUGMSG(ZONE_ENTRY,(L"SC_SetCleanRebootFlag exit\r\n"));
}



//------------------------------------------------------------------------------
// Relocate the kernel
//------------------------------------------------------------------------------
void 
KernelRelocate(
    ROMHDR *const pTOC
    )
{
    ULONG loop;
    COPYentry *cptr;

#ifdef DEBUG    
    if (pTOC == (ROMHDR *const) 0xffffffff) {
        OEMWriteDebugString(TEXT("ERROR: Kernel must be part of ROM image!\r\n"));
        while (1) ; 
    }
#endif
    KInfoTable[KINX_PTOC] = (long)pTOC;
    // This is where the data sections become valid... don't read globals until after this
    for (loop = 0; loop < pTOC->ulCopyEntries; loop++) {
        cptr = (COPYentry *)(pTOC->ulCopyOffset + loop*sizeof(COPYentry));
        if (cptr->ulCopyLen) {
            memcpy((LPVOID)cptr->ulDest,(LPVOID)cptr->ulSource,cptr->ulCopyLen);
        }
        if (cptr->ulCopyLen != cptr->ulDestLen) {
            memset((LPVOID)(cptr->ulDest+cptr->ulCopyLen),0,cptr->ulDestLen-cptr->ulCopyLen);
        }
    }
    // Now you can read global variables...
    PtrKData = &KData;  // Set this to allow displaying the kpage from the KD.
    MainMemoryEndAddress = pTOC->ulRAMEnd;
    FirstROM.pTOC = pTOC;
    FirstROM.pNext = 0;
    ROMChain = &FirstROM;
}




//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
SC_NotifyForceCleanboot(void) 
{
    if (pNotifyForceCleanboot)
        pNotifyForceCleanboot(0, 0, 0, 0);
}




// should match same define in filesys\heap\heap.c
#define MAX_FILESYSTEM_HEAP_SIZE 256*1024*1024

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
DWORD 
CalcFSPages(
    DWORD pages
    ) 
{
    DWORD fspages;

    if (pages <= 2*1024*1024/PAGE_SIZE) {
        fspages = (pages*(pTOC->ulFSRamPercent&0xff))/256;
    } else {
        fspages = ((2*1024*1024/PAGE_SIZE)*(pTOC->ulFSRamPercent&0xff))/256;
        if (pages <= 4*1024*1024/PAGE_SIZE) {
            fspages += ((pages-2*1024*1024/PAGE_SIZE)*((pTOC->ulFSRamPercent>>8)&0xff))/256;
        } else {
            fspages += ((2*1024*1024/PAGE_SIZE)*((pTOC->ulFSRamPercent>>8)&0xff))/256;
            if (pages <= 6*1024*1024/PAGE_SIZE) {
                fspages += ((pages-4*1024*1024/PAGE_SIZE)*((pTOC->ulFSRamPercent>>16)&0xff))/256;
            } else {
                fspages += ((2*1024*1024/PAGE_SIZE)*((pTOC->ulFSRamPercent>>16)&0xff))/256;
                fspages += ((pages-6*1024*1024/PAGE_SIZE)*((pTOC->ulFSRamPercent>>24)&0xff))/256;
            }
        }
    }
    return fspages;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
DWORD 
CarveMem(
    LPBYTE pMem, 
    DWORD dwExt, 
    DWORD dwLen, 
    DWORD dwFSPages
    ) 
{
    DWORD dwPages, dwNow, loop;
    LPBYTE pCur, pPage;

    dwPages = dwLen / PAGE_SIZE;
    pCur = pMem + dwExt;
    while (dwPages && dwFSPages) {
        pPage = pCur;
        pCur += PAGE_SIZE;
        dwPages--;
        dwFSPages--;
        *(LPBYTE *)pPage = LogPtr->pFSList;
        LogPtr->pFSList = pPage;
        dwNow = dwPages;
        if (dwNow > dwFSPages)
            dwNow = dwFSPages;
        if (dwNow > (PAGE_SIZE/4) - 2)
            dwNow = (PAGE_SIZE/4) - 2;
        *((LPDWORD)pPage+1) = dwNow;
        for (loop = 0; loop < dwNow; loop++) {
            *((LPDWORD)pPage+2+loop) = (DWORD)pCur;
            pCur += PAGE_SIZE;
        }
        dwPages -= dwNow;
        dwFSPages -= dwNow;
    }
    return dwFSPages;
}



void RemovePage(DWORD dwAddr);

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
GrabFSPages(void) 
{
    LPBYTE pPageList;
    DWORD loop,max;

    pPageList = LogPtr->pFSList;
    while (pPageList) {
        max = *((LPDWORD)pPageList+1);
        RemovePage((DWORD)pPageList);
        for (loop = 0; loop < max; loop++)
            RemovePage(*((LPDWORD)pPageList+2+loop));
        pPageList = *(LPBYTE *)pPageList;
    }
}

//------------------------------------------------------------------------------
//
// Divide the declared physical RAM between Object Store and User RAM.
//
//------------------------------------------------------------------------------
void 
KernelFindMemory(void) 
{
    ROMChain_t *pList, *pListPrev = NULL;
    extern const wchar_t NKCpuType [];
    MEMORY_SECTION MemSections[MAX_MEMORY_SECTIONS];
    DWORD cSections;
    DWORD dwRAMStart, dwRAMEnd;
    DWORD i;

    for (pList = OEMRomChain; pList; pList = pList->pNext) {
        if (pList->pTOC == pTOC) {
            // main region is already in the OEMROMChain, just use it
            ROMChain = OEMRomChain;
            break;
        }
        pListPrev = pList;
    }
    if (OEMRomChain && !pList) {
        DEBUGCHK (pListPrev);
        pListPrev->pNext = ROMChain;
        ROMChain = OEMRomChain;
    }

    // initialize Pageing Pool (MUST OCCUR BEFORE GETTING LogPtr since it'll change MemForPT)
    InitPgPool ();

    // *MUST* make the pointer uncached ( thus or in 0x20000000)
    //
    // LogPtr at the start of RAM holds some header information
    //
    LogPtr = (fslog_t *)PAGEALIGN_UP ((pTOC->ulRAMFree+MemForPT)| 0x20000000);

    RETAILMSG(1,(L"Booting Windows CE version %d.%2.2d for (%s)\r\n",CE_MAJOR_VER,CE_MINOR_VER, NKCpuType));
#ifdef MIPS
    if (IsMIPS16Supported)
        RETAILMSG (1, (L"MIPS16 Instructions Supported\r\n"));
    else
        RETAILMSG (1, (L"MIPS16 Instructions NOT Supported\r\n"));
#endif
    DEBUGMSG(1,(L"&pTOC = %8.8lx, pTOC = %8.8lx, pTOC->ulRamFree = %8.8lx, MemForPT = %8.8lx\r\n", &pTOC, pTOC, pTOC->ulRAMFree, MemForPT));

    if (LogPtr->version != CURRENT_FSLOG_VERSION) {
        RETAILMSG(1,(L"\r\nOld or invalid version stamp in kernel structures - starting clean!\r\n"));
        LogPtr->magic1 = LogPtr->magic2 = 0;
        LogPtr->version = CURRENT_FSLOG_VERSION;
    }

    if (fForceCleanBoot || (LogPtr->magic1 != LOG_MAGIC)) {
        DWORD fspages, mainpages, secondpages, cExtSections;

        //
        // Ask OEM if extension RAM exists.
        //
        if (pNKEnumExtensionDRAM) {
            cExtSections = (*pNKEnumExtensionDRAM)(MemSections, MAX_MEMORY_SECTIONS - 1);
            DEBUGCHK(cExtSections < MAX_MEMORY_SECTIONS);
        } else if (OEMGetExtensionDRAM(&MemSections[0].dwStart, &MemSections[0].dwLen)) {
            cExtSections = 1;
        } else {
            cExtSections = 0;
        }

        dwRAMStart = pTOC->ulRAMStart;
        dwRAMEnd = MainMemoryEndAddress;
        mainpages = (PAGEALIGN_DOWN(MainMemoryEndAddress) - PAGEALIGN_UP(pTOC->ulRAMFree+MemForPT))/PAGE_SIZE - 4096/PAGE_SIZE;

        //
        // Setup base RAM area
        //
        LogPtr->fsmemblk[0].startptr  = PAGEALIGN_UP(pTOC->ulRAMFree+MemForPT) + 4096;
        LogPtr->fsmemblk[0].extension = PAGEALIGN_UP(mainpages);
        mainpages -= LogPtr->fsmemblk[0].extension/PAGE_SIZE;
        LogPtr->fsmemblk[0].length    = mainpages * PAGE_SIZE;
        cSections = 1;

        //
        // Setup extension RAM sections
        //
        secondpages = 0;
        for (i = 0; i< cExtSections; i++) {
            if (MemSections[i].dwLen < (PAGEALIGN_UP(MemSections[i].dwStart) - MemSections[i].dwStart)) {
                MemSections[i].dwStart = MemSections[i].dwLen = 0;
            } else {
                MemSections[i].dwLen -= (PAGEALIGN_UP(MemSections[i].dwStart) - MemSections[i].dwStart);
                MemSections[i].dwStart = PAGEALIGN_UP(MemSections[i].dwStart);
                MemSections[i].dwLen = PAGEALIGN_DOWN(MemSections[i].dwLen);

                if (dwRAMStart > MemSections[i].dwStart)
                    dwRAMStart = MemSections[i].dwStart;
                if (dwRAMEnd < (MemSections[i].dwStart + MemSections[i].dwLen))
                    dwRAMEnd = MemSections[i].dwStart + MemSections[i].dwLen;

                LogPtr->fsmemblk[cSections].startptr = MemSections[i].dwStart;
                LogPtr->fsmemblk[cSections].extension = PAGEALIGN_UP(MemSections[i].dwLen / PAGE_SIZE);
                LogPtr->fsmemblk[cSections].length = MemSections[i].dwLen - LogPtr->fsmemblk[cSections].extension;
                secondpages += LogPtr->fsmemblk[cSections].length / PAGE_SIZE;
                cSections++;
            }
        }
        memset(&LogPtr->fsmemblk[cSections], 0, sizeof(mem_t) * (MAX_MEMORY_SECTIONS-cSections));

        fspages = CalcFSPages(mainpages+secondpages);

        // ask OAL to change fs pages if required
        if (pOEMCalcFSPages)
            fspages = (*pOEMCalcFSPages)(mainpages+secondpages, fspages);

        if (fspages * PAGE_SIZE > MAX_FILESYSTEM_HEAP_SIZE) {
            fspages = MAX_FILESYSTEM_HEAP_SIZE/PAGE_SIZE;
        }
        RETAILMSG(1,(L"Configuring: Primary pages: %d, Secondary pages: %d, Filesystem pages = %d\r\n",
            mainpages,secondpages,fspages));

        //
        // Now split the base and extension RAM sections into Object Store and
        // User RAM.
        //
        LogPtr->pFSList = 0;
        for (i = 0; i <cSections; i++) {
            fspages = CarveMem((LPBYTE)LogPtr->fsmemblk[i].startptr,LogPtr->fsmemblk[i].extension,LogPtr->fsmemblk[i].length,fspages);
        }
        OEMCacheRangeFlush (0, 0, CACHE_SYNC_WRITEBACK);
        //
        // Mark the header as initialized.
        //
        LogPtr->magic1 = LOG_MAGIC;
        LogPtr->magic2 = 0;
        RETAILMSG(1,(L"\r\nBooting kernel with clean memory configuration:\r\n"));
    } else {
        //
        // The magic number was found. Use the memory configuration already
        // set up at LogPtr.
        //
        RETAILMSG(1,(L"\r\nBooting kernel with existing memory configuration:\r\n"));
        dwRAMStart = pTOC->ulRAMStart;
        dwRAMEnd = MainMemoryEndAddress;
        cSections = 0;
        for (i = 0; i < MAX_MEMORY_SECTIONS; i++) {
            if (LogPtr->fsmemblk[i].length) {
                cSections++;
                if (dwRAMStart > LogPtr->fsmemblk[i].startptr)
                    dwRAMStart = LogPtr->fsmemblk[i].startptr;
                if (dwRAMEnd <(LogPtr->fsmemblk[i].startptr + LogPtr->fsmemblk[i].extension + LogPtr->fsmemblk[i].length))
                    dwRAMEnd = LogPtr->fsmemblk[i].startptr + LogPtr->fsmemblk[i].extension + LogPtr->fsmemblk[i].length;
            }
        }
    }

    RETAILMSG(1, (L"Memory Sections:\r\n"));
    for (i = 0; i <cSections; i++) {
        RETAILMSG(1,(L"[%d] : start: %8.8lx, extension: %8.8lx, length: %8.8lx\r\n",
                    i, LogPtr->fsmemblk[i].startptr,LogPtr->fsmemblk[i].extension,LogPtr->fsmemblk[i].length));
    }

    LogPtr->pKList = 0;
    MemoryInfo.pKData = (LPVOID)dwRAMStart;
    MemoryInfo.pKEnd  = (LPVOID)dwRAMEnd;
    MemoryInfo.cFi = cSections;
    MemoryInfo.pFi = &FreeInfo[0];
    for (i = 0; i < cSections; i++) {
        FreeInfo[i].pUseMap = (LPBYTE)LogPtr->fsmemblk[i].startptr;
        memset((LPVOID)LogPtr->fsmemblk[i].startptr, 0, LogPtr->fsmemblk[i].extension);
        FreeInfo[i].paStart = GetPFN(LogPtr->fsmemblk[i].startptr+LogPtr->fsmemblk[i].extension - PAGE_SIZE) + PFN_INCR;
        FreeInfo[i].paEnd = FreeInfo[i].paStart + PFN_INCR * (LogPtr->fsmemblk[i].length / PAGE_SIZE);
        FreeInfo[i].paRealEnd = FreeInfo[i].paEnd;
    }
    memset(&FreeInfo[cSections], 0, sizeof(FREEINFO) * (MAX_MEMORY_SECTIONS-cSections));

    GrabFSPages();
    KInfoTable[KINX_GWESHEAPINFO] = 0;
    KInfoTable[KINX_TIMEZONEBIAS] = 0;
}



//------------------------------------------------------------------------------
// Relocate a page
//------------------------------------------------------------------------------
BOOL 
RelocatePage(
    e32_lite *eptr, 
    o32_lite *optr, 
    ulong BasePtr, 
    ulong BaseAdj, 
    DWORD pMem, 
    DWORD pData, 
    DWORD prevdw
    ) 
{
    DWORD Comp1 = 0, Comp2;
#if defined(MIPS)
    DWORD PrevPage;
#endif
    struct info *blockptr, *blockstart;
    ulong blocksize;
    LPDWORD FixupAddress;
#ifndef x86
    LPWORD FixupAddressHi;
    BOOL MatchedReflo=FALSE;
#endif
    DWORD FixupValue;
    LPWORD currptr;
    DWORD offset;
    
#if defined(x86)
#define RELOC_LIMIT 8192
#else
#define RELOC_LIMIT 4096
#endif

    DWORD reloc_limit = RELOC_LIMIT;

#ifdef MIPS
    if (IsMIPS16Supported)
        reloc_limit = 8192;
#endif

    LeaveCriticalSection(&PagerCS);
    if (!(blocksize = eptr->e32_unit[FIX].size)) { // No relocations
        EnterCriticalSection(&PagerCS);
        return TRUE;
    }
    blockstart = blockptr = (struct info *)(ZeroPtr(BasePtr)+BaseAdj+eptr->e32_unit[FIX].rva);
    if (BaseAdj != ProcArray[0].dwVMBase)
        BaseAdj = 0;    // for processes
    DEBUGMSG(ZONE_LOADER1,(TEXT("Relocations: BasePtr = %8.8lx, BaseAdj = %8.8lx, VBase = %8.8lx, pMem = %8.8lx, pData = %8.8lx\r\n"),
        BasePtr,BaseAdj, eptr->e32_vbase, pMem, pData));
    if (!(offset = BasePtr - BaseAdj - eptr->e32_vbase)) {
        EnterCriticalSection(&PagerCS);
        DEBUGMSG(ZONE_LOADER1,(TEXT("Relocations: No Relocation Required\r\n")));
        return TRUE;                                                    // no adjustment needed
    }
    DEBUGMSG(ZONE_LOADER1,(TEXT("RelocatePage: Offset is %8.8lx\r\n"),offset));
    if ((ZeroPtr(pMem) >= ZeroPtr(BasePtr+eptr->e32_unit[FIX].rva)) &&
        (ZeroPtr(pMem) < ZeroPtr(BasePtr+eptr->e32_unit[FIX].rva+eptr->e32_unit[FIX].size))) {
        EnterCriticalSection(&PagerCS);
        return TRUE;
    }
    while (((ulong)blockptr < (ulong)blockstart + blocksize) && blockptr->size) {
        currptr = (LPWORD)(((ulong)blockptr)+sizeof(struct info));
        if ((ulong)currptr >= ((ulong)blockptr+blockptr->size)) {
            blockptr = (struct info *)(((ulong)blockptr)+blockptr->size);
            continue;
        }
        if ((ZeroPtr(BasePtr+blockptr->rva) > ZeroPtr(pMem)) || (ZeroPtr(BasePtr+blockptr->rva)+reloc_limit <= ZeroPtr(pMem))) {
            blockptr = (struct info *)(((ulong)blockptr)+blockptr->size);
            continue;
        }
        goto fixuppage;
    }
    EnterCriticalSection(&PagerCS);
    return TRUE;
fixuppage:
    DEBUGMSG(ZONE_LOADER1,(L"Fixing up %8.8lx %8.8lx, %8.8lx\r\n",blockptr->rva,optr->o32_rva,optr->o32_realaddr));
    while ((ulong)currptr < ((ulong)blockptr+blockptr->size)) {
#ifdef x86
        Comp1 = ZeroPtr(pMem);
        Comp2 = ZeroPtr(BasePtr + blockptr->rva);
        Comp1 = (Comp2 + 0x1000 == Comp1) ? 1 : 0;
#else
        Comp1 = ZeroPtr(pMem) - ZeroPtr(BasePtr + blockptr->rva);
        Comp2 = (DWORD)(*currptr&0xfff);
#if defined(MIPS)
        // Comp1 is relative start of page being relocated
        // Comp2 is relative address of fixup
        // For MIPS16 jump, deal with fixups on this page or the preceding page
        // For all other fixups, deal with this page only
        if (IsMIPS16Supported && (*currptr>>12 == IMAGE_REL_BASED_MIPS_JMPADDR16)) {
            if ((Comp1 > Comp2 + PAGE_SIZE) || (Comp1 + PAGE_SIZE <= Comp2)) {
                currptr++;
                continue;
            }
            // PrevPage: is fixup located on the page preceding the one being relocated?
            PrevPage = (Comp1 > Comp2);
            // Comp1: is fixup located in the block preceding the one that contains the current page?
            Comp1 = PrevPage && ((Comp1 & 0xfff) == 0);
        } else {
#endif
            if ((Comp1 > Comp2) || (Comp1 + PAGE_SIZE <= Comp2)) {
                if (*currptr>>12 == IMAGE_REL_BASED_HIGHADJ)
                    currptr++;
                currptr++;
                continue;
            }
#if defined(MIPS)
            // Comp1: is fixup located in the block preceding the one that contains the current page? (No.)
            if (IsMIPS16Supported)
                Comp1 = 0;
        }
#endif
#endif
#if defined(x86)
        FixupAddress = (LPDWORD)(((pData&0xfffff000) + (*currptr&0xfff)) - 4096*Comp1);
#else
        FixupAddress = (LPDWORD)((pData&0xfffff000) + (*currptr&0xfff));
#ifdef MIPS
        if (IsMIPS16Supported)
            FixupAddress = (LPDWORD) (FixupAddress - 4096*Comp1);
#endif
#endif
        DEBUGMSG(ZONE_LOADER2,(TEXT("type %d, low %8.8lx, page %8.8lx, addr %8.8lx\r\n"),
            (*currptr>>12)&0xf, (*currptr)&0x0fff,blockptr->rva,FixupAddress));
        switch (*currptr>>12) {
            case IMAGE_REL_BASED_ABSOLUTE: // Absolute - no fixup required.
                break;
#ifdef x86
            case IMAGE_REL_BASED_HIGHLOW: // Word - (32-bits) relocate the entire address.
                if (Comp1 && (((DWORD)FixupAddress & 0xfff) > 0xffc)) {
                    switch ((DWORD)FixupAddress & 0x3) {
                        case 1:
                            FixupValue = (prevdw>>8) + (*((LPBYTE)FixupAddress+3) <<24 ) + offset;
                            *((LPBYTE)FixupAddress+3) = (BYTE)(FixupValue >> 24);
                            break;
                        case 2:
                            FixupValue = (prevdw>>16) + (*((LPWORD)FixupAddress+1) << 16) + offset;
                            *((LPWORD)FixupAddress+1) = (WORD)(FixupValue >> 16);
                            break;
                        case 3:
                            FixupValue = (prevdw>>24) + ((*(LPDWORD)((LPBYTE)FixupAddress+1)) << 8) + offset;
                            *(LPWORD)((LPBYTE)FixupAddress+1) = (WORD)(FixupValue >> 8);
                            *((LPBYTE)FixupAddress+3) = (BYTE)(FixupValue >> 24);
                            break;
                    }
                } else if (!Comp1) {
                    if (((DWORD)FixupAddress & 0xfff) > 0xffc) {
                        switch ((DWORD)FixupAddress & 0x3) {
                            case 1:
                                FixupValue = *(LPWORD)FixupAddress + (((DWORD)*((LPBYTE)FixupAddress+2))<<16) + offset;
                                *(LPWORD)FixupAddress = (WORD)FixupValue;
                                *((LPBYTE)FixupAddress+2) = (BYTE)(FixupValue>>16);
                                break;
                            case 2:
                                *(LPWORD)FixupAddress += (WORD)offset;
                                break;
                            case 3:
                                *(LPBYTE)FixupAddress += (BYTE)offset;
                                break;
                        }
                    } else
                        *FixupAddress += offset;
                }
                break;
#else
            case IMAGE_REL_BASED_HIGH: // Save the address and go to get REF_LO paired with this one
                FixupAddressHi = (LPWORD)FixupAddress;
                MatchedReflo = TRUE;
                break;
            case IMAGE_REL_BASED_LOW: // Low - (16-bit) relocate high part too.
                if (MatchedReflo) {
                    FixupValue = (DWORD)(long)((*FixupAddressHi) << 16) +
                        *(LPWORD)FixupAddress + offset;
                    *FixupAddressHi = (short)((FixupValue + 0x8000) >> 16);
                    MatchedReflo = FALSE;
                } else
                    FixupValue = *(short *)FixupAddress + offset;
                *(LPWORD)FixupAddress = (WORD)(FixupValue & 0xffff);
                break;
            case IMAGE_REL_BASED_HIGHLOW: // Word - (32-bits) relocate the entire address.
                    if ((DWORD)FixupAddress & 0x3)
                        *(UNALIGNED DWORD *)FixupAddress += (DWORD)offset;
                    else
                        *FixupAddress += (DWORD)offset;
                break;
            case IMAGE_REL_BASED_HIGHADJ: // 32 bit relocation of 16 bit high word, sign extended
                DEBUGMSG(ZONE_LOADER2,(TEXT("Grabbing extra data %8.8lx\r\n"),*(currptr+1)));
                *(LPWORD)FixupAddress += (WORD)((*(short *)(++currptr)+offset+0x8000)>>16);
                break;
            case IMAGE_REL_BASED_MIPS_JMPADDR: // jump to 26 bit target (shifted left 2)
                FixupValue = ((*FixupAddress)&0x03ffffff) + (offset>>2);
                *FixupAddress = (*FixupAddress & 0xfc000000) | (FixupValue & 0x03ffffff);
                break;
#if defined(MIPS)
            case IMAGE_REL_BASED_MIPS_JMPADDR16: // MIPS16 jal/jalx to 26 bit target (shifted left 2)
                if (IsMIPS16Supported) {
                    if (PrevPage && (((DWORD)FixupAddress & (PAGE_SIZE-1)) == PAGE_SIZE-2)) {
                        // Relocation is on previous page, crossing into the current one
                        // Do this page's portion == last 2 bytes of jal/jalx == least-signif 16 bits of address
                        *((LPWORD)FixupAddress+1) += (WORD)(offset >> 2);
                        break;
                    } else if (!PrevPage) {
                        // Relocation is on this page. Put the most-signif bits in FixupValue
                        FixupValue = (*(LPWORD)FixupAddress) & 0x03ff;
                        FixupValue = ((FixupValue >> 5) | ((FixupValue & 0x1f) << 5)) << 16;
                        if (((DWORD)FixupAddress & (PAGE_SIZE-1)) == PAGE_SIZE-2) {
                            // We're at the end of the page. prevdw has the 2 bytes we peeked at on the next page,
                            // so use them instead of loading them from FixupAddress+1
                            FixupValue |= (WORD)prevdw;
                            FixupValue += offset >> 2;
                        } else {
                            // All 32 bits are on this page. Go ahead and fixup last 2 bytes
                            FixupValue |= *((LPWORD)FixupAddress+1);
                            FixupValue += offset >> 2;
                            *((LPWORD)FixupAddress+1) = (WORD)(FixupValue & 0xffff);
                        }
                        // In either case, we now have the right bits for the upper part of the address
                        // Rescramble them and put them in the first 2 bytes of the fixup
                        FixupValue = (FixupValue >> 16) & 0x3ff;
                        *(LPWORD)FixupAddress = (WORD)((*(LPWORD)FixupAddress & 0xfc00) | (FixupValue >> 5) | ((FixupValue & 0x1f) << 5));
                    }
                }
                break;
#endif
#endif
            default :
                DEBUGMSG(ZONE_LOADER1,(TEXT("Not doing fixup type %d\r\n"),*currptr>>12));
                DEBUGCHK(0);
                break;
        }
        //DEBUGMSG(ZONE_LOADER2,(TEXT("reloc complete, new op %8.8lx\r\n"),*FixupAddress));
        currptr++;
    }
#ifdef MIPS
    if (IsMIPS16Supported) {
#endif
#if defined(x86) || defined(MIPS)
    if (Comp1) {
        blockptr = (struct info *)(((ulong)blockptr)+blockptr->size);
        if (((ulong)blockptr < (ulong)blockstart + blocksize) && blockptr->size) {
            currptr = (LPWORD)(((ulong)blockptr)+sizeof(struct info));
            if ((ulong)currptr < ((ulong)blockptr+blockptr->size))
                if ((ZeroPtr(BasePtr+blockptr->rva) <= ZeroPtr(pMem)) && (ZeroPtr(BasePtr+blockptr->rva)+4096 > ZeroPtr(pMem)))
                    goto fixuppage;
        }
    }
#endif
#ifdef MIPS
    }
#endif
    EnterCriticalSection(&PagerCS);
    return TRUE;
}



//------------------------------------------------------------------------------
// Relocate a DLL or EXE
//------------------------------------------------------------------------------
BOOL 
Relocate(
    e32_lite *eptr, 
    o32_lite *oarry, 
    ulong BasePtr, 
    ulong BaseAdj
    ) 
{
    o32_lite *dataptr;
    struct info *blockptr, *blockstart;
    ulong blocksize;
    LPDWORD FixupAddress;
    LPWORD FixupAddressHi, currptr;
    WORD curroff;
    DWORD FixupValue, offset;
    BOOL MatchedReflo=FALSE;
    int loop;

    if (!(blocksize = eptr->e32_unit[FIX].size)) // No relocations
        return TRUE;

    DEBUGMSG(ZONE_LOADER1,(TEXT("Relocations: BasePtr = %8.8lx, BaseAdj = %8.8lx, VBase = %8.8lx\r\n"),
        BasePtr,BaseAdj, eptr->e32_vbase));
    if (!(offset = BasePtr - BaseAdj - eptr->e32_vbase))
        return TRUE;                                                    // no adjustment needed
    DEBUGMSG(ZONE_LOADER1,(TEXT("Relocate: Offset is %8.8lx\r\n"),offset));
    blockstart = blockptr = (struct info *)(BasePtr+eptr->e32_unit[FIX].rva);
    while (((ulong)blockptr < (ulong)blockstart + blocksize) && blockptr->size) {
        currptr = (LPWORD)(((ulong)blockptr)+sizeof(struct info));
        if ((ulong)currptr >= ((ulong)blockptr+blockptr->size)) {
            blockptr = (struct info *)(((ulong)blockptr)+blockptr->size);
            continue;
        }
        dataptr = 0;
        while ((ulong)currptr < ((ulong)blockptr+blockptr->size)) {
            curroff = *currptr&0xfff;
            if (!curroff && !blockptr->rva) {
                currptr++;
                continue;
            }
            if (!dataptr || (dataptr->o32_rva > blockptr->rva + curroff) ||
                    (dataptr->o32_rva + dataptr->o32_vsize <= blockptr->rva + curroff)) {
                for (loop = 0; loop < eptr->e32_objcnt; loop++) {
                    dataptr = &oarry[loop];
                    if ((dataptr->o32_rva <= blockptr->rva + curroff) &&
                        (dataptr->o32_rva+dataptr->o32_vsize > blockptr->rva + curroff))
                        break;
                }
            }
            DEBUGCHK(loop != eptr->e32_objcnt);
            FixupAddress = (LPDWORD)(blockptr->rva - dataptr->o32_rva + curroff + dataptr->o32_realaddr);
            DEBUGMSG(ZONE_LOADER2,(TEXT("type %d, low %8.8lx, page %8.8lx, addr %8.8lx, op %8.8lx\r\n"),
                (*currptr>>12)&0xf, (*currptr)&0x0fff,blockptr->rva,FixupAddress,*FixupAddress));
            switch (*currptr>>12) {
                case IMAGE_REL_BASED_ABSOLUTE: // Absolute - no fixup required.
                    break;
                case IMAGE_REL_BASED_HIGH: // Save the address and go to get REF_LO paired with this one
                    FixupAddressHi = (LPWORD)FixupAddress;
                    MatchedReflo = TRUE;
                    break;
                case IMAGE_REL_BASED_LOW: // Low - (16-bit) relocate high part too.
                    if (MatchedReflo) {
                        FixupValue = (DWORD)(long)((*FixupAddressHi) << 16) +
                            *(LPWORD)FixupAddress + offset;
                        *FixupAddressHi = (short)((FixupValue + 0x8000) >> 16);
                        MatchedReflo = FALSE;
                    } else
                        FixupValue = *(short *)FixupAddress + offset;
                    *(LPWORD)FixupAddress = (WORD)(FixupValue & 0xffff);
                    break;
                case IMAGE_REL_BASED_HIGHLOW: // Word - (32-bits) relocate the entire address.
                    if ((DWORD)FixupAddress & 0x3)
                        *(UNALIGNED DWORD *)FixupAddress += (DWORD)offset;
                    else
                        *FixupAddress += (DWORD)offset;
                    break;
                case IMAGE_REL_BASED_HIGHADJ: // 32 bit relocation of 16 bit high word, sign extended
                    DEBUGMSG(ZONE_LOADER2,(TEXT("Grabbing extra data %8.8lx\r\n"),*(currptr+1)));
                    *(LPWORD)FixupAddress += (WORD)((*(short *)(++currptr)+offset+0x8000)>>16);
                    break;
                case IMAGE_REL_BASED_MIPS_JMPADDR: // jump to 26 bit target (shifted left 2)
                    FixupValue = ((*FixupAddress)&0x03ffffff) + (offset>>2);
                    *FixupAddress = (*FixupAddress & 0xfc000000) | (FixupValue & 0x03ffffff);
                    break;
#if defined(MIPS)
                case IMAGE_REL_BASED_MIPS_JMPADDR16: // MIPS16 jal/jalx to 26 bit target (shifted left 2)
                    if (IsMIPS16Supported) {
                        FixupValue = (*(LPWORD)FixupAddress) & 0x03ff;
                        FixupValue = (((FixupValue >> 5) | ((FixupValue & 0x1f) << 5)) << 16) | *((LPWORD)FixupAddress+1);
                        FixupValue += offset >> 2;
                        *((LPWORD)FixupAddress+1) = (WORD)(FixupValue & 0xffff);
                        FixupValue = (FixupValue >> 16) & 0x3ff;
                        *(LPWORD)FixupAddress = (WORD) ((*(LPWORD)FixupAddress & 0x1c00) | (FixupValue >> 5) | ((FixupValue & 0x1f) << 5));
                        break;
                    }
                    // fall through
#endif
                default :
                    DEBUGMSG(ZONE_LOADER1,(TEXT("Not doing fixup type %d\r\n"),*currptr>>12));
                    DEBUGCHK(0);
                    break;
            }
            DEBUGMSG(ZONE_LOADER2,(TEXT("reloc complete, new op %8.8lx\r\n"),*FixupAddress));
            currptr++;
        }
        blockptr = (struct info *)(((ulong)blockptr)+blockptr->size);
    }
    return TRUE;
}

DWORD ResolveImpOrd(PMODULE pMod, DWORD ord);
DWORD ResolveImpStr(PMODULE pMod, LPCSTR str);
DWORD ResolveImpHintStr(PMODULE pMod, DWORD hint, LPCHAR str);

#define MAX_AFE_FILESIZE 32

DWORD Katoi(LPCHAR str) {
    DWORD retval = 0;

    while (*str) {
        retval = retval * 10 + (*str-'0');
        str++;
    }
    return retval;
}



//------------------------------------------------------------------------------
// Get address from export entry
//------------------------------------------------------------------------------
DWORD 
AddrFromEat(
    PMODULE pMod, 
    DWORD eat
    ) 
{
    WCHAR filename[MAX_AFE_FILESIZE];
    PMODULE pMod2;
    LPCHAR str;
    int loop;

    if (!eat)
	return 0;

    if ((eat < pMod->e32.e32_unit[EXP].rva) ||
        (eat >= pMod->e32.e32_unit[EXP].rva + pMod->e32.e32_unit[EXP].size)) {
        DEBUGMSG (ZONE_LOADER2, (L"eat = %8.8lx\n", eat));
        if (pMod->DbgFlags & DBG_IS_DEBUGGER) {
            // The following debugchk is no longer true since we now support loading
            // DLL in MODULE section into kernel (with K flag set).
            // DEBUGCHK (pMod->oe.filetype != FT_ROMIMAGE);
            return eat + (DWORD)pMod->BasePtr;
        }

        // if the module base is in secure section, there is no split for RO/RW sections -> just return base + eat
        if (IsSecureVa (pMod->BasePtr)) {
            return eat + ZeroPtr (pMod->BasePtr);
        } else {

            // ROM DLL splits RW and RO section, need to find the right return address
            o32_lite *optr = pMod->o32_ptr;
            int loop;
            for (loop = 0; loop < pMod->e32.e32_objcnt; loop ++, optr ++) {
                if ((eat >= optr->o32_rva) && (eat < optr->o32_rva + optr->o32_vsize))
                    return optr->o32_realaddr + eat - optr->o32_rva;
            }

            DEBUGMSG (ZONE_LOADER1, (L"AddrFromEat: FAILED! Mod = '%s', Proc = '%s', eat = %8.8lx\n", 
                pMod->lpszModName, pCurProc->lpszProcName, eat));
            return 0;
        }
    }

    str = (LPCHAR)(eat + (ulong)pMod->BasePtr);
    for (loop = 0; loop < MAX_AFE_FILESIZE-1; loop++)
        if ((filename[loop] = (WCHAR)*str++) == (WCHAR)'.')
            break;
    filename[loop] = 0;
    if (!(pMod2 = LoadOneLibraryW (filename,0,0)))
        return 0;
    if (*str == '#')
        return ResolveImpOrd(pMod2,Katoi(str));
    else
        return ResolveImpStr(pMod2,str);
}



//------------------------------------------------------------------------------
// Get address from ordinal
//------------------------------------------------------------------------------
DWORD 
ResolveImpOrd(
    PMODULE pMod, 
    DWORD ord
    ) 
{
    struct ExpHdr *expptr;
    LPDWORD eatptr;
    DWORD hint;
    DWORD retval;

    if (!pMod->e32.e32_unit[EXP].rva)
        return 0;
    expptr = (struct ExpHdr *)((ulong)pMod->BasePtr+pMod->e32.e32_unit[EXP].rva);

    eatptr = (LPDWORD)(expptr->exp_eat + (ulong)pMod->BasePtr);
    hint = ord - expptr->exp_ordbase;
    retval = (hint >= expptr->exp_eatcnt ? 0 : AddrFromEat(pMod,eatptr[hint]));
//  ERRORMSG(!retval,(TEXT("Can't find ordinal %d in module %s\r\n"),ord,pMod->lpszModName));
    return retval;
}



//------------------------------------------------------------------------------
// Get address from string
//------------------------------------------------------------------------------
DWORD 
ResolveImpStr(
    PMODULE pMod, 
    LPCSTR str
    ) 
{
    struct ExpHdr *expptr;
    LPCHAR *nameptr;
    LPDWORD eatptr;
    LPWORD ordptr;
    DWORD retval;
    ulong loop;

    if (!pMod->e32.e32_unit[EXP].rva)
        return 0;
    expptr = (struct ExpHdr *)((ulong)pMod->BasePtr+pMod->e32.e32_unit[EXP].rva);
    nameptr = (LPCHAR *)(expptr->exp_name + (ulong)pMod->BasePtr);
    eatptr = (LPDWORD)(expptr->exp_eat + (ulong)pMod->BasePtr);
    ordptr = (LPWORD)(expptr->exp_ordinal + (ulong)pMod->BasePtr);
    for (loop = 0; loop < expptr->exp_namecnt; loop++)
        if (!strcmp(str,nameptr[loop]+(ulong)pMod->BasePtr))
            break;
    if (loop == expptr->exp_namecnt)
        retval = 0;
    else
        retval = (loop >= expptr->exp_eatcnt ? 0 : AddrFromEat(pMod,eatptr[ordptr[loop]]));
//  ERRORMSG(!retval,(TEXT("Can't find import %a in module %s\r\n"),str,pMod->lpszModName));
    return retval;
}



//------------------------------------------------------------------------------
// Get address from hint and string
//------------------------------------------------------------------------------
DWORD 
ResolveImpHintStr(
    PMODULE pMod, 
    DWORD hint, 
    LPCHAR str
    ) 
{
    struct ExpHdr *expptr;
    LPCHAR *nameptr;
    LPDWORD eatptr;
    LPWORD ordptr;
    DWORD retval;

    if (!pMod->e32.e32_unit[EXP].rva)
        return 0;
    expptr = (struct ExpHdr *)((ulong)pMod->BasePtr+pMod->e32.e32_unit[EXP].rva);
    nameptr = (LPCHAR *)(expptr->exp_name + (ulong)pMod->BasePtr);
    eatptr = (LPDWORD)(expptr->exp_eat + (ulong)pMod->BasePtr);
    ordptr = (LPWORD)(expptr->exp_ordinal + (ulong)pMod->BasePtr);
    if ((hint >= expptr->exp_namecnt) || (strcmp(str,nameptr[hint] + (ulong)pMod->BasePtr)))
        retval = ResolveImpStr(pMod,str);
    else
        retval = AddrFromEat(pMod,eatptr[ordptr[hint]]);
//  ERRORMSG(!retval,(TEXT("Can't find import %a (hint %d) in %s\r\n"),str,hint,pMod->lpszModName));
    return retval;
}



//------------------------------------------------------------------------------
// Increment process reference count to module, return old count
//------------------------------------------------------------------------------
WORD 
IncRefCount(
    PMODULE pMod
    ) 
{
    if (!(pMod->inuse & (1<<pCurProc->procnum))) {
        pMod->inuse |= (1<<pCurProc->procnum);
        pMod->calledfunc &= ~(1<<pCurProc->procnum);
    }

    return pMod->refcnt[pCurProc->procnum]++;
}



//------------------------------------------------------------------------------
// Decrement process reference count to module, return new count
//------------------------------------------------------------------------------
WORD 
DecRefCount(
    PMODULE pMod
    ) 
{
    if (!(--pMod->refcnt[pCurProc->procnum]))
        pMod->inuse &= ~(1<<pCurProc->procnum);

    return pMod->refcnt[pCurProc->procnum];
}



typedef BOOL (*entry_t)(HANDLE,DWORD,LPVOID);
typedef BOOL (*comentry_t)(HANDLE,DWORD,LPVOID,LPVOID,DWORD,DWORD);

//------------------------------------------------------------------------------
// Remove module from linked list
//------------------------------------------------------------------------------
void 
UnlinkModule(
    PMODULE pMod
    ) 
{
    PMODULE ptr1, ptr2;

    EnterCriticalSection(&ModListcs);
    if (pModList == pMod)
        pModList = pMod->pMod;
    else if (pModList) {
        ptr1 = pModList;
        ptr2 = pModList->pMod;
        while (ptr2 && (ptr2 != pMod)) {
            ptr1 = ptr2;
            ptr2 = ptr2->pMod;
        }
        if (ptr2)
            ptr1->pMod = ptr2->pMod;
    }
    LeaveCriticalSection(&ModListcs);
}



//------------------------------------------------------------------------------
// Unmap module from process
//------------------------------------------------------------------------------
void 
UnCopyRegions(
    PMODULE pMod
    ) 
{
    if (IsModCodeAddr (pMod->BasePtr) || IsInResouceSection(pMod->BasePtr)) {

        // decommit RW pages
        if (pMod->rwHigh)
            VirtualFree ((LPVOID) pMod->rwLow, pMod->rwHigh - pMod->rwLow, MEM_DECOMMIT);

        // decommit RO pages only if it's no longer in use
        if (!pMod->inuse) {
            VirtualFree (pMod->BasePtr, pMod->e32.e32_vsize, MEM_DECOMMIT);
        }
    } else {
        // free VM of current process for this module.
        VirtualFree ((LPVOID) ZeroPtr (pMod->BasePtr), pMod->e32.e32_vsize, MEM_DECOMMIT|0x80000000);
        VirtualFree ((LPVOID) ZeroPtr (pMod->BasePtr), 0, MEM_RELEASE);
    }
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
FreeModuleMemory(
    PMODULE pMod
    ) 
{
    LPName pAlloc = (LPName) ((DWORD) pMod->o32_ptr - 4);
    LPVOID BasePtr = pMod->BasePtr;

    UnlinkModule(pMod);

    // decommit RO section and possibly RW section if it's not split
    if (BasePtr) {

        VirtualFree (BasePtr, pMod->e32.e32_vsize, MEM_DECOMMIT|0x80000000);

        // decommit the RW section in Secure slot for ROM modules
        if (pMod->rwHigh)
            VirtualFree ((LPVOID) MapPtrProc (pMod->rwLow, ProcArray), pMod->rwHigh - pMod->rwLow, MEM_DECOMMIT);

        // release the VM if it's not in ROM
        if ((FT_OBJSTORE == pMod->oe.filetype) | IsInResouceSection (BasePtr))
            VirtualFree (BasePtr, 0, MEM_RELEASE);

    }
    if (pMod->o32_ptr)
        FreeMem (pAlloc, pAlloc->wPool);
    CloseExe(&pMod->oe);

    // free the pages in the paging pool
    FreeAllPagesFromQ (&pMod->pgqueue);

    FreeMem(pMod,HEAP_MODULE);
}



void FreeLibraryByName(LPCHAR lpszName);

//------------------------------------------------------------------------------
// Free library from proc (by name), zeroing reference count
//------------------------------------------------------------------------------
VOID 
FreeLibraryFromProc(
    PMODULE pMod, 
    PPROCESS pproc
    ) 
{
    if (pMod->refcnt[pproc->procnum]) {
        pMod->refcnt[pproc->procnum] = 0;
        pMod->inuse &= ~(1 << pproc->procnum);
        DEBUGMSG (ZONE_LOADER1, (L"FreeLibraryFromProc: freeing %s, pMod->inuse = %8.8lx\n", pMod->lpszModName, pMod->inuse));
        if (SystemAPISets[SH_PATCHER])
            FreeDllPatch(pproc,pMod);
        if (!pMod->inuse) {
            KDUpdateSymbols(((DWORD)pMod->BasePtr)+1, TRUE);
            if (IsCeLogKernelEnabled()) {
                CELOG_ModuleFree(pCurProc->hProc, (HANDLE)pMod);
            }
            FreeModuleMemory(pMod);
        }
    }
}



//------------------------------------------------------------------------------
// Free all libraries from proc
//------------------------------------------------------------------------------
VOID 
FreeAllProcLibraries(
    PPROCESS pProc
    ) 
{
    PMODULE pMod, pNext;

    EnterCriticalSection(&LLcs);
    pMod = pModList;
    while (pMod) {
        pNext = pMod->pMod;
        FreeLibraryFromProc(pMod,pProc);
        pMod = pNext;
    }
    LeaveCriticalSection(&LLcs);
}


void SwitchToKernel(PCALLSTACK pcstk);
void SwitchBack();

//------------------------------------------------------------------------------
// Pass Reason/Reserved to DLL entry point for pMod
//------------------------------------------------------------------------------
BOOL 
CallDLLEntry(
    PMODULE pMod, 
    DWORD Reason, 
    LPVOID Reserved
    ) 
{
    BOOL retval = TRUE;
    DWORD LastError = KGetLastError(pCurThread);
    DWORD dwOldMode;
    CALLBACKINFO cbi;
    CALLSTACK  *pcstk = NULL;

    if (pMod->startip && !(pMod->wFlags & DONT_RESOLVE_DLL_REFERENCES)) {
        if ((dwOldMode = GET_TIMEMODE(pCurThread)) == TIMEMODE_KERNEL)
            GoToUserTime();
        if (Reason == DLL_PROCESS_ATTACH) {
            if (pMod->calledfunc & (1<<pCurProc->procnum))
                goto DontCall;
            pMod->calledfunc |= (1<<pCurProc->procnum);
        } else if (Reason == DLL_PROCESS_DETACH) {
            if (!(pMod->calledfunc & (1<<pCurProc->procnum)))
                goto DontCall;
            pMod->calledfunc &= ~(1<<pCurProc->procnum);
        }
        cbi.hProc = hCurProc;                       /* destination process */
        cbi.pfn = (FARPROC) pMod->startip;          /* function to call in dest. process */
        cbi.pvArg0 = pMod;                           /* arg0 data */

        DEBUGCHK (pCurThread->tlsNonSecure);
        
        pCurThread->tlsNonSecure[TLSSLOT_KERNEL] |= TLSKERN_IN_LOADER;
        
        __try {
            if ((pCurProc != &ProcArray[0]) && !(pMod->wFlags & LOAD_LIBRARY_IN_KERNEL)) {
                if (pCurThread->pcstkTop
                    && (pCurThread->pcstkTop->dwPrcInfo & CST_MODE_FROM_USER)
                    && (pCurThread->pcstkTop->dwPrcInfo & CST_IN_KERNEL)) {
                    DEBUGMSG (ZONE_LOADER1, (L"CallDLLEntry: calling DllMain in UserMode (%x)\n", 
                        pMod->startip));

                    retval = PerformCallBack (&cbi, Reason, Reserved, 
                        (LPVOID)ZeroPtr(pMod->BasePtr), pMod->e32.e32_sect14rva, pMod->e32.e32_sect14size);
                } else {
                    DEBUGMSG (ZONE_LOADER1, (L"UserDLL: CallDLLEntry: calling DllMain in KMode (%x)\n", 
                        pMod->startip));
                    retval = ((comentry_t)pMod->startip)((HANDLE)pMod,Reason,Reserved, (LPVOID)ZeroPtr(pMod->BasePtr), pMod->e32.e32_sect14rva, pMod->e32.e32_sect14size);
                }
            } else {
                CALLSTACK cstk;
                BOOL fSwitched = FALSE;

                DEBUGMSG (ZONE_LOADER1, (L"CallDLLEntry: calling DllMain in KMode (%x)\n", pMod->startip));

                if ((pMod->wFlags & LOAD_LIBRARY_IN_KERNEL) && (pCurProc != &ProcArray[0])) {
                    // Switch into the kernel to call the DLL entry
                    SwitchToKernel(&cstk);
                    fSwitched = TRUE;
                }

                retval = ((entry_t)pMod->startip)((HANDLE)pMod,Reason,Reserved);

                if (fSwitched) {
                    SwitchBack();
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            retval = FALSE;
        }

        if (LLcs.LockCount == 1) {
            pCurThread->tlsNonSecure[TLSSLOT_KERNEL] &= ~TLSKERN_IN_LOADER;
        }

        if (pcstk) {
            DEBUGCHK (pcstk == pCurThread->pcstkTop);
            pCurThread->pcstkTop = pcstk->pcstkNext;
            FreeMem (pcstk, HEAP_CALLSTACK);
        }
DontCall:
        if (dwOldMode == TIMEMODE_KERNEL)
            GoToKernTime();
    }
    KSetLastError(pCurThread,LastError);
    DEBUGMSG (!retval, (L"CallDLLEntry: FAILED module '%s', reason = %d\n", pMod->lpszModName, Reason));
    return retval;
}



BOOL UnDoDepends(e32_lite *eptr, DWORD BaseAddr);

//------------------------------------------------------------------------------
// Decrement ref count on pMod (from hCurProc), freeing if needed
//------------------------------------------------------------------------------
BOOL 
FreeOneLibrary (
    PMODULE pMod, 
    BOOL bCallEntry
    ) 
{

    if (!IsValidModule (pMod) || !HasModRefProcPtr(pMod,pCurProc)) {
        DEBUGMSG (ZONE_LOADER1, (L"!FreeOneLibrary Failed: INVALID MODULE, pMod = 0x%8.8lx)\n", pMod));
        return FALSE;
    }

    DEBUGMSG (ZONE_LOADER1, (L"FreeOneLibrary: %s (%x)\n", pMod->lpszModName, bCallEntry));

    if (!DecRefCount(pMod)) {
        // coredll cannot be freed before process exit
        if (pMod == (PMODULE) hCoreDll) {
            DEBUGMSG (1, (L"Trying to free coredll before process exit. ignored.\r\n"));
            DEBUGCHK (0);
            IncRefCount (pMod);
            return FALSE;
        }
        if (bCallEntry) {
            // need to mark this Module in use so the pager will still page-in the module
            // while calling DllMain.
            pMod->inuse |= 1 << pCurProc->procnum;
            CallDLLEntry(pMod,DLL_PROCESS_DETACH,0);
            pMod->inuse &= ~(1 << pCurProc->procnum);
        }
        if (pMod->e32.e32_sect14size)
            FreeLibraryByName("mscoree.dll");
        pMod->dwNoNotify &= ~(1 << pCurProc->procnum);
        if (pMod->pmodResource) {
            FreeOneLibrary(pMod->pmodResource, 0); // DONT call dllentry of RES dll
        }
        if (!(pMod->wFlags & DONT_RESOLVE_DLL_REFERENCES))
            UnDoDepends (&pMod->e32, (DWORD) pMod->BasePtr);
        UnCopyRegions(pMod);
        if (SystemAPISets[SH_PATCHER])
            FreeDllPatch(pCurProc, pMod);
        if (pCurThread->pThrdDbg && ProcStarted(pCurProc) && pCurThread->pThrdDbg->hEvent) {
            pCurThread->pThrdDbg->dbginfo.dwProcessId = (DWORD)hCurProc;
            pCurThread->pThrdDbg->dbginfo.dwThreadId = (DWORD)hCurThread;
            pCurThread->pThrdDbg->dbginfo.dwDebugEventCode = UNLOAD_DLL_DEBUG_EVENT;
            pCurThread->pThrdDbg->dbginfo.u.UnloadDll.lpBaseOfDll = (LPVOID)ZeroPtr(pMod->BasePtr);
            SetEvent(pCurThread->pThrdDbg->hEvent);
            SC_WaitForMultiple(1,&pCurThread->pThrdDbg->hBlockEvent,FALSE,INFINITE);
        }

        if (!pMod->inuse) {
            if (IsCeLogKernelEnabled()) {
                CELOG_ModuleFree(pCurProc->hProc, (HANDLE)pMod);
            }
            KDUpdateSymbols(((DWORD)pMod->BasePtr)+1, TRUE);
            FreeModuleMemory(pMod);
        }
    } else {
        // the process stil have other reference to this module, just take care of 
        // MUI and return
        DEBUGCHK (pMod->inuse);
        // MUI resource has the same reference count as its corresponding module
        if (pMod->pmodResource) {
            DecRefCount (pMod->pmodResource);
        }
    }
    return TRUE;
}



//------------------------------------------------------------------------------
// Decrement ref count on library, freeing if needed
//------------------------------------------------------------------------------
void 
FreeLibraryByName(
    LPCHAR lpszName
    ) 
{
    PMODULE pMod;
    LPWSTR pTrav1;
    LPBYTE pTrav2;

    for (pMod = pModList; pMod; pMod = pMod->pMod) {
        for (pTrav1 = pMod->lpszModName, pTrav2 = lpszName; *pTrav1 && (*pTrav1 == (WCHAR)*pTrav2); pTrav1++, pTrav2++)
            ;
        if (*pTrav1 == (WCHAR)*pTrav2) {
            FreeOneLibrary (pMod, 1);
            return;
        }
    }
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
KAsciiToUnicode(
    LPWSTR wchptr, 
    LPBYTE chptr, 
    int maxlen
    ) 
{
    while ((maxlen-- > 1) && *chptr) {
        *wchptr++ = (WCHAR)*chptr++;
    }
    *wchptr = 0;
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void KUnicodeToAscii(LPCHAR chptr, LPCWSTR wchptr, int maxlen) {
    while ((maxlen-- > 1) && *wchptr) {
        *chptr++ = (CHAR)*wchptr++;
    }
    *chptr = 0;
}

#define MAX_DLLNAME_LEN 32
//------------------------------------------------------------------------------
// Do imports from an exe/dll
//------------------------------------------------------------------------------
DWORD 
DoImports(
    e32_lite *eptr, 
    o32_lite *oarry, 
    ulong BaseAddr, 
    LPCTSTR szFilename
    ) 
{
    ulong blocksize;
    LPDWORD ltptr, atptr;
    PMODULE pMod;
    PMODULE pModHook;
    DWORD retval, loop;
    WCHAR ucptr[MAX_DLLNAME_LEN];
    struct ImpHdr *blockptr, *blockstart;
    struct ImpProc *impptr;
    BOOL fUseAlternateImports;

    DEBUGMSG(ZONE_LOADER1,(TEXT("DoImports: eptr = %8.8lx, BaseAddr = %8.8lx\r\n"),
        eptr, BaseAddr));
    if (!(blocksize = eptr->e32_unit[IMP].size)) // No relocations
        return 0;
    fUseAlternateImports = g_pfnInitLoaderHook ? g_pfnInitLoaderHook (BaseAddr, szFilename) : FALSE;
    blockstart = blockptr = (struct ImpHdr *)(BaseAddr+eptr->e32_unit[IMP].rva);
    DEBUGMSG(ZONE_LOADER1,(TEXT("DoImports: blockstart = %8.8lx, blocksize = %8.8lx, BaseAddr = %8.8lx\r\n"),
        blockstart, blocksize, BaseAddr));
    while (blockptr->imp_lookup) {
        KAsciiToUnicode(ucptr,(LPCHAR)BaseAddr+blockptr->imp_dllname,MAX_DLLNAME_LEN);
/*
        if (IsKernelVa (BaseAddr)) {
            // library loaded in kernel can't have nay import
            RETAILMSG (1, (L"DoImport Failed! Library '%s' loaded into kernel is importing from other library '%s'\r\n",
                    szFilename, ucptr));
            return ERROR_BAD_EXE_FORMAT;
        }
 */
        pMod = LoadOneLibraryW(ucptr,0,0);
        if (!pMod) {
            RETAILMSG (1, (L"DoImport Failed! Unable to import Library '%s' for '%s'\r\n",
                    ucptr, szFilename));
            if (!(retval = GetLastError()))
                retval = ERROR_OUTOFMEMORY;
            while (blockstart != blockptr) {
                FreeLibraryByName((LPBYTE)BaseAddr+blockstart->imp_dllname);
                blockstart++;
            }
            return retval;
        }
        DEBUGMSG(ZONE_LOADER1,(TEXT("Doing imports from module %a (pMod %8.8lx)\r\n"),
            BaseAddr+blockptr->imp_dllname,pMod));
        ltptr = (LPDWORD)(BaseAddr+blockptr->imp_lookup);

        if (IsModCodeAddr (BaseAddr)) {
            // since atptr can be in RW section and we'll split RW and RO section for ROM DLL,
            // we need to find the real address for it.
            o32_lite *optr = oarry;
            for (loop = 0; loop < eptr->e32_objcnt; loop ++, optr ++) {
                if ((blockptr->imp_address >= optr->o32_rva)
                    && (blockptr->imp_address < optr->o32_rva + optr->o32_vsize))
                    break;
            }
            if (loop == eptr->e32_objcnt)
                return ERROR_BAD_EXE_FORMAT;

            atptr = (LPDWORD) (optr->o32_realaddr+ blockptr->imp_address - optr->o32_rva);
        } else
            atptr = (LPDWORD) ZeroPtr (BaseAddr+blockptr->imp_address);

        DEBUGMSG(ZONE_LOADER1,(TEXT("BaseAddr = %8.8lx blockptr->imp_lookup = %8.8lx blockptr->imp_address = %8.8lx\n"),
            BaseAddr, blockptr->imp_lookup, blockptr->imp_address));

        DEBUGMSG(ZONE_LOADER1,(TEXT("ltptr = %8.8lx atptr = %8.8lx\n"),ltptr, atptr));

        while (*ltptr) {
            pModHook = fUseAlternateImports ? g_pfnWhichMod (pMod, szFilename, ucptr, BaseAddr, *ltptr) : pMod;
            if (*ltptr & 0x80000000) {
                retval = ResolveImpOrd(pModHook,*ltptr&0x7fffffff);
        DEBUGMSG(ZONE_LOADER2,(TEXT("DI1: retval = %8.8lx, *ltptr = %8.8lx, atptr = %8.8lx\n"),retval, *ltptr, atptr));
            } else {
                impptr = (struct ImpProc *)((*ltptr&0x7fffffff)+BaseAddr);
                retval = ResolveImpHintStr(pModHook,impptr->ip_hint,(LPBYTE)impptr->ip_name);
        DEBUGMSG(ZONE_LOADER2,(TEXT("DI2: retval = %8.8lx impptr->iphint = %8.8lx, immptr->ip_name, atptr = %8.8lx\n"),
            impptr->ip_hint, impptr->ip_name, atptr));
            }
            if (!retval) {
                // intentionall using NKDbgPrintfW here to let the message visible even in SHIP BUILD
                NKDbgPrintfW (L"ERROR: function @ Ordinal %d missing in Module '%s'\r\n", *ltptr&0x7fffffff, pModHook->lpszModName);
                NKDbgPrintfW (L"!!! Please Check your SYSGEN variable !!!\r\n");
                DEBUGCHK (0);
                return ERROR_BAD_EXE_FORMAT;
            }
            if (*atptr != retval) {
                DWORD dwRet = 0;
                UTlsPtr()[TLSSLOT_KERNEL] |= TLSKERN_NOFAULT | TLSKERN_NOFAULTMSG;
                __try {
                    *atptr = retval;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    // this is the case for the -optidata linker flag, where the idata is in the .rdata
                    // section, and thus read-only
                    o32_lite *dataptr;
                    VirtualFree(atptr,4,MEM_DECOMMIT);
                    for (loop = 0; loop < eptr->e32_objcnt; loop++) {
                        dataptr = &oarry[loop];
                        if ((ZeroPtr (dataptr->o32_realaddr) <= (DWORD)atptr) && (ZeroPtr (dataptr->o32_realaddr)+dataptr->o32_vsize > (DWORD)atptr)) {
                            dataptr->o32_access = PAGE_EXECUTE_READWRITE;
                            break;
                        }
                    }
                    if (loop == eptr->e32_objcnt) {
                        dwRet = ERROR_BAD_EXE_FORMAT;
                    } else {
                        *atptr = retval;
                    }
                }
                UTlsPtr()[TLSSLOT_KERNEL] &= ~(TLSKERN_NOFAULT | TLSKERN_NOFAULTMSG);
                if (dwRet) {
                    return dwRet;
                }
            } else
                DEBUGCHK(*atptr == retval);
            ltptr++;
            atptr++;
        }
        blockptr++;
    }
    return 0;
}



//------------------------------------------------------------------------------
// Adjust permissions on regions, decommitting discardable ones
//------------------------------------------------------------------------------
BOOL 
AdjustRegions(
    e32_lite *eptr, 
    o32_lite *oarry
    ) 
{
    int loop;
    DWORD oldprot;

    // set the eptr->e32_unit[FIX] to 0 so we won't relocate anymore
    eptr->e32_unit[FIX].size = 0;
    
    for (loop = 0; loop < eptr->e32_objcnt; loop++) {
        if (oarry->o32_flags & IMAGE_SCN_MEM_DISCARDABLE) {
            VirtualFree((LPVOID)oarry->o32_realaddr,oarry->o32_vsize,MEM_DECOMMIT);
            oarry->o32_realaddr = 0;
        } else if (!IsKernelVa (oarry->o32_realaddr))
            VirtualProtect((LPVOID)oarry->o32_realaddr,oarry->o32_vsize,oarry->o32_access,&oldprot);
        oarry++;
    }
    return TRUE;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
BOOL 
GetNameFromOE(
    CEOIDINFOEX *pceoi, 
    openexe_t *oeptr
    ) 
{
    LPWSTR pstr;
    LPBYTE pbyte;
    BOOL retval = 1;

    SetNKCallOut (pCurThread);
    __try {

        if (oeptr->filetype == FT_ROMIMAGE) {
            memcpy(pceoi->infFile.szFileName, L"\\Windows\\", 9*sizeof(WCHAR));
            pstr = &pceoi->infFile.szFileName[9];
            pbyte = oeptr->tocptr->lpszFileName;
            while (*pbyte)
                *pstr++ = (WCHAR)*pbyte++;
            *pstr = 0;
        } else if (oeptr->bIsOID) {
            CEGUID sysguid;
            CREATE_SYSTEMGUID(&sysguid);
            pceoi->wVersion = CEOIDINFOEX_VERSION;
            if (!(CeOidGetInfoEx2(&sysguid, oeptr->ceOid, pceoi))
                || (pceoi->wObjType != OBJTYPE_FILE))
                retval = 0;
        } else
            kstrcpyW(pceoi->infFile.szFileName, oeptr->lpName->name);

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        retval = 0;
    }
    ClearNKCallOut (pCurThread);

    return retval;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
BOOL 
ReopenExe(
    openexe_t *oeptr, 
    LPCWSTR pExeName, 
    BOOL bAllowPaging
    ) 
{
    DWORD bytesread;
    BY_HANDLE_FILE_INFORMATION bhfi;

    if (!(oeptr->lpName = AllocName(MAX_PATH)))
        return 0;
    if ((oeptr->hf = CreateFileW(pExeName,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0)) == INVALID_HANDLE_VALUE) {
        FreeName(oeptr->lpName);
        return 0;
    }
    oeptr->bIsOID = 1;
    oeptr->filetype = FT_OBJSTORE;
    if (GetFileInformationByHandle(oeptr->hf,&bhfi) && (bhfi.dwOID != (ULONG)INVALID_HANDLE_VALUE)) {
        FreeName(oeptr->lpName);
        oeptr->ceOid = bhfi.dwOID;
    } else {
        LPName pName;
        oeptr->bIsOID = 0;
        if (pName = AllocName((strlenW(pExeName)+1)*2)) {
            FreeName(oeptr->lpName);
            oeptr->lpName = pName;
        }
        kstrcpyW(oeptr->lpName->name,pExeName);
    }
    oeptr->pagemode = (bAllowPaging && !(pTOC->ulKernelFlags & KFLAG_DISALLOW_PAGING) &&
        ReadFileWithSeek(oeptr->hf,0,0,0,0,0,0)) ? PM_FULLPAGING : PM_NOPAGING;
    if ((SetFilePointer(oeptr->hf,0x3c,0,FILE_BEGIN) != 0x3c) ||
        !ReadFile(oeptr->hf,(LPBYTE)&oeptr->offset,4,&bytesread,0) || (bytesread != 4)) {
        CloseExe(oeptr);
        return 0;
    }
    return 1;
}



openexe_t globoe;
BOOL globpageallowed;

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
HANDLE 
JitOpenFile(
    LPCWSTR pName
    ) 
{
    CALLBACKINFO cbi;

    cbi.hProc = ProcArray[0].hProc;
    cbi.pfn = (FARPROC)ReopenExe;
    cbi.pvArg0 = (LPVOID)&globoe;
    if (!PerformCallBack4Int(&cbi,pName,globpageallowed))
        return INVALID_HANDLE_VALUE;
    if (globoe.filetype != FT_OBJSTORE) {
        CloseExe(&globoe);
        return INVALID_HANDLE_VALUE;
    }
    return globoe.hf;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
JitCloseFile(
    HANDLE hFile
    ) 
{
    DEBUGCHK(globoe.hf == hFile);
    CloseExe(&globoe);
}



DWORD ModuleJit(LPCWSTR, LPWSTR, HANDLE *);
// defined in kmisc.c
extern BOOL fJitIsPresent;

//------------------------------------------------------------------------------
// Load E32 header
//------------------------------------------------------------------------------
DWORD 
LoadE32(
    openexe_t *oeptr, 
    e32_lite *eptr, 
    DWORD *lpflags, 
    DWORD *lpEntry, 
    BOOL bIgnoreCPU, 
    BOOL bAllowPaging, 
    LPBYTE pbTrustLevel
    ) 
{
    e32_exe e32;
    e32_exe *e32ptr;
    e32_rom *e32rptr, e32rom;
    int bytesread;
    CEOIDINFOEX ceoi;
    CALLBACKINFO cbi;

top:

    switch (oeptr->filetype) {
    case FT_OBJSTORE:
        DEBUGMSG(ZONE_LOADER1,(TEXT("PEBase at %8.8lx\r\n"),oeptr->offset));
        if (SetFilePointer(oeptr->hf,oeptr->offset,0,FILE_BEGIN) != oeptr->offset)
            return ERROR_BAD_EXE_FORMAT;
        if (!ReadFile(oeptr->hf,(LPBYTE)&e32,sizeof(e32_exe),&bytesread,0) || (bytesread != sizeof(e32_exe)))
            return ERROR_BAD_EXE_FORMAT;
        e32ptr = &e32;
        break;

    case FT_EXTIMAGE:
    case FT_ROMIMAGE:
        if (FT_ROMIMAGE == oeptr->filetype) {
            e32rptr = (struct e32_rom *)(oeptr->tocptr->ulE32Offset);
        } else if (ReadExtImageInfo (oeptr->hf, IOCTL_BIN_GET_E32, sizeof (e32rom), &e32rom))
            e32rptr = &e32rom;
        else {
            DEBUGMSG(ZONE_LOADER1,(TEXT("LoadE32: Error reading e32 from file hf=%8.8lx\r\n"),oeptr->hf));
            return ERROR_BAD_EXE_FORMAT;
        }

        eptr->e32_objcnt     = e32rptr->e32_objcnt;
        if (lpflags)
            *lpflags         = e32rptr->e32_imageflags;
        if (lpEntry)
            *lpEntry         = e32rptr->e32_entryrva;
        eptr->e32_vbase      = e32rptr->e32_vbase;
        eptr->e32_vsize      = e32rptr->e32_vsize;
        if (e32rptr->e32_subsys == IMAGE_SUBSYSTEM_WINDOWS_CE_GUI) {
            eptr->e32_cevermajor = (BYTE)e32rptr->e32_subsysmajor;
            eptr->e32_ceverminor = (BYTE)e32rptr->e32_subsysminor;
        } else {
            eptr->e32_cevermajor = 1;
            eptr->e32_ceverminor = 0;
        }
        eptr->e32_stackmax = e32rptr->e32_stackmax;
        memcpy(&eptr->e32_unit[0],&e32rptr->e32_unit[0],
            sizeof(struct info)*LITE_EXTRA);
        eptr->e32_sect14rva = e32rptr->e32_sect14rva;
        eptr->e32_sect14size = e32rptr->e32_sect14size;
        return 0;
        
    default:
        DEBUGCHK (0);
        return ERROR_BAD_EXE_FORMAT;
    }
    if (*(LPDWORD)e32ptr->e32_magic != 0x4550)
        return ERROR_BAD_EXE_FORMAT;

    if (!bIgnoreCPU && fJitIsPresent && e32ptr->e32_unit[14].rva) {
        if (GetNameFromOE(&ceoi, oeptr)) {
            DWORD dwRet;
            CloseExe(oeptr);
            globpageallowed = bAllowPaging;
            dwRet = ModuleJit(ceoi.infFile.szFileName,0,&oeptr->hf);
            switch (dwRet) {
                /*
                    We use -1 in the case where ModuleJit is a stub or
                    when the source file is not a CEF file. We want to
                    behave as if the translator was never invoked
                    since URT files will get dealt with in CreateNewProc,
                    long after the CEF translators are done.
                */
                case -1:
                    break;
                case OEM_CERTIFY_FALSE:
                    return NTE_BAD_SIGNATURE;
                case OEM_CERTIFY_TRUST:
                case OEM_CERTIFY_RUN:
                    if (pbTrustLevel)
                        *pbTrustLevel = (BYTE)dwRet;
                    memcpy(oeptr,&globoe,sizeof(openexe_t));
                    goto top;
                default:
                    cbi.hProc = ProcArray[0].hProc;
                    cbi.pfn = (FARPROC)ReopenExe;
                    cbi.pvArg0 = (LPVOID)oeptr;
                    if (!PerformCallBack4Int(&cbi,ceoi.infFile.szFileName,bAllowPaging))
                        return ERROR_BAD_EXE_FORMAT;
            }
        }
    }

    if (!bIgnoreCPU && !e32ptr->e32_unit[14].rva && (e32ptr->e32_cpu != THISCPUID)) {
#ifdef SH3
        if ((e32ptr->e32_cpu != IMAGE_FILE_MACHINE_SH3DSP) || !SH3DSP)
            return ERROR_BAD_EXE_FORMAT;
#elif defined(MIPS)
        if ((e32ptr->e32_cpu != IMAGE_FILE_MACHINE_MIPS16) || !IsMIPS16Supported)
            return ERROR_BAD_EXE_FORMAT;
#elif defined(ARMV4I)
        if (e32ptr->e32_cpu != IMAGE_FILE_MACHINE_ARM)
            return ERROR_BAD_EXE_FORMAT;
#else
        return ERROR_BAD_EXE_FORMAT;
#endif
    }
    eptr->e32_objcnt     = e32ptr->e32_objcnt;
    if (lpflags)
        *lpflags         = e32ptr->e32_imageflags;
    if (lpEntry)
        *lpEntry         = e32ptr->e32_entryrva;
    eptr->e32_vbase      = e32ptr->e32_vbase;
    eptr->e32_vsize      = e32ptr->e32_vsize;
    if (e32ptr->e32_subsys == IMAGE_SUBSYSTEM_WINDOWS_CE_GUI) {
        eptr->e32_cevermajor = (BYTE)e32ptr->e32_subsysmajor;
        eptr->e32_ceverminor = (BYTE)e32ptr->e32_subsysminor;
    } else {
        eptr->e32_cevermajor = 1;
        eptr->e32_ceverminor = 0;
    }
    if ((eptr->e32_cevermajor > 2) || ((eptr->e32_cevermajor == 2) && (eptr->e32_ceverminor >= 2)))
        eptr->e32_stackmax = e32ptr->e32_stackmax;
    else
        eptr->e32_stackmax = 64*1024; // backward compatibility
    if ((eptr->e32_cevermajor > CE_MAJOR_VER) ||
        ((eptr->e32_cevermajor == CE_MAJOR_VER) && (eptr->e32_ceverminor > CE_MINOR_VER))) {
        return ERROR_OLD_WIN_VERSION;
    }
    eptr->e32_sect14rva = e32ptr->e32_unit[14].rva;
    eptr->e32_sect14size = e32ptr->e32_unit[14].size;
    memcpy(&eptr->e32_unit[0],&e32ptr->e32_unit[0],
        sizeof(struct info)*LITE_EXTRA);
    return 0;
}


//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
DWORD 
PageInPage(
    openexe_t *oeptr, 
    LPVOID pMem2, 
    DWORD offset, 
    o32_lite *o32ptr, 
    DWORD size
    ) 
{
    DWORD bytesread;
    DWORD retval = PAGEIN_SUCCESS;
    DWORD cbToRead = min(o32ptr->o32_psize - offset, size);

    DEBUGCHK(PagerCS.OwnerThread == hCurThread);
    DEBUGCHK(oeptr->filetype == FT_OBJSTORE);
    LeaveCriticalSection(&PagerCS);
    if ((o32ptr->o32_psize > offset)
        && (!ReadFileWithSeek(oeptr->hf,pMem2,cbToRead,&bytesread,0,o32ptr->o32_dataptr + offset,0)
            || (bytesread != cbToRead))) {
        retval = (ERROR_DEVICE_REMOVED == KGetLastError (pCurThread))? PAGEIN_FAILURE : PAGEIN_RETRY;
    }
    EnterCriticalSection(&PagerCS);
    return retval;
}



//------------------------------------------------------------------------------
// PageInFromROM
//------------------------------------------------------------------------------
int PageInFromROM (o32_lite *optr, DWORD addr, openexe_t *oeptr, LPBYTE pPgMem)
{
    LPVOID pMem, pMem2;
    DWORD  offset = ZeroPtr (addr) - ZeroPtr (optr->o32_realaddr);
    DWORD  retval = PAGEIN_SUCCESS;

    DEBUGCHK (!(addr & (PAGE_SIZE-1)));    // addr must be page aligned.

    if ((optr->o32_vsize > optr->o32_psize)
        || (FT_EXTIMAGE == oeptr->filetype)
        || (optr->o32_flags & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_COMPRESSED))
        || (optr->o32_dataptr & (PAGE_SIZE-1))) {


        if (pPgMem) {
            // using paging pool
            DEBUGMSG(ZONE_PAGING,(L"    PI-ROM: USING PAGING POOL pPgMem = %8.8lx\r\n", pPgMem));
            pMem2 = pPgMem;
            
        } else {

            // allocate temporary memory in secure slot and perform paging there. Will VirtualCopy to
            // destination and freed later
            if (!(pMem = VirtualAlloc ((LPVOID)ProcArray[0].dwVMBase, 0x10000, MEM_RESERVE, PAGE_NOACCESS))) {
                DEBUGMSG(ZONE_PAGING,(L"    PI-ROM: VA1 Failed!\r\n"));
                return PAGEIN_RETRY;
            }

            // pMem2 is pointing to the reserved area with the same 64K offset as addr
            pMem2 = (LPVOID) ((DWORD)pMem + (addr & 0xffff));
            if (!VirtualAlloc(pMem2,PAGE_SIZE,MEM_COMMIT,PAGE_EXECUTE_READWRITE)) {
                DEBUGMSG(ZONE_PAGING,(L"    PI-ROM: VA2 Failed!\r\n"));
                VirtualFree (pMem, 0, MEM_RELEASE);
                return PAGEIN_RETRY;
            }
        }

        DEBUGMSG(ZONE_PAGING,(TEXT("PI-ROM: memcopying!\r\n")));

        // copy (decompress) the data into the temporary page commited
        if (FT_EXTIMAGE == oeptr->filetype) {
            DWORD cbRead;
            if (!ReadFileWithSeek (oeptr->hf, pMem2, PAGE_SIZE, &cbRead, 0, optr->o32_dataptr + offset, 0)) {
                retval = PAGEIN_RETRY;
            }
        } else if (optr->o32_flags & IMAGE_SCN_COMPRESSED)
            DecompressROM ((LPVOID)(optr->o32_dataptr), optr->o32_psize, pMem2, PAGE_SIZE, offset);
        else if (optr->o32_psize > offset)
            memcpy ((LPVOID)pMem2, (LPVOID)(optr->o32_dataptr+offset), min(optr->o32_psize-offset,PAGE_SIZE));

        if (PAGEIN_SUCCESS == retval) {
            OEMCacheRangeFlush (pMem2, PAGE_SIZE, CACHE_SYNC_WRITEBACK);

            // now VirtualCopy it to the destination
            if (!VirtualCopy ((LPVOID) addr, pMem2, PAGE_SIZE, optr->o32_access)) {
                retval = PAGEIN_RETRY;
                DEBUGMSG(ZONE_PAGING,(L"  PI-ROM: Page in failure in VC 2!\r\n"));
            }
        }

        if (!pPgMem) {
            // decommit and release the temporary memory
            if (!VirtualFree (pMem2, PAGE_SIZE, MEM_DECOMMIT))
                DEBUGMSG(ZONE_PAGING,(L"  PI-ROM: Page in failure in VF1!\r\n"));
            if (!VirtualFree (pMem, 0, MEM_RELEASE))
                DEBUGMSG(ZONE_PAGING,(L"  PI-ROM: Page in failure in VF2!\r\n"));
        }

    // else - RO, uncompressed, VirtualCopy directly from ROM
    } else if (!VirtualCopy ((LPVOID) addr, (LPVOID)(optr->o32_dataptr+offset), PAGE_SIZE, optr->o32_access)) {
        DEBUGMSG(ZONE_PAGING,(TEXT("PI-ROM: virtualcopying Failed!\r\n")));
        retval = PAGEIN_RETRY;
    }
    return retval;
}

//------------------------------------------------------------------------------
// PageInFromRAM
//------------------------------------------------------------------------------
int PageInFromRAM (openexe_t *oeptr, e32_lite *eptr, DWORD dwVMBase, DWORD BasePtr, o32_lite *optr, DWORD addr, BOOL fRelocate, LPBYTE pPgMem)
{
    LPVOID pMem, pMem2;
    DWORD  zaddr, offset, prevdw = 0;
    int    retval = PAGEIN_SUCCESS;

    DEBUGCHK (!(addr & (PAGE_SIZE-1)));    // addr must be page aligned.

    zaddr = ZeroPtr (addr);
    offset = zaddr - ZeroPtr (optr->o32_realaddr);

    if (pPgMem) {
        // using paging pool
        DEBUGMSG(ZONE_PAGING,(L"    PI-RAM: USING PAGING POOL pPgMem = %8.8lx\r\n", pPgMem));
        pMem2 = pPgMem;
        
    } else {
        
        // allocate temporary memory in secure slot to perform pagein, will VirtualCopy to destination
        // and freed later
        if (!(pMem = VirtualAlloc ((LPVOID)ProcArray[0].dwVMBase, 0x10000, MEM_RESERVE, PAGE_NOACCESS))) {
            DEBUGMSG(ZONE_PAGING,(L"    PI-RAM: VA1 Failed!\r\n"));
            return PAGEIN_RETRY;
        }

        // pMem2 is pointing to the reserved area with the same 64K offset as addr
        pMem2 = (LPVOID) ((DWORD)pMem + (addr & 0xffff));
        if (!VirtualAlloc (pMem2, PAGE_SIZE, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
            DEBUGMSG(ZONE_PAGING,(L"    PI-RAM: VA2 Failed!\r\n"));
            VirtualFree (pMem, 0, MEM_RELEASE);
            return PAGEIN_RETRY;
        }
    }


    // if we need to relocate, peek into previous/next page 
    if (fRelocate) {
#ifdef x86
        if (offset >= 4) {
            retval = PageInPage (oeptr, &prevdw, offset-4, optr, 4);
            DEBUGMSG(ZONE_PAGING && (PAGEIN_SUCCESS != retval),(L"    PI-RAM: PIP Failed 2!\r\n"));
        }
#elif defined(MIPS)
        // If we're not on the last page, peek at the first 2 bytes of the next page;
        // we may need them for a relocation that crosses into the next page.
        if (IsMIPS16Supported && (offset+PAGE_SIZE < optr->o32_vsize)) {
            retval = PageInPage (oeptr, &prevdw, offset+PAGE_SIZE, optr, 2);
            DEBUGMSG(ZONE_PAGING && (PAGEIN_SUCCESS != retval),(L"    PI-RAM: PIP Failed 2!\r\n"));
        }
#endif
    }

    if (PAGEIN_SUCCESS == retval) {
        // page in the real page
        if ((retval = PageInPage (oeptr, pMem2, offset, optr, PAGE_SIZE)) != PAGEIN_SUCCESS) {
            DEBUGMSG(ZONE_PAGING,(L"    PI-RAM: PIP Failed!\r\n"));

        // relocate the page if requested
        } else if (fRelocate && !RelocatePage (eptr, optr, BasePtr, dwVMBase, zaddr, (DWORD)pMem2, prevdw)) {
            DEBUGMSG(ZONE_PAGING,(L"  PI-RAM: Page in failure in RP!\r\n"));
            retval = PAGEIN_FAILURE;
        }

        // VirtualCopy to destination if paged-in successfully
        if (PAGEIN_SUCCESS == retval) {
            MEMORY_BASIC_INFORMATION mbi;

            if (!VirtualQuery ((LPVOID)addr, &mbi, sizeof(mbi))) {
                retval = PAGEIN_FAILURE;
                DEBUGMSG(ZONE_PAGING,(L"  PI-RAM: Page in failure in VQ 2!\r\n"));

            } else if (mbi.State != MEM_COMMIT) {
                OEMCacheRangeFlush (pMem2, PAGE_SIZE, CACHE_SYNC_WRITEBACK);
                if (VirtualCopy ((LPVOID) addr, pMem2, PAGE_SIZE, optr->o32_access))
                    retval = PAGEIN_SUCCESS;
                else {
                    retval = PAGEIN_FAILURE;
                    DEBUGMSG(ZONE_PAGING,(L"  PI-RAM: Page in failure in VC 1!\r\n"));
                }
            }
        }
    }

    if (!pPgMem) {
        // free the temporary memory
        if (!VirtualFree(pMem2,PAGE_SIZE,MEM_DECOMMIT))
            DEBUGMSG(ZONE_PAGING,(L"  PI-RAM: Page in failure in VF1!\r\n"));
        if (!VirtualFree(pMem,0,MEM_RELEASE))
            DEBUGMSG(ZONE_PAGING,(L"  PI-RAM: Page in failure in VF2!\r\n"));
    }

    return retval;
}

//------------------------------------------------------------------------------
// PageInModule
//------------------------------------------------------------------------------
int PageInModule (PMODULE pMod, DWORD addr)
{
    e32_lite *eptr = &pMod->e32;
    o32_lite *optr;
    DWORD    zaddr;
    DWORD    addrNK;
    BOOL     retval = PAGEIN_RETRY;
    LPBYTE   pPagingMem = NULL;
    WORD     idxPgMem;

    DEBUGMSG (ZONE_PAGING, (L"PageInModule: Paging in %8.8lx\r\n", addr));

    // use page start to page in
    addr &= -PAGE_SIZE;

    if (!(optr = FindOptr (pMod->o32_ptr, eptr->e32_objcnt, addr)))
        // fail if can't find it in any section
        return PAGEIN_FAILURE;

    zaddr = ZeroPtr (addr);
    addrNK = (DWORD) MapPtrProc (zaddr, ProcArray);

    // check to see if it's already in secure slot
    if (IsCommittedSecureSlot (zaddr)) {

        retval = PAGEIN_SUCCESS;

    } else {
        // page-in directly to addr if the page is R/W and not shared
        if ((optr->o32_flags & IMAGE_SCN_MEM_WRITE) && !(optr->o32_flags & IMAGE_SCN_MEM_SHARED)) {
            addrNK = addr;

        // only get page from paging pool if we're fully paged
        } else if (PM_FULLPAGING == pMod->oe.pagemode) {
            pPagingMem = GetPagingPage (&pMod->pgqueue, optr, pMod->oe.filetype, &idxPgMem);
        }

        DEBUGMSG (ZONE_PAGING, (L"PageInModule: Paging in %8.8lx, using %8.8lx\r\n", addr, addrNK));
        // call the paging funciton based on filetype
        retval = (FT_OBJSTORE != pMod->oe.filetype)
            ? PageInFromROM (optr, addrNK, &pMod->oe, pPagingMem)
            : PageInFromRAM (&pMod->oe, eptr, ProcArray[0].dwVMBase, (DWORD) pMod->BasePtr, 
                             optr, addrNK, !(pMod->wFlags & LOAD_LIBRARY_AS_DATAFILE), pPagingMem);
    }

    DEBUGCHK ((PAGEIN_SUCCESS != retval) || (addr == addrNK) || IsCommittedSecureSlot (zaddr));

    if ((PAGEIN_SUCCESS == retval) && (addr != addrNK)) {

        if (!(optr->o32_flags & IMAGE_SCN_MEM_WRITE) || (optr->o32_flags & IMAGE_SCN_MEM_SHARED)) {
            // for RO sections not in slot 1, virtual copy from secure slot
            DEBUGMSG (ZONE_PAGING, (L"PageInModule: VirtualCopy %8.8lx <- %8.8lx!\r\n", addr, addrNK));

            OEMCacheRangeFlush ((LPVOID) addrNK, PAGE_SIZE, CACHE_SYNC_WRITEBACK);
            if (!VirtualCopy ((LPVOID)addr, (LPVOID)addrNK, PAGE_SIZE, optr->o32_access)) {
                DEBUGMSG(ZONE_PAGING,(L"PIM: VirtualCopy Failed 1\r\n"));
                retval = PAGEIN_RETRY;
            }

        } else {
            // there is already a copy of the RW data in secure slot. Just use it.
            LPVOID pMem, pMem2;
            if (!(pMem = VirtualAlloc((LPVOID)ProcArray[0].dwVMBase,0x10000,MEM_RESERVE,PAGE_NOACCESS))) {
                DEBUGMSG(ZONE_PAGING,(L"CM: Failed 2\r\n"));
                return PAGEIN_RETRY;
            }
            pMem2 = ((LPVOID)((DWORD)pMem + (zaddr&0xffff)));
            if (!VirtualAlloc(pMem2,PAGE_SIZE,MEM_COMMIT,PAGE_EXECUTE_READWRITE)) {
                DEBUGMSG(ZONE_PAGING,(L"CM: Failed 3\r\n"));
                VirtualFree(pMem,0,MEM_RELEASE);
                return PAGEIN_RETRY;
            }
            memcpy (pMem2, (LPVOID)addrNK, PAGE_SIZE);
            OEMCacheRangeFlush (pMem2, PAGE_SIZE, CACHE_SYNC_WRITEBACK);
            if (!VirtualCopy ((LPVOID)addr, pMem2, PAGE_SIZE, optr->o32_access)) {
                DEBUGMSG(ZONE_PAGING,(L"CM: Failed 4\r\n"));
                VirtualFree(pMem,0,MEM_RELEASE);
                return PAGEIN_RETRY;
            }
            if (!VirtualFree(pMem2,PAGE_SIZE,MEM_DECOMMIT))
                DEBUGMSG(ZONE_PAGING,(L"  Page in failure in VF1x!\r\n"));
            if (!VirtualFree(pMem,0,MEM_RELEASE))
                DEBUGMSG(ZONE_PAGING,(L"  Page in failure in VF2x!\r\n"));
        }
    }

    if (PAGEIN_SUCCESS == retval) {
        PagedInCount++;
    }
    if (pPagingMem) {
        if (PAGEIN_SUCCESS == retval) {
            AddPageToQueue (&pMod->pgqueue, idxPgMem, addrNK, pMod);
        } else {
            AddPageToFreeList (idxPgMem);
        }
    }
    return retval;
}


//------------------------------------------------------------------------------
// PageInProcess
//------------------------------------------------------------------------------
int PageInProcess (PPROCESS pProc, DWORD addr) 
{
    e32_lite *eptr = &pProc->e32;
    o32_lite *optr;
    BOOL     retval;
    LPBYTE   pPagingMem;
    WORD     idxPgMem;

    DEBUGMSG (ZONE_PAGING, (L"PageInProcess: Paging in %8.8lx\r\n", addr));

    // use page start to page in
    addr &= -PAGE_SIZE;

    if (!(optr = FindOptr (pProc->o32_ptr, eptr->e32_objcnt, addr))) {
        DEBUGMSG (ZONE_PAGING, (L"PIP: FindOptr failed\r\n"));
        return PAGEIN_FAILURE;
    }
    pPagingMem = GetPagingPage (&pProc->pgqueue, optr, pProc->oe.filetype, &idxPgMem);

    retval = (FT_OBJSTORE != pProc->oe.filetype)
            ? PageInFromROM (optr, addr, &pProc->oe, pPagingMem)
            : PageInFromRAM (&pProc->oe, eptr, pProc->dwVMBase, (DWORD) pProc->BasePtr, optr, addr, TRUE, pPagingMem);

    if (PAGEIN_SUCCESS == retval) {
        PagedInCount++;
    }
    if (pPagingMem) {
        if (PAGEIN_SUCCESS == retval) {
            AddPageToQueue (&pProc->pgqueue, idxPgMem, addr, pProc);
        } else {
            AddPageToFreeList (idxPgMem);
        }
    }
    return retval;
}


//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
DWORD 
AccessFromFlags(
    DWORD flags
    ) 
{
    if (flags & IMAGE_SCN_MEM_WRITE)
        return (flags & IMAGE_SCN_MEM_EXECUTE ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
    else
        return (flags & IMAGE_SCN_MEM_EXECUTE ? PAGE_EXECUTE_READ : PAGE_READONLY);
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
UnloadMod(
    PMODULE pMod
    ) 
{
    int loop, loop2;
    o32_lite *optr;

    for (loop = 0; loop < pMod->e32.e32_objcnt; loop++) {
        optr = &pMod->o32_ptr[loop];
        if (!(optr->o32_flags & (IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_NOT_PAGED)) &&
            ((pMod->oe.filetype != FT_ROMIMAGE) || (optr->o32_flags & IMAGE_SCN_COMPRESSED) ||
             ((o32_rom *)(pMod->oe.tocptr->ulO32Offset+loop*sizeof(o32_rom)))->o32_dataptr & (PAGE_SIZE-1))) {
            EnterCriticalSection(&VAcs);
            VirtualFree(MapPtrProc(ZeroPtr(optr->o32_realaddr),&ProcArray[0]),optr->o32_vsize,MEM_DECOMMIT);

            // only need to loop through the procarray if the DLL is not in shared area
            if (IsSecureVa (pMod->e32.e32_vbase)) {
                for (loop2 = 1; loop2 < MAX_PROCESSES; loop2++)
                    if (HasModRefProcIndex(pMod,loop2))
                        VirtualFree(MapPtrProc(ZeroPtr(optr->o32_realaddr),&ProcArray[loop2]),optr->o32_vsize,MEM_DECOMMIT);
            }
            LeaveCriticalSection(&VAcs);
        }
    }
    FreeAllPagesFromQ (&pMod->pgqueue);
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
UnloadExe(
    PPROCESS pProc
    ) 
{
    int loop;
    o32_lite *optr;

    if (pProc->oe.pagemode == PM_FULLPAGING) {
        for (loop = 0; loop < pProc->e32.e32_objcnt; loop++) {
            optr = &pProc->o32_ptr[loop];
            if (!(optr->o32_flags & (IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_NOT_PAGED)) &&
                ((pProc->oe.filetype != FT_ROMIMAGE) || (optr->o32_flags & IMAGE_SCN_COMPRESSED) ||
                 ((o32_rom *)(pProc->oe.tocptr->ulO32Offset+loop*sizeof(o32_rom)))->o32_dataptr & (PAGE_SIZE-1)))
                VirtualFree(MapPtrProc(ZeroPtr(optr->o32_realaddr),pProc),optr->o32_vsize,MEM_DECOMMIT);
        }
        FreeAllPagesFromQ (&pProc->pgqueue);
    }
}

BOOL ReadO32FromFile (openexe_t *oeptr, DWORD dwOffset, o32_obj *po32)
{
    ulong   bytesread;

    DEBUGCHK (FT_OBJSTORE == oeptr->filetype);
    return (SetFilePointer (oeptr->hf, dwOffset, 0, FILE_BEGIN) == dwOffset)
        && ReadFile (oeptr->hf, (LPBYTE)po32, sizeof(o32_obj), &bytesread, 0)
        && (bytesread == sizeof(o32_obj))
        && (!po32->o32_psize || (SetFilePointer(oeptr->hf,0,0,FILE_END) >= po32->o32_dataptr + po32->o32_psize));
}


DWORD ReadSection (openexe_t *oeptr, o32_lite *optr, BOOL fLoadInKernel)
{
    ulong nToRead = min(optr->o32_psize, optr->o32_vsize);
    ulong bytesread;
    LPBYTE realaddr = (LPBYTE) optr->o32_realaddr;

    switch (oeptr->filetype) {
    
  
    case FT_OBJSTORE:
        if (PM_NOPAGING == oeptr->pagemode) {
            // commit the memory
            if (!fLoadInKernel 
                && !VirtualAlloc (realaddr, optr->o32_vsize, MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                return ERROR_OUTOFMEMORY;

            // read the section
            if (nToRead
                && ((SetFilePointer (oeptr->hf, optr->o32_dataptr, 0, FILE_BEGIN) != optr->o32_dataptr)
                    || !ReadFile (oeptr->hf, realaddr, nToRead, &bytesread, 0)
                    || (bytesread != nToRead))) {
                return ERROR_BAD_EXE_FORMAT;
            }
        }

        break;

    case FT_EXTIMAGE:
        // only read it if not pageable
        if ((PM_NOPAGING == oeptr->pagemode) || (IMAGE_SCN_MEM_NOT_PAGED & optr->o32_flags)) {
            // if it's R/W and not shared, read directly into process slot
            if (!(optr->o32_flags & IMAGE_SCN_MEM_SHARED) && (optr->o32_flags & IMAGE_SCN_MEM_WRITE)) {
                realaddr = (LPBYTE) ZeroPtr (realaddr);
            }
            DEBUGMSG(ZONE_LOADER1,(TEXT("non-pageable External MODULE! (%8.8lx <- %8.8lx, len = %8.8lx)\r\n"),
                                        realaddr, optr->o32_dataptr, optr->o32_vsize));
            if (!fLoadInKernel && !(VirtualAlloc (realaddr, optr->o32_vsize, MEM_COMMIT, PAGE_EXECUTE_READWRITE))) {
                return ERROR_OUTOFMEMORY;
            }
            if (optr->o32_flags & IMAGE_SCN_COMPRESSED)
                nToRead = optr->o32_vsize;      // always read vsize if compressed.
                
            if (nToRead
                && (!ReadFileWithSeek (oeptr->hf, realaddr, nToRead, &bytesread, NULL, (DWORD) optr->o32_dataptr, 0)
                    || (bytesread != nToRead))) {
                return ERROR_BAD_EXE_FORMAT;
            }
        }
        break;

    case FT_ROMIMAGE:
        if ((optr->o32_vsize > optr->o32_psize)
            || (optr->o32_flags & (IMAGE_SCN_COMPRESSED | IMAGE_SCN_MEM_WRITE))
            || (optr->o32_dataptr & (PAGE_SIZE-1))) {

            if ((PM_NOPAGING == oeptr->pagemode) || (IMAGE_SCN_MEM_NOT_PAGED & optr->o32_flags)) {
                // if it's R/W and not shared, read directly into process slot
                if (!(optr->o32_flags & IMAGE_SCN_MEM_SHARED) && (optr->o32_flags & IMAGE_SCN_MEM_WRITE)) {
                    realaddr = (LPBYTE) ZeroPtr (realaddr);
                }
                DEBUGMSG(ZONE_LOADER1,(TEXT("memcopying! (%8.8lx <- %8.8lx, len = %8.8lx)\r\n"),
                                        realaddr, optr->o32_dataptr, optr->o32_vsize));
                if (!fLoadInKernel && !(VirtualAlloc (realaddr, optr->o32_vsize, MEM_COMMIT, PAGE_EXECUTE_READWRITE)))
                    return ERROR_OUTOFMEMORY;

                if (nToRead) {
                    if (optr->o32_flags & IMAGE_SCN_COMPRESSED) {
                        DecompressROM ((LPVOID)(optr->o32_dataptr), optr->o32_psize,
                            realaddr, optr->o32_vsize,0);
                        DEBUGMSG(ZONE_LOADER1,(TEXT("(4) realaddr = %8.8lx\r\n"), realaddr));
                    } else {
                        memcpy (realaddr, (LPVOID)(optr->o32_dataptr), nToRead);
                        DEBUGMSG(ZONE_LOADER1,(TEXT("(5) realaddr = %8.8lx, psize = %8.8lx\r\n"), 
                            realaddr, optr->o32_psize));
                    }
                }
            }

        } else {
            DEBUGMSG(ZONE_LOADER1,(TEXT("virtualcopying %8.8lx <- %8.8lx (%lx)!\r\n"),
                    realaddr, optr->o32_dataptr, optr->o32_vsize));
            if (!fLoadInKernel && !(VirtualCopy(realaddr,(LPVOID)(optr->o32_dataptr),optr->o32_vsize, optr->o32_access)))
                return ERROR_OUTOFMEMORY;
        }
        break;
    default:
        DEBUGCHK (0);
        return ERROR_BAD_EXE_FORMAT;
    }
    return 0;
}


//------------------------------------------------------------------------------
//
// For NK and Modules with K flag (fixed up by romimage), RO and RW data are
// seperated and the size of RO section is smaller than the vsize specified in
// e32 header. Debugger gets confused because we then sees NK and Kernal DLLs
// overlapping with each other. This function is to re-caculate the size to reflect
// the real size of the so debugger will work on kernel libraries
//
//------------------------------------------------------------------------------
void UpdateKmodVSize (openexe_t *oeptr, e32_lite *eptr)
{

    if (FT_ROMIMAGE == oeptr->filetype) {

        unsigned long dwEndAddr = 0;
        int loop;
        o32_rom *o32rp;
        for (loop = 0; loop < eptr->e32_objcnt; loop++) {
            o32rp = (o32_rom *)(oeptr->tocptr->ulO32Offset+loop*sizeof(o32_rom));
            // find the last RO section
            if ((o32rp->o32_dataptr == o32rp->o32_realaddr)
                && (o32rp->o32_realaddr > dwEndAddr)) {
                dwEndAddr = o32rp->o32_realaddr + o32rp->o32_vsize;
            }
        }
        DEBUGCHK (dwEndAddr);
        // page align the end addr
        dwEndAddr = (dwEndAddr + PAGE_SIZE - 1) & -PAGE_SIZE;
        eptr->e32_vsize = dwEndAddr - eptr->e32_vbase;
        DEBUGMSG (1, (L"Updated eptr->e32_vsize to = %8.8lx\r\n", eptr->e32_vsize));
    }
}


#define LO32_IN_KERNEL      0x1
#define LO32_MODULE         0x2

//------------------------------------------------------------------------------
// Load all O32 headers
//------------------------------------------------------------------------------
DWORD 
LoadO32(
    openexe_t *oeptr, 
    e32_lite *eptr, 
    o32_lite *oarry, 
    ulong BasePtr,
    DWORD fFlags
    ) 
{
    int loop;
    o32_obj o32;
    o32_rom *o32rp;
    o32_lite *optr;
    DWORD retval = 0;

    if ((FT_EXTIMAGE == oeptr->filetype) 
        && !ReadExtImageInfo (oeptr->hf, IOCTL_BIN_GET_O32, eptr->e32_objcnt * sizeof(o32_lite), oarry)) {
        return ERROR_BAD_EXE_FORMAT;
    }

    for (loop = 0, optr = oarry; loop < eptr->e32_objcnt; loop++, optr ++) {

        DEBUGMSG(ZONE_LOADER2,(TEXT("LoadO32 : section %d\r\n"),loop));

        switch (oeptr->filetype) {
        case FT_OBJSTORE:

            // RAM files, read O32 header from file
            if (!ReadO32FromFile (oeptr, oeptr->offset+sizeof(e32_exe)+loop*sizeof(o32_obj), &o32))
                return ERROR_BAD_EXE_FORMAT;

            // update optr
            optr->o32_realaddr = BasePtr + o32.o32_rva;
            DEBUGMSG(ZONE_LOADER1,(TEXT("(1) optr->o32_realaddr = %8.8lx\r\n"), optr->o32_realaddr));
            optr->o32_vsize = o32.o32_vsize;
            optr->o32_rva = o32.o32_rva;
            optr->o32_flags = o32.o32_flags;
            optr->o32_psize = o32.o32_psize;
            optr->o32_dataptr = o32.o32_dataptr;
            if (optr->o32_rva == eptr->e32_unit[RES].rva)
                optr->o32_flags &= ~IMAGE_SCN_MEM_DISCARDABLE;
            optr->o32_access = AccessFromFlags(optr->o32_flags);
            break;

        case FT_ROMIMAGE:
            //
            // ROM (XIP)
            //
            o32rp = (o32_rom *)(oeptr->tocptr->ulO32Offset+loop*sizeof(o32_rom));
            DEBUGMSG(ZONE_LOADER1,(TEXT("(2) o32rp->o32_realaddr = %8.8lx\r\n"), o32rp->o32_realaddr));
            optr->o32_realaddr = o32rp->o32_realaddr;
            optr->o32_vsize = o32rp->o32_vsize;
            optr->o32_psize = o32rp->o32_psize;
            optr->o32_rva = o32rp->o32_rva;
            optr->o32_flags = o32rp->o32_flags;
            if (optr->o32_rva == eptr->e32_unit[RES].rva)
                optr->o32_flags &= ~IMAGE_SCN_MEM_DISCARDABLE;
            optr->o32_dataptr = o32rp->o32_dataptr;

            // fall through (FT_EXTIMAGE already read o32 from filesys)

        case FT_EXTIMAGE:
            // calculate access from flags             
            optr->o32_access = AccessFromFlags(optr->o32_flags);
            if (IsInResouceSection (BasePtr))
                optr->o32_realaddr = BasePtr + optr->o32_rva;
            else if ((fFlags & LO32_MODULE) && !IsModCodeAddr (BasePtr)) {
                optr->o32_realaddr = (DWORD) MapPtrProc (optr->o32_realaddr, ProcArray);
            }

            // don't load the RW section of a ROM DLL
            if ((fFlags & LO32_MODULE)
                && !IsModCodeAddr (BasePtr)
                && !(fFlags & LO32_IN_KERNEL)
                && (!(optr->o32_flags & IMAGE_SCN_MEM_SHARED) && (optr->o32_flags & IMAGE_SCN_MEM_WRITE)))
                continue;
            break;

        default:
            DEBUGCHK (0);
            return ERROR_BAD_EXE_FORMAT;
        }

        // read the section data (will be no-op if pageable)
        if (retval = ReadSection (oeptr, optr, fFlags & LO32_IN_KERNEL)) {
            DEBUGMSG (ZONE_LOADER1, (L"LoadO32 Failed\n"));
            break;
        }

    }
    return retval;
}



//------------------------------------------------------------------------------
// Find starting IP for EXE (or entrypoint for DLL)
//------------------------------------------------------------------------------
ulong 
FindEntryPoint(
    DWORD entry, 
    e32_lite *eptr, 
    o32_lite *oarry
    ) 
{
    o32_lite *optr;
    int loop;

    for (loop = 0; loop < eptr->e32_objcnt; loop++) {
        optr = &oarry[loop];
        if ((entry >= optr->o32_rva) ||
            (entry < optr->o32_rva+optr->o32_vsize)) {
            DEBUGMSG (ZONE_LOADER2, (L"FindEntryPoint realaddr = %8.8lx, entry = %8.8lx, rva = %8.8lx\n",
                optr->o32_realaddr, entry, optr->o32_rva));
            return ZeroPtr (optr->o32_realaddr + entry - optr->o32_rva);
        }
    }
    return 0;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
BOOL 
OpenAnExe(
    LPWSTR exename, 
    openexe_t *oe, 
    BOOL bAllowPaging
    ) 
{
    static WCHAR fname[MAX_PATH];
    DWORD len;

    len = strlenW(exename);
    if (OpenExe(exename,oe,0,bAllowPaging))
        return TRUE;
    if (len + 4 < MAX_PATH) {
        memcpy(fname,exename,len*sizeof(WCHAR));
        memcpy(fname+len,L".exe",8);
        fname[len+4] = 0;
        return OpenExe(fname,oe,0,bAllowPaging);
    }
    return FALSE;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
int 
OpenADll(
    LPWSTR dllname, 
    openexe_t *oe, 
    BOOL bAllowPaging
    ) 
{
    static WCHAR fname[MAX_PATH];
    DWORD len;

    len = strlenW(dllname);

    if (OpenExe(dllname,oe,1,bAllowPaging))
        return 2;       // has not been extended

    if (len + 4 < MAX_PATH) {
        memcpy(fname,dllname,len*sizeof(WCHAR));
        memcpy(fname+len,L".dll",8);
        fname[len+4] = 0;
        if (OpenExe(fname,oe,1,bAllowPaging))
            return 1;   // has been extended
    }
    return 0;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
DWORD 
CalcStackSize(
    DWORD cbStack
    ) 
{
    if (!cbStack)
        cbStack = pCurProc->e32.e32_stackmax;
    cbStack = (cbStack + PAGE_SIZE - 1) & ~(PAGE_SIZE-1);
    if (cbStack < (MIN_STACK_RESERVE*2))
        cbStack = (MIN_STACK_RESERVE*2);
    if (cbStack > MAX_STACK_RESERVE)
        cbStack = MAX_STACK_RESERVE;
    return cbStack;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
LoadSwitch(
    LSInfo_t *plsi
    ) 
{
    KCALLPROFON(46);
    pCurThread->bSuspendCnt = 1;
    DEBUGCHK(!((pCurThread->wInfo >> DEBUG_LOOPCNT_SHIFT) & 1));
    SET_RUNSTATE(pCurThread,RUNSTATE_BLOCKED);
    RunList.pth = 0;
    SetReschedule();
    DEBUGCHK(plsi->pHelper->bSuspendCnt == 1);
    --(plsi->pHelper->bSuspendCnt);
    MakeRun(plsi->pHelper);
    KCALLPROFOFF(46);
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
FinishLoadSwitch(
    LSInfo_t *plsi
    ) 
{
    DWORD retval;
    PTHREAD pthread;
    HANDLE hEvent;      // save the hEvent since we need to set it after free 
                        // the thread's stack
    BOOL fSuspend;      // same reason as above, need to save it locally
    LPVOID stkbase;     // same reason as above, need to save it locally
    
    SETCURKEY((DWORD)-1);
    pthread = HandleToThread(plsi->hth);
    DEBUGCHK(pthread);
    MDCreateMainThread2(pthread,plsi->cbStack,
        (LPVOID)(pthread->pOwnerProc->e32.e32_sect14rva ? CTBFf : MTBFf),
        (LPVOID)plsi->startip,TH_UMODE,
        (ULONG)pthread->pOwnerProc->hProc,0,(plsi->cmdlen+1)*sizeof(WCHAR),
        (plsi->namelen+1)*sizeof(WCHAR),
        pthread->pOwnerProc->e32.e32_sect14rva ? (DWORD)pthread->pOwnerProc : SW_SHOW);
    pthread->aky = pthread->pOwnerProc->aky;
    // save the original stack info in thread structure
    if (KERN_TRUST_FULL == pthread->pOwnerProc->bTrustLevel) {
        pthread->dwOrigBase = KSTKBASE(pthread);
        pthread->tlsSecure = pthread->tlsPtr;
    } else {
        pthread->dwOrigBase = pthread->tlsNonSecure[PRETLS_STACKBASE];
    }
    pthread->dwOrigStkSize = (pthread->pOwnerProc->e32.e32_stackmax + 0xffff) & 0xffff0000;

    AddAccess(&pthread->aky,ProcArray[0].aky);
    stkbase = plsi->lpOldStack;
    fSuspend = (plsi->flags & CREATE_SUSPENDED);
    hEvent = plsi->hEvent;
    
    // CANT PRINT MESSAGE HERE -- STACK OVERFLOW SINCE WE'RE ON SPECIAL STACK
    //NKDbgPrintfW (L"pthread->tlsSecure = %8.8lx, pthread->tlsSecure = %8.8lx\n", pthread->tlsNonSecure, pthread->tlsSecure);

    // NOTE: lpsi is no longer accessible after this line.
    if (KERN_TRUST_FULL == pthread->pOwnerProc->bTrustLevel) {
        retval = VirtualFree(stkbase,CNP_STACK_SIZE,MEM_DECOMMIT);
        DEBUGCHK(retval);
        retval = VirtualFree(stkbase,0,MEM_RELEASE);
        DEBUGCHK(retval);
    }
    if (!fSuspend)
        KCall((PKFN)ThreadResume,pthread);
    SetEvent(hEvent);
    KCall((PKFN)KillSpecialThread);
    DEBUGCHK(0);
}

BOOL ReserveDllRW (DWORD dwVMBase)
{
    if (!g_pROMInfo) {
        // use ROMChain to perform reservation

        ROMChain_t *pROM;
        DWORD rwBase;

        // reserver R/W data for shared ROM modules in all XIP
        for (pROM = ROMChain; pROM; pROM = pROM->pNext) {
            rwBase = pROM->pTOC->dllfirst << 16;
            if (rwBase && (rwBase != pROM->pTOC->dlllast)) {
                DEBUGMSG (ZONE_LOADER1, (L"reserving Memory (%8.8lx, %8.8lx, %8.8lx)\r\n", rwBase + dwVMBase, rwBase, pROM->pTOC->dlllast));
                if (!VirtualAlloc((LPVOID)(rwBase + dwVMBase), pROM->pTOC->dlllast - rwBase, MEM_RESERVE|MEM_IMAGE, PAGE_NOACCESS)) {
                    ERRORMSG (1, (L"Failed to reserve Memory (%8.8lx, %8.8lx, %8.8lx)\r\n", rwBase + dwVMBase, rwBase, pROM->pTOC->dlllast));
                    return FALSE;
                }
            }
        }
    } else {
        // use ROMINFO to perform reservation
        int i;
        LARGE_INTEGER *pLimits = (LARGE_INTEGER *) (g_pROMInfo + 1);
        for (i = 0; i < g_pROMInfo->nROMs; i ++, pLimits ++) {
            if (pLimits->LowPart && (pLimits->LowPart != (DWORD) pLimits->HighPart)) {
                DEBUGMSG (ZONE_LOADER1, (L"reserving Memory(2) (%8.8lx, %8.8lx, %8.8lx)\r\n", 
                    pLimits->LowPart+ dwVMBase, pLimits->LowPart, pLimits->HighPart));
                if (!VirtualAlloc((LPVOID)(pLimits->LowPart + dwVMBase), pLimits->HighPart - pLimits->LowPart, MEM_RESERVE|MEM_IMAGE, PAGE_NOACCESS)) {
                    ERRORMSG (1, (L"Failed to reserve Memory(2) (%8.8lx, %8.8lx, %8.8lx)\r\n",
                        pLimits->LowPart+ dwVMBase, pLimits->LowPart, pLimits->HighPart));
                    return FALSE;
                }
            }
        }
    }

    return TRUE;

}


static VOID DoProcessDetachAllDLLs(BOOL fUndoDepend);


//------------------------------------------------------------------------------
// Startup thread for new process (does loading)
//------------------------------------------------------------------------------
void 
CreateNewProc(
    ulong nextfunc, 
    ulong param
    )
{
    e32_lite *eptr;
    LPVOID lpStack = 0;
    DWORD flags, entry, loop, inRom, dwretval;
    LPWSTR procname, uptr;
    CALLBACKINFO cbi;
    LPProcStartInfo lppsi = (LPProcStartInfo)param;
    LSInfo_t lsi;
    ulong startip;
    PCALLSTACK pcstk = 0;
    LPDWORD pOldTls;
    BOOL fImportDone = FALSE;

    DEBUGCHK(RunList.pth == pCurThread);
    eptr = &pCurProc->e32;
    DEBUGMSG(ZONE_LOADER1,(TEXT("CreateNewProc %s (base %8.8lx)\r\n"),lppsi->lpszImageName,pCurProc->dwVMBase));
    EnterCriticalSection(&LLcs);

    lppsi->lppi->hProcess = 0;      // fail by default
    if (pCurProc == &ProcArray[0]) {
        if (!OpenAnExe(lppsi->lpszImageName,&pCurProc->oe,(lppsi->fdwCreate & 0x80000000) ? 0 : 1)) {
            lppsi->lppi->dwThreadId = ERROR_FILE_NOT_FOUND;
            goto cnperr;
        }
    } else {
        cbi.hProc = ProcArray[0].hProc;
        cbi.pfn = (FARPROC)OpenAnExe;
        cbi.pvArg0 = MapPtr(lppsi->lpszImageName);
        if (!PerformCallBack4Int(&cbi,MapPtr(&pCurProc->oe),(lppsi->fdwCreate & 0x80000000) ? 0 : 1)) {
            lppsi->lppi->dwThreadId = ERROR_FILE_NOT_FOUND;
            goto cnperr;
        }
    }
    if (loop = LoadE32(&pCurProc->oe,eptr,&flags,&entry,0,1,&pCurProc->bTrustLevel)) {
        lppsi->lppi->dwThreadId = loop;
        goto cnperr;
    }
    // we now always pass the binary to VerifyBinary, no matter if it's .NETCF app or not
    // if (!eptr->e32_sect14rva && (loop = VerifyBinary(&pCurProc->oe,lppsi->lpszImageName,&pCurProc->bTrustLevel))) {
    if (loop = VerifyBinary(&pCurProc->oe,lppsi->lpszImageName,&pCurProc->bTrustLevel)) {
        lppsi->lppi->dwThreadId = loop;
        goto cnperr;
    }
    DEBUGMSG(ZONE_LOADER1,(TEXT("lppsi->lpszImageName = '%s', pCurProc->bTrustLevel = %d\r\n"), 
                lppsi->lpszImageName, pCurProc->bTrustLevel));
    if (flags & IMAGE_FILE_DLL) {
        lppsi->lppi->dwThreadId = ERROR_BAD_EXE_FORMAT;
        goto cnperr;
    }
    if ((flags & IMAGE_FILE_RELOCS_STRIPPED) && ((eptr->e32_vbase < 0x10000) || (eptr->e32_vbase > 0x400000))) {
        DEBUGMSG(ZONE_LOADER1,(TEXT("No relocations - can't load/fixup!\r\n")));
        lppsi->lppi->dwThreadId = ERROR_BAD_EXE_FORMAT;
        goto cnperr;
    }
    pCurProc->BasePtr = (flags & IMAGE_FILE_RELOCS_STRIPPED) ? (LPVOID)eptr->e32_vbase : (LPVOID)0x10000;
    if (!VirtualAlloc(pCurProc->BasePtr,eptr->e32_vsize,MEM_RESERVE|MEM_IMAGE, PAGE_NOACCESS)) {
        lppsi->lppi->dwThreadId = ERROR_OUTOFMEMORY;
        goto cnperr;
    }

    // reserve the RW sections of Modules
    if (!ReserveDllRW (pCurProc->dwVMBase)) {
        lppsi->lppi->dwThreadId = ERROR_OUTOFMEMORY;
        goto cnperr;
    }
    if (!(pCurProc->o32_ptr = VirtualAlloc(0,eptr->e32_objcnt*sizeof(o32_lite),
        MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE))) {
        lppsi->lppi->dwThreadId = ERROR_OUTOFMEMORY;
        goto cnperr;
    }
    pCurProc->o32_ptr = MapPtr(pCurProc->o32_ptr);
    DEBUGMSG(ZONE_LOADER1,(TEXT("BasePtr is %8.8lx\r\n"),pCurProc->BasePtr));
    if (dwretval = LoadO32(&pCurProc->oe,eptr,pCurProc->o32_ptr,(ulong)pCurProc->BasePtr, 0)) {
        lppsi->lppi->dwThreadId = dwretval;
        goto cnperr;
    }
    for (loop = 0; loop < eptr->e32_objcnt; loop++) {
        if (!(pCurProc->o32_ptr[loop].o32_flags & IMAGE_SCN_MEM_DISCARDABLE) &&
            (pCurProc->o32_ptr[loop].o32_flags & IMAGE_SCN_MEM_WRITE)) {
            DWORD base, size;
            base = pCurProc->o32_ptr[loop].o32_realaddr;
            size = pCurProc->o32_ptr[loop].o32_vsize;
            size = (size + 3) & ~3;
            if ((size % PAGE_SIZE) && ((PAGE_SIZE - (size % PAGE_SIZE)) >= eptr->e32_objcnt*sizeof(o32_lite))) {
                memcpy((LPBYTE)base+size,pCurProc->o32_ptr,eptr->e32_objcnt*sizeof(o32_lite));
                VirtualFree(pCurProc->o32_ptr,0,MEM_RELEASE);
                pCurProc->o32_ptr = (o32_lite *)MapPtrProc((base+size),pCurProc);
                pCurProc->o32_ptr[loop].o32_vsize = size + eptr->e32_objcnt*sizeof(o32_lite);
                break;
            }
        }
    }
    if (pCurProc->oe.filetype == FT_OBJSTORE) {
        if ((pCurProc->oe.pagemode == PM_NOPAGING) && !Relocate(eptr, pCurProc->o32_ptr, (ulong)pCurProc->BasePtr,0)) {
            lppsi->lppi->dwThreadId = ERROR_OUTOFMEMORY;
            goto cnperr;
        }
        inRom = 0;
    } else
        inRom = 1;
    eptr->e32_stackmax = CalcStackSize(eptr->e32_stackmax);
    if (!(lpStack = VirtualAlloc((LPVOID)pCurProc->dwVMBase,eptr->e32_stackmax,MEM_RESERVE|MEM_AUTO_COMMIT,PAGE_NOACCESS))) {
        lppsi->lppi->dwThreadId = ERROR_OUTOFMEMORY;
        goto cnperr;
    }
    if (!VirtualAlloc((LPVOID)((DWORD)lpStack+eptr->e32_stackmax-MIN_STACK_SIZE), MIN_STACK_SIZE, MEM_COMMIT, PAGE_READWRITE)) {
        lppsi->lppi->dwThreadId = ERROR_OUTOFMEMORY;
        goto cnperr;
    }
    pCurThread->tlsSecure = pCurThread->tlsPtr;
    //NKDbgPrintfW (L"pCurThread->tlsSecure = %8.8lx\n", pCurThread->tlsPtr);
    pCurThread->tlsNonSecure = (LPDWORD)
            // CNP_STACK_SIZE MUST BE 64K or the calculation won't work.
            (((DWORD)lpStack + eptr->e32_stackmax) - (CNP_STACK_SIZE - ((DWORD) pCurThread->tlsPtr & 0xffff)));



    pCurThread->tlsNonSecure[PRETLS_STACKBASE] = (DWORD) lpStack;
    pCurThread->tlsNonSecure[PRETLS_STACKBOUND] = (DWORD) pCurThread->tlsNonSecure & ~(PAGE_SIZE-1);
    pCurThread->tlsNonSecure[PRETLS_PROCSTKSIZE] = eptr->e32_stackmax;

    if (!bAllKMode) {
        if (!(pcstk = AllocMem (HEAP_CALLSTACK))) {
            lppsi->lppi->dwThreadId = ERROR_OUTOFMEMORY;
            goto cnperr;
        }
        memset (pcstk, 0, sizeof (CALLSTACK));
        pcstk->dwPrcInfo = CST_IN_KERNEL | CST_MODE_FROM_USER;    // fake that we're calling from umode into kernel
        pcstk->dwPrevSP = (DWORD) pCurThread->tlsNonSecure - SECURESTK_RESERVE;
        pcstk->pcstkNext = 0;
        pcstk->pprcLast = pCurProc;
        pcstk->akyLast = pCurProc->aky | ProcArray[0].aky;
#ifdef DEBUG
        pcstk->retAddr = (LPVOID) 0xabababab;
#endif
        pCurThread->pcstkTop = pcstk;
        
    }

    // inject DLLs into process if specified
    if (pInjectDLLs) {
        LPTSTR p = (LPTSTR)(pInjectDLLs->name);
        while(*p) {
            DEBUGMSG(ZONE_LOADER1, (TEXT("Inject %s into Process %s\r\n"), p, lppsi->lpszImageName));
            LoadOneLibraryW(p, 0, 0);
            p += (strlenW(p)+1);
        }
    }

    LoadOneLibraryW(L"coredll.dll",0,0);

    // load MUI if it exist
    pCurProc->pmodResource = LoadMUI (hCurProc, (LPBYTE) MapPtrProc (pCurProc->BasePtr, pCurProc), eptr);

    loop = DoImports(eptr, pCurProc->o32_ptr, (ulong)pCurProc->BasePtr, lppsi->lpszImageName);

    if (loop) {
        lppsi->lppi->dwThreadId = loop;
        goto cnperr;
    }
    fImportDone = TRUE;
    if ((pCurProc->oe.pagemode == PM_NOPAGING) && !AdjustRegions(eptr, pCurProc->o32_ptr)) {
        lppsi->lppi->dwThreadId = ERROR_OUTOFMEMORY;
        goto cnperr;
    }
    if (eptr->e32_sect14rva) {
        HANDLE hLib;
        if (!(hLib = LoadOneLibraryW(L"mscoree.dll",0,0))) {
            lppsi->lppi->dwThreadId = ERROR_DLL_INIT_FAILED;
            goto cnperr;
        }
        if (!(startip = (DWORD)GetProcAddressA(hLib,"_CorExeMain"))) {
            lppsi->lppi->dwThreadId = ERROR_DLL_INIT_FAILED;
            goto cnperr;
        }
    } else
        startip = FindEntryPoint(entry, eptr,pCurProc->o32_ptr);

    // free th faked callstack if there is one
    if (pcstk) {
        FreeMem (pcstk, HEAP_CALLSTACK);
        pCurThread->pcstkTop = 0;
        pcstk = 0;
    }
    
    if (inRom && SystemAPISets[SH_PATCHER] && !PatchExe(pCurProc, lppsi->lpszImageName))
        goto cnperr;
    DEBUGMSG(ZONE_LOADER1,(TEXT("Starting up the process!!! IP = %8.8lx\r\n"),startip));

    lppsi->lppi->hProcess = hCurProc;
    lppsi->lppi->hThread = hCurThread;
    lppsi->lppi->dwProcessId = (DWORD)hCurProc;
    lppsi->lppi->dwThreadId = (DWORD)hCurThread;

    LeaveCriticalSection(&LLcs);
    
    uptr = procname = lppsi->lpszImageName;
    while (*uptr) {
        if ((*uptr == (WCHAR)'\\') || (*uptr == (WCHAR)'/'))
            procname = uptr+1;
        uptr++;
    }
    lsi.startip = startip;
    lsi.flags = lppsi->fdwCreate;
    lsi.lpStack = lpStack;
    lsi.lpOldStack = (LPVOID) KSTKBASE(pCurThread);
    lsi.cbStack = eptr->e32_stackmax;
    lsi.cmdlen = strlenW(lppsi->lpszCmdLine);
    lsi.namelen = strlenW(procname);
    lsi.hEvent = lppsi->he;
    lsi.hth = hCurThread;
    pOldTls = pCurThread->tlsPtr;
    pCurProc->pcmdline = MDCreateMainThread1(pCurThread,lpStack,eptr->e32_stackmax,(LPBYTE)lppsi->lpszCmdLine,
        (lsi.cmdlen+1)*sizeof(WCHAR),(LPBYTE)procname,(lsi.namelen+1)*sizeof(WCHAR));

    // on ALL-K-MODE system, we don't switch stack while calling DllMain, need to copy the TLS back or
    // we'll lose TLS data changed in DllMain.
    if (bAllKMode) {
        memcpy (pCurThread->tlsPtr, pOldTls, sizeof(DWORD)*TLS_MINIMUM_AVAILABLE);
    }
    KPROCSTKSIZE(pCurThread) = eptr->e32_stackmax;
    KDUpdateSymbols(((DWORD)pCurProc->BasePtr)+1, FALSE);


    DEBUGMSG(ZONE_LOADER1,(TEXT("Calling into the kernel for threadswitch!\r\n")));

	DEBUGCHK (!pCurThread->pThrdDbg);
    if (pCurProc->hDbgrThrd) {
        extern LPTHRDDBG AllocDbg(HANDLE hProc);
        LPTHRDDBG pThrdDbg = AllocDbg(NULL);
        if (pThrdDbg) {
            PTHREAD pDbgrThrd, pth = pCurThread;
            EnterCriticalSection(&DbgApiCS);
            pDbgrThrd = HandleToThread(pCurProc->hDbgrThrd);
            pth->pThrdDbg = pThrdDbg;
            pth->pThrdDbg->hNextThrd = pDbgrThrd->pThrdDbg->hFirstThrd;
            pDbgrThrd->pThrdDbg->hFirstThrd = pth->hTh;
            LeaveCriticalSection(&DbgApiCS);
        }
    }

    BlockWithHelperAlloc((FARPROC)LoadSwitch,(FARPROC)FinishLoadSwitch,(LPVOID)&lsi);
    DEBUGCHK(0);
    // Never returns
cnperr:

    // No need to VirtualFree, since entire section will soon be blown away
    DoProcessDetachAllDLLs(fImportDone);

    // free th faked callstack if there is one
    if (pcstk) {
        FreeMem (pcstk, HEAP_CALLSTACK);
        pCurThread->pcstkTop = 0;
    }
    *(__int64 *)&pCurThread->ftCreate = 0; // so we don't get an end time
    CloseExe(&pCurProc->oe);
    DEBUGMSG(ZONE_LOADER1,(TEXT("CreateNewProc failure!\r\n")));
    DEBUGMSG(1,(TEXT("CreateNewProc failure on %s!\r\n"),lppsi->lpszImageName));
    LeaveCriticalSection(&LLcs);
    SetEvent(lppsi->he);
    SC_CloseProcOE(0);
    CloseAllCrits();
    SC_CloseAllHandles();
    SC_NKTerminateThread(0);
    DEBUGCHK(0);
    // Never returns
}



//------------------------------------------------------------------------------
// Copy DLL regions from Proc0 to pProc
//------------------------------------------------------------------------------
BOOL 
CopyRegions (PMODULE pMod) 
{
    // Don't copy from Proc0 to Proc0 - should only be for coredll.dll
    if (pCurProc->procnum) {

        int loop;
        struct o32_lite *optr;
        LPVOID BasePtr = MapPtrProc (ZeroPtr (pMod->BasePtr), pCurProc);
        DWORD  dwErr = 0;
        LPBYTE addr;

        DEBUGCHK (!(pMod->wFlags & LOAD_LIBRARY_IN_KERNEL));
        if ((BasePtr != pMod->BasePtr)  // for modules in slot 1, they'll be equal
            && !VirtualAlloc (BasePtr, pMod->e32.e32_vsize, MEM_RESERVE|MEM_IMAGE, PAGE_NOACCESS)) {
            DEBUGMSG (ZONE_LOADER1, (L"CopyRegion Failed to reserver memory!\r\n"));
            return FALSE;
        }

        if ((FT_ROMIMAGE != pMod->oe.filetype) && (PM_NOPAGING != pMod->oe.pagemode)) {
            return TRUE;
        }

        // if we reach here, we're either XIP or the module is not pageable.
        for (loop = 0, optr = pMod->o32_ptr; loop < pMod->e32.e32_objcnt; loop ++, optr ++) {

            addr = (LPBYTE) BasePtr+optr->o32_rva;

            if (!(optr->o32_flags & IMAGE_SCN_MEM_SHARED) && (optr->o32_flags & IMAGE_SCN_MEM_WRITE)) {
                // RW, not shared
                if (FT_OBJSTORE != pMod->oe.filetype) {
                    if (dwErr = ReadSection (&pMod->oe, optr, 0)) {
                        break;
                    }
                } else {
                    DEBUGMSG(ZONE_LOADER1,(TEXT("CopyRegion: Commiting %8.8lx\n"), addr));
                    if (!(VirtualAlloc(addr,optr->o32_vsize,MEM_COMMIT,PAGE_EXECUTE_READWRITE))) {
                        DEBUGMSG(ZONE_LOADER1,(TEXT("CopyRegion: VirtualAlloc %8.8l (0x%x)x\n"),
                                addr, optr->o32_vsize));
                        break;
                    }

                    memcpy (addr,(LPVOID)optr->o32_realaddr,optr->o32_vsize);
                }

            } else if ((BasePtr != pMod->BasePtr)
                    && !(optr->o32_flags & IMAGE_SCN_MEM_DISCARDABLE)) {
                // RO or shared sections
                if ((PM_NOPAGING == pMod->oe.pagemode)
                    || ((FT_ROMIMAGE == pMod->oe.filetype)
                        && !(optr->o32_flags & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_COMPRESSED))
                        && (optr->o32_vsize <= optr->o32_psize) 
                        && !(optr->o32_dataptr & (PAGE_SIZE-1)))) {
                    if (!VirtualCopy (addr, (LPVOID)optr->o32_realaddr, optr->o32_vsize, optr->o32_access)) {
                        DEBUGMSG(ZONE_LOADER1,(TEXT("CopyRegion: VirtualCopy failed %8.8lx <- %8.8l (0x%x)x\n"),
                                addr, optr->o32_realaddr, optr->o32_vsize));
                        break;
                    }
                }
            }
        }

        // have we loop through all the sections?
        if (loop < pMod->e32.e32_objcnt) {
            // failed
            DEBUGMSG (ZONE_LOADER1, (L"CopyRegion Failed! loop = %d\r\n", loop));
            if (BasePtr != pMod->BasePtr) {
                VirtualFree (BasePtr, pMod->e32.e32_vsize, MEM_DECOMMIT);
                VirtualFree (BasePtr, 0, MEM_RELEASE);
            } else if (pMod->rwHigh) {
                VirtualFree ((LPVOID) pMod->rwLow, pMod->rwHigh - pMod->rwLow, MEM_DECOMMIT);
            }
            KSetLastError (pCurThread, dwErr? dwErr : ERROR_OUTOFMEMORY);
            return FALSE;
        }
        //OEMCacheRangeFlush (0, 0, CACHE_SYNC_WRITEBACK|CACHE_SYNC_INSTRUCTIONS);
    }

    return TRUE;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
PMODULE 
FindModByName(
    LPCWSTR dllname,
    DWORD wFlags
    ) 
{
    PMODULE pMod;
    WCHAR ch = lowerW(*dllname);

    for (pMod = pModList; pMod; pMod = pMod->pMod) {
        if ((ch == pMod->lpszModName[0]) 
            && !((pMod->wFlags ^ wFlags) & LOAD_LIBRARY_AS_DATAFILE)    // the LOAD_LIBRARY_AS_DATA_FILE bit is the same
            && !strcmpdllnameW(pMod->lpszModName,dllname))
            break;
    }
    return pMod;
}



//------------------------------------------------------------------------------
// Issue all connected DLL's a DLL_THREAD_DETACH
//------------------------------------------------------------------------------
VOID 
SC_ThreadDetachAllDLLs(void) 
{
    PMODULE pMod;

    DEBUGMSG(ZONE_ENTRY,(L"SC_ThreadDetachAllDLLs entry\r\n"));
    EnterCriticalSection(&LLcs);
    for (pMod = pModList; pMod; pMod = pMod->pMod)
        if (HasModRefProcPtr(pMod,pCurProc) && AllowThreadMessage(pMod,pCurProc))
            CallDLLEntry(pMod,DLL_THREAD_DETACH,0);
    LeaveCriticalSection(&LLcs);
    DEBUGMSG(ZONE_ENTRY,(L"SC_ThreadDetachAllDLLs exit\r\n"));
}



//------------------------------------------------------------------------------
// Issue all connected DLL's a DLL_PROCESS_DETACH
//------------------------------------------------------------------------------
static VOID DoProcessDetachAllDLLs(BOOL fUndoDepend) 
{
    PMODULE pMod;

    // unload all implicitly loaded dll
    if (fUndoDepend)
        UnDoDepends (&pCurProc->e32, (DWORD) pCurProc->BasePtr);
    
    for (pMod = pModList; pMod; pMod = pMod->pMod)
        if (HasModRefProcPtr(pMod,pCurProc)) 
            __try {
#ifdef DEBUG
                if (!IsPreloadedDlls (pMod)) {
                    RETAILMSG (1, (L"Process %s loaded Module %s without freeing it before process exit\r\n",
                        pCurProc->lpszProcName, pMod->lpszModName));
                    // if we hit this debugchk, the app has an un-balanced Load/FreeLibrary
                    // DEBUGCHK (0);
                }
#endif
                CallDLLEntry(pMod,DLL_PROCESS_DETACH,(LPVOID)1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
}

VOID SC_ProcessDetachAllDLLs(void) 
{
    DEBUGMSG(ZONE_ENTRY,(L"SC_ProcessDetachAllDLLs entry\r\n"));
    EnterCriticalSection(&LLcs);

    DoProcessDetachAllDLLs (TRUE);

    LeaveCriticalSection(&LLcs);
    DEBUGMSG(ZONE_ENTRY,(L"SC_ProcessDetachAllDLLs exit\r\n"));
}



//------------------------------------------------------------------------------
// Issue all connected DLL's a DLL_THREAD_ATTACH
//------------------------------------------------------------------------------
VOID 
SC_ThreadAttachAllDLLs(void) 
{
    PMODULE pMod;

    DEBUGMSG(ZONE_ENTRY,(L"SC_ThreadAttachAllDLLs entry\r\n"));
    EnterCriticalSection(&LLcs);
    for (pMod = pModList; pMod; pMod = pMod->pMod)
        if (HasModRefProcPtr(pMod,pCurProc) && AllowThreadMessage(pMod,pCurProc))
            CallDLLEntry(pMod,DLL_THREAD_ATTACH,0);
    LeaveCriticalSection(&LLcs);
    DEBUGMSG(ZONE_ENTRY,(L"SC_ThreadAttachAllDLLs exit\r\n"));
}


//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
BOOL UnDoDepends(e32_lite *eptr, DWORD BaseAddr)
{
    ulong blocksize;
    PMODULE pMod2;
    WCHAR ucptr[MAX_DLLNAME_LEN];
    struct ImpHdr *blockptr;

    if (!(blocksize = eptr->e32_unit[IMP].size)) // No relocations
        return TRUE;
    blockptr = (struct ImpHdr *)(BaseAddr+eptr->e32_unit[IMP].rva);
    while (blockptr->imp_lookup) {
        KAsciiToUnicode(ucptr,(LPCHAR)BaseAddr+blockptr->imp_dllname,MAX_DLLNAME_LEN);
        pMod2 = FindModByName(ucptr, 0);
        

        if (pMod2 && (pMod2->inuse & (1 << pCurProc->procnum))) {
            FreeOneLibrary (pMod2, 1);
        }
        blockptr++;
    }

    // Notify the shim engine to undo its dependencies.
    if (g_pfnUndoDepends) {
        g_pfnUndoDepends (eptr, BaseAddr);
    }

    return TRUE;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
DWORD InitModule (PMODULE pMod, LPWSTR lpszFileName, LPWSTR lpszDllName, DWORD fLbFlags, WORD wFlags)
{
    e32_lite    *eptr;
    LPName      pAlloc = 0;
    BOOL        bExtended;
    DWORD       flags, entry, retval, len, loop;
    BOOL        bAllowPaging = !(fLbFlags & LLIB_NO_PAGING);

    // initialize pMod fields
    pMod->startip = 0;
    pMod->lpSelf = pMod;
    pMod->calledfunc = 0;
    pMod->oe.filetype = 0;
    pMod->ZonePtr = 0;
    pMod->inuse = 0;
    pMod->DbgFlags = 0;
    pMod->BasePtr = 0;
    pMod->o32_ptr = 0;
    pMod->lpszModName = 0;
    pMod->dwNoNotify = 0;
    pMod->wFlags = wFlags;
    pMod->pmodResource = 0;
    pMod->bTrustLevel = KERN_TRUST_FULL;
    pMod->rwLow = -1;
    pMod->rwHigh = 0;
    pMod->pMod = 0;
    pMod->pgqueue.idxHead = INVALID_PG_INDEX;
    memset (&pMod->refcnt[0],0,sizeof(pMod->refcnt[0])*MAX_PROCESSES);
    memset (&pMod->e32, 0, sizeof (pMod->e32));

    if (pCurProc == &ProcArray[0]) {
        bExtended = OpenADll(lpszFileName,&pMod->oe,bAllowPaging);
    } else {
        CALLBACKINFO cbi;
        cbi.hProc = ProcArray[0].hProc;
        cbi.pfn = (FARPROC)OpenADll;
        cbi.pvArg0 = MapPtr(lpszFileName);
        bExtended = PerformCallBack4Int (&cbi,MapPtr(&pMod->oe),bAllowPaging);
    }
    if (!bExtended) {
        return ERROR_MOD_NOT_FOUND;
    }

    // bExtended 0 is failure, we use the return value 2 as "no need to extend"
    if (bExtended == 2)
        bExtended = 0;

    // load O32 info for the module
    eptr = &pMod->e32;
    if (retval = LoadE32(&pMod->oe,eptr,&flags,&entry,(pMod->wFlags & LOAD_LIBRARY_AS_DATAFILE) ? 1 : 0, bAllowPaging,&pMod->bTrustLevel)) {
        return retval;
    }

    if (wFlags & LOAD_LIBRARY_AS_DATAFILE) {
        DEBUGMSG(ZONE_LOADER1,(TEXT("Loading '%s' as Data File\r\n"), lpszFileName));
        // allocate top-down if we can't load it in the dll's preferred load base.
        if (!(pMod->BasePtr = VirtualAlloc ((LPVOID)RESOURCE_BASE_ADDRESS, eptr->e32_vsize,
                                MEM_RESERVE|MEM_TOP_DOWN|MEM_IMAGE,PAGE_NOACCESS))) {
            return ERROR_OUTOFMEMORY;
        }
        
    } else {

        if (wFlags & LOAD_LIBRARY_IN_KERNEL) {
            UpdateKmodVSize (&pMod->oe, eptr);
        }

        // verify binary
        if (!eptr->e32_sect14rva && (retval = VerifyBinary(&pMod->oe,lpszFileName,&pMod->bTrustLevel))) {
            return retval;
        }
        DEBUGMSG(ZONE_LOADER1,(TEXT("lpszFileName = '%s', pMod->bTrustLevel = %d\r\n"), 
                lpszFileName, pMod->bTrustLevel));

        // check trust level and parameters
        if ((pCurProc->bTrustLevel == KERN_TRUST_FULL) && (pMod->bTrustLevel == KERN_TRUST_RUN)) {
            return (DWORD)NTE_BAD_SIGNATURE;
        }
        if (!(flags & IMAGE_FILE_DLL) || (flags & IMAGE_FILE_RELOCS_STRIPPED)) {
            return ERROR_BAD_EXE_FORMAT;
        }

        // allocate memory for the DLL
        if (pMod->oe.filetype == FT_OBJSTORE) {
            //
            // If not an XIP DLL, then reserve enough virtual address space
            // to contain the entire image. (top down from the bottom of the
            // existing DLL's)
            //
            if (wFlags & LOAD_LIBRARY_IN_KERNEL) {
                PHYSICAL_ADDRESS paRet;
                //
                // Loading in the kernel address space (statically-mapped
                // virtual address) so get some physically contiguous pages.
                //
                paRet = GetContiguousPages( (DWORD) (eptr->e32_vsize + PAGE_SIZE - 1) / PAGE_SIZE, 0, 0);
                if (paRet == INVALID_PHYSICAL_ADDRESS || !(pMod->BasePtr = (PVOID) Phys2Virt(paRet))) {
                    return ERROR_OUTOFMEMORY;
                }
            } else {
                // try to honor the Dll's relocation base to prevent relocation
                if ((pTOC->ulKernelFlags & KFLAG_HONOR_DLL_BASE) && (eptr->e32_vbase + eptr->e32_vsize < ROMDllLoadBase)) {
                    pMod->BasePtr = VirtualAlloc ((LPVOID)(ProcArray[0].dwVMBase + eptr->e32_vbase), eptr->e32_vsize,
                                        MEM_RESERVE|MEM_IMAGE, PAGE_NOACCESS);

                    DEBUGMSG (pMod->BasePtr, (L"Loading DLL '%s' at the preferred loading address %8.8lx\n", lpszFileName, ZeroPtr (pMod->BasePtr)));
                }

                // allocate top-down if we can't load it in the dll's preferred load base.
                if (!pMod->BasePtr
                    && !(pMod->BasePtr = VirtualAlloc ((LPVOID)ProcArray[0].dwVMBase,eptr->e32_vsize,
                                        MEM_RESERVE|MEM_TOP_DOWN|MEM_IMAGE,PAGE_NOACCESS))) {
                    return ERROR_OUTOFMEMORY;
                }
            }
            if (ZeroPtr(pMod->BasePtr) < (DWORD)DllLoadBase)
                DllLoadBase = ZeroPtr(pMod->BasePtr);
        } else {
            pMod->BasePtr = (LPVOID) MapPtrProc (eptr->e32_vbase, ProcArray);

            if ((wFlags & LOAD_LIBRARY_IN_KERNEL) && !IsKernelVa (pMod->BasePtr)) {
                return ERROR_BAD_EXE_FORMAT;
            }
        }
    }

    // allocate memory for both name and the o32 objects in one piece


    len = (strlenW(lpszDllName) + 1)*sizeof(WCHAR);

    // +8 for ".dll" extension, +2 for DWORD alignment
    if (!(pAlloc = AllocName (eptr->e32_objcnt * sizeof(o32_lite) + len + (bExtended? 10 : 2))))
        return ERROR_OUTOFMEMORY;

    DEBUGCHK (!((DWORD) pAlloc & 3));

    pMod->o32_ptr = (o32_lite *) ((DWORD) pAlloc + 4);    // skip the header info

    //
    // Read in O32 information for this module.
    //
    DEBUGMSG(ZONE_LOADER1,(TEXT("BasePtr is %8.8lx\r\n"),pMod->BasePtr));

    if (retval = LoadO32(&pMod->oe,eptr,pMod->o32_ptr,(ulong)pMod->BasePtr, 
                    LO32_MODULE | ((wFlags & LOAD_LIBRARY_IN_KERNEL)? LO32_IN_KERNEL : 0))) {
        DEBUGMSG(ZONE_LOADER1,(TEXT("LoadO32 failed 0x%8.8lx\r\n"), retval));
        return retval;
    }

    // update module name
    pMod->lpszModName = (LPWSTR) (pMod->o32_ptr + eptr->e32_objcnt);
    for (loop = 0; loop < len/sizeof(WCHAR); loop++)
        pMod->lpszModName[loop] = lowerW(lpszDllName[loop]);

    if (bExtended)
        memcpy((LPBYTE)pMod->lpszModName+len-2,L".dll",10);

    if (pMod->oe.filetype == FT_OBJSTORE) {
        //
        // Non-XIP image needs to be relocated.
        //
        if ( (pMod->oe.pagemode == PM_NOPAGING) && 
            !(pMod->wFlags & LOAD_LIBRARY_AS_DATAFILE) && 
            !Relocate (eptr, pMod->o32_ptr, (ulong)pMod->BasePtr,
                        ((wFlags & LOAD_LIBRARY_IN_KERNEL) ? 0 : ProcArray[0].dwVMBase))) {

            return ERROR_OUTOFMEMORY;
        }
    } else {
        o32_lite *optr = pMod->o32_ptr;
        //
        // If the module is loaded into the Slot 1 (DLL High) area or into 
        // the kernel we need to record where the read/write section has 
        // been located for this module.
        //
        if (IsModCodeAddr (pMod->BasePtr) || IsKernelVa(pMod->BasePtr)) {
            // find the high/low of RW sections
            for (loop = 0; loop < eptr->e32_objcnt; loop ++, optr ++) {
                if ((optr->o32_flags & IMAGE_SCN_MEM_WRITE) && !(optr->o32_flags & IMAGE_SCN_MEM_SHARED)) {
                    if (pMod->rwLow > optr->o32_realaddr)
                        pMod->rwLow = optr->o32_realaddr;
                    if (pMod->rwHigh < optr->o32_realaddr + optr->o32_vsize)
                        pMod->rwHigh =  optr->o32_realaddr + optr->o32_vsize;
                }
            }
        }
        DEBUGMSG(ZONE_LOADER1,(TEXT("InitModule: rwHigh = %8.8lx, rwLow = %8.8lx\r\n"),pMod->rwHigh, pMod->rwLow));
    }

    if (pMod->oe.pagemode == PM_NOPAGING) {
        if (!AdjustRegions(eptr, pMod->o32_ptr)) {
            return ERROR_OUTOFMEMORY;
        }
        if (!IsKernelVa(pMod->BasePtr)) {
            OEMCacheRangeFlush (pMod->BasePtr, eptr->e32_vsize, CACHE_SYNC_WRITEBACK);
        }
    }

    if (entry) {
        if ((wFlags & LOAD_LIBRARY_IN_KERNEL) || !pMod->e32.e32_sect14rva)
            pMod->startip = FindEntryPoint(entry,&pMod->e32,pMod->o32_ptr);
        else {
            HANDLE hLib;
            if (!(hLib = LoadOneLibraryW (L"mscoree.dll",0,0))
                || !(pMod->startip = (DWORD)GetProcAddressA (hLib,"_CorDllMain"))) {
                return ERROR_DLL_INIT_FAILED;
            }
        }
    }

    DEBUGMSG(ZONE_LOADER1,(TEXT("Done InitModule, StartIP = %8.8lx\r\n"), pMod->startip));
    return 0;
}

BOOL (* pfnOEMIsKernelSpaceDll) (LPCWSTR pszDllName);

//------------------------------------------------------------------------------
// Load a library (DLL)
//------------------------------------------------------------------------------
PMODULE 
LoadOneLibraryW(
    LPCWSTR lpszFileName, 
    DWORD fLbFlags,
    WORD wFlags
    ) 
{
    PMODULE pMod = 0, pModRes;
    LPCWSTR dllname;
    DWORD dwErr = 0;
    DWORD prevRefCnt = 0;
    BOOL fSpecial = FALSE;

    if ((DWORD)lpszFileName & 0x1) {
        wFlags |= LOAD_LIBRARY_IN_KERNEL;
        (DWORD)lpszFileName &= ~0x1;
    }

    DEBUGMSG (ZONE_LOADER1, (L"LoadOneLibraryPart2: %s, %d, 0x%x\n", lpszFileName, fLbFlags, wFlags));
    if (wFlags & LOAD_LIBRARY_AS_DATAFILE) {
        wFlags |= DONT_RESOLVE_DLL_REFERENCES;
    }

    if (wFlags & LOAD_LIBRARY_IN_KERNEL) {
        if (pCurProc->bTrustLevel != KERN_TRUST_FULL) {
            KSetLastError(pCurThread,(DWORD)NTE_BAD_SIGNATURE);
            return 0;
        }

        fLbFlags |= LLIB_NO_PAGING | LLIB_NO_MUI;
    }

    dllname = lpszFileName+strlenW(lpszFileName);
    while ((dllname != lpszFileName) && (*(dllname-1) != (WCHAR)'\\') && (*(dllname-1) != (WCHAR)'/')) {
        dllname--;
    }

    if (pfnOEMIsKernelSpaceDll && (fSpecial = pfnOEMIsKernelSpaceDll (dllname))) {
        wFlags |= LOAD_LIBRARY_IN_KERNEL;
        fLbFlags |= LLIB_NO_PAGING | LLIB_NO_MUI;
    }

    DEBUGMSG(ZONE_LOADER1,(TEXT("LoadOneLibrary %s (%s)\r\n"),lpszFileName,dllname));

    pMod = FindModByName(dllname, wFlags);
    if (pMod) {
        //
        // Module has been loaded previously.
        //
        // fail if the module can't be debugged and a debugger is attached.
        if ((FT_OBJSTORE != pMod->oe.filetype) && !ChkDebug (&pMod->oe)) {
            KSetLastError(pCurThread,(DWORD)NTE_BAD_SIGNATURE);
            return 0;
        }
        if ((pCurProc->bTrustLevel == KERN_TRUST_FULL) && (pMod->bTrustLevel == KERN_TRUST_RUN)) {
            KSetLastError(pCurThread,(DWORD)NTE_BAD_SIGNATURE);
            return 0;
        }

        // kernel library special handling (NITRO SPECIFIC)
        if (IsKernelVa (pMod->BasePtr) && fSpecial) {
            if (!(pMod->inuse & pCurProc->aky)) {
                DEBUGMSG (1, (L"KernelLibrary can only be loaded by 1 process\r\n"));
                return 0;
            }
            IncRefCount (pMod);
            return pMod;
        }

        if (pMod->wFlags != wFlags) {
            KSetLastError(pCurThread,ERROR_BAD_EXE_FORMAT);
            return 0;
        }

        // inc the ref count and copy region if loaded in the process for the first time
        if (!(prevRefCnt = IncRefCount (pMod)) && !(wFlags & (LOAD_LIBRARY_IN_KERNEL|LOAD_LIBRARY_AS_DATAFILE)) && !CopyRegions (pMod)) {
            DecRefCount (pMod);
            dwErr = ERROR_OUTOFMEMORY;
        } else {
            if ((fLbFlags & LLIB_NO_PAGING) && (pMod->oe.pagemode == PM_FULLPAGING)) {
                // the module was loaded with LoadLibrary and now trying to LoadDriver on it.
                o32_lite *optr = pMod->o32_ptr;
                int       i;
                pMod->oe.pagemode = PM_NOPAGEOUT;
                // if we're using paging pool, need to return the pages back to the paging pool first
                EnterCriticalSection (&PagerCS);
                if (INVALID_PG_INDEX != pMod->pgqueue.idxHead) {
                    UnloadMod (pMod);
                    FreeAllPagesFromQ (&pMod->pgqueue);
                }
                LeaveCriticalSection (&PagerCS);
                DEBUGMSG(ZONE_LOADER1,(TEXT("LoadOneLibraryPart2 - change from pageable to not paged\r\n")));
                // page in all the pages of the module
                for (i = 0; i < pMod->e32.e32_objcnt; i ++, optr ++) {
                     DEBUGMSG(ZONE_LOADER1,(TEXT("LoadOneLibraryPart2 - paging in %8.8lx->%8.8lx, (%a)\r\n"),
                        ZeroPtr (optr->o32_realaddr), ZeroPtr (optr->o32_realaddr) + optr->o32_vsize, 
                        (optr->o32_flags & IMAGE_SCN_MEM_WRITE)? "WRITE" : "READ"));
                    if (!DoLockPages ((LPVOID)ZeroPtr (optr->o32_realaddr), optr->o32_vsize, NULL, 
                            LOCKFLAG_QUERY_ONLY | ((optr->o32_flags & IMAGE_SCN_MEM_WRITE)? LOCKFLAG_WRITE : LOCKFLAG_READ))) {
                        dwErr = ERROR_OUTOFMEMORY;
                        pMod->oe.pagemode = PM_FULLPAGING;
                        break;
                    }
                }
            }
            if (dwErr) {
                // the region of pMod has already Copied, need to release it.
                if (!prevRefCnt)
                    UnCopyRegions (pMod);
                DecRefCount (pMod);
            } else if (pMod->pmodResource) {
                // just increment the ref-count. DO NOT call CopyRegion for resource only files.
                IncRefCount (pMod->pmodResource);
            }
        }

    } else {
        //
        // Module instance does not currently exist.
        //
        if (!(pMod = AllocMem(HEAP_MODULE))) {
            KSetLastError(pCurThread, ERROR_OUTOFMEMORY);
            return NULL;
        }

        // initialize the module structure
        if ((dwErr = InitModule (pMod, (LPWSTR) lpszFileName, (LPWSTR) dllname, fLbFlags, wFlags))
            || (!(wFlags & LOAD_LIBRARY_IN_KERNEL) && !IsModCodeAddr (pMod->BasePtr) && !IsInResouceSection (pMod->BasePtr) && !CopyRegions (pMod))) {
            if (!dwErr)
                dwErr = ERROR_OUTOFMEMORY;
        } else {

            // increase the refcount
            IncRefCount (pMod);

            // Make pMod the first element of the list
            EnterCriticalSection(&ModListcs);
            pMod->pMod = pModList;
            pModList = pMod;
            LeaveCriticalSection(&ModListcs);

            // can't load MUI before this since we might have to query resource section of current module.
            if (!(fLbFlags & LLIB_NO_MUI)) {
                pMod->pmodResource = LoadMUI (pMod, pMod->BasePtr, &pMod->e32);
            }
            
        }
    }

    DEBUGCHK (pMod);
    pModRes = pMod->pmodResource;

    if (!dwErr && !prevRefCnt && !(pMod->wFlags & DONT_RESOLVE_DLL_REFERENCES)) {
        dwErr = DoImports(&pMod->e32, pMod->o32_ptr, (ulong)pMod->BasePtr, dllname);
        if (dwErr) {
            // DoImports WILL Cleanup the library it loaded if it failed. DO NOT
            // call UnDoDepends Here.
            //
            // UnDoDepends (pMod);

            // at this point, MUI has already loaded, need to free it too
            if (pModRes) {
                UnCopyRegions (pModRes);
                DecRefCount (pModRes);
            }
            UnCopyRegions (pMod);
            DecRefCount (pMod);
        }
    }

    if (dwErr || prevRefCnt) {
        if (dwErr) {
            if (!pMod->inuse) {
                FreeModuleMemory (pMod);
                if (pModRes) {
                    DEBUGCHK (!pModRes->inuse);
                    FreeModuleMemory (pModRes);
                }
            }
            KSetLastError (pCurThread, dwErr);
            return NULL;
        }
        // the module has loaded in the same process before, just return it
        return pMod;
    }



    // notify kD
    if (!(pMod->DbgFlags & DBG_SYMBOLS_LOADED))
        KDUpdateSymbols(((DWORD)pMod->BasePtr)+1, FALSE);

    // update module reference
    if (fSpecial) {
        pMod->wFlags &= ~LOAD_LIBRARY_IN_KERNEL;

    } else if (wFlags & LOAD_LIBRARY_IN_KERNEL) {
        pMod->inuse |= 1;
        pMod->refcnt[0]++;
        pMod->DbgFlags |= DBG_IS_DEBUGGER;
    }

    // check patch
    if ((pMod->oe.filetype == FT_ROMIMAGE)
        && SystemAPISets[SH_PATCHER]
        && !PatchDll(pCurProc, pMod, lpszFileName)) {
        DEBUGMSG(1,(TEXT("LoadOneLibrary Patcher failure for %s!\r\n"),lpszFileName));
        FreeOneLibrary(pMod, 0);
        dwErr = NTE_BAD_SIGNATURE;
    }

    if (wFlags & LOAD_LIBRARY_IN_KERNEL) {
        //
        // Done loading the code into the kernel space via the data cache.
        // Now we need to flush the data cache since there may be a separate 
        // instruction cache.
        //
        OEMCacheRangeFlush(0, 0, CACHE_SYNC_WRITEBACK);
    }

    if (!dwErr
        && !CallDLLEntry (pMod, DLL_PROCESS_ATTACH,
                          ((wFlags & LOAD_LIBRARY_IN_KERNEL) ? (LPVOID) NKKernelLibIoControl : 0))) {
        DEBUGMSG(1,(TEXT("LoadOneLibrary libentry failure for %s!\r\n"),lpszFileName));
        FreeOneLibrary(pMod, 0);
        dwErr = ERROR_DLL_INIT_FAILED;
    }


    if (dwErr) {
        DEBUGMSG (ZONE_LOADER1, (L"LoadOneLibraryPart2 '%s' failed, err = 0x%x\r\n", lpszFileName, dwErr));
        KSetLastError (pCurThread, dwErr);
        pMod = NULL;
    } else if (pCurThread->pThrdDbg && ProcStarted(pCurProc) && pCurThread->pThrdDbg->hEvent) {
        pCurThread->pThrdDbg->dbginfo.dwProcessId = (DWORD)hCurProc;
        pCurThread->pThrdDbg->dbginfo.dwThreadId = (DWORD)hCurThread;
        pCurThread->pThrdDbg->dbginfo.dwDebugEventCode = LOAD_DLL_DEBUG_EVENT;
        pCurThread->pThrdDbg->dbginfo.u.LoadDll.hFile = NULL;
        pCurThread->pThrdDbg->dbginfo.u.LoadDll.lpBaseOfDll = (LPVOID)ZeroPtr(pMod->BasePtr);
        pCurThread->pThrdDbg->dbginfo.u.LoadDll.dwDebugInfoFileOffset = 0;
        pCurThread->pThrdDbg->dbginfo.u.LoadDll.nDebugInfoSize = 0;
        pCurThread->pThrdDbg->dbginfo.u.LoadDll.lpImageName = pMod->lpszModName;
        pCurThread->pThrdDbg->dbginfo.u.LoadDll.fUnicode = 1;
        SetEvent(pCurThread->pThrdDbg->hEvent);
        SC_WaitForMultiple(1,&pCurThread->pThrdDbg->hBlockEvent,FALSE,INFINITE);
    }

    if (IsCeLogKernelEnabled() && pMod) {
        CELOG_ModuleLoad(pCurProc->hProc, (HANDLE)pMod, lpszFileName, (DWORD)pMod->BasePtr);
    }
    return pMod;
}



//------------------------------------------------------------------------------
// Win32 LoadLibrary call
//------------------------------------------------------------------------------
HANDLE 
SC_LoadLibraryW(
    LPCWSTR lpszFileName
    ) 
{
    HANDLE retval;

    DEBUGMSG(ZONE_ENTRY,(L"SC_LoadLibraryW entry: %8.8lx\r\n",lpszFileName));
    EnterCriticalSection(&LLcs);
    retval = LoadOneLibraryW(lpszFileName,0,0);
    LeaveCriticalSection(&LLcs);
    DEBUGMSG(ZONE_ENTRY,(L"SC_LoadLibraryW exit: %8.8lx\r\n",retval));
    return retval;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
HINSTANCE 
SC_LoadLibraryExW(
    LPCWSTR lpLibFileName, 
    HANDLE hFile, 
    DWORD dwFlags
    ) 
{
    HANDLE retval;

    DEBUGMSG(ZONE_ENTRY,(L"SC_LoadLibraryExW entry: %8.8lx %8.8lx %8.8lx\r\n",lpLibFileName, hFile, dwFlags));
    if (hFile || (dwFlags & ~(LOAD_LIBRARY_AS_DATAFILE|DONT_RESOLVE_DLL_REFERENCES))) {
        KSetLastError(pCurThread,ERROR_INVALID_PARAMETER);
        retval = 0;
    } else {
        EnterCriticalSection(&LLcs);
        DEBUGCHK(!(dwFlags & 0xffff0000));
        retval = LoadOneLibraryW(lpLibFileName,0,(WORD)dwFlags);
        LeaveCriticalSection(&LLcs);
    }
    DEBUGMSG(ZONE_ENTRY,(L"SC_LoadLibraryExW exit: %8.8lx\r\n",retval));
    return retval;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
HMODULE 
SC_GetModuleHandleW(
    LPCWSTR lpModuleName
    ) 
{
    HMODULE retval;
    LPCWSTR dllname;

    if (!lpModuleName)
        return pCurThread->pOwnerProc->hProc;
    EnterCriticalSection(&LLcs);
    dllname = lpModuleName+strlenW(lpModuleName);
    while ((dllname != lpModuleName) && (*(dllname-1) != (WCHAR)'\\') && (*(dllname-1) != (WCHAR)'/'))
        dllname--;
    retval = (HMODULE)FindModByName(dllname, 0);
    if (!retval)
        retval = (HMODULE)FindModByName(dllname, LOAD_LIBRARY_AS_DATAFILE);
    if (retval && !HasModRefProcPtr((PMODULE)retval,pCurProc))
        retval = 0;
    LeaveCriticalSection(&LLcs);
    return retval;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
HANDLE 
SC_LoadDriver(
    LPCWSTR lpszFileName
    ) 
{
    HANDLE retval;

    TRUSTED_API (L"SC_LoadDriver", NULL);

    DEBUGMSG(ZONE_ENTRY,(L"SC_LoadDriver entry: %8.8lx\r\n",lpszFileName));
    EnterCriticalSection(&LLcs);
    retval = LoadOneLibraryW(lpszFileName,LLIB_NO_PAGING,0);
    LeaveCriticalSection(&LLcs);
    DEBUGMSG(ZONE_ENTRY,(L"SC_LoadDriver exit: %8.8lx\r\n",retval));
    return retval;
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
HANDLE
SC_LoadKernelLibrary(
    LPCWSTR lpszFileName
    )
{
    PKERNELMOD pKMod;

    TRUSTED_API (L"SC_LoadKernelLibrary", NULL);

    DEBUGMSG(ZONE_ENTRY,(L"SC_LoadKernelLibrary entry: %s\r\n",lpszFileName));

    pKMod = AllocMem(HEAP_KERNELMOD);
    if (pKMod == NULL) {
        KSetLastError(pCurThread, ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    pKMod->bIRQ = 0;
    pKMod->pMod = NULL;
    pKMod->pfnIoctl = NULL;
    pKMod->dwInstData = 0;
    pKMod->lpSelf = pKMod;

    EnterCriticalSection(&LLcs);

    pKMod->pMod = (PMODULE) LoadOneLibraryW(lpszFileName, LLIB_NO_PAGING, LOAD_LIBRARY_IN_KERNEL);
    if (pKMod->pMod != NULL) {
        pKMod->pfnIoctl = GetOneProcAddress((HANDLE)pKMod->pMod, TEXT("IOControl"), TRUE);
    } else {
        FreeMem(pKMod, HEAP_KERNELMOD);
        pKMod = NULL;
    }

    LeaveCriticalSection(&LLcs);

    DEBUGMSG(ZONE_ENTRY, (L"SC_LoadKernelLibrary exit: %8.8lx\r\n", pKMod ? pKMod->pMod : NULL));
    return (HANDLE) pKMod;
}


//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
HANDLE
SC_LoadIntChainHandler(
    LPCWSTR lpszFileName,
    LPCWSTR lpszFunctionName,
    BYTE    bIRQ
    ) 
{
    BOOL fRet;
    PKERNELMOD pKMod = NULL;

    TRUSTED_API (L"SC_LoadIntChainHandler", NULL);

    DEBUGMSG(ZONE_ENTRY,(L"SC_LoadIntChainHandler entry: %s\r\n",lpszFileName));

    pKMod = AllocMem(HEAP_KERNELMOD);
    if (pKMod == NULL) {
        KSetLastError(pCurThread,ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    pKMod->bIRQ = bIRQ;
    pKMod->pMod = NULL;
    pKMod->pfnIoctl = NULL;
    pKMod->dwInstData = 0;
    pKMod->lpSelf = pKMod;

    EnterCriticalSection(&LLcs);

    pKMod->pMod = (PMODULE) LoadOneLibraryW(lpszFileName, LLIB_NO_PAGING, LOAD_LIBRARY_IN_KERNEL);

    if (pKMod->pMod != NULL) {
        FARPROC pfnInit = (FARPROC) GetOneProcAddress((HANDLE) pKMod->pMod, TEXT("CreateInstance"), TRUE);

        // get the instance data from the library
        if (pfnInit) {
            pKMod->dwInstData = (DWORD) pfnInit ();
            
            if (-1 == pKMod->dwInstData) {
                goto ERROR_RET;
            }
        }
        pKMod->pfnIoctl = GetOneProcAddress((HANDLE) pKMod->pMod, TEXT("IOControl"), TRUE);

        if (lpszFunctionName != NULL) {

            pKMod->pfnHandler = (FARPROC) GetOneProcAddress((HANDLE) pKMod->pMod, lpszFunctionName, TRUE);

            if (pKMod->pfnHandler != NULL) {
                fRet = HookIntChain(pKMod->bIRQ, pKMod);

                if (fRet == FALSE) {
                    goto ERROR_RET;
                }
            } else {
                //
                // If a function was asked for that couldn't be found, unload
                // the driver and error out. Note that if lpszFunctionName is 
                // NULL we load the code into the kernel and leave it alone.
                // 
                goto ERROR_RET;
            }
        }
    } else {
        goto ERROR_RET;
    }

    LeaveCriticalSection(&LLcs);

    DEBUGMSG(ZONE_ENTRY,(L"SC_LoadIntChainHandler exit: %8.8lx\r\n",pKMod->pMod));
    return (HANDLE) pKMod;

ERROR_RET:
    if (pKMod->pMod) {
        FreeOneLibrary(pKMod->pMod, 1);
    }
    if (pKMod) {
        FreeMem(pKMod, HEAP_KERNELMOD);
    }
    LeaveCriticalSection(&LLcs);
    DEBUGMSG(ZONE_ENTRY,(L"SC_LoadIntChainHandler exit: FAILED (%d)\r\n", pCurThread->dwLastError));
    return NULL;
}


//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
BOOL
SC_FreeIntChainHandler(
    HANDLE hLib
    ) 
{
    BOOL fRet;
    PKERNELMOD pKMod = (PKERNELMOD) hLib;
    FARPROC pfnDestroy;

    TRUSTED_API (L"SC_FreeIntChainHandler", FALSE);
    
    DEBUGMSG(ZONE_ENTRY,(L"SC_FreeIntChainHandler entry: %8.8lx\r\n", hLib));

    if ((DWORD) pKMod <= KMOD_MAX || !IsValidModule(pKMod)) {
        KSetLastError(pCurThread,ERROR_INVALID_HANDLE);
        return FALSE;
    } 

    EnterCriticalSection(&LLcs);

    // Destroy instance
    pfnDestroy = (FARPROC)GetOneProcAddress((HANDLE)pKMod->pMod, TEXT("DestroyInstance"), TRUE);
    
    if (pfnDestroy) {
        pfnDestroy(pKMod->dwInstData);
    }

    // Unhook handler
    if (pKMod->pfnHandler != NULL) {
        fRet = UnhookIntChain(pKMod->bIRQ, pKMod);

        if (fRet == TRUE) {
            fRet = FreeOneLibrary(pKMod->pMod, 1);
        }
    } else {
        fRet = FreeOneLibrary(pKMod->pMod, 1);
    }

    LeaveCriticalSection(&LLcs);

    if (pKMod) {
        FreeMem(pKMod, HEAP_KERNELMOD);
    }

    DEBUGMSG(ZONE_ENTRY,(L"SC_FreeIntChainHandler exit: %8.8lx\r\n",fRet));
    return fRet;
}


//------------------------------------------------------------------------------
// Win32 FreeLibrary call
//------------------------------------------------------------------------------
BOOL 
SC_FreeLibrary(
    HANDLE hInst
    ) 
{
    BOOL retval;
    PMODULE pMod;

    DEBUGMSG(ZONE_ENTRY,(L"SC_FreeLibrary entry: %8.8lx\r\n",hInst));
    if (!(pMod = (PMODULE)hInst))
        return FALSE;
    EnterCriticalSection(&LLcs);
    retval = FreeOneLibrary(pMod, 1);
    LeaveCriticalSection(&LLcs);
    DEBUGMSG(ZONE_ENTRY,(L"SC_FreeLibrary exit: %8.8lx\r\n",retval));
    return retval;
}



#define MAXIMPNAME 128

//------------------------------------------------------------------------------
// GetProcAddress, no critical section protection
//------------------------------------------------------------------------------
LPVOID 
GetOneProcAddress(
    HANDLE hInst, 
    LPCVOID lpszProc, 
    BOOL bUnicode
    ) 
{
    PMODULE pMod;
    DWORD retval;
    CHAR lpszImpName[MAXIMPNAME];

    pMod = (PMODULE)hInst;
    if (!IsValidModule(pMod)) {
        KSetLastError(pCurThread,ERROR_INVALID_HANDLE);
        retval = 0;
    } else {
        if ((DWORD)lpszProc>>16) {
            if (bUnicode) {
                KUnicodeToAscii(lpszImpName,lpszProc,MAXIMPNAME);
                retval = ResolveImpStr(pMod,lpszImpName);
            } else
                retval = ResolveImpStr(pMod,lpszProc);
        } else
            retval = ResolveImpOrd(pMod,(DWORD)lpszProc);
        if (!retval)
            KSetLastError(pCurThread,ERROR_INVALID_PARAMETER);
    }
    return (LPVOID)retval;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
BOOL 
DisableThreadCalls(
    PMODULE pMod
    ) 
{
    BOOL retval;

    KCALLPROFON(6);
    if (IsValidModule(pMod)) {
        pMod->dwNoNotify |= (1 << pCurProc->procnum);
        retval = TRUE;
    } else {
        KSetLastError(pCurThread,ERROR_INVALID_HANDLE);
        retval = FALSE;
    }
    KCALLPROFOFF(6);
    return retval;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
BOOL 
SC_DisableThreadLibraryCalls(
    PMODULE hLibModule
    ) 
{
    return KCall((PKFN)DisableThreadCalls,hLibModule);
}



//------------------------------------------------------------------------------
// Win32 GetProcAddress call
//------------------------------------------------------------------------------
LPVOID 
SC_GetProcAddressA(
    HANDLE hInst, 
    LPCSTR lpszProc
    ) 
{
    LPVOID retval = 0;

    DEBUGMSG(ZONE_ENTRY,(L"SC_GetProcAddressA entry: %8.8lx %8.8lx\r\n",hInst,lpszProc));
    __try {
        retval = GetOneProcAddress(hInst,lpszProc, FALSE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        KSetLastError(pCurThread,ERROR_INVALID_HANDLE);
    }
    DEBUGMSG(ZONE_LOADER1,(TEXT("SC_GetProcAddressA %8.8lx %8.8lx/%a -> %8.8lx\r\n"),hInst,lpszProc,(DWORD)lpszProc>0xffff ? lpszProc : "<ordinal>",retval));
    DEBUGMSG(ZONE_ENTRY,(L"SC_GetProcAddressA exit: %8.8lx\r\n",retval));
    return retval;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
LPVOID 
SC_GetProcAddressW(
    HANDLE hInst, 
    LPWSTR lpszProc
    ) 
{
    LPVOID retval = 0;

    DEBUGMSG(ZONE_ENTRY,(L"SC_GetProcAddressW entry: %8.8lx %8.8lx\r\n",hInst,lpszProc));
    __try {
        retval = GetOneProcAddress(hInst,lpszProc, TRUE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        KSetLastError(pCurThread,ERROR_INVALID_HANDLE);
    }
    DEBUGMSG(ZONE_LOADER1,(TEXT("SC_GetProcAddressW %8.8lx %s -> %8.8lx\r\n"),hInst,(DWORD)lpszProc>0xffff ? lpszProc : L"<ordinal>",retval));
    DEBUGMSG(ZONE_ENTRY,(L"SC_GetProcAddressW exit: %8.8lx\r\n",retval));
    return retval;
}


#ifndef x86

typedef struct {
    void *          func;
    unsigned int    prologLen   : 8;
    unsigned int    funcLen     : 22;
    unsigned int    is32Bit     : 1;
    unsigned int    hasHandler  : 1;
} pdata_t;

typedef struct _pdata_tag {
    struct _pdata_tag *pNextTable;
    int numentries; // number of entries below
    pdata_t entry[];    // entries in-lined like in a .pdata section
} XPDATA, *LPXPDATA;

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
BOOL 
MatchesExtRange(
    ULONG pc, 
    LPXPDATA pPData
    ) 
{
    ULONG start, end;

    if (pPData->numentries) {
        start = (ULONG)pPData->entry[0].func;
        end = (ULONG)pPData->entry[pPData->numentries-1].func + pPData->entry[pPData->numentries-1].funcLen;
        if ((pc >= start) && (pc < end))
            return TRUE;
        pc = ZeroPtr(pc);
        if ((pc >= start) && (pc < end))
            return TRUE;
    }
    return FALSE;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
PVOID 
PDataFromExt(
    ULONG pc, 
    PPROCESS pProc, 
    PULONG pSize
    ) 
{
    LPXPDATA pPData;
    PVOID pResult = 0;

    if (pProc->pExtPdata) {
        __try {
            pPData = *(LPXPDATA *)pProc->pExtPdata;
            while (pPData) {
                if (MatchesExtRange(pc,pPData)) {
                    pResult = &pPData->entry[0];
                    *pSize = pPData->numentries * sizeof(pPData->entry[0]);
                    break;
                }
                pPData = pPData->pNextTable;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return pResult;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
PVOID 
PDataFromPC(
    ULONG pc, 
    PPROCESS pProc, 
    PULONG pSize
    ) 
{
    ULONG mappc, unmappc;
    PMODULE pMod;
    PVOID retval = 0;

    mappc = (ULONG)MapPtrProc(pc,pProc);
    unmappc = ZeroPtr(pc);
    *pSize = 0;
    if ((mappc >= (ULONG)ProcArray[0].e32.e32_vbase) && (mappc < (ULONG)ProcArray[0].BasePtr + (ULONG)ProcArray[0].e32.e32_vsize)) {
        *pSize = ProcArray[0].e32.e32_unit[EXC].size;
        retval = (PVOID)((ULONG)ProcArray[0].BasePtr + ProcArray[0].e32.e32_unit[EXC].rva);
        DEBUGMSG ((pProc != ProcArray) && ZONE_SEH, (L"PDataFromPC: Returning SLOT1 ADDRESS while pProc != ProcArray %8.8lx %8.8lx %8.8lx!!!\n",
            ProcArray[0].BasePtr, ProcArray[0].e32.e32_unit[EXC].rva, retval));
    } else if ((mappc >= (ULONG)MapPtrProc(pProc->BasePtr,pProc)) &&
        (mappc < (ULONG)MapPtrProc(pProc->BasePtr,pProc)+pProc->e32.e32_vsize)) {
        *pSize = pProc->e32.e32_unit[EXC].size;
        retval = MapPtrProc((ULONG)pProc->BasePtr+pProc->e32.e32_unit[EXC].rva,pProc);
    } else if (!(retval = PDataFromExt(mappc,pProc,pSize))) {
        EnterCriticalSection(&ModListcs);
        pMod = pModList;
        while (pMod) {
            if ((unmappc >= ZeroPtr(pMod->BasePtr)) &&
                (unmappc < ZeroPtr(pMod->BasePtr)+pMod->e32.e32_vsize)) {
                *pSize = pMod->e32.e32_unit[EXC].size;
//                retval = (PVOID)(ProcArray[0].dwVMBase + ZeroPtr((ULONG)pMod->BasePtr+pMod->e32.e32_unit[EXC].rva));
                if (IsModCodeAddr (unmappc))
                    retval = (PVOID)((ULONG)pMod->BasePtr+pMod->e32.e32_unit[EXC].rva);
                else
                    retval = (PVOID)(pProc->dwVMBase + ZeroPtr((ULONG)pMod->BasePtr+pMod->e32.e32_unit[EXC].rva));
                DEBUGMSG (ZONE_SEH, (L"PDataFromPC: Returning %8.8lx\n", retval));
                break;
            }
            pMod = pMod->pMod;
        }
        LeaveCriticalSection(&ModListcs);
    }
    return retval;
}

#endif



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
int 
LoaderPageIn(
    BOOL bWrite, 
    DWORD addr
    ) 
{
    BOOL retval = PAGEIN_FAILURE;
    PMODULE pMod = 0;
    DWORD zaddr, low, high;
    int loop;
    MEMORY_BASIC_INFORMATION mbi;

    addr = (DWORD)MapPtr(addr);
    zaddr = ZeroPtr(addr);

    DEBUGMSG(ZONE_PAGING,(L"LPI: addr = %8.8lx, zaddr = %8.8lx\r\n", addr, zaddr));
    if (zaddr >= (DWORD)DllLoadBase) {
        // in DLL
        EnterCriticalSection(&ModListcs);
        pMod = pModList;
        while (pMod) {
            low = ZeroPtr(pMod->BasePtr);
            high = low + pMod->e32.e32_vsize;
            if (((zaddr >= low) && (zaddr < high))
                || ((zaddr >= pMod->rwLow) && (zaddr < pMod->rwHigh))) {
                //DEBUGCHK (((zaddr >= low) && (zaddr < high)) || !IsSecureVa (addr) || !SystemAPISets[SH_FILESYS_APIS]);
                break;
            }
            pMod = pMod->pMod;
        }
        LeaveCriticalSection(&ModListcs);
        if (!pMod) {
            DEBUGMSG(ZONE_PAGING,(L"    DLL slot not found!\r\n"));
            return retval;
        }
        if (!IsModCodeAddr (addr) && !IsInResouceSection (addr) && !IsSecureVa (addr)) {
            DWORD procBit = 1 << ((addr >> VA_SECTION)-1);
            if (!(pMod->inuse & procBit)) {
                DEBUGMSG(ZONE_PAGING,(L"    DLL not loaded in the process (procBit = %8.8lx, inuse = %8.8lx)!\r\n",
                    procBit, pMod->inuse));
                return retval;
            }
        }
    } else {
        for (loop = 0; loop < MAX_PROCESSES; loop++) {
            if ((addr >= ProcArray[loop].dwVMBase) && (addr < ProcArray[loop].dwVMBase + (1<<VA_SECTION))) {
                break;
            }
        }
        if (loop == MAX_PROCESSES) {
            DEBUGMSG(ZONE_PAGING,(L"    Proc slot not found!\r\n"));
            return retval;
        }
    }
    EnterCriticalSection(&PagerCS);
    DEBUGMSG(ZONE_PAGING,(L"LPI: %8.8lx (%d)\r\n",addr,bWrite));
    VirtualQuery((LPVOID)addr,&mbi,sizeof(mbi));
    if (mbi.State == MEM_COMMIT) {
        if ((mbi.Protect == PAGE_READWRITE) || (mbi.Protect == PAGE_EXECUTE_READWRITE) ||
            (!bWrite && ((mbi.Protect == PAGE_READONLY) || (mbi.Protect == PAGE_EXECUTE_READ))))
            retval = PAGEIN_SUCCESS;
        DEBUGMSG(ZONE_PAGING,(L"LPI: %8.8lx (%d) -> %d (1) [%x]\r\n",addr,bWrite,retval,mbi.Protect));
        LeaveCriticalSection(&PagerCS);
        return retval;
    }
    if (pMod)
        retval = PageInModule(pMod, addr);
    else
        retval = PageInProcess(&ProcArray[loop], addr);
    DEBUGMSG(ZONE_PAGING,(L"LPI: %8.8lx (%d) -> %d (2)\r\n",addr,bWrite,retval));
    LeaveCriticalSection(&PagerCS);
    return retval;
}



//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
PageOutProc(void) 
{
    int loop;

    DEBUGMSG(ZONE_PAGING,(L"POP: Starting page free count: %d\r\n",PageFreeCount));
    for (loop = 0; loop < MAX_PROCESSES; loop++)
        if (ProcArray[loop].dwVMBase && ProcStarted(&ProcArray[loop])) {
            __try {
                UnloadExe(&ProcArray[loop]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // Pretend it didn't happen
                RETAILMSG(1, (TEXT("Exception in UnloadExe... continuing to next process\r\n")));
            }
        }
    DEBUGMSG(ZONE_PAGING,(L"POP: Ending page free count: %d\r\n",PageFreeCount));
}



extern long PageOutNeeded;

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void 
PageOutMod(void) 
{
    static int storedModNum;
    int modCount;
    PMODULE pMod, pModStart;

    if (PageOutNeeded) {
        EnterCriticalSection(&ModListcs);
        DEBUGMSG(ZONE_PAGING,(L"POM: Starting page free count: %d\r\n",PageFreeCount));
        // start from the net module of the last one we paged out
        for (pMod = pModList, modCount = 0; pMod && (modCount < storedModNum); pMod = pMod->pMod, modCount++)
            ;

        if (!pMod) {
            pMod = pModList;
            modCount = 0;
        }
        pModStart = pMod;
        do {
            __try {
                if (pMod->oe.pagemode == PM_FULLPAGING) {
                    UnloadMod(pMod);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // Pretend it didn't happen
                RETAILMSG(1, (TEXT("Exception in UnloadMod... continuing to next module\r\n")));
            }
            if (pMod = pMod->pMod) {
                modCount++;
            } else {
                pMod = pModList;
                modCount = 0;
            }
        } while (PageOutNeeded && (pMod != pModStart));

        // record the number where we page out last.
        storedModNum = modCount;
        DEBUGMSG(ZONE_PAGING,(L"POM: Ending page free count: %d\r\n",PageFreeCount));
        LeaveCriticalSection(&ModListcs);
    }
}
