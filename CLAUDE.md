# Repository Guidelines

## Readability Guideline

关键的逻辑需要简要提供中文注释

## Project Structure & Module Organization

`src/Plugin.cpp` contains the SKSE entry point, logging, and the `kDataLoaded` listener. `src/PCH.h` supplies CommonLibSSE-NG's precompiled headers to the generated plugin declaration. Add plugin implementation files under `src/`; keep declarations near their implementation or in a dedicated `include/` directory when shared across modules. `CMakeLists.txt` defines the DLL, `CMakePresets.json` defines the Visual Studio build, and `vcpkg.json` lists third-party packages. `README.md` covers dependency setup. `extern/`, `build/`, and CLion-generated directories are local, ignored directories. `tests/` holds the CTest targets, which run outside Skyrim. `assets/fonts/` holds the only packaged game asset: the Font Awesome icon font merged into the ImGui panel font, which `CMakeLists.txt` copies beside the DLL and into `DEPLOY_ROOT` on every build.

## Build, Test, and Development Commands

Follow `README.md` to install vcpkg and clone the CommonLibSSE-NG `ng` branch into `extern/`.

Only compile the code to verify, no need to check for deployment and sha256.

### Required build toolchain

For C++ compilation verification, run the repository one-click tool from the project root. It is the required entry point for agents and fixes the CLion CMake executable plus the `vs2022` and `debug` presets; do not invoke CMake directly or supply a separate toolchain file, triplet, generator, compiler, or `VCPKG_ROOT` override:

```powershell
.\scripts\Build-Debug.ps1
```

When toolchain identity must be assured, or the existing cache may have been created by another CMake version, refresh the cache first:

```powershell
.\scripts\Build-Debug.ps1 -Fresh
```

你需要提权运行，否则会出现权限问题

`AUTO_DEPLOY` is enabled by `vs2022`, so a successful Debug build writes the DLL to `D:/Tes5/JueLunV4/Mod Organizer 2/mods/CustomAnimationFramework/SKSE/Plugins/` as well as to `build/vs2022/Data/SKSE/Plugins/Debug/`. Treat that copy as a build side effect when deciding whether compilation is in scope. No automated test command exists yet; verify changes with a successful Debug build and, for runtime behavior, launch the matching Skyrim and SKSE installation with Address Library.

If fail is caused by toolchain setup, do not try to fix it yourself, report to the user.

## Coding Style & Naming Conventions

Use C++23, four-space indentation, and braces on separate lines, matching `src/Plugin.cpp`. Name classes and functions descriptively; use `PascalCase` for types and `camelCase` for local variables. Keep engine hooks and runtime-specific logic isolated so they can be reviewed independently. No formatter or linter is configured; keep changes consistent with nearby code.

## Testing Guidelines

There is no test framework or coverage target. Add focused tests when introducing logic that can run outside Skyrim; place them in `tests/`, name files `<Feature>Tests.cpp`, and register a CMake test target. Document any in-game checks, including Skyrim runtime version and expected plugin behavior, in the pull request.

## Commits & Pull Requests

Use Conventional Commits for commit messages. Use 中文. type can be feat, refactor, fix, chore, etc.

When useful, add a blank line after the subject and a body with concise bullet points describing the main changes or rationale. Keep the subject short and imperative.

```
feat(animation): 添加基本解析能力

- 详细说明
- 详细说明
```

Pull requests should describe the change, list build and runtime checks performed, and link related issues when available. Include logs or screenshots only when they help explain game behavior or a visual change.
