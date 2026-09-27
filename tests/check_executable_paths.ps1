# Run from an x86 Visual Studio Developer shell with MFC installed.
param(
  [string]$LuaRuntimeDirectory = (Join-Path $PSScriptRoot '..\WinDebug'),
  [string]$StartupRevision
)
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
if ($StartupRevision) {
  $startup = (& git -C $repo show ($StartupRevision + ':MUSHclient.cpp')) -join [Environment]::NewLine
  if ($LASTEXITCODE -ne 0) { throw 'Unable to read the startup revision.' }
}
$init = Get-Definition $startup 'BOOL CMUSHclientApp::InitInstance()'
$pathStart = $init.IndexOf('  DWORD executablePathLength =')
if ($pathStart -lt 0) { $pathStart = $init.IndexOf('  if (GetModuleFileName (') }
$pathEnd = $init.IndexOf('  // stupid cursor', $pathStart)
$workingStart = $init.IndexOf('// find the working directory at startup time')
$workingEnd = $init.IndexOf('  bc_init_numbers();', $workingStart)
$preferencesAt = $init.IndexOf('CString strIniFile')
if ($pathStart -lt 0 -or $pathEnd -le $pathStart -or $workingStart -le $pathEnd -or
    $workingEnd -le $workingStart -or $preferencesAt -le $workingEnd) {
  throw 'Expected startup directory initialization before preferences.'
}
$startupBlock = $init.Substring($pathStart, $pathEnd - $pathStart) +
  $init.Substring($workingStart, $workingEnd - $workingStart)
$extractDirectory = Get-Definition (Read-Source 'Utilities.cpp') 'CString ExtractDirectory ('
$infoCase = [regex]::Match((Read-Source 'scripting/methods/methods_info.cpp'),
  'case\s+60:\s+SetUpVariantString[^\r\n]+').Value
if (-not $infoCase) { throw 'Missing plugin directory info case.' }
$directory = Join-Path ([IO.Path]::GetTempPath()) ('mushclient-startup-paths-' + [guid]::NewGuid())
$appDirectory = Join-Path $directory 'installation with spaces'
$launchDirectory = Join-Path $directory 'unrelated launch directory'
foreach ($base in @($appDirectory, $launchDirectory)) {
  [void](New-Item -ItemType Directory -Path (Join-Path $base 'worlds\plugins') -Force)
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
[IO.File]::WriteAllText($cpp, $template.Replace('@FUNCTIONS@', $extractDirectory).Replace('@STARTUP@', $startupBlock).Replace('@INFO_CASE@', $infoCase))
Write-Output "Test artifacts: $directory"
& cl.exe /nologo /EHsc /MTd /D_DEBUG /D_CRT_SECURE_NO_WARNINGS /Od /std:c++14 "/I$repo" $cpp "/Fe$exe" "/Fo$appDirectory\test.obj" (Join-Path $repo 'lua5.1.lib') /link /SUBSYSTEM:CONSOLE
if ($LASTEXITCODE -ne 0) { throw 'Startup path fixture compilation failed' }
$previousPath = $env:PATH
try {
  $env:PATH = (Resolve-Path $LuaRuntimeDirectory).ProviderPath + ';' + $env:PATH
  Push-Location -LiteralPath $launchDirectory
  try {
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Startup path regression tests failed' }
    $movedDirectory = Join-Path $directory 'relocated installation'
    Copy-Item -LiteralPath $appDirectory -Destination $movedDirectory -Recurse
    & (Join-Path $movedDirectory 'test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Relocated installation tests failed' }
  } finally { Pop-Location }
} finally { $env:PATH = $previousPath }
