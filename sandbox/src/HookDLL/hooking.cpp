// HookDLL.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "hooking.h"

//
// Defines
//

#define DETOUR_END

//
// Lib Load APIs.
//

//
// Globals
//
extern decltype(LdrLoadDll) *TrueLdrLoadDll;
extern decltype(LdrGetProcedureAddressEx) *TrueLdrGetProcedureAddressEx;
extern decltype(LdrGetDllHandleEx) *TrueLdrGetDllHandleEx;

extern decltype(NtOpenFile) *TrueNtOpenFile;
extern decltype(NtCreateFile) *TrueNtCreateFile;
extern decltype(NtReadFile) *TrueNtReadFile;
extern decltype(NtWriteFile) *TrueNtWriteFile;
extern decltype(NtDeleteFile) *TrueNtDeleteFile;
extern decltype(NtSetInformationFile) *TrueNtSetInformationFile;
extern decltype(NtQueryDirectoryFile) *TrueNtQueryDirectoryFile;
extern decltype(NtQueryInformationFile) *TrueNtQueryInformationFile;

extern decltype(NtProtectVirtualMemory) *TrueNtProtectVirtualMemory;
extern decltype(NtQueryVirtualMemory) *TrueNtQueryVirtualMemory;
extern decltype(NtReadVirtualMemory) *TrueNtReadVirtualMemory;
extern decltype(NtWriteVirtualMemory) *TrueNtWriteVirtualMemory;
extern decltype(NtMapViewOfSection) *TrueNtMapViewOfSection;
extern decltype(NtUnmapViewOfSection) *TrueNtUnmapViewOfSection;
extern decltype(NtAllocateVirtualMemory) *TrueNtAllocateVirtualMemory;
extern decltype(NtFreeVirtualMemory) *TrueNtFreeVirtualMemory;

extern decltype(NtOpenKey) *TrueNtOpenKey;
extern decltype(NtOpenKeyEx) *TrueNtOpenKeyEx;
extern decltype(NtCreateKey) *TrueNtCreateKey;
extern decltype(NtQueryValueKey) *TrueNtQueryValueKey;
extern decltype(NtSetValueKey) *TrueNtSetValueKey;
extern decltype(NtDeleteKey) *TrueNtDeleteKey;
extern decltype(NtDeleteValueKey) *TrueNtDeleteValueKey;

extern decltype(NtOpenProcess) *TrueNtOpenProcess;
extern decltype(NtTerminateProcess) *TrueNtTerminateProcess;
extern decltype(NtCreateUserProcess) *TrueNtCreateUserProcess;
extern decltype(NtCreateThread) *TrueNtCreateThread;
extern decltype(NtCreateThreadEx) *TrueNtCreateThreadEx;
extern decltype(NtSuspendThread) *TrueNtSuspendThread;
extern decltype(NtResumeThread) *TrueNtResumeThread;
extern decltype(NtContinue) *TrueNtContinue;

extern decltype(NtQuerySystemInformation) *TrueNtQuerySystemInformation;
extern decltype(RtlDecompressBuffer) *TrueRtlDecompressBuffer;
extern decltype(NtDelayExecution) *TrueNtDelayExecution;
extern decltype(NtLoadDriver) *TrueNtLoadDriver;

__vsnwprintf_fn_t _vsnwprintf = nullptr;
__snwprintf_fn_t _snwprintf = nullptr;
strlen_fn_t _strlen = nullptr;
pfn_wcsstr _wcsstr = nullptr;
pfnStringFromGUID2 _StringFromGUID2 = nullptr;
pfnStringFromCLSID _StringFromCLSID = nullptr;
pfnCoTaskMemFree _CoTaskMemFree = nullptr;
pfnCoCreateInstanceEx TrueCoCreateInstanceEx = nullptr;

