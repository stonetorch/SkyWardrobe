# 外部输入法接入

实现日期：2026-10-05。实现、Debug 构建和离线逻辑测试已完成；IMU 接入修正后，用户确认实际问题已修复。SimpleIME / Skyrim Input Method 仍需分别游戏验收，候选窗口不可见不属于此次确认范围。没有实现独立 Windows IMM / TSF 后备。完整调研、失败原因和输入流程见 [ImGui 中文输入完整报告](reports/imgui-chinese-input-retrospective.md)。

## 支持入口

| 模组 / DLL | 文字来源 | 接入方式 |
| --- | --- | --- |
| SimpleIME / `SimpleIME.dll` | 菜单 Scaleform 字符 | 原生文本焦点申请 + 位于 SimpleIME 下方的接收菜单 |
| Skyrim Input Method / `SkyrimInputMethod.dll` | 菜单 Scaleform 字符 | 原生文本焦点申请 + 同一接收菜单 |
| Input Method Unlocked / `InputMethodUnlocked.dll` | 游戏 `RE::CharEvent` | 原生文本焦点申请 + IMU 过滤后的游戏字符和编辑按键，不打开菜单桥 |

在 DataLoaded 自动检测 DLL，仅选择一个 provider。多个已知输入法 DLL 同时加载时显示冲突，不主动申请文本输入；没有已知 provider 时保留游戏字符和剪贴板输入。DLL 存在并不证明其版本已启用或运行接口兼容；每个安装版本仍需实测。

## 生命周期与线程

`InputMethod.cpp` 集中管理文本输入；`Overlay.cpp` 保留鼠标虚拟指针、绘制和窗口消息链。

ImGui 帧尾发布当前 `PlatformImeData.WantTextInput` 和活动输入框 ID，避免使用上一帧 `io.WantTextInput` 导致申请反复切换。活动 ID 为零时视为结束编辑，即使 IME 标志尚未清除，也不重新申请。同一输入框连续绘制不会重复申请。只有进入文本编辑才经 `REL 67252 / 68552` 调用原生 `AllowTextInput`；本地 CommonLib 同名方法直接修改计数，无法通知 hook 原生入口的输入法。

原生申请、释放和菜单显示请求统一进入 SKSE UITask。申请状态由本模块单独保存，关闭时只释放自己持有的一份；切换活动输入框先释放旧申请再申请新焦点。字符事件只复制值到加锁队列，绘制线程统一调用本模组 ImGui。队列带编辑会话编号，输入框切换或关闭会丢弃已排队的旧事件，重置未完成的 UTF-16 代理对。

关闭衣柜、应用失焦、游戏新建 / 读档或接收菜单被强制关闭都会释放申请。前台窗口只检查进程身份，允许 SimpleIME 将键盘焦点移到同进程的 IME 窗口；这与 Alt+Tab 切到其他进程区别处理。其他有菜单输入上下文或暂停标志的菜单打开时，衣柜退出接管。

隐藏衣柜时在绘制线程清除 ImGui 活动控件，避免下次打开直接恢复旧输入框的编辑焦点并重新启用输入法。

## 菜单桥与按键

无 SWF 的 `SkyWardrobe Text Input` 菜单位于 depth 10，低于 SimpleIME 的 depth 13。它使用菜单输入上下文，但不暂停游戏、不使用游戏鼠标光标，视觉仍由现有 ImGui 绘制。

衣柜的前置 sink 继续接收相对鼠标移动。SimpleIME / Skyrim Input Method 在接收菜单打开后，键盘事件通过 `MenuControls::ProcessEvent` 显式进入正常菜单输入链，让外部输入法先过滤组词和候选操作；前置 sink 仍返回 `kStop`，避免同一批事件再次分发及触发 PlayerControls。菜单桥把输入法放行的 GFx 编辑按键送给本模组 ImGui；游戏鼠标坐标不写入 ImGui。

Input Method Unlocked 在 `AddButtonEvent` 前过滤组词键，在键盘 Poll 时插入上屏字符。因此它不注册、不打开 `SkyWardrobe Text Input` 菜单，也不将按键再转交 MenuControls。衣柜直接接收已经过滤后的游戏按键；组词中退格由 IMU / Windows 输入法处理，组词结束后放行的退格交给 ImGui。仅界面可见不会申请输入法，进入有效文本编辑才申请。

原始 WndProc 在外部输入法模式下不提前消费 Esc，也不在文本输入期间处理打开快捷键。组词 Esc/Enter/Space/数字/方向键依赖输入法先过滤；放行的 Esc 退出当前文本编辑，无文本编辑时关闭衣柜。IMU 的关闭判断使用游戏按键首次按下，长按 Esc 不会在退出编辑后的下一帧继续关闭衣柜。原窗口 A/W 属性和 `CallWindowProcA/W` 链被保留，避免改变 Input Method Unlocked 的 ANSI 文字转换语义。

