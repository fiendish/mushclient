#pragma once

#include <windows.h>

// Install the process crash handler. Return ERROR_SUCCESS or a Win32 error code.
DWORD InstallCrashDumpHandler();
