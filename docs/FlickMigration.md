# FLICK UI 迁移

2026-10-21：UI 绘制、字体、输入及窗口接入 FLICK 公共 API 7。运行时安装提供该 API 的 FLICK，DLL 名称为 `FUCK.dll`；插件不包含框架 DLL。缺少框架或 API 版本过旧时，日志与控制台打印明确提示，控制台打开命令不会访问空接口。

## 业务边界

衣柜扫描、别名与拼音搜索、分类及目录、服装组及引用、角色管理、多预设与自动换装、工作/后备/原版层、实际装备、SOS、安全模式、JSON 与 co-save 均沿用现有实现。没有修改数据格式，也没有迁移或重写用户数据。

`src/ui/Overlay.cpp` 通过 `ITool` 注册侧栏入口，通过 `IWindow` 注册衣柜窗口。控制台 `wardrobe` / `wa` / `sa`、默认关闭的 F11 快捷键及重新绑定继续有效。打开前先申请关闭控制台；文本编辑期间快捷键不抢占输入，弹窗或编辑中的 Esc 留给宿主处理，普通 Esc 关闭衣柜。窗口拖动、尺寸和位置记忆由 FLICK 工作区管理，旧 `SkyWardrobe/ImGui.ini` 不再读取。

`src/ui/FlickUi.*` 是 UI 适配层，所有绘制调用均通过 FLICK 的函数表：

- 标准文本框、按钮、选择项、表格、标签、拖放、弹窗、字体测量及图元都在宿主执行。
- 原有自绘卡片保留两行标题、小字号备注、完整悬浮提示、收藏、右键菜单和双击应用。交互控件最后提交，避免文字覆盖 last-item 状态。
- 表格外计算网格可见行，独立部件列保留固定行高裁剪，保持完整滚动范围。
- 快捷栏仍支持跟随/自由拖动、尺寸调整、溢出滚动和右键配置。手册保留独立窗口、主题切换和指向可见控件的箭头。
- 标准 SDK 的字符串辅助函数使用 2048 字节临时缓冲；适配层按已有文本长度增加 64 KiB 输入余量，避免加载、编辑长备注时被截断。每帧一次提交仍受该缓冲上限约束。

FLICK 在没有可见窗口时不会继续调用工具绘制。因此 `src/wardrobe/RuntimeTick.cpp` 保留一个只处理业务轮询的 Present 回调，继续在原来的渲染线程调用 `ProcessUiRequests()`，完成后台保存、角色状态/自动换装和控制台功能请求。该回调不创建 ImGui context、不绘制、不接管输入，避免关闭 UI 后业务停转，也避免将 Service 的更新移到另一线程造成竞争。

## 鼠标与输入法

鼠标移动由 FLICK 经 Skyrim `MenuCursor` / `CursorMenu` 处理。已核对的公开 API 没有每窗口鼠标速度接口，故旧 `cursor_sensitivity` 原样保留但不用于新 UI；界面设置不提供无效滑块，并显示当前限制。保留 `ui_scale`，作为衣柜相对框架默认字号的缩放。

全部搜索、别名、备注及编辑框调用框架的 `InputText` / `InputTextMultiline`。FLICK 启停游戏文本输入并接收 `CharEvent`；SkyWardrobe 不再截断输入事件流，不再安装原生 IMM、旧外部桥接或 WndProc。输入法与中文字体按 FLICK 的环境配置，不能将 UTF-8 离线测试等同于 Windows 候选窗、中文组词或特定修复模组的游戏实测。

`font_path` 在 FLICK 使用缺省字体时用于启动字体配置；未配置时使用 Windows 微软雅黑。`font_size`、`small_font_size`、`ime_backend`、`ime_disabled` 等旧配置保存时保留，新 UI 不使用。原生 IME 源码和旧测试保留作历史/可移植模块参考，不编译进插件。

## 中文与图标字体修复

初版迁移错误地假定 FLICK 默认字体包含中文字形，并且框架能够找到插件原来的 Font Awesome 路径。实际默认字体不覆盖中文，框架也不搜索 `SKSE/Plugins/SkyWardrobe/Fonts`；已安装的 FLICK 包没有附带 `fa-solid-900.ttf`。

构建现在将图标字体及许可证同时放入 FLICK 标准目录 `Data/FUCKs/FUCK/fonts/`。启动时若三个标准搜索目录都缺少图标字体，则从插件原有资源路径补齐；不替换已有字体。图标可用标记根据实际资源检查设置，缺失时使用原有文字回退。

在 SKSE 插件加载阶段、FLICK 创建字体图集之前，`FlickFonts::PrepareForGame()` 为 `Data/FUCKs/FUCK/defaultstyle.ini` 的 `[Style] sFont` 补上中文字体。仅在该项缺失、为空或为 `Default` 时设置；字体优先取旧 `SkyWardrobe.ini` 中存在的 `font_path`，否则使用 Windows 的 `Fonts/msyh.ttc`。显式选择的字体以及其他样式项、注释和换行保持不变，重复启动不改写配置。此项是 FLICK 共享字体设置，会一并改善其他使用默认字体的 FLICK 页面；用户可继续在 FLICK 中选择其他中文字体。

