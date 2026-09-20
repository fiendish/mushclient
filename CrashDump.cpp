#include <windows.h>
#include <shlobj.h>
#include <dbghelp.h>

#include "CrashDump.h"

#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#endif

namespace
{

const char kDumpDirectory[] = "\\MUSHclient";
const char kDumpFile[] = "\\MUSHclient-crash.dmp";
char g_dumpPath[MAX_PATH] = { 0 };
volatile LONG g_dumpInProgress = 0;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = NULL;

typedef BOOL (WINAPI *MiniDumpWriteDumpFunction)(
    HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFunction g_writeDump = NULL;

LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS *exception)
{
  // DbgHelp cannot write two dumps at the same time.
  if (InterlockedCompareExchange(&g_dumpInProgress, 1, 0) == 0)
    {
    HANDLE file = CreateFileA(g_dumpPath, GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
      {
      MINIDUMP_EXCEPTION_INFORMATION information;
      information.ThreadId = GetCurrentThreadId();
      information.ExceptionPointers = exception;
      information.ClientPointers = FALSE;

      const BOOL written = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(),
                                       file, MiniDumpNormal, &information,
                                       NULL, NULL);
      const DWORD writeError = written ? ERROR_SUCCESS : GetLastError();
      const BOOL closed = CloseHandle(file);
      const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
      if (!written || !closed)
        {
        // Do not leave a partial dump that appears valid.
        DeleteFileA(g_dumpPath);
        SetLastError(written ? closeError : writeError);
        OutputDebugStringA("MUSHclient could not save its crash dump.\n");
        }
      }
    else
      OutputDebugStringA("MUSHclient could not create its crash dump.\n");

    InterlockedExchange(&g_dumpInProgress, 0);
    }

  // Keep the handler that was installed before this one, if any.
  if (g_previousFilter)
    return g_previousFilter(exception);
  return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

DWORD InstallCrashDumpHandler()
{
  // Resolve DbgHelp before a crash. Loading a DLL in the handler can deadlock.
  HMODULE dbgHelp = LoadLibraryA("Dbghelp.dll");
  if (!dbgHelp)
    return GetLastError();

  MiniDumpWriteDumpFunction writeDump =
      reinterpret_cast<MiniDumpWriteDumpFunction>(
          GetProcAddress(dbgHelp, "MiniDumpWriteDump"));
  if (!writeDump)
    {
    const DWORD error = GetLastError();
    FreeLibrary(dbgHelp);
    return error;
    }

  char directory[MAX_PATH];
  const HRESULT folderResult = SHGetFolderPathA(NULL,
      CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT,
      directory);
  if (FAILED(folderResult))
    {
    FreeLibrary(dbgHelp);
    return HRESULT_CODE(folderResult);
    }

  const size_t baseLength = lstrlenA(directory);
  if (baseLength + sizeof(kDumpDirectory) + sizeof(kDumpFile) - 1 > MAX_PATH)
    {
    FreeLibrary(dbgHelp);
    return ERROR_FILENAME_EXCED_RANGE;
    }

  lstrcatA(directory, kDumpDirectory);
  if (!CreateDirectoryA(directory, NULL))
    {
    const DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS)
      {
      FreeLibrary(dbgHelp);
      return error;
      }
    const DWORD attributes = GetFileAttributesA(directory);
    if (attributes == INVALID_FILE_ATTRIBUTES)
      {
      const DWORD attributesError = GetLastError();
      FreeLibrary(dbgHelp);
      return attributesError;
      }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
      {
      FreeLibrary(dbgHelp);
      return ERROR_ALREADY_EXISTS;
      }
    }

  lstrcpyA(g_dumpPath, directory);
  lstrcatA(g_dumpPath, kDumpFile);
  g_writeDump = writeDump;
  // Keep DbgHelp loaded because g_writeDump points into it.
  g_previousFilter = SetUnhandledExceptionFilter(WriteCrashDump);
  return ERROR_SUCCESS;
}
