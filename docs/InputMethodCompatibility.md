# UI 输入法兼容性方案

调研日期：2026-10-05。本文保存前期方案；外部模组接入已按后续请求实现，当前行为与补充核对见 [InputMethodIntegration.md](InputMethodIntegration.md)。独立 IMM 后备没有实现，Skyrim 内输入法验收尚未进行。外部项目的接入证据见 [外部输入法调研](reports/ime-external-research.md)。

## 结论

优先兼容现有输入法模组，复用它们处理 Windows 输入法、组词与候选窗的能力。SkyWardrobe 负责宣布文本焦点、接收已提交字符、协调编辑按键和清理状态。

独立 Windows 输入法可以作为后备，但应先做窗口/无边框模式的 IMM 可行性验证。完整 TSF 文本服务、游戏内候选窗与独占全屏支持不属于一个简单的 ImGui 开关。

## 当前实现

- `src/ui/Overlay.cpp` 初始化独立的 ImGui context，调用 Win32/DX11 后端初始化。
- `OverlayInputSink` 被放在 Skyrim 输入 sink 的前面；衣柜可见时接收鼠标、键盘和 `RE::CharEvent`，并返回 `kStop`。
- 字符通过加锁的 `inputRecords` 队列，在 Present 绘制线程调用 `io.AddInputCharacter`。
- `WindowProc` 处理打开快捷键、重绑定和 Esc，然后转发原窗口过程；没有调用 Win32 后端消息处理器，也没有专门处理 IME 状态。
- 未调用 `ControlMap::AllowTextInput`；衣柜并未注册为 Skyrim `IMenu`，没有 `ProcessMessage` 接收 Scaleform 文字消息。
- 鼠标由游戏相对移动事件累计，所以有意不调用 `ImGui_ImplWin32_NewFrame`，避免绝对鼠标坐标覆盖虚拟指针。
- `Panel.cpp` 的搜索、别名、备注等使用标准 ImGui 文本控件；数据按 UTF-8 保存。当前 README 提供 Ctrl+V 输入中文的路径。

因此，现有 `CharEvent` 接口只解决“收到字符后写入输入框”，并没有解决输入法何时启用、组词按键由谁处理、候选窗位置、不同输入法模组如何投递最终文字。

本地 CommonLib 的 `ControlMap::AllowTextInput` 修改 `textEntryCount`，具有计数语义；`BSInputEventQueue::MAX_CHAR_EVENTS` 是 5。不要每帧增加文本输入计数，也不要把这一固定容量误当成我们自己的动态字符队列容量。

## 共同的接入层

建议在 `src/ui/` 增加一个独立输入法适配模块，Overlay 只保留初始化、帧边界与窗口消息转发入口。

### 文本焦点与生命周期