pfnInternetOpenA TrueInternetOpenA = nullptr;
pfnInternetConnectA TrueInternetConnectA = nullptr;
pfnInternetConnectW TrueInternetConnectW = nullptr;
pfnHttpOpenRequestA TrueHttpOpenRequestA = nullptr;
pfnHttpOpenRequestW TrueHttpOpenRequestW = nullptr;
pfnHttpSendRequestA TrueHttpSendRequestA = nullptr;
pfnHttpSendRequestW TrueHttpSendRequestW = nullptr;
pfnInternetReadFile TrueInternetReadFile = nullptr;

CRITICAL_SECTION gInsideHookLock, gHookDllLock;
BOOL gInsideHook = FALSE;
DWORD dwTlsIndex;
extern CRITICAL_SECTION gDbgHelpLock;
HookInfo gHookInfo;

#define MAX_FRAME 5
PVOID gFrames[MAX_FRAME];

//
// ETW provider GUID and global provider handle.
// GUID:
//   {a4b4ba50-a667-43f5-919b-1e52a6d69bd5}
//

GUID ProviderGuid = {0xa4b4ba50, 0xa667, 0x43f5, {0x91, 0x9b, 0x1e, 0x52, 0xa6, 0xd6, 0x9b, 0xd5}};

REGHANDLE ProviderHandle;
#define ATTACH(x) DetAttach(&(PVOID &)True##x, Hook##x, #x)
#define DETACH(x) DetDetach(&(PVOID &)True##x, Hook##x, #x)

typedef struct _STACKTRACE
{
    //
    // Number of frames in Frames array.
    //
    UINT FrameCount;

    //
    // PC-Addresses of frames. Index 0 contains the topmost frame.
    //
    ULONGLONG Frames[ANYSIZE_ARRAY];
} STACKTRACE, *PSTACKTRACE;

PMODULE_INFORMATION_TABLE
SfwQueryModules(IN PPEB pPeb)
{
    ULONG Count = 0;
    ULONG CurCount = 0;
    PLIST_ENTRY pEntry = NULL;
    PLIST_ENTRY pHeadEntry = NULL;
    PPEB_LDR_DATA pLdrData = NULL;
    PMODULE_ENTRY CurModule = NULL;
    PLDR_DATA_TABLE_ENTRY pLdrEntry = NULL;
    PMODULE_INFORMATION_TABLE pModuleInformationTable = NULL;

    pLdrData = pPeb->Ldr;
    pHeadEntry = &pLdrData->InMemoryOrderModuleList;

    // Count user modules : iterate through the entire list
    pEntry = pHeadEntry->Flink;
    while (pEntry != pHeadEntry)
    {
        Count++;
        pEntry = pEntry->Flink;
    }

    // Allocate a MODULE_INFORMATION_TABLE
    pModuleInformationTable =
        (PMODULE_INFORMATION_TABLE)RtlAllocateHeap(RtlProcessHeap(), 0, sizeof(MODULE_INFORMATION_TABLE));
    if (!pModuleInformationTable)
    {
        LogMessage(L"Cannot allocate a MODULE_INFORMATION_TABLE.");
        return NULL;
    }

    // Allocate the correct amount of memory depending of the modules count
    pModuleInformationTable->Modules =
        (PMODULE_ENTRY)RtlAllocateHeap(RtlProcessHeap(), 0, Count * sizeof(MODULE_ENTRY));
    if (!pModuleInformationTable->Modules)
    {
        LogMessage(L"Cannot allocate a MODULE_INFORMATION_TABLE.");
        return NULL;
    }

    // Fill the basic information of MODULE_INFORMATION_TABLE
    pModuleInformationTable->ModuleCount = Count;

    // Fill all the modules information in the table
    pEntry = pHeadEntry->Flink;
    while (pEntry != pHeadEntry)
    {
        // Retrieve the current MODULE_ENTRY
        CurModule = &pModuleInformationTable->Modules[CurCount++];

        // Retrieve the current LDR_DATA_TABLE_ENTRY
        pLdrEntry = CONTAINING_RECORD(pEntry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);

        // Fill the MODULE_ENTRY with the LDR_DATA_TABLE_ENTRY information
        RtlCopyMemory(&CurModule->BaseName, &pLdrEntry->BaseDllName, sizeof(CurModule->BaseName));
        RtlCopyMemory(&CurModule->FullName, &pLdrEntry->FullDllName, sizeof(CurModule->FullName));
        RtlCopyMemory(&CurModule->SizeOfImage, &pLdrEntry->SizeOfImage, sizeof(CurModule->SizeOfImage));
        RtlCopyMemory(&CurModule->BaseAddress, &pLdrEntry->DllBase, sizeof(CurModule->BaseAddress));
        RtlCopyMemory(&CurModule->EntryPoint, &pLdrEntry->EntryPoint, sizeof(CurModule->EntryPoint));

        // Iterate to the next entry
        pEntry = pEntry->Flink;
    }

    return pModuleInformationTable;
}

