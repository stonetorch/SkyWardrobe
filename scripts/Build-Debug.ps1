[CmdletBinding()]
param(
    [switch]$Fresh,
    [switch]$FreecamDiagnostics,
    [ValidateSet('en', 'zh-CN')]
    [string]$Language = 'zh-CN',
    [switch]$AlwaysShowLog,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'

# 固定使用项目要求的 CLion CMake，避免 PATH 或其他 IDE 的 CMake 污染构建缓存。
$clionCmake = 'C:\Program Files\JetBrains\CLion 2026.2.3\bin\cmake\win\x64\bin\cmake.exe'
$projectRoot = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path -LiteralPath $clionCmake -PathType Leaf)) {
    Write-Error "未找到 CLion CMake：$clionCmake"
    exit 1
}

# CMake 参数统一用正斜杠：既便于拼接 -D，也让来自 git（正斜杠）与 Split-Path（反斜杠）的路径可直接比较。
function ConvertTo-CMakePath {
    param([string]$Path)

    return ($Path -replace '\\', '/')
}

function Get-NormalizedPath {
    param([string]$Path)

    return (ConvertTo-CMakePath $Path).TrimEnd('/').ToLowerInvariant()
}

# 判断路径是否为真实目录（而非 junction 或符号链接）。
# reparse point 属性位是唯一在 Windows PowerShell 5.1 与 PowerShell 7 上都可靠的判据；
# Test-Path 会跟随链接，无法区分真实目录与指向别处的链接。
function Test-RealDirectory {
    param([string]$Path)

    try {
        $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    }
    catch {
        return $false
    }
    return -not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
}

# 定位依赖目录：主仓库用自己的 extern/，worktree 直接引用主仓库的 extern/。
# 这里刻意不创建任何链接——删除 worktree 时的递归删除会跟随 reparse point，把主仓库依赖一并清空。
function Resolve-DependencyRoot {
    param([string]$ProjectRoot)

    $localExtern = Join-Path $ProjectRoot 'extern'
    if (Test-RealDirectory -Path $localExtern) {
        return [PSCustomObject]@{ Root = $localExtern; MainRoot = $ProjectRoot; IsLocal = $true }
    }

    # 注意：原生命令失败不会触发 $ErrorActionPreference，必须显式判断 $LASTEXITCODE；
    # 而 git 本身不存在属于 CommandNotFoundException，会直接终止，所以先探测一次。
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw "项目内没有可用的 extern 目录，且未找到 git，无法定位主仓库依赖。请按 README.md 安装依赖。"
    }

    # --path-format=absolute 必须显式指定：主仓库下 --git-common-dir 返回相对路径 .git，
    # 直接 Split-Path 取父目录会得到空串。
    $commonDir = & git -C $ProjectRoot rev-parse --path-format=absolute --git-common-dir 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $commonDir) {
        throw "无法通过 git 定位主仓库，当前目录既不是主仓库也不是它的 worktree。请按 README.md 安装依赖。"
    }

    $mainRoot = Split-Path -Parent ([string]$commonDir)
    $mainExtern = Join-Path $mainRoot 'extern'
    if (-not (Test-RealDirectory -Path $mainExtern)) {
        throw "主仓库依赖目录不可用：$mainExtern。请按 README.md 在主仓库安装 extern 依赖。"
    }

    return [PSCustomObject]@{ Root = $mainExtern; MainRoot = $mainRoot; IsLocal = $false }
}

function Assert-DependencyComplete {
    param([string]$ExternRoot)

    # 只校验这两项：vcpkg.exe 缺失时 vcpkg 自带的 bootstrap 会补上，不必在此拦截。
    foreach ($relative in @('vcpkg/scripts/buildsystems/vcpkg.cmake', 'CommonLibSSE-NG/CMakeLists.txt')) {
        $full = Join-Path $ExternRoot $relative
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
            throw "依赖不完整，缺少 $full。请按 README.md 的说明安装 extern 依赖。"
        }
    }
}

# 比较两份 vcpkg.json 的内容。主仓库与 worktree 的检出换行可能不同（CRLF / LF），
# 直接比哈希会把内容相同的清单误判为不同，所以先归一化换行。
function Test-ManifestEqual {
    param([string]$Left, [string]$Right)

    if (-not (Test-Path -LiteralPath $Left -PathType Leaf) -or
        -not (Test-Path -LiteralPath $Right -PathType Leaf)) {
        return $false
    }

    $leftText = (Get-Content -LiteralPath $Left -Raw) -replace "`r`n", "`n"
    $rightText = (Get-Content -LiteralPath $Right -Raw) -replace "`r`n", "`n"
    return $leftText -ceq $rightText
}

# 回读 CMake 缓存确认工具链覆盖生效。预设的 toolchainFile 字段指向 ${sourceDir}/extern，
# worktree 中该目录不存在；若命令行 -D 没能覆盖它，构建会静默用错工具链路径。
function Assert-ToolchainOverride {
    param([string]$CacheFile, [string]$ExpectedToolchain)

    if (-not (Test-Path -LiteralPath $CacheFile -PathType Leaf)) {
        throw "配置结束后未找到 CMake 缓存：$CacheFile"
    }

    # 类型段不固定：命令行 -D 写入的是 UNINITIALIZED，预设字段写入的是 FILEPATH，两种都要认。
    $entry = Select-String -LiteralPath $CacheFile -Pattern '^CMAKE_TOOLCHAIN_FILE:[A-Z]+=(.*)$' |
        Select-Object -First 1
    $actual = if ($entry) { $entry.Matches[0].Groups[1].Value.Trim() } else { '' }

    if ((Get-NormalizedPath $actual) -ne (Get-NormalizedPath $ExpectedToolchain)) {
        throw "工具链覆盖未生效：CMake 缓存中为 '$actual'，期望 '$ExpectedToolchain'。请检查 CMakePresets.json 的 toolchainFile 设置。"
    }
}

