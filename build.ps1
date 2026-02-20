$BuildMode = "DEBUG"
$ExtraCppFlags = ""
$ExtraNdkFlags = ""

function Write-LogInfo([string]$msg) {
    Write-Host "[$([char]27)[32mINFO$([char]27)[0m] $msg"
}

function Write-LogError([string]$msg) {
    Write-Host "[$([char]27)[31mERROR$([char]27)[0m] $msg" -ForegroundColor Red
}

function Show-Help {
    @"
Usage: .\build.ps1 [options]
Build ArcDarkModule using Android NDK

Options:
    --rel                       Build in RELEASE mode
    --rebuild                   Rebuild the project (clean build)
    --cpp-build-flags <flags>   Additional compiler flags
    --ndk-build-flags <flags>   Additional ndk-build flags
    --help                      Show this help message
"@
    exit 0
}

$i = 0
while ($i -lt $args.Length) {
    switch ($args[$i]) {
        "--rel" {
            $BuildMode = "RELEASE"
            $i++
        }
        "--rebuild" {
            if (Test-Path "outputs") { Remove-Item -Recurse -Force "outputs" }
            $ExtraNdkFlags += " -B "
            $i++
        }
        "--cpp-build-flags" {
            $ExtraCppFlags += " $($args[$i+1]) "
            $i += 2
        }
        "--ndk-build-flags" {
            $ExtraNdkFlags += " $($args[$i+1]) "
            $i += 2
        }
        "--help" {
            Show-Help
        }
        Default {
            Write-LogError "Unknown option: $($args[$i])"
            Show-Help
            exit 1
        }
    }
}

if (-not (Test-Path "outputs")) {
    New-Item -ItemType Directory -Path "outputs" | Out-Null
}

Write-LogInfo "Building ArcDarkModule in $([char]27)[36m$BuildMode$([char]27)[0m mode"

$CppBuildFlags = ""
$NdkBuildCmd = Join-Path $env:ANDROID_NDK_HOME "ndk-build.cmd"

$ArgList = @(
    "NDK_PROJECT_PATH=$PSScriptRoot",
    "APP_BUILD_SCRIPT=$PSScriptRoot\Android.mk",
    "NDK_APPLICATION_MK=$PSScriptRoot\Application.mk",
    "NDK_OUT=$PSScriptRoot\outputs\obj",
    "NDK_LIBS_OUT=$PSScriptRoot\outputs\libs",
    "-j$env:NUMBER_OF_PROCESSORS",
    "NDK_DEBUG=$($BuildMode -eq "DEBUG" ? 1 : 0)"
)

if ($ExtraNdkFlags -ne "") { $ArgList += $ExtraNdkFlags.Trim() }
if ($CppBuildFlags -ne "") { $ArgList += "APP_CPPFLAGS=$CppBuildFlags" }

Write-LogInfo "NDK build command: $NdkBuildCmd $ArgList"

& $NdkBuildCmd @ArgList > "outputs/build.log" 2>&1

if ($LASTEXITCODE -ne 0) {
    Write-LogError "Build failed with exit code $LASTEXITCODE"
    Write-LogError "Check outputs/build.log for details"
    
    Write-Host "`n[33m=== Last 20 lines of build log ===[0m" -ForegroundColor Yellow
    if (Test-Path "outputs/build.log") {
        Get-Content "outputs/build.log" -Tail 20
    }
    Write-Host "[33m===============================[0m" -ForegroundColor Yellow
    exit 1
}

if ($BuildMode -eq "RELEASE") {
    Write-LogInfo "Stripping libraries for release"
    $Stripper = Join-Path $env:ANDROID_NDK_HOME "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
    if (Test-Path $Stripper) {
        & $Stripper -s outputs/libs/arm64-v8a/libarc_dark.so
    }
}

$TmpDir = "outputs/module_tmp"
if (Test-Path $TmpDir) { Remove-Item -Recurse -Force $TmpDir }
New-Item -ItemType Directory -Path "$TmpDir/zygisk" | Out-Null

Copy-Item "outputs/libs/arm64-v8a/libarc_dark.so" -Destination "$TmpDir/zygisk/arm64-v8a.so"
Copy-Item "module.prop" -Destination "$TmpDir/"

$ZipPath = "outputs/ArcDarkModule.zip"
if (Test-Path $ZipPath) { Remove-Item $ZipPath }
Compress-Archive -Path "$TmpDir\*" -DestinationPath $ZipPath

Remove-Item -Recurse -Force $TmpDir
Write-LogInfo "OK!"