VOID
SfwGetExecutableModuleInfo()
{
#if defined(_WIN64)
    PPEB pPeb = (PPEB)__readgsqword(0x60);

#elif defined(_WIN32)
    PPEB pPeb = (PPEB)__readfsdword(0x30);
#endif

    // Get all loaded modules infos.
    PMODULE_INFORMATION_TABLE pModuleInformationTable = SfwQueryModules(pPeb);

    PMODULE_ENTRY CurModule = NULL;
    for (ULONG Index = 0; Index < pModuleInformationTable->ModuleCount; pModuleInformationTable++)
    {
        CurModule = &pModuleInformationTable->Modules[Index++];

        // Look up the executable module name.
        if (wcscmp(CurModule->FullName.Buffer, pPeb->ProcessParameters->ImagePathName.Buffer) == 0)
        {
            gHookInfo.ExecutableModuleStart = (ULONG)CurModule->BaseAddress;
            gHookInfo.ExecutableModuleEnd = CurModule->SizeOfImage;
            break;
        }
    }

    LogMessage(L"Main module start %p", gHookInfo.ExecutableModuleStart);
    LogMessage(L"Main module end %p", gHookInfo.ExecutableModuleEnd);
}

BOOL
IsInsideHook()
/*++

Routine Description:

    This function checks if are already inside a hook handler.
    This helps avoid infinite recursions which happens in hooking
    as some APIs inside the hook handler end up calling functions
    which are detoured as well.

    There are some few issues you have to be concerned about
    if you are injecting a 64bits DLL inside a WoW64 process.
        1.  Implicit TLS (__declspec(thread)) relies heavily on the
            CRT, which is not available to us.
        2.  Explicit TLS APIs (TlsAlloc() / TlsFree(), etc.) are
            implemented entirely in kernel32.dll, whose 64-bit
            version is not loaded into WoW64 processes.

    In our case, we always injects DLL of the same architecture
    as the process. So it should be safe to use TLS. The TLS
    allocation should happen before attaching the hooks as TlsAlloc
    end up calling RtlAllocateHeap() which might be hooked as well.

Return Value:
    TRUE: if we are inside a hook handler.
    FALSE: otherwise.
--*/
{
    if (!TlsGetValue(dwTlsIndex))
    {
        TlsSetValue(dwTlsIndex, (LPVOID)TRUE);
        return FALSE;
    }
    return TRUE;
}

VOID
ReleaseHookGuard()
{
    TlsSetValue(dwTlsIndex, (LPVOID)FALSE);
}

VOID
EnterHookGuard()
{
    TlsSetValue(dwTlsIndex, (LPVOID)TRUE);
}

LONG
CheckDetourAttach(LONG err)
{
    switch (err)
    {
    case ERROR_INVALID_BLOCK: /*printf("ERROR_INVALID_BLOCK: The function referenced is too small to be detoured.");*/
        break;
    case ERROR_INVALID_HANDLE: /*printf("ERROR_INVALID_HANDLE: The ppPointer parameter is null or points to a null
                                  pointer.");*/
        break;
    case ERROR_INVALID_OPERATION: /*	printf("ERROR_INVALID_OPERATION: No pending transaction exists."); */
        break;
    case ERROR_NOT_ENOUGH_MEMORY: /*printf("ERROR_NOT_ENOUGH_MEMORY: Not enough memory exists to complete the
                                     operation.");*/
        break;
    case NO_ERROR:
        break;
    default: /*printf("CheckDetourAttach failed with unknown error code.");*/
        break;
    }
    return err;
}