SimpleIME 的自定义 `0x80000001` 字符由它的菜单先转换为标准 `kCharEvent`；衣柜只解析转换后的事件，不接管它的内存释放。SkyrimInputMethod 的自定义 C++ 字符结构其实使用标准 `kCharEvent` 标签，`wcharCode` 位于偏移 4。两者均立即复制这个字段，不保留事件指针。

## 字符与剪贴板

SimpleIME / SkyrimInputMethod 只接受 Scaleform 字符，丢弃原始游戏字符；Input Method Unlocked 只接受游戏 Unicode 码点。这是来源选择，不按字符值或时间去重，因此连续相同汉字会保留。

菜单型输入法自行投递 Unicode 粘贴时，桥不向 ImGui 再投递 Ctrl+V 快捷键。若 SimpleIME 关闭 Unicode 粘贴并放行原始 Ctrl+V 字符，或 SkyrimInputMethod 放行 `0x16`，使用 ImGui 现有剪贴板快捷键回退。Input Method Unlocked 与无 provider 模式直接保留 ImGui 的 Ctrl+V。

Scaleform UTF-16 高/低代理对可跨帧合成码点，编辑会话切换时重置；IMU 游戏事件按 Unicode 码点处理。最终能显示的补充平面字形仍取决于当前 ImGui 编译配置和字体，解码测试不等同于 Emoji 或所有 Unicode 字形显示验收。

## 候选窗与版本边界

候选窗、组词缓冲、Windows 输入上下文和 Windows 字符消息完全由外部 provider 处理；ImGui 默认 IME 回调替换为只发布焦点，不修改 provider 的候选窗或上下文。

SimpleIME 需在自身设置选择 Follow Cursor / None；衣柜没有实现跨 DLL 的插入点定位 API。Input Method Unlocked 的候选窗位置由它自身决定。其发布版 2.0 文档写一次最多 5 字符；本次取得的 main 是 **2.1.0**（commit `ac8647021079067c90406f8c2d3c694457c900cb`），用自有队列分帧填充游戏 5 字符队列。不能将发布版 2.0 限制直接泛化为 main 的截断行为。现场日志确认安装版本为 2.1.0，修正后收到用户成功反馈；该反馈没有逐项证明长句、所有输入法或候选定位均已验收。

## 验证

2026-10-05 用户报告 IMU 中文不上屏、Esc 无法退出、退格和输入法退出异常。读取现场日志确认 Skyrim 1.5.97 / IMU 2.1.0 已加载。修复移除 IMU 的菜单桥依赖、恢复其过滤后的直接编辑按键路径、拒绝零 ID 文本焦点并在隐藏时清除 ImGui 活动控件。修正与构建后，用户明确确认“问题得到了修复”。候选窗口不可见按用户要求暂不处理；其原因和最终显示情况没有单独确认。

诊断日志立即刷新：`External IME provider` 表示检测结果；`IME text lease acquired/released` 记录本模组申请、释放与游戏文本计数；`IMU character arrived` 每个编辑会话最多记录一次非 ASCII 字符到达时的焦点条件，不记录字符内容。若不上屏但出现该记录，可区分接收时是否处于有效编辑及申请状态；没有记录则尚无 IMU 字符到达衣柜的证据。释放后 count 非零也可能是其他菜单持有申请，衣柜不会清空别人的计数。

- 提权 `scripts/Build-Debug.ps1`：Debug 编译通过。
- `build/vs2022/Debug/SkyWardrobeInputMethodTests.exe`：通过。覆盖 provider 选择与冲突、文字来源防重、剪贴板策略、活动输入框会话失效、重复汉字、跨帧代理对和异常编码。
- 不检查部署或 SHA-256；游戏内验收清单在 [RuntimeChecks.md](RuntimeChecks.md)。

## 一手来源

- [SimpleIME 作者 ImGui 接入指南](https://www.nexusmods.com/skyrimspecialedition/articles/11843)
- [SimpleIME 菜单分派](https://github.com/cyfewlp/SimpleIME/blob/main/src/menu/ImeMenu.cpp)、[字符结构](https://github.com/cyfewlp/SimpleIME/blob/main/include/RE/GFxCharEvent.h)
- [SkyrimInputMethod 字符结构](https://github.com/Kerite/SkyrimInputMethod/blob/main/src/RE/CustomRE.h)、[提交](https://github.com/Kerite/SkyrimInputMethod/blob/main/src/Utils.cpp)、[过滤调用点](https://github.com/Kerite/SkyrimInputMethod/blob/main/src/Hooks/InputManager.h)
- [Input Method Unlocked 2.0 作者说明](https://www.nexusmods.com/skyrimspecialedition/mods/191339?tab=description)
- [本次读取的 IMU 2.1.0 字符队列](https://github.com/VanCZ1/Input-Method-Unlocked/blob/ac8647021079067c90406f8c2d3c694457c900cb/src/InputMethod.cpp)、[按键过滤](https://github.com/VanCZ1/Input-Method-Unlocked/blob/ac8647021079067c90406f8c2d3c694457c900cb/src/Input.cpp)、[hook](https://github.com/VanCZ1/Input-Method-Unlocked/blob/ac8647021079067c90406f8c2d3c694457c900cb/src/Hooks.cpp)