1. 使用 `ImGuiPlatformImeData::WantTextInput` 或帧结束后的 `io.WantTextInput` 识别文本编辑状态，不能只根据衣柜可见就启用。
2. 只在进入/退出文本编辑时配对调用游戏原生 `AllowTextInput(true/false)`。渲染线程发布期望状态，由游戏线程同步计数，并记录自己是否持有该状态。**本地 CommonLib 的同名方法只直接修改计数，没有进入游戏函数；对于 hook 游戏函数的 SimpleIME，直接调用这个包装方法可能不会触发输入法启停。** 作者的集成示例明确通过 `REL::Relocation` 的 `67252 / 68552` 进入游戏原生函数，正式实现应封装该入口并核对支持的游戏版本。[SimpleIME 作者指南](https://www.nexusmods.com/skyrimspecialedition/articles/11843)
3. 关闭衣柜、失焦、菜单被抢占、切换适配后端时释放自己的状态，取消或终止本轮组词，清理按键与待提交输入。
4. 使用编辑会话标识处理延迟事件；上一个输入框关闭之后到达的字符不能写进下一个输入框。

### 字符与按键

- 鼠标继续使用现有 Skyrim 事件路径。
- 游戏字符、Scaleform 消息、Windows 提交串均先转换/复制到自有队列，只有绘制线程调用 ImGui。不能把事件指针保存到下一帧，也不能在其他 DLL 的 ImGui context 上写入。
- 每个后端明确普通文字和组词提交各自采用哪条通道；同一提交不能同时读取 `GCS_RESULTSTR`、`WM_CHAR` 和游戏字符事件。
- 不按字符值或短时间窗口去重，否则连续输入相同汉字会丢字。
- 组词期间，Esc 先取消组词；确认候选的 Enter/Space、翻页/选择用的方向键和数字不能同时触发输入框提交、衣柜关闭或导航。按键抑制必须覆盖当前提交的结束边界，并成对处理按下/释放。
- 外部模组可能在进入我们 sink 前消费组词按键，也可能消费 Win32 消息；应实测 hook/sink 顺序，不能仅凭 `kStop` 推断兼容或冲突。

## 外部输入法路线

### Input Method Unlocked 2.0：改动最少的验证对象

其作者说明 mod menu 需要配对调用 `AllowTextInput`，2.0 修改字符事件以支持模组菜单。这提示它可能最接近现有 `AsCharEvent` 接收链路，因此可以先验证焦点声明能否让文字直接到达衣柜。**本次未成功读取其具体分派源码，事件结构与投递点仍须核实。**[作者 v2.0 说明](https://www.nexusmods.com/skyrimspecialedition/mods/191339?tab=description)

该版本公开说明窗口/无边框、IMM 兼容性及一次最多五个候选字符的限制。衣柜扩大自己的队列不能恢复外部模组已经丢弃的字符；长提交必须作为验收项，不能宣布通用输入完全兼容。

### SimpleIME / SkyrimInputMethod：需要检查菜单消息通道

这些项目的实现/作者集成说明涉及向 Skyrim 菜单投递 Scaleform 字符消息。`RE::CharEvent` 与 `BSUIScaleformData` 中的字符事件是两个入口，现有 sink 不会自动收到后者。[SimpleIME 作者指南](https://www.nexusmods.com/skyrimspecialedition/articles/11843)、[SkyrimInputMethod 提交源码](https://github.com/Kerite/SkyrimInputMethod/blob/main/src/Utils.cpp)

对独立 overlay，建议验证一个轻量的 Skyrim `IMenu` 消息桥：文本编辑时使其成为实际字符接收目标，自定义 `ProcessMessage` 复制已提交字符到自有队列；视觉仍由现有 ImGui 绘制。

后续核对已取得 SIM `CustomRE.h`：它的自定义 C++ 字符结构使用标准 `kCharEvent` 标签。SimpleIME 则有内部 IME 字符标签及转换步骤；实际实现位于 SimpleIME 菜单下方，读取转换后的标准事件。

这个桥不是重写成 Flash UI。是否可以不加载 SWF、应选择哪些菜单标志、如何避免改变暂停/鼠标/保存语义、上层菜单出现后如何退出，必须通过最小游戏实验确定。CommonLib 的默认 `IMenu::ProcessMessage` 在无 movie 时返回 `kPassOn`，因此需要自己处理字符消息，不能依赖默认处理。

SimpleIME 作者的 ImGui 集成说明建议候选窗使用跟随鼠标或固定位置，因为其 GFx 光标定位无法直接获得独立 ImGui 的插入点。精确跟随衣柜输入光标需要额外协议或上游支持，不能因两边都使用 ImGui 就共享内部指针。作者还明确将同时抢占输入焦点或相同窗口 `WM_CHAR` 管线的行为视为不兼容，因此外部模式不应启用原生 Windows 文本 hook。[SimpleIME 作者指南](https://www.nexusmods.com/skyrimspecialedition/articles/11843)

不依赖未公开的 DLL 内部类布局、地址或 ImGui ABI；本次没有发现可直接传递本模组 ImGui context/光标的稳定通用协议。

## 独立 Windows 输入法后备

ImGui 1.92.7 已有默认 Win32 IME 位置回调：从 viewport 的 HWND 获得输入上下文并设置 composition/candidate window 位置。`ImGui_ImplWin32_Init` 设置该 HWND。默认回调中的自动关联输入上下文调用被注释掉，所以这不能保证 Skyrim 窗口能够启用 IME。[ImGui core](https://github.com/ocornut/imgui/blob/v1.92.7/imgui.cpp)

小范围原型建议如下：

1. 记录游戏窗口是否 Unicode、窗口线程、当前输入上下文、实际收到的字符/IME 消息以及普通英文字符的来源。
2. 在 HWND 所属线程启用/关联输入上下文并维护自己的占用范围；关闭后恢复原上下文。保存现有上下文，不能无条件创建并永久占用全局 IME 状态。[ImmAssociateContext](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immassociatecontext)
3. 通过 ImGui IME 回调发布插入点，在窗口线程设置候选窗位置；正确处理缩放、客户区/屏幕坐标及焦点变化。
4. 明确采用一种组词结果接收方式。可以直接取 `WM_IME_COMPOSITION` 的 Unicode 结果串，也可以在确认窗口与消息转换路径后接收字符消息；两者不可无条件叠加。[WM_IME_COMPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-composition)
5. 只增加必要的文字/IME 消息处理，避免将整套 Win32 鼠标与键盘事件再次喂给 ImGui。官方后端对 Unicode 与非 Unicode 窗口采取不同的字符处理，不能简单把所有 `WM_CHAR` 都当成 UTF-16。[Win32 backend](https://github.com/ocornut/imgui/blob/v1.92.7/backends/imgui_impl_win32.cpp)
6. 保留原 WndProc 链；IME 消息的消费与转发按选定后端逐项决定，不能截断其他模组的处理，也不能在已经处理后再次触发默认文字提交。

初期只以微软拼音、窗口/无边框模式为验收目标。系统候选窗在独占全屏上可能无法显示；自绘候选窗与完整 TSF 会显著扩大工作范围。微软的游戏 IME 文档说明了此类全屏窗口限制，但其旧 SDK 示例不能作为现代每一种 IME 的兼容保证。[游戏 IME 文档](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/using-an-input-method-editor-in-a-game)

## 后端选择

建议提供“外部模组 / Windows 后备 / 关闭”选择，并在诊断中显示选中的通道。自动模式只针对已核实的模组身份/版本选择；遇到未知输入法 hook 时不强行关联或替换全局输入上下文。外部模组与 Windows 后备应由一个后端选择器互斥启用。

是否检测到 DLL 只能说明模组存在，不能证明它已启用、版本接口兼容或文字能到达衣柜。自动回退应基于已明确的能力条件；不能用“本次没有输入字符”作为失败依据，也不能在一轮组词中途切换后端。

## 建议实施顺序和验收

1. 实现通用焦点声明、状态清理、字符队列与诊断，先验证 Input Method Unlocked 2.0 的现有字符路径。
2. 完成 SimpleIME 的菜单消息桥与候选窗配置验证，再用相同桥检查 SkyrimInputMethod。记录实际测试的 DLL/版本，不将一个版本的行为泛化到所有版本。
3. 无外部输入法模组时做 Windows IMM 最小原型，结果决定是否正式提供独立后备。

每个外部模组单独安装测试，至少覆盖：搜索框、别名、备注；英文/中文切换；空格/数字/方向键选候选；Enter 确认；Esc 取消；连续相同汉字；超过五字的整句提交；Ctrl+V；输入框切换；关闭衣柜/Alt+Tab/重新打开；控制台与原生菜单输入恢复；候选窗在缩放后的合理位置；虚拟鼠标命中不变。

涉及字符转换、后端选择和状态机的纯逻辑可加离线测试；窗口 hook、IME 模组投递、候选窗显示与菜单栈需要 Skyrim 内实测。正式改实现后的编译入口仍为提权运行 `scripts/Build-Debug.ps1`；本次只有文档变更，不需要构建。