static const char *
DetRealName(const char *psz)
{
    const char *pszBeg = psz;
    // Move to end of name.
    while (*psz)
    {
        psz++;
    }
    // Move back through A-Za-z0-9 names.
    while (psz > pszBeg && ((psz[-1] >= 'A' && psz[-1] <= 'Z') || (psz[-1] >= 'a' && psz[-1] <= 'z') ||
                            (psz[-1] >= '0' && psz[-1] <= '9')))
    {
        psz--;
    }
    return psz;
}

VOID
DetAttach(PVOID *ppvReal, PVOID pvMine, PCCH psz)
{
    PVOID pvReal = NULL;
    if (ppvReal == NULL)
    {
        ppvReal = &pvReal;
    }

    LONG l = DetourAttach(ppvReal, pvMine);
    if (l != NO_ERROR)
    {
        WCHAR Buffer[128];
        _snwprintf(
            Buffer,
            RTL_NUMBER_OF(Buffer),
            L"Detour Attach failed: %ws: error %d",
            MultiByteToWide((CHAR *)DetRealName(psz)),
            l);
        EtwEventWriteString(ProviderHandle, 0, 0, Buffer);
        // Decode((PBYTE)*ppvReal, 3);
    }
}

VOID
DetDetach(PVOID *ppvReal, PVOID pvMine, PCCH psz)
{
    LONG l = DetourDetach(ppvReal, pvMine);
    if (l != NO_ERROR)
    {
        WCHAR Buffer[128];
        _snwprintf(
            Buffer,
            RTL_NUMBER_OF(Buffer),
            L"Detour Detach failed: %s: error %d",
            MultiByteToWide((CHAR *)DetRealName(psz)),
            l);
        EtwEventWriteString(ProviderHandle, 0, 0, Buffer);
    }
}

PVOID
GetAPIAddress(PSTR FunctionName, PWSTR ModuleName)
{
    NTSTATUS Status;

    ANSI_STRING RoutineName;
    RtlInitAnsiString(&RoutineName, FunctionName);

    UNICODE_STRING ModulePath;
    RtlInitUnicodeString(&ModulePath, ModuleName);

    HANDLE ModuleHandle = NULL;
    Status = LdrGetDllHandle(NULL, 0, &ModulePath, &ModuleHandle);
    if (Status != STATUS_SUCCESS)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"LdrGetDllHandle failed");
        return NULL;
    }

    PVOID Address;
    Status = LdrGetProcedureAddress(ModuleHandle, &RoutineName, 0, &Address);
    if (Status != STATUS_SUCCESS)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"LdrGetProcedureAddress failed");
        return NULL;
    }

    return Address;
}

