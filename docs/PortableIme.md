# 可切换的 IME 集成

本次新增原生 Windows IMM 支持，面向所有通过 IMM 或 TSF 的 IMM 兼容层工作的输入法，不按语言、输入法品牌或输入文字选择实现。纯 TSF text store 尚未实现；不能把 IMM 支持等同于所有 TSF 输入服务都兼容。

## 快速测试

打开衣柜的界面设置，在 `IME 实现` 中切换，随后点击 `IME 测试 A` 开始输入。测试框不保存文字，也不写入日志。`IME 测试 B` 用于检查切换输入框后是否残留提交。

| 编号/配置值 | 实现 | 行为 |
| --- | --- | --- |
| 0 | 自动 | 检测到单个已知模组则使用外部桥；否则先选共享 IMM，被禁用时选独立 IMM |
| 1 | IMM shared | 复用游戏窗口现有 HIMC，不创建或替换关联；缺少上下文时显示不可用，普通游戏字符和粘贴仍可用 |
| 2 | IMM private | 仅在文本编辑期间创建并关联自有 HIMC，退出编辑时恢复旧关联并销毁自有资源 |
| 3 | 外部模组桥接 | 保留 SimpleIME、Skyrim Input Method、Input Method Unlocked 现有接入路径 |
| 4 | 禁用 / 剪贴板 | 不主动接入 IME，保留游戏字符与 ImGui 粘贴 |

下方三个复选框控制各条路径是否允许使用。取消勾选会持久禁用；显式选择了被禁用的实现时，实际实现为禁用，不偷偷替换测试对象。界面同时显示请求和实际采用的实现。

配置保存在现有 `Data/SKSE/Plugins/SkyWardrobe.ini` 中：

```ini
ime_backend=0
ime_disabled=0
```

`ime_disabled` 是位掩码：2 禁用 shared，4 禁用 private，8 禁用外部桥，可相加。例如 `ime_disabled=6` 禁用两条原生路径。修改只影响 SkyWardrobe，不卸载或禁用其他 DLL。

反馈时给出实现编号、`active/context/composing/commits/failures`、Skyrim 版本、Windows 版本、输入法名称与版本、输入法修复模组组合、窗口/无边框/独占全屏模式，以及具体失败操作。这样可以单独禁用失败路径。

建议每种组合用以下顺序快速验收：

1. 普通拉丁文字、空格、标点、Ctrl+V；确认无重复或缺字。
2. 用自己的语言输入，至少测试日语转换、韩语组字或中文组词中的适用项目；检查系统组词窗和候选窗。
3. 连续相同文字、超过五字符的长句；退格、方向键、数字选词、Enter/Space 确认。
4. Esc 先取消组词，再退出文本编辑；没有文本编辑时关闭衣柜。
5. 组词期间切换 A/B、关闭衣柜、Alt+Tab、切换语言；旧提交不得进入新输入框。
6. 在已有修复模组的安装中使用自动/外部桥，再检查其原生游戏菜单输入仍正常。

## 兼容性边界

检测到 SimpleIME.dll、SkyrimInputMethod.dll 或 InputMethodUnlocked.dll 时，原生模式不接管窗口，即使手动选择原生模式也会显示实际禁用。多个已知模组同时加载时不申请文本输入。禁用外部桥不会绕过已加载 DLL 去争夺 HIMC。

未知模组仍可能先吞掉窗口消息、改变 HWND 关联或修改游戏输入流。原生模块发现 HIMC 被换掉，会停止该编辑会话，不恢复覆盖对方的新关联；重新聚焦才开始下一会话。这降低所有权冲突，不能证明任意 hook 组合都可共存。

原生模式使用系统组词窗/候选窗，位置取自 ImGui 当前插入点。独占全屏下系统窗口是否可见、现代 TSF 输入法的 IMM 兼容行为，需要现场验证。没有自绘候选列表或完整 `ITextStoreACP` 实现。

文字通过 Unicode 管线处理，输入逻辑不依赖中文字体。最终字形仍受配置字体、ImGui 的 ImWchar 编译宽度及文本布局能力限制；双向文字、复杂脚本排版和补充平面字形显示不能仅凭字符成功入队认定正确。项目及其 ImGui 库必须以一致配置编译，不能只在本插件单独定义 `IMGUI_USE_WCHAR32`。

## 迁移到另一 ImGui 项目

复制 `src/ui/ime/BackendPolicy.h`、`Win32Ime.h`、`Win32Ime.cpp`，链接 Windows `imm32`。模块仅依赖 C++ 标准库与 Windows SDK，独立于 SKSE、CommonLib、ImGui 与衣柜业务。

宿主适配顺序：

1. 在已有窗口过程接入成功后调用 `Attach(HWND)`。窗口过程最前部调用 `Handle`；返回值有值时使用该 LRESULT，无值时继续原窗口过程。保持窗口原有 A/W 属性。
2. 编辑焦点、活动控件或后端改变时，更新唯一会话 generation，并用 `Publish(Request)` 发送后端、编辑状态、客户区插入点和行高。该接口跨线程复制请求；IMM 操作全部在 HWND 窗口线程进行。
3. 渲染线程用 `Drain()` 取 UTF-16 提交，仅接受仍匹配的 generation，解码后调用自己的 ImGui context。一个后端只接入一个提交来源；不要根据字符值或时间去重。
4. 原生激活时停用游戏字符来源。共享上下文不可用时可保留游戏字符后备。组词与结束边界调用 `FilterKey` 过滤游戏轮询按键，释放被阻止的按键前继续抑制；`Snapshot().suppressKeys` 时清除 ImGui 已按下状态。每个绘制帧结束调用 `EndFrame`。
5. 关闭界面、失焦、切换会话时发布 `editing=false`，让窗口线程取消组词并归还自有状态。保持对象存活至窗口销毁；若宿主支持 DLL 热卸载，必须先在窗口线程发布/处理停用并移除窗口转发，然后销毁对象。本插件按进程生命周期驻留。

`InputMethod.cpp` 是 SkyWardrobe 专用适配层，负责外部模组、会话、游戏菜单和 ImGui 字符注入；`Overlay.cpp` 负责 HWND/插入点/帧边界转发。其他项目不需要复制 Skyrim 菜单桥。

## 验证范围与依据

离线测试覆盖后端优先级、禁用、外部冲突隔离、Unicode 解码与编辑会话失效。编译结果记录在本次任务交付中；实际 IME、候选窗、Skyrim 按键顺序仍须执行上面的游戏测试。

实现使用 Microsoft 文档规定的 [GCS_RESULTSTR](https://learn.microsoft.com/en-us/windows/win32/intl/ime-composition-string-values) 读取 Unicode 提交串；保留 [WM_IME_SETCONTEXT](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-setcontext) 的系统窗口显示行为。提交串处理后从默认消息处理参数中移除结果标志，避免再次生成同一提交的 WM_IME_CHAR；普通字符单独接收 WM_CHAR/WM_UNICHAR。
