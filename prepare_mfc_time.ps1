param (
    [Parameter(Mandatory = $true)][string]$MfcDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

# Workaround for MFC shipped with MSVC 14.44.35207 (VS 2022).
# MSVC 14.51.36231 fixes the year check, but still needs both UTC conversions below.
# On MFC upgrades, recheck epoch dates and both occurrences of the autumn repeated hour.
# Remove this script and its MUSHclient.vcxproj wiring once all three fixes are upstream.

function Write-IfChanged ($Path, $Text) {
    if (!(Test-Path -LiteralPath $Path) -or [IO.File]::ReadAllText($Path) -cne $Text) {
        [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
    }
}

$header = [IO.File]::ReadAllText((Join-Path $MfcDirectory 'include\atltime.h'))
$oldGuard = 'ATLENSURE( nYear >= 1970 );'
$newGuard = 'ATLENSURE( nYear >= 1970 || (nYear == 1969 && nMonth == 12 && nDay == 31) );'
if ($header.Contains($oldGuard)) {
    # Backport the UTC-boundary fix: 1970-01-01 UTC can be 1969-12-31 locally.
    $header = $header.Replace($oldGuard, $newGuard)
} elseif (!$header.Contains($newGuard)) {
    throw 'Unrecognized MFC CTime year check; review the timestamp backport.'
}

# FILETIME is already UTC; converting through local time loses the DST occurrence.
$constructor = '(?s)inline CTime::CTime\(\s*_In_ const FILETIME& fileTime,\s*_In_ int nDST\)\s*\{'
$constructors = [regex]::Matches($header, $constructor)
if ($constructors.Count -ne 1) {
    throw 'Unrecognized MFC FILETIME constructor; review the timestamp backport.'
}
$utcRead = @"

    SYSTEMTIME utc;
    if (nDST == -1 && FileTimeToSystemTime(&fileTime, &utc) && utc.wYear >= 1970)
    {
        struct tm value = {};
        value.tm_year = utc.wYear - 1900;
        value.tm_mon = utc.wMonth - 1;
        value.tm_mday = utc.wDay;
        value.tm_hour = utc.wHour;
        value.tm_min = utc.wMinute;
        value.tm_sec = utc.wSecond;
        m_time = _mkgmtime64(&value);
        if (m_time == -1)
            AtlThrow(E_INVALIDARG);
        return;
    }
    // Preserve explicit nDST overrides and legacy handling of pre-epoch dates.
"@
$header = $header.Insert($constructors[0].Index + $constructors[0].Length, $utcRead)

# Rebuild the whole object so the linker does not pull in its unpatched CTime copy.
$source = [IO.File]::ReadAllText((Join-Path $MfcDirectory 'src\mfc\filest.cpp'))
$includes = @"
#include <afxwin.h>
BOOL AFXAPI AfxFullPath(LPTSTR lpszPathOut, LPCTSTR lpszFileIn);
UINT AFXAPI AfxGetFileTitle(LPCTSTR lpszPathName, LPTSTR lpszTitle, UINT nMax);
UINT AFXAPI AfxGetFileName(LPCTSTR lpszPathName, LPTSTR lpszTitle, UINT nMax);
"@
if (!$source.Contains('#include "stdafx.h"')) {
    throw 'Unrecognized MFC filest.cpp includes; review the timestamp backport.'
}
$source = $source.Replace('#include "stdafx.h"', $includes)

# Preserve UTC when saving; LocalFileTimeToFileTime applies today's DST offset.
$conversion = '(?s)void AFX_CDECL AfxTimeToFileTime\(const CTime& time, LPFILETIME pFileTime\)\s*\{.*?\r?\n\}'
if ([regex]::Matches($source, $conversion).Count -ne 1) {
    throw 'Unrecognized MFC time-to-file conversion; review the timestamp backport.'
}
$utcConversion = @"
void AFX_CDECL AfxTimeToFileTime(const CTime& time, LPFILETIME pFileTime)
{
    struct tm utc;
    if (pFileTime == NULL || time.GetGmtTm(&utc) == NULL)
        AfxThrowInvalidArgException();
    SYSTEMTIME value = {};
    value.wYear = (WORD)(utc.tm_year + 1900);
    value.wMonth = (WORD)(utc.tm_mon + 1);
    value.wDay = (WORD)utc.tm_mday;
    value.wHour = (WORD)utc.tm_hour;
    value.wMinute = (WORD)utc.tm_min;
    value.wSecond = (WORD)utc.tm_sec;
    if (!SystemTimeToFileTime(&value, pFileTime))
        CFileException::ThrowOsError((LONG)GetLastError());
}
"@
$source = [regex]::Replace($source, $conversion, $utcConversion)

[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
Write-IfChanged (Join-Path $OutputDirectory 'atltime.h') $header
Write-IfChanged (Join-Path $OutputDirectory 'filest.cpp') $source