BOOL
ProcessAttach()
{
    //
    // Register ETW provider.
    //

    EtwEventRegister(&ProviderGuid, NULL, NULL, &ProviderHandle);

    //
    // Allocate a TLS index.
    //

    if ((dwTlsIndex = TlsAlloc()) == TLS_OUT_OF_INDEXES)
        LogMessage(L"TlsAlloc failed");

    AllocateSpaceSymbol();

    //
    // Save real API addresses.
    //

    TrueLdrLoadDll = LdrLoadDll;
    TrueLdrGetProcedureAddressEx = LdrGetProcedureAddressEx;
    TrueLdrGetDllHandleEx = LdrGetDllHandleEx;
    TrueNtOpenFile = NtOpenFile;
    TrueNtCreateFile = NtCreateFile;
    TrueNtWriteFile = NtWriteFile;
    TrueNtReadFile = NtReadFile;
    TrueNtDeleteFile = NtDeleteFile;
    TrueNtSetInformationFile = NtSetInformationFile;
    TrueNtQueryDirectoryFile = NtQueryDirectoryFile;
    TrueNtQueryInformationFile = NtQueryInformationFile;
    TrueNtDelayExecution = NtDelayExecution;
    TrueNtProtectVirtualMemory = NtProtectVirtualMemory;
    TrueNtQueryVirtualMemory = NtQueryVirtualMemory;
    TrueNtReadVirtualMemory = NtReadVirtualMemory;
    TrueNtWriteVirtualMemory = NtWriteVirtualMemory;
    TrueNtFreeVirtualMemory = NtFreeVirtualMemory;
    TrueNtMapViewOfSection = NtMapViewOfSection;
    TrueNtUnmapViewOfSection = NtUnmapViewOfSection;
    TrueNtAllocateVirtualMemory = NtAllocateVirtualMemory;
    TrueNtProtectVirtualMemory = NtProtectVirtualMemory;
    TrueNtOpenKey = NtOpenKey;
    TrueNtOpenKeyEx = NtOpenKeyEx;
    TrueNtCreateKey = NtCreateKey;
    TrueNtQueryValueKey = NtQueryValueKey;
    TrueNtSetValueKey = NtSetValueKey;
    TrueNtDeleteKey = NtDeleteKey;
    TrueNtDeleteValueKey = NtDeleteValueKey;
    TrueNtCreateUserProcess = NtCreateUserProcess;
    TrueNtCreateThread = NtCreateThread;
    TrueNtCreateThreadEx = NtCreateThreadEx;
    TrueNtResumeThread = NtResumeThread;
    TrueNtSuspendThread = NtSuspendThread;
    TrueNtOpenProcess = NtOpenProcess;
    TrueNtTerminateProcess = NtTerminateProcess;
    TrueNtContinue = NtContinue;
    TrueRtlDecompressBuffer = RtlDecompressBuffer;
    TrueNtQuerySystemInformation = NtQuerySystemInformation;
    TrueNtLoadDriver = NtLoadDriver;

    //
    // Resolve the ones not exposed by ntdll.
    //

    _vsnwprintf = (__vsnwprintf_fn_t)GetAPIAddress((PSTR) "_vsnwprintf", (PWSTR)L"ntdll.dll");
    if (_vsnwprintf == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"_vsnwprintf() is NULL");
    }
    _snwprintf = (__snwprintf_fn_t)GetAPIAddress((PSTR) "_snwprintf", (PWSTR)L"ntdll.dll");
    if (_vsnwprintf == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"_snwprintf() is NULL");
    }

    _wcsstr = (pfn_wcsstr)GetAPIAddress((PSTR) "wcsstr", (PWSTR)L"ntdll.dll");
    if (_wcsstr == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"wcsstr() is NULL");
    }

    //
    // Initializes a critical section objects.
    // Uesd for capturing stack trace and IsInsideHook.
    //

    InitializeCriticalSection(&gDbgHelpLock);
    InitializeCriticalSection(&gInsideHookLock);
    InitializeCriticalSection(&gHookDllLock);

    //
    // Initialize Hook Information.
    //
    gHookInfo = {0};

    //
    // Get the executable module start / end.
    //

    SfwGetExecutableModuleInfo();

    //
    // Hook Native APIs.
    //

    HookNtAPIs();

    return TRUE;
}

