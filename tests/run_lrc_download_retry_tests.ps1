$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Vcvars = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat"
$Source = Join-Path $Root "tests/lrc_download_retry_tests.cpp"
$OutputDirectory = Join-Path $Root "build/tests"
$Output = Join-Path $OutputDirectory "lrc_download_retry_tests.exe"

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$Command = '"{0}" && cl /nologo /W4 /WX /EHsc /std:c++17 /Fe:"{1}" "{2}"' -f $Vcvars, $Output, $Source
cmd /c $Command
if ($LASTEXITCODE -ne 0) { throw "lrc_download_retry test build failed." }

& $Output
if ($LASTEXITCODE -ne 0) { throw "lrc_download_retry tests failed." }
