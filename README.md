# SkyWardrobe

Skyrim SE / AE 的独立 SKSE 衣柜管理器。扫描已加载的 ARMO 资源，以 UTF-8 JSON 管理别名、备注、收藏、分类、语义部件与预设；Skyrim Outfit System SE Revived 负责最终外观覆盖。默认只改变外观，可选通过原版路径同时实际穿戴鞋子或全部服装。

## 使用

运行依赖：匹配游戏版本的 SKSE、Address Library，以及 [Skyrim Outfit System SE Revived](https://github.com/lispcoc/SkyrimOutfitSystemSE)。SOS 未安装或原生 DLL 未加载时，SkyWardrobe 全部功能禁用，面板只显示依赖缺失提示，不会尝试 VM 调用。安装兼容版本后需要重启游戏。SOS 需在自己的 MCM 中启用。

控制台命令无需 ESP、Papyrus 编译产物或 ConsoleUtil：

```text
wardrobe
wa
sa
wardrobe reload
wardrobe preset "city-black"
wardrobe clear
wardrobe close
```

`preset` 支持预设 ID 或完整名称。`wardrobe` 打开后自动关闭控制台。关闭按钮和 Esc 恢复游戏视角输入；外部输入法编辑期间，Esc 先由输入法取消组词或退出文本编辑，再按 Esc 关闭衣柜。界面设置可以启用 / 禁用快捷键并重新绑定；默认禁用，预选 F11。窗口可拖动与调整大小，布局保存在 `SkyWardrobe/ImGui.ini`；UI 缩放与鼠标灵敏度分别设置。

## 衣柜功能

- **全部服装、★ Favorites、未分类、工作搭配**：左侧切换视图。原始目录只扫描，不批量写入 JSON。
- **注册插件**：在“分类与数据 → 注册插件分类”勾选 ESP / ESM / ESL，分类动态包含其全部来源 ARMO，显示记录数量。覆盖原有 ARMO 的补丁不会把服装移出来源插件分类。
- **多对多分类**：创建分类，右键服装勾选多个分类；分类可重命名、增加拼音搜索词或删除。
- **服装卡片**：标题使用别名或原名，备注使用灰色小字号；悬浮显示完整文字、稳定 ID 与 biped mask。右键编辑收藏、别名、中文备注、搜索词、分类与部件。
- **搜索**：名称、别名、备注、插件、分类和搜索词共同参与匹配。中文通过 cpp-pinyin 自动生成全拼及首字母索引，`裙子` 可用 `qun zi`、`qunzi`、`qz`、`qzi`、`qu zi` 搜索，分类及预设同样支持；已有 `pinyin` / `search` 用于人工纠音和扩展搜索词。中文通过 Ctrl+V 粘贴，批量数据可直接编辑 JSON。
- **语义部件**：全身、上装、下装、内衣、胸衣、内裤、鞋、袜、手套、头饰、头发、项链、披风、配饰与自定义 1–8。自定义部件可以改显示名称。未标记服装按常见 biped 槽位给出初始建议，用户标记始终优先。
- **部件分列**：只显示当前结果中有服装的部件；在“Interface / 界面设置”中设置按部件分列、自动调整列布局及列数（1–12）。自动调整先分配空余列，仅在设置列数全部占用后才按可用高度合并较短的部件分类，计入分类标题、卡片、间距、分类上方留白及内边距，合并列完整展示所有项目且无需滚动；腾出的列优先分给仍需滚动的部件。窗口高度、字体缩放或筛选结果变化后重新计算。容纳不下的独立列保留各自的纵向滚动，总列数超出设置时水平滚动。
- **工作搭配**：单击卡片加入；重复点击保持选中。占用相同 biped 槽位的旧部件会被替换。右侧可移除部件；双击或“应用外观”应用整体搭配。可选实时预览，默认关闭。

- **实际装备**：工作搭配面板提供“关闭”（默认）、“仅鞋子装备”、“全部装备”。启用后，SOS 外观回读成功才通过原版 `ActorEquipManager::EquipObject` 穿戴对应服装；鞋子按脚部槽位 37 判断，语义部件标签不影响实际装备。模式全局保存，启用模式在下次应用时生效；切到关闭立即安排临时物品回收，SOS 外观继续保留。实际装备会改变护甲、附魔及装备效果。

- **物品与回收**：玩家已有服装使用原物品实例，缺少的服装自动添加一件。启用模式下切换搭配，卸下衣柜此前实际穿上的、不再需要的服装，回收不再需要的临时物品；清除外观或关闭实际装备时，只卸下并回收临时物品，玩家原有物品保持当前穿戴，不恢复被替换前的装备。临时物品按原版唯一标识定位，记账随 SKSE co-save 保存；已移出玩家物品栏的物品不会在其他容器里被删除，也不会按同款 FormID 替代回收。
- **预设**：工作搭配与已保存预设分离，修改时显示 `*`。支持保存为新预设、更新名称 / 搜索词、一键覆盖当前预设、恢复、删除及撤销上次覆盖。删除预设保留工作搭配。
- **清除外观覆盖**：设置 SOS 当前 Outfit 为空并刷新玩家，工作搭配和已保存预设保留。SOS 自己的地点 / 战斗自动换装可能在后续事件中再次选择服装；需要持续使用衣柜外观时请在 SOS 中关闭相应自动切换。

当前外观应用目标是玩家。内部只维护一个 SOS Outfit：`__WARDROBE_RUNTIME__`。不将每个 JSON 预设复制为 SOS Outfit。快速请求会串行执行，等待期间的新选择只保留最新一次；加载游戏时取消旧回调链。后台检查 API 启用状态、完整解析所有 FormKey，依次覆盖 / 设置 / 刷新并回读当前 Outfit。缺失服装时拒绝应用整个预设，并显示缺失 ID。

## 外部输入法

自动接入 **SimpleIME**、**Skyrim Input Method - Use System's IME**、**Input Method Unlocked**，无需为衣柜安装额外桥接 DLL。三种接入均已编译实现；IMU 在 Skyrim 1.5.97 / IMU 2.1.0 环境修正后得到用户成功反馈，另外两种仍需分别游戏验收。只启用其中一个输入法模组；同时加载多个时衣柜不申请外部输入法焦点，设置页显示冲突提示。

SimpleIME 的候选窗位置请设为 **Follow Cursor** 或 **None**，其 GFx 光标定位无法直接跟随独立 ImGui 插入点。Input Method Unlocked 的运行限制依其安装版本而定，窗口 / 无边框模式及 IMM 兼容输入法是作者公布的支持范围。没有外部输入法模组时可继续使用 Ctrl+V；本插件不提供独立 IMM / TSF 后备。

界面设置显示识别到的输入法。搜索、别名、备注等标准文本框共用接入；焦点退出、关闭衣柜、切出游戏或读档会清理本模组申请和待处理文字。实现与来源见 [输入法接入说明](docs/InputMethodIntegration.md)，游戏验收见 [RuntimeChecks](docs/RuntimeChecks.md)。

信息搜集、初版失败分析、最终关键代码及完整输入流程见 [ImGui 中文输入兼容完整报告](docs/reports/imgui-chinese-input-retrospective.md)。

## JSON 与字体配置

```text
Data/SKSE/Plugins/
├─ SkyWardrobe.dll
├─ SkyWardrobe.ini
└─ SkyWardrobe/
   ├─ Wardrobe.json
   ├─ Wardrobe.json.bak
   ├─ ImGui.ini
   ├─ AliasPacks/*.json
   ├─ Fonts/fa-solid-900.ttf
   └─ Examples/
```

首次保存时创建数据库。配置示例在 [assets/examples](assets/examples)，格式说明见 [docs/DataFormat.md](docs/DataFormat.md)。示例不会覆盖正式配置文件，也不会自动导入不存在的示例插件。

用户数据优先于别名包，别名包优先于插件原名。只改收藏不会覆盖别名包提供的名称。空别名表示回退原名；“恢复别名包 / 原始数据”删除该件服装的用户覆盖。别名包按文件名顺序加载，后面的包覆盖前面的同一件服装。

注册插件时自动创建 `AliasPacks/00_Generated.<插件文件名>.json` 模板，填充全部扫描服装的原名与当前识别部件；已有模板会保留。选择已注册插件可“重新生成别名包模板”，覆盖该生成文件。编辑后使用“重新加载 JSON 与别名包”载入。

`Wardrobe.json` 先写 `.tmp`，写入关闭成功后备份现有文件，最后使用 Windows 原子替换。错误 JSON 不会替换内存中的原数据，也不会被后续保存覆盖；界面会提示错误，可修正文件后“重新加载”。无效别名包独立跳过并显示文件名和错误。丢失模组的元数据与预设保留。

字体默认使用 Windows 微软雅黑；备注加载独立的小字号字体。可配置其他支持中文的字体路径，修改后重启游戏：

```ini
hotkey_enabled=0
toggle_key=122
ui_scale=1.5
cursor_sensitivity=1.0
font_path=C:/Windows/Fonts/msyh.ttc
font_size=20
small_font_size=16
```

## 构建与验证

保留此目录已有的 CommonLibSSE-NG、vcpkg、CLion CMake 与 Visual Studio 2022 工具链。若从空目录准备，CommonLibSSE-NG `ng` 分支放入 `extern/CommonLibSSE-NG`，vcpkg 放入 `extern/vcpkg`；第三方依赖由现有 manifest 管理。nlohmann/json 3.12.0 的官方单头文件与 MIT 许可证随源码保存。

按项目要求，在提权 PowerShell 中从项目根目录运行：

```powershell
.\scripts\Build-Debug.ps1 -Fresh
.\scripts\Build-Debug.ps1
.\scripts\Build-Debug.ps1 -Language en
```

默认编译中文。沿用编译期开关 `CAF_UI_LANGUAGE` / `CAF_UI_ZH_CN`，以保持既有 i18n 构建入口兼容。构建目标及 DLL 已改为 **SkyWardrobe**，输出：

```text
build/vs2022/Data/SKSE/Plugins/Debug/SkyWardrobe.dll
```

`vs2022` 预设启用 AUTO_DEPLOY，构建副作用目录已改为 `mods/SkyWardrobe/SKSE/Plugins`。不进行部署检查或 SHA-256 验证。此前项目残留的动画实现与测试不属于本插件构建目标。

数据模型与文件保存测试随同 Debug 构建，可独立执行：

```powershell
.\build\vs2022\Debug\SkyWardrobeModelTests.exe
.\build\vs2022\Debug\SkyWardrobePinyinSearchTests.exe .\build\vs2022\vcpkg_installed\x64-windows-static-md\share\cpp-pinyin\dict
```

测试覆盖普通 / ESL 稳定 ID、非法运行时 ID、拼音缩写与顺序、中文匹配、部件分列、别名包与用户覆盖、UTF-8 保存、备份及坏 JSON 保护。游戏内验收清单见 [docs/RuntimeChecks.md](docs/RuntimeChecks.md)。编译与数据测试通过不代表已完成游戏内外观、控制台或鼠标验收。

## 技术参考

SOS 的原生函数签名核对自 [SkyrimOutfitSystemNativeFuncs.psc](https://github.com/lispcoc/SkyrimOutfitSystemSE/blob/master/src/papyrus/skyrimoutfitsystemnativefuncs.psc)，语义核对自 [OutfitSystem.cpp](https://github.com/lispcoc/SkyrimOutfitSystemSE/blob/master/src/cpp/OutfitSystem.cpp)。独立前端调用方式参考 [SosGui](https://github.com/cyfewlp/SosGui)，未复制其实现或 SOS 的外观引擎源码。

控制台注册复用引擎未使用槽位；若没有未使用槽位，则使用遗留 `SetDebugText` 调试命令槽，保留 opcode 并使用引擎字符串参数编译器。已占用全部这些槽位的其他控制台扩展需要检查兼容性。