BOOL
ProcessDetach()
{
    HookBegingTransation();

    //
    // Lib Load APIs.
    //

    // DETACH(LdrLoadDll);
    // DETACH(LdrGetProcedureAddressEx);
    // DETACH(LdrGetDllHandleEx);

    //
    // File APIs.
    //

    DETACH(NtOpenFile);
    DETACH(NtCreateFile);
    // DETACH(NtReadFile);
    // DETACH(NtWriteFile);
    // DETACH(NtDeleteFile);
    // DETACH(NtSetInformationFile);
    // DETACH(NtQueryDirectoryFile);
    // DETACH(NtQueryInformationFile);
    // DETACH(MoveFileWithProgressTransactedW);

    //
    // Registry APIs.
    //

    // DETACH(NtOpenKey);
    // DETACH(NtOpenKeyEx);
    // DETACH(NtCreateKey);
    // DETACH(NtQueryValueKey);
    // DETACH(NtSetValueKey);
    // DETACH(NtDeleteKey);
    // DETACH(NtDeleteValueKey);

    //
    // Process/Thread APIs.
    //

    // DETACH(NtOpenProcess);
    // DETACH(NtTerminateProcess);
    // DETACH(NtCreateUserProcess);
    // DETACH(NtCreateThread);
    // DETACH(NtCreateThreadEx);
    // DETACH(NtSuspendThread);
    // DETACH(NtResumeThread);
    // DETACH(NtContinue);

    //
    // System APIs.
    //

    // DETACH(NtQuerySystemInformation);
    // DETACH(RtlDecompressBuffer);
    // DETACH(NtDelayExecution);
    // DETACH(NtLoadDriver);

    //
    // Memory APIs.
    //

    // DETACH(NtQueryVirtualMemory);
    // DETACH(NtReadVirtualMemory);
    // DETACH(NtWriteVirtualMemory);
    // DETACH(NtFreeVirtualMemory);
    // DETACH(NtMapViewOfSection);
    //// DETACH(NtAllocateVirtualMemory);
    // DETACH(NtUnmapViewOfSection);
    // DETACH(NtProtectVirtualMemory);

    HookCommitTransaction();

    TlsFree(dwTlsIndex);
    SymCleanup(NtCurrentProcess());
    EtwEventUnregister(ProviderHandle);

    EtwEventWriteString(ProviderHandle, 0, 0, L"Detached success");

    return STATUS_SUCCESS;
}

BOOL
HookBegingTransation()
{
    LONG Status;

    //
    // Begin a new transaction for attaching detours.
    //

    Status = DetourTransactionBegin();
    if (Status != NO_ERROR)
    {
        LogMessage(L"DetourTransactionBegin() failed with %d", Status);
        return FALSE;
    }

    //
    // Enlist a thread for update in the current transaction.
    //

    Status = DetourUpdateThread(NtCurrentThread());
    if (Status != NO_ERROR)
    {
        LogMessage(L"DetourUpdateThread() failed with %d", Status);
        return FALSE;
    }

    return TRUE;
}

BOOL
HookCommitTransaction()
{
    /*
    Commit the current transaction.
    */

    PVOID *ppbFailedPointer = NULL;

    LONG error = DetourTransactionCommitEx(&ppbFailedPointer);
    if (error != NO_ERROR)
    {
        LogMessage(
            L"Attach transaction failed to commit. Error %d (%p/%p)", error, ppbFailedPointer, *ppbFailedPointer);
        return FALSE;
    }

    EtwEventWriteString(ProviderHandle, 0, 0, L"Detours Attached");
    return TRUE;
}

VOID
HookOleAPIs(BOOL Attach)
{
    LogMessage(L"Attaching to ole32");

    _StringFromCLSID = (pfnStringFromCLSID)GetAPIAddress((PSTR) "StringFromCLSID", (PWSTR)L"ole32.dll");
    if (_StringFromCLSID == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"StringFromCLSID() is NULL");
    }

    _CoTaskMemFree = (pfnCoTaskMemFree)GetAPIAddress((PSTR) "CoTaskMemFree", (PWSTR)L"ole32.dll");
    if (_CoTaskMemFree == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"CoTaskMemFree() is NULL");
    }

    TrueCoCreateInstanceEx = (pfnCoCreateInstanceEx)GetAPIAddress((PSTR) "CoCreateInstanceEx", (PWSTR)L"ole32.dll");
    if (TrueCoCreateInstanceEx == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"CoCreateInstanceEx() is NULL");
    }

    HookBegingTransation();

    if (Attach)
    {
        ATTACH(CoCreateInstanceEx);
    }
    else
    {
        DETACH(CoCreateInstanceEx);
    }

    HookCommitTransaction();

    LogMessage(L"Hooked to ole32 done");

    gHookInfo.IsOleHooked = TRUE;
}

