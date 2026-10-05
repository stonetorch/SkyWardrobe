# UI 输入法兼容性：外部项目与 Windows 接入调研

调研日期：2026-10-05。本文记录源码与作者资料，未修改插件实现，未运行 Skyrim。网页的 main/master 分支会继续变化；发布版行为必须另行验证。

后续实现调研已成功读取缺失源码：SIM `CustomRE.h` 使用标准 `kCharEvent`；IMU main 2.1.0 将 Unicode 码点排入自有队列，分帧填充 `BSInputEventQueue`，并在键盘事件生成前过滤组词按键。前期未取到源码的描述是当时的证据范围，当前核对与接入见 [InputMethodIntegration.md](../InputMethodIntegration.md)。独立 IMM 后备未实现。

## 已确认的外部方案

### SimpleIME

实际项目为 [Skyrim IME Support - SimpleIME](https://www.nexusmods.com/skyrimspecialedition/mods/140136)，源码为 [cyfewlp/SimpleIME](https://github.com/cyfewlp/SimpleIME)。README 将当前实现描述为 TSF 输入法集成；不能依据 Nexus 简介中较旧的 “ImGui + Win32 + IMM” 判断当前架构。

**作者明确提供 ImGui 接入说明**：输入框获得/失去焦点时调用 `ControlMap::AllowTextInput(true/false)`；接收 `BSUIScaleformData` 中的 `GFxCharEvent`，把字符送给本模组的 `ImGuiIO::AddInputCharacter`。示例以 `WantTextInput` 的边沿变化同步焦点，并在 `IMenu::ProcessMessage` 或事件分派 hook 中接收字符。默认候选窗位置来自 GFx caret，自定义 ImGui 无法自动跟随；作者要求改用 Follow Cursor 或 None。作者还明确将同时抢键盘焦点或同一窗口 WM_CHAR 管线的实现列为冲突边界。[作者兼容指南](https://www.nexusmods.com/skyrimspecialedition/articles/11843)

**源码证据**：`ImeMenu::ProcessScaleformEvent` 把内部 `kImeCharEvent` 改为普通 `kCharEvent` 后向下传递；普通字符在输入处理器活动时被消费。文件里的 `SendCharEventToImGuiIfWant` 调用 SimpleIME 自己的 `ImGui::GetIO()`，没有自动获得其他 DLL 的 ImGui context。[ImeMenu.cpp](https://raw.githubusercontent.com/cyfewlp/SimpleIME/main/src/menu/ImeMenu.cpp)

**结论与边界**：公开集成契约是游戏焦点和 Scaleform 字符管线，本次查到的作者文档、源码及 include 目录没有呈现供外部独立 context 注册文本接收器/光标位置的稳定 API；这不等于已经穷尽证明不存在任何未公开接口。对 SkyWardrobe，单独保留 `RE::CharEvent` 输入监听不足，需要接入顶层菜单消息分发。没有 SWF 的自定义 IMenu 是否能正确参与其完整输入管线，仍需最小游戏实验。[include 目录](https://github.com/cyfewlp/SimpleIME/tree/main/include)

### Skyrim Input Method - Use System's IME

实际项目为 [Nexus 92965](https://www.nexusmods.com/skyrimspecialedition/mods/92965)，作者链接到 [Kerite/SkyrimInputMethod](https://github.com/Kerite/SkyrimInputMethod)，并注明继承 [kassent/ChineseInput](https://github.com/kassent/ChineseInput)。本次没有确认名为 SkyrimIME 或 SSE IME 的另一个独立项目，不应以这些泛称安排适配。

**源码证据**：`ProcessAllowTextInput` 在 `textEntryCount == 0` 且开启时启用 IME，在 count 为 1 且关闭时禁用；其 hook 随后调用原函数。候选面板也由 `textEntryCount` 门控。它包装 DirectInput 键盘并改为非独占模式，同时拦截原生 Scaleform 字符消息。[InputManager.cpp](https://raw.githubusercontent.com/Kerite/SkyrimInputMethod/main/src/Hooks/InputManager.cpp)、[InputPanel.cpp](https://raw.githubusercontent.com/Kerite/SkyrimInputMethod/main/src/InputPanel.cpp)

**提交路径确证**：`Utils::SendUnicodeMessage` 在 `textEntryCount` 为零时直接返回，随后建立 `GFxCharEvent` 和 `BSUIScaleformData`，向 `InterfaceStrings::topMenu` 发送 `kScaleformEvent`。IMM 结果串和粘贴均走此路径。它不是在 `BSInputDeviceManager` 中生成 Skyrim `CharEvent`。[Utils.cpp](https://raw.githubusercontent.com/Kerite/SkyrimInputMethod/main/src/Utils.cpp)

**推断**：与 SimpleIME 可以共用焦点生命周期及 Scaleform 字符桥，但须处理它的自定义事件标签；不能假定 AddMessage 处观察到的事件已经是标准 kCharEvent。不能让自己的原生 Windows 模式与该输入法插件同时持有 IME 管线。

### Input Method Unlocked

实际项目为 [Nexus 191339](https://www.nexusmods.com/skyrimspecialedition/mods/191339)，源码为 [VanCZ1/Input-Method-Unlocked](https://github.com/VanCZ1/Input-Method-Unlocked)。作者 v2.0 说明明确要求 mod menu 调用 `AllowTextInput(true/false)`，并说明改过 char event 来兼容模组菜单；作者限制为窗口/无边框模式、不能鼠标选择候选、仅 IMM 兼容输入法、候选窗一次最多 5 个字符。作者报告测试版本为 1.6.1170。

**证据范围**：本次网页工具能读取作者说明与源码仓库首页，但源码目录/关键文件获取失败，因此未独立核实其具体 char event 结构与分派实现。以上是作者声明，不能直接当作 SkyWardrobe 已兼容的运行结论。

## 独立 Windows 输入法备选

Dear ImGui 已有两块相关能力：核心默认 `Platform_SetImeDataFn` 通过 `PlatformHandleRaw` 找 HWND，使用 `ImmGetContext`、`ImmSetCompositionWindow`、`ImmSetCandidateWindow` 定位输入法界面；启停上下文的 `ImmAssociateContextEx` 调用是注释状态。它负责定位，未替宿主游戏恢复被禁用的输入法上下文。[imgui.cpp](https://raw.githubusercontent.com/ocornut/imgui/master/imgui.cpp)

官方 Win32 backend 将 Unicode 窗口的 `WM_CHAR` 转成 `AddInputCharacterUTF16`，并处理非 Unicode 窗口的代码页转换；2025-12 新增 `WM_IME_COMPOSITION/WM_IME_CHAR` 的 MBCS 兼容逻辑。这要求宿主实际把 Windows 消息交给 backend，单纯 `ImGui_ImplWin32_Init` 无法建立文字输入管线。[imgui_impl_win32.cpp](https://raw.githubusercontent.com/ocornut/imgui/master/backends/imgui_impl_win32.cpp)

**低成本实验推断**：窗口/无边框模式下，限定当前输入框焦点期间关联 HWND 的 IMM 上下文，转发字符与必要 IME 消息，复用 ImGui caret 定位，可能足够支持常见 IMM 兼容输入法。应保留消息链、原上下文、焦点/关闭清理，并确保 Windows 和 Skyrim 字符来源只提交一次。游戏的 DirectInput 捕获、窗口 Unicode 属性、消息线程和其他插件 hooks 仍可能要求额外工作，不能把普通 Win32 示例的可用性直接外推到 Skyrim。

Microsoft 将 `ImmAssociateContextEx` 定义为修改窗口与输入上下文关联；`IACE_DEFAULT` 恢复默认上下文，`IACE_CHILDREN` 只影响创建该窗口的线程的现有子窗口。这类操作应尊重窗口线程及原有上下文生命周期。[Microsoft IMM API](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immassociatecontextex)

**完整原生 TSF 不属于便捷备选**：独立 TSF 编辑控件需实现 text store (`ITextStoreACP`)、线程/文档管理、context、selection 与文本/布局通知，并遵守文档锁和 sink 通知协议；与 ImGui 输入框内部 UTF-8 文本状态同步还需设计接口。只有证明 IMM 无法满足目标用户输入法后才值得考虑。[Microsoft Text Stores](https://learn.microsoft.com/en-us/windows/win32/tsf/text-stores)、[ITextStoreACP](https://learn.microsoft.com/en-us/windows/win32/api/textstor/nn-textstor-itextstoreacp)、[Document Locks](https://learn.microsoft.com/en-us/windows/win32/tsf/document-locks)

## 建议的验收顺序

1. 先补齐 `WantTextInput` 对游戏文本焦点的成对申请/释放，避免每帧累加引用计数；关闭 UI、失去焦点、异常退出路径均释放本模组自己的申请。**本地 CommonLibSSE-NG 的 `ControlMap::AllowTextInput` 实现只修改计数；不能以调用这个 wrapper 代替通知外部插件。** SimpleIME 作者示例明确用 `REL::Relocation` 的 `67252 / 68552` 调原生引擎函数，应经可被输入法插件 hook 的原生入口；实现时另核对本地版本及运行时适配。
2. 一个测试输入框记录字符来源：Skyrim `CharEvent`、Scaleform 字符、Windows 字符分别可辨；最终均交给同一线程安全队列，在渲染线程提交到本 context。
3. SimpleIME/SIM 各自单独安装测试候选、确认、取消、长句、删除、Esc、关闭 UI、切到控制台再回来；尤其确认顶层菜单消息桥和吞键行为。
4. IMU 先核对当前发布源码再实验，检查 5 字限制是否影响产品体验。
5. 原生 IMM 仅在没有外部输入法 provider 时试验窗口/无边框模式，成功后再决定是否作为可选设置。

以上均为设计建议，尚无游戏运行验证。