不向运行中的宿主图集添加字体，不替换 FLICK DLL，不分发 Windows 字体。更新后需完全退出并重新启动游戏，让框架重新建立字体图集。`SkyWardrobeFlickFontsTests` 覆盖字体目录补齐、缺省配置、已有自定义配置保护、重复启动及缺失字体错误处理。

## SDK 与构建

### 缩放与复选框交互修复

衣柜页由多层 child 组成，原先仅调用 `SetWindowFontScale` 修改外层窗口，内层实际使用的字号仍是框架字号，因此“界面缩放”在其他页生效，在衣柜页没有同步变化。现在用成对的 `ContentScaleScope` 显式推入字号和间距，覆盖整个内容区及其子窗口；退出后恢复宿主字体与样式。服装卡片、侧栏、工作搭配和相关浮窗使用同一倍率，并清除旧版本留在外层窗口的倍率，避免重复相乘。

FLICK 的原生 CheckBox 依赖 `Checkbox` / `Checkbox-Filled` 图片资源，且只给图片区域注册点击。适配层现在通过宿主 Selectable 提供整行点击，再通过宿主图元绘制方框与勾号，不依赖这些外部图片。开关值、禁用状态及原有保存/业务回调保持原有调用方式；弹窗中的勾选不会自动关闭弹窗。

`SkyWardrobeFlickUiTests` 增加了勾选/取消、稳定 ID、无点击时不修改、弹窗保留，以及四种倍率跨两层 child 和字体/样式恢复检查。游戏内需验证“界面设置”的缩放滑块对衣柜页与其他页同步生效，并验证预览、收藏、插件注册和界面选项的方框与文字均可点击。

官方 SDK 及版本锁定记录见 [third_party/flick/README.md](../src/third_party/flick/README.md)。主插件仅包含与宿主匹配的 ImGui 头文件和枚举定义，不链接 vcpkg ImGui，不跨 DLL 读取字体、IO、context 或 draw list 的内部布局。ImGui 指针仅原样交还宿主 API；拖放 payload 按 SDK 公共结构读取。

按仓库要求提权运行 `scripts/Build-Debug.ps1`。`SkyWardrobeFlickUiTests` 通过模拟宿主函数表检查长 UTF-8 文本、中文提交、多行输入、焦点发布、可见行范围和双击目标隔离；这些检查不证明游戏内渲染或输入法可用。

## 游戏内验收

记录 Skyrim runtime、SKSE、FLICK、SOS、Windows、输入法和窗口模式，执行：

1. 通过 FLICK 侧栏、控制台三个别名以及启用后的热键打开；关闭按钮与 Esc 关闭；编辑文本、输入法组词和弹窗期间 Esc 不提前关闭衣柜。读档、新游戏关闭衣柜，不残留输入捕获。
2. 在搜索、别名、备注、服装组/分类/目录名及单行、多行测试框输入中文；检查候选窗、选词、连续提交、删除、Ctrl+V、A/B 输入框切换、切出游戏以及关闭重开。中文字体和图标均需可读，字符不能重复或串到另一输入框。
3. 单击卡片加入/移除，双击只应用；检查收藏、元数据编辑、长备注和中文标题。普通网格与按部件浏览滚动到中间/末尾，切换列数、缩放和窗口高度，确认条目无遗漏、无叠行、无跳动。
4. 检查目录/组/分类的拖动排序、跨目录、悬停展开、边缘滚动与右键精确移动。快捷栏检查跟随、自由拖动、小屏幕横纵滚动、编辑/失效目标及配置保存。
5. 遍历角色、预设、自动换装、三层外观、鞋子/全部实际装备、安全模式及玩家 Global/SaveLocal 存储。关闭衣柜后等待状态变化，确认自动换装、后台保存及控制台 reload/clear/preset 仍执行。
6. 打开操作手册，切换主题、移动手册和主窗口，检查箭头及弹窗期间的隐藏规则。重启检查 FLICK 记住窗口位置和大小。

上述游戏内项目尚未执行。构建不会代替此清单，也不额外做部署或 SHA-256 检查。

## 离线验证记录

通过仓库规定的提权 `scripts/Build-Debug.ps1` 完成 Debug 编译。以下可执行检查已通过：

- `SkyWardrobeFlickUiTests`：长 UTF-8、中文/多行提交、文本焦点清理、UTF-8 卡片截断、可见行和双击目标。
- `SkyWardrobeModelTests`：衣柜模型与持久化。
- `SkyWardrobeMultiLayerTests`：多层合成、迁移及 5664 个布局案例，另有 128 个旧 ImGui 离线布局案例。
- `SkyWardrobeQuickAccessTests`：快捷栏配置。
- `SkyWardrobePlayerOutfitStorageTests`：玩家搭配存储模式。

`git diff --check` 通过。旧 ImGui 布局案例验证原有布局辅助模型，不代表 FLICK 的实际绘制结果。

## 官方来源

- [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603)
- [公共 SDK](https://github.com/Fuzzlesz/FUCK_API)
- [宿主源码](https://github.com/Fuzzlesz/FUCK)