VOID
HookNetworkAPIs(BOOL Attach)
{
    LogMessage(L"Attaching to wininet");

    TrueInternetOpenA = (pfnInternetOpenA)GetAPIAddress((PSTR) "InternetOpenA", (PWSTR)L"wininet.dll");
    if (TrueInternetOpenA == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"InternetOpenA() is NULL");
    }

    TrueInternetConnectA = (pfnInternetConnectA)GetAPIAddress((PSTR) "InternetConnectA", (PWSTR)L"wininet.dll");
    if (TrueInternetOpenA == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"InternetConnectA() is NULL");
    }

    TrueInternetConnectW = (pfnInternetConnectW)GetAPIAddress((PSTR) "InternetConnectW", (PWSTR)L"wininet.dll");
    if (TrueInternetOpenA == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"InternetConnectW() is NULL");
    }

    TrueHttpOpenRequestA = (pfnHttpOpenRequestA)GetAPIAddress((PSTR) "HttpOpenRequestA", (PWSTR)L"wininet.dll");
    if (TrueHttpOpenRequestA == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"HttpOpenRequestA() is NULL");
    }

    TrueHttpOpenRequestW = (pfnHttpOpenRequestW)GetAPIAddress((PSTR) "HttpOpenRequestW", (PWSTR)L"wininet.dll");
    if (TrueHttpOpenRequestW == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"HttpOpenRequestW() is NULL");
    }

    TrueHttpSendRequestA = (pfnHttpSendRequestA)GetAPIAddress((PSTR) "HttpSendRequestA", (PWSTR)L"wininet.dll");
    if (TrueHttpOpenRequestA == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"HttpSendRequestA() is NULL");
    }

    TrueHttpSendRequestW = (pfnHttpSendRequestW)GetAPIAddress((PSTR) "HttpSendRequestW", (PWSTR)L"wininet.dll");
    if (TrueHttpOpenRequestW == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"HttpSendRequestW() is NULL");
    }

    TrueInternetReadFile = (pfnInternetReadFile)GetAPIAddress((PSTR) "InternetReadFile", (PWSTR)L"wininet.dll");
    if (TrueHttpOpenRequestW == NULL)
    {
        EtwEventWriteString(ProviderHandle, 0, 0, L"InternetReadFile() is NULL");
    }

    HookBegingTransation();

    if (Attach)
    {
        ATTACH(InternetOpenA);
        ATTACH(InternetConnectA);
        ATTACH(InternetConnectW);
        ATTACH(HttpOpenRequestA);
        ATTACH(HttpOpenRequestW);
        ATTACH(HttpSendRequestA);
        ATTACH(HttpSendRequestW);
        ATTACH(InternetReadFile);
    }
    else
    {
        DETACH(InternetOpenA);
        DETACH(InternetConnectA);
        DETACH(InternetConnectW);
        DETACH(HttpOpenRequestA);
        DETACH(HttpOpenRequestW);
        DETACH(HttpSendRequestA);
        DETACH(HttpSendRequestW);
        DETACH(InternetReadFile);
    }

    HookCommitTransaction();

    LogMessage(L"Attaching to wininet done");

    gHookInfo.IsWinInetHooked = TRUE;
}