function Invoke-CMakeStage {
    param(
        [string]$stageName,
        [string[]]$arguments
    )

    # cmake 与 vcpkg 会把警告写到 stderr，而 Windows PowerShell 5.1 会把原生命令的 stderr 包装成
    # ErrorRecord；在 $ErrorActionPreference='Stop' 下这会直接抛异常，连已捕获的日志都丢掉。
    # 这里临时放宽为 Continue，让退出码成为唯一的失败判据。
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # 成功时暂存 CMake 输出，避免一键检查被正常构建日志淹没；失败时再完整回显。
        $stageLog = @(& $clionCmake @arguments 2>&1)
        $stageExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }

    return [PSCustomObject]@{
        Name = $stageName
        ExitCode = $stageExitCode
        Log = $stageLog
    }
}

Push-Location -LiteralPath $projectRoot
try {
    $dependency = Resolve-DependencyRoot -ProjectRoot $projectRoot
    Assert-DependencyComplete -ExternRoot $dependency.Root
    $externRoot = ConvertTo-CMakePath $dependency.Root

    $configureArguments = @('--preset', 'vs2022')
    if ($Fresh) {
        # 仅在明确要求时清空缓存，日常检查可复用既有配置以缩短等待时间。
        $configureArguments = @('--fresh') + $configureArguments
    }
    # 每次明确设置诊断开关，避免旧缓存使运行日志意外缺失或一直开启。
    $freecamDiagnosticsValue = if ($FreecamDiagnostics) { 'ON' } else { 'OFF' }
    $configureArguments += "-DCAF_FREECAM_DIAGNOSTICS=$freecamDiagnosticsValue"
    # 每次明确设置语言，避免复用缓存时意外沿用上一种语言。
    $configureArguments += "-DCAF_UI_LANGUAGE=$Language"

    if (-not $dependency.IsLocal) {
        Write-Host "复用主仓库依赖目录：$externRoot"

        # 预设里的 toolchainFile 指向 ${sourceDir}/extern，worktree 中没有该目录；
        # 命令行 -D 优先于预设，这里用主仓库路径覆盖它。
        $configureArguments += "-DCMAKE_TOOLCHAIN_FILE=${externRoot}/vcpkg/scripts/buildsystems/vcpkg.cmake"
        $configureArguments += "-DCOMMONLIB_SOURCE_DIR=${externRoot}/CommonLibSSE-NG"

        # 预设把 DEPLOY_ROOT 硬编码为 MO2 的 mod 目录，且与 sourceDir 无关，
        # 主仓库与所有 worktree 会往同一个 DLL 覆盖写。worktree 只做编译验证，不参与部署。
        $configureArguments += '-DAUTO_DEPLOY=OFF'

        # 与主仓库共用一个 install root，省去每个 worktree 重新编译全部第三方依赖。
        # 仅当两侧 vcpkg.json 完全一致时才共享：该目录下的 manifest-info.json 记录 manifest 路径，
        # 依赖清单不同的分支会把额外的 port 装进主仓库的目录。
        $sharedInstalledDir = Join-Path $dependency.MainRoot 'build/vs2022/vcpkg_installed'
        if (Test-RealDirectory -Path $sharedInstalledDir) {
            if (Test-ManifestEqual -Left (Join-Path $projectRoot 'vcpkg.json') `
                    -Right (Join-Path $dependency.MainRoot 'vcpkg.json')) {
                $configureArguments += "-DVCPKG_INSTALLED_DIR=$(ConvertTo-CMakePath $sharedInstalledDir)"
            }
            else {
                Write-Warning '本项目 vcpkg.json 与主仓库不一致，本次不共享已编译依赖，改在本地构建目录安装。'
            }
        }
    }

    $configureResult = Invoke-CMakeStage -stageName '配置' -arguments $configureArguments
    if ($configureResult.ExitCode -ne 0) {
        Write-Host "配置阶段失败（退出码 $($configureResult.ExitCode)）："
        $configureResult.Log | Write-Output
        exit $configureResult.ExitCode
    }
    if ($AlwaysShowLog) {
        $configureResult.Log | Write-Output
    }

    if (-not $dependency.IsLocal) {
        Assert-ToolchainOverride -CacheFile (Join-Path $projectRoot 'build/vs2022/CMakeCache.txt') `
            -ExpectedToolchain "${externRoot}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    }

    if ($ConfigureOnly) {
        Write-Output 'CMake 配置完成（未编译）。'
        return
    }

    # Debug 编译用于尽早发现 C++ 语法、类型与链接错误。
    $buildResult = Invoke-CMakeStage -stageName 'Debug 编译' -arguments @('--build', '--preset', 'debug')
    if ($buildResult.ExitCode -ne 0) {
        Write-Host "Debug 编译阶段失败（退出码 $($buildResult.ExitCode)）："
        $buildResult.Log | Write-Output
        exit $buildResult.ExitCode
    }
    if ($AlwaysShowLog) {
        $buildResult.Log | Write-Output
    }

    Write-Output 'Debug 编译成功。'
}
finally {
    Pop-Location
}
