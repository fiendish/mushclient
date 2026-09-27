# Run from an x86 Visual Studio Developer shell with MFC installed.
param([string]$LuaRuntimeDirectory = (Join-Path $PSScriptRoot '..\WinDebug'))
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath
function Read-Source([string]$Path) {
  [IO.File]::ReadAllText((Join-Path $repo $Path), [Text.Encoding]::GetEncoding(1252))
}
function Get-Definition([string]$Source, [string]$Signature) {
  $start = $Source.IndexOf($Signature, [StringComparison]::Ordinal)
  if ($start -lt 0) { throw "Missing definition: $Signature" }
  $start = $Source.LastIndexOf([char]10, $start) + 1
  $tokens = [regex]::Matches($Source.Substring($start),
    '//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*''|[{}]')
  $depth = 0
  foreach ($token in $tokens) {
    if ($token.Value -eq '{') { $depth++ }
    elseif ($token.Value -eq '}') {
      $depth--
      if ($depth -eq 0) { return $Source.Substring($start, $token.Index + 1) }
    }
  }
  throw "Unclosed definition: $Signature"
}
$startup = Read-Source 'MUSHclient.cpp'
$init = Get-Definition $startup 'BOOL CMUSHclientApp::InitInstance()'
$initializeAt = $init.IndexOf('InitializeApplicationDirectory (m_strMUSHclientFileName)')
$preferencesAt = $init.IndexOf('CString strIniFile')
if ($initializeAt -lt 0 -or $preferencesAt -lt 0 -or $initializeAt -gt $preferencesAt) {
  throw 'Directory must be initialized before preferences are loaded'
}
$utilities = Read-Source 'Utilities.cpp'
$definitions = @(
  Get-Definition $utilities 'CString ExtractDirectory ('
  Get-Definition $startup 'static bool InitializeApplicationDirectory ('
  Get-Definition $utilities 'const char * Make_Absolute_Path ('
  Get-Definition (Read-Source 'scripting/methods/methods_utilities.cpp') 'long CMUSHclientDoc::ChangeDir('
  Get-Definition $utilities 'void ChangeToFileBrowsingDirectory ()'
  Get-Definition $utilities 'void ChangeToStartupDirectory ()'
  Get-Definition $utilities 'void MakeTableItem (lua_State *L, const char * name, const CString & str)'
  Get-Definition $utilities 'void MakeTableItem (lua_State *L, const char * name, const string & str)'
  Get-Definition $utilities 'void MakeTableItem (lua_State *L, const char * name, const double n)'
  Get-Definition $utilities 'void MakeTableItem (lua_State *L, const char * name, const COleDateTime d)'
  Get-Definition (Read-Source 'scripting/lua_utils.cpp') 'static int info ('
)
$info = Read-Source 'scripting/methods/methods_info.cpp'
$cases = foreach ($id in @(57, 58, 60, 85)) {
  $match = [regex]::Match($info, ('case\s+' + $id + ':\s+SetUpVariantString[^\r\n]+'))
  if (-not $match.Success) { throw "Missing directory info case $id" }
  $match.Value
}
$directory = Join-Path ([IO.Path]::GetTempPath()) ('mushclient-paths-' + [guid]::NewGuid())
$appDirectory = Join-Path $directory 'installation with spaces'
$launchDirectory = Join-Path $directory 'unrelated launch directory'
foreach ($base in @($appDirectory, $launchDirectory)) {
  [void](New-Item -ItemType Directory -Path (Join-Path $base 'worlds\plugins\state') -Force)
  [void](New-Item -ItemType Directory -Path (Join-Path $base 'logs') -Force)
  $marker = if ($base -eq $appDirectory) { 'installation' } else { 'wrong directory' }
  foreach ($file in @('MUSHclient.ini', 'mushclient_prefs.sqlite', 'worlds\relative.mcl')) {
    [IO.File]::WriteAllText((Join-Path $base $file), $marker)
  }
  [IO.File]::WriteAllText((Join-Path $base 'worlds\plugins\aardwolf_colors.lua'),
    ("function strip_colours(s) return '" + $marker + "' end"))
}
$template = Read-Source 'tests/executable_paths.cpp.in'
$cpp = Join-Path $appDirectory 'test.cpp'
$exe = Join-Path $appDirectory 'test.exe'
[IO.File]::WriteAllText($cpp, $template.Replace('@FUNCTIONS@', ($definitions -join [Environment]::NewLine)).Replace('@INFO_CASES@', ($cases -join [Environment]::NewLine)))
Write-Output "Test artifacts: $directory"
& cl.exe /nologo /EHsc /MTd /D_DEBUG /D_CRT_SECURE_NO_WARNINGS /Od /std:c++14 "/I$repo" $cpp "/Fe$exe" "/Fo$appDirectory\test.obj" (Join-Path $repo 'lua5.1.lib') /link /SUBSYSTEM:CONSOLE
if ($LASTEXITCODE -ne 0) { throw 'Path regression fixture compilation failed' }
$previousPath = $env:PATH
try {
  $env:PATH = (Resolve-Path $LuaRuntimeDirectory).ProviderPath + ';' + $env:PATH
  Push-Location -LiteralPath $launchDirectory
  try {
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Executable path regression tests failed' }
    $movedDirectory = Join-Path $directory 'relocated installation'
    Copy-Item -LiteralPath $appDirectory -Destination $movedDirectory -Recurse
    & (Join-Path $movedDirectory 'test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Relocated installation tests failed' }
  } finally { Pop-Location }
} finally { $env:PATH = $previousPath }