VOID
HookNtAPIs()
{
    LogMessage(L"HookNtAPIs Begin");

    HookBegingTransation();

    //
    // Lib Load APIs.
    //

    ATTACH(LdrLoadDll);
    ATTACH(LdrGetProcedureAddressEx);
    // ATTACH(LdrGetDllHandleEx);

    //
    // File APIs.
    //

    ATTACH(NtOpenFile);
    ATTACH(NtCreateFile);
    ATTACH(NtReadFile);
    ATTACH(NtWriteFile);
    // ATTACH(NtDeleteFile);
    ATTACH(NtSetInformationFile);
    // ATTACH(NtQueryDirectoryFile);
    // ATTACH(NtQueryInformationFile);

    //
    // Registry APIs.
    //

    ATTACH(NtOpenKey);
    ATTACH(NtOpenKeyEx);
    ATTACH(NtCreateKey);
    ATTACH(NtQueryValueKey);
    ATTACH(NtSetValueKey);
    ATTACH(NtDeleteKey);
    ATTACH(NtDeleteValueKey);

    //
    // Process/Thread APIs.
    //

    /* ATTACH(NtOpenProcess);
     ATTACH(NtTerminateProcess);
     ATTACH(NtCreateUserProcess);
     ATTACH(NtCreateThread);
     ATTACH(NtCreateThreadEx);
     ATTACH(NtSuspendThread);
     ATTACH(NtResumeThread);*/
    ATTACH(NtContinue);

    //
    // System APIs.
    //

    // ATTACH(NtQuerySystemInformation);
    // ATTACH(RtlDecompressBuffer);
    // ATTACH(NtDelayExecution);
    // ATTACH(NtLoadDriver);

    //
    // Memory APIs.
    //

    // ATTACH(NtQueryVirtualMemory);
    // ATTACH(NtReadVirtualMemory);
    // ATTACH(NtWriteVirtualMemory);
    // ATTACH(NtFreeVirtualMemory);
    // ATTACH(NtMapViewOfSection);
    // ATTACH(NtAllocateVirtualMemory);
    // ATTACH(NtUnmapViewOfSection);
    // ATTACH(NtProtectVirtualMemory);

    HookCommitTransaction();

    LogMessage(L"HookNtAPIs End");
}

VOID
HookDll(PWCHAR DllName)
{
    EnterCriticalSection(&gHookDllLock);

    if (_wcsstr(DllName, L"ole32.dll") != NULL)
    {
        if (!gHookInfo.IsOleHooked)
        {
            HookOleAPIs(TRUE);
        }
    }
    else if (_wcsstr(DllName, L"wininet.dll") != NULL)
    {
        if (!gHookInfo.IsWinInetHooked)
        {
            HookNetworkAPIs(TRUE);
        }
    }

    LeaveCriticalSection(&gHookDllLock);
}

BOOL
SfwIsCalledFromSystemMemory(DWORD FramesToCapture)
{
    //
    // Capture up to 5 stack frames from the current call stack.
    // We're going to skip the first two stack frame returned
    // because that's the SfwIsCalledFromSystemMemory() and the
    // Hook Handler function itself, which we don't care about.
    //

    RtlZeroMemory(gFrames, MAX_FRAME * sizeof(PVOID));
    USHORT frames = RtlCaptureStackBackTrace(2, FramesToCapture, gFrames, NULL);

    for (ULONG i = 0; i < frames; i++)
    {
        ULONG CalledFrom = (ULONG)gFrames[i];
        if (CalledFrom >= gHookInfo.ExecutableModuleStart &&
            CalledFrom <= gHookInfo.ExecutableModuleStart + gHookInfo.ExecutableModuleEnd)
        {
            return FALSE;
        }
    }
    return TRUE;
}

NTSTATUS
SfwSymInit()
{
    //
    // Set up the symbol options so that we can gather information from the current
    // executable's PDB files, as well as the Microsoft symbol servers.  We also want
    // to undecorate the symbol names we're returned.  If you want, you can add other
    // symbol servers or paths via a semi-colon separated list in SymInitialized.
    //

    if (!SymInitialize(NtCurrentProcess(), NULL, TRUE))
    {
        LogMessage(L"SymInitialize returned error : %d", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    DWORD SymOptions = SymGetOptions();
    SymOptions |= SYMOPT_LOAD_LINES;
    SymOptions |= SYMOPT_FAIL_CRITICAL_ERRORS;
    SymOptions = SymSetOptions(SymOptions);

    return STATUS_SUCCESS;
}