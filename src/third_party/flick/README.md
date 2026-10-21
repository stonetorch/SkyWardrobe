# FLICK public SDK

Vendored without changes:

- `FUCK_API.h`: https://github.com/Fuzzlesz/FUCK_API at `20e95841dbda15fbfaf7a27252f6c6c98b8b3985` (API 7).
- `LICENSE.md`: upstream API license.
- `imgui/imgui.h`, `imgui/imconfig.h`, `imgui/LICENSE.txt`: https://github.com/powerof3/imgui at `fbbe3efd107e960f864d2944fdf280b465110bad` (1.92.8), pinned by the FLICK host's `cmake/ports/imgui/portfile.cmake`.
- Host inspected: https://github.com/Fuzzlesz/FUCK at `7f85048e695e1cb2eb5d64d0d714501b4d5dee16` (1.7.1 source).

The plugin includes the host-compatible headers with `IMGUI_USE_WCHAR32`. It does not compile or link an ImGui implementation. Font pointers remain opaque; drawing, text metrics, input, and font operations go through FLICK's public interface. Offline tests of pre-existing layout helpers still use the separate vcpkg ImGui dependency.

The DLL remains named `FUCK.dll` in the public SDK. Product documentation calls the framework FLICK.
