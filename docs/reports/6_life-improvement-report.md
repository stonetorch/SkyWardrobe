# 衣柜操作改进与逐角色自动换装报告

日期：2026-10-14。设计依据：[6_life-improvement.md](../backlog/6_life-improvement.md)。

## 完成范围

本次实现服装组目录与排序、角色管理、默认实时预览、逐角色条件预设、搜索结果批量快捷操作，复用现有衣柜、拼音搜索、ImGui 输入、快捷浮窗、异步 JSON 保存和 SOS 外观后端。服装组引用仍只用于导航，不递归参与选衣。

| 功能 | 实现行为 |
|---|---|
| 导航 | 搜索与全部服装、收藏、未分类、工作搭配固定；下面分类服装组、服装组、注册模组共用一个滚动区域。单层目录，标题左对齐，成员一级小幅缩进。 |
| 排序与移动 | 分类在本区排序；目录和普通组支持拖动、插入线、跨目录移动、悬停展开与边缘滚动。右键移动支持开头、末尾、第 N 位、指定条目前后，按完整列表计算位置。目录可重命名和删除，删除保留成员。 |
| 模组条目 | 底部保持插件文件名顺序，不能单项排序；可拖出/右键移动到普通目录后排序。仍是 pluginCategories 引用，成员动态取自来源插件，不能手工增删，不重复显示；可移回底部。 |
| 角色管理 | 独立角色管理 Tab，添加打开面板前注视的人物、搜索附近人物、移除管理。ActorTypeNPC 筛选，排除死亡人物及非人物种族。玩家固定第一且不可移除；主衣柜和预设页提供角色选择器。 |
| 实时预览 | 缺省开启，保存用户选择。工作搭配按角色隔离，切换角色不应用。手动编辑暂缓自动换装，关闭面板保留；下次条件变化恢复，也可手动恢复。 |
| 预设 | 预设属于具体角色，同角色可有多个、同时选中一个。战斗、地下城、城市、城镇、默认按优先级匹配；单组、随机避连选、顺序循环；候选搜索、拖动及精确移动，独立于侧栏顺序。 |
| 批量操作 | 全部搜索结果快照，包括滚动区域外服装，排除引用。普通模式改工作搭配，快捷模式改成员或部件；只保存一次并提交最终预览一次。取消部件标记清除匹配的用户覆盖，回退别名包/自动识别。 |

## 角色与异步应用隔离

`ActorRuntime` 在游戏线程解析人物引用、采样角色自己的战斗状态和地点关键词（包括父地点），界面消费字符串快照。当前版本每 0.5 秒调度采样，主菜单和暂停菜单跳过。注视目标在打开面板、停用视角输入前复制 handle，随后在游戏线程解析。

普通角色用引用自身的稳定 FormKey，动态角色用独立标识和 co-save FormID 重映射；不会按姓名或 NPC 基础类型合并。每个角色的运行时 Outfit 使用稳定角色键的 64 位 FNV-1a 摘要，名称固定 21 字符。避免直接使用运行时 FormID，防止加载顺序变化后覆盖其他角色旧 Outfit。

SOS 后端按角色合并待执行请求，整体串行处理。操作捕获角色 handle、装备模式、世界 epoch 和角色请求版本；同角色新操作取消旧回调链，切换编辑角色不重定向在途操作。读档取消旧世界请求。实际装备从固定玩家推广到目标 Actor，临时物品与原有已装备物品按角色隔离，仍按 ExtraUniqueID 精确回收指定实例。

## 自动换装与手动编辑

自动状态和工作搭配分开，自动换装不改写正在编辑的工作区。规则选择先过滤不存在、空或缺失成员的普通服装组，再选择候选组，沿用组内随机槽位互斥生成。持续相同条件不重新选取；没有可用规则时保留外观并显示状态。单组模式界面保留一个候选，随机/顺序允许多个。

轮换的 `last` 只在 SOS 回读及实际装备操作成功后提交，失败不推进。角色、预设和条件分别保存进度。战斗前保存当前结果，退出战斗且回到原条件时恢复该结果，同时保留战斗进度，避免下一场战斗回到第一套。重新加载复用成功结果，避免重新随机。

启用自动换装会先保存并关闭该角色 SOS 自动切换，即使暂时没有有效候选，也不清空外观。角色先注册到 SOS，再设置开关。关闭自动或移除管理时恢复原值；未加载人物保留待处理标记，加载后完成。移除管理还清理本插件外观与临时物品，保留配置供重新添加。

使用的 Actor、外观和自动切换函数签名来自 [SOS NativeFuncs](https://github.com/lispcoc/SkyrimOutfitSystemSE/blob/master/src/papyrus/skyrimoutfitsystemnativefuncs.psc)，按角色读写及父地点遍历行为核对 [SOS OutfitSystem.cpp](https://github.com/lispcoc/SkyrimOutfitSystemSE/blob/master/src/cpp/OutfitSystem.cpp)。本插件保留缺失 SOS 时全部禁用且禁止任何 VM 探测的原有保护。

## 数据迁移与存档

schemaVersion 仍为 1，新增字段按缺省补全。旧玩家工作搭配和实际装备模式迁入 actors.player，根级旧字段保留玩家镜像。旧预设绑定玩家，原服装组迁为默认条件；自动初始关闭。livePreview 缺省 true，显式 false 保留。共享服装元数据、别名包和组引用不复制。

JSON 增加 directories、navigation、actors，presets 增加 actor 和 rules。迁移检查字段类型、规则模式、运行状态结构和物品 FormKey，坏数据沿用原有拒绝载入/禁止覆盖机制。完整字段说明见 [DataFormat.md](../DataFormat.md)。

co-save 沿用 SWDE：实际装备记录升级为版本 2 并兼容版本 1 玩家记录；增加动态引用绑定、接管前 SOS 开关和角色上下文快照。读档恢复该存档的结果、进度、工作搭配、选中预设及模式，目录和规则定义仍来自 JSON，角色是否已移除由当前管理配置决定。旧版本存档没有新快照时，首次条件评估会生成结果。

## 验证

按仓库要求提权运行 `scripts/Build-Debug.ps1`，使用脚本固定的 CLion CMake、vs2022/debug 预设；未直接调用 CMake，未改工具链。

| 检查 | 结果 |
|---|---|
| Debug 编译与链接 | 最终提权脚本退出码 0，输出“Debug 编译成功”。 |
| SkyWardrobeLifeImprovementTests | 通过：旧玩家迁移、缺省/显式预览、迁移幂等、角色数据保留、稳定 Outfit 命名、排序去重和边界、随机避重复、顺序循环、战斗恢复保留进度、坏字段拒绝。 |
| SkyWardrobeModelTests | 通过：原有数据模型、组与 JSON 保存回归检查。 |
| SkyWardrobeActualEquipmentTests | 通过：实际装备计划回归检查；不等同于 NPC 原版装备引擎验收。 |
| SkyWardrobeQuickAccessTests | 通过：快捷浮窗配置回归检查；预设目标现在限定当前角色，选择和应用入口分别处理。 |
| git diff --check | 通过；Git 的 LF/CRLF 提示不属于差异检查错误。 |
| Skyrim 游戏内检查 | 未执行。 |

构建期间有一轮在 DLL 编译链接完成后，AUTO_DEPLOY 的复制命令报告 `Permission denied`，脚本退出 1；其他轮次脚本成功。未修改构建设置规避副作用，未检查部署结果或 SHA-256。该记录与 C++ 源码编译失败区分。

### 最终执行记录

最终运行提权 `scripts/Build-Debug.ps1`，退出码 0，输出 `Debug 编译成功。`。随后执行以下四个 Debug 可执行文件，全部退出码 0：

```text
SkyWardrobeLifeImprovementTests: Life improvement migration, ordering and rule selection checks passed
SkyWardrobeModelTests: Wardrobe model and persistence checks passed
SkyWardrobeActualEquipmentTests: Actual equipment tests passed
SkyWardrobeQuickAccessTests: Quick access settings tests passed
```

`git diff --check` 退出码 0。未运行游戏、未检查部署或 SHA-256。工作区中已有/并行编辑的 `docs/backlog/7_multi-layer.md` 未由本次任务修改。

## 游戏内验收边界

编译和离线模型检查确认源码及数据逻辑，不确认拖动手感、真实滚动范围、准星人物识别、NPC 外观/实际装备、SOS 竞争、动态人物读档或真实地点分类效果。现场验收清单已加入 [RuntimeChecks.md](../RuntimeChecks.md)，需要记录实际 Skyrim、SKSE 和 SOS 版本。

首版地点条件覆盖战斗、地下城、城市、城镇、默认；没有增加天气或任意条件表达式。动态人物只在拥有对应 co-save 绑定的存档中解析。没有修改本插件之外的角色服装系统或打包安装流程。

## 主要代码位置

- `src/wardrobe/LifeModel.h`：迁移、验证、排序、规则候选选择、稳定 Outfit 名称和战斗恢复。
- `src/wardrobe/ActorRuntime.{h,cpp}`：人物识别、引擎快照、动态引用和 co-save 状态桥接。
- `src/wardrobe/Service.{h,cpp}`：角色工作区、批量事务、逐角色预设评估与成功回调提交。
- `src/wardrobe/SosBackend.{h,cpp}`：角色绑定的串行请求、取消、接管和恢复。
- `src/wardrobe/ActualEquipment.{h,cpp}`：逐角色实际装备和兼容的存档记账。
- `src/wardrobe/Panel.cpp`、`PanelNavigation.inc`、`PanelActors.inc`：固定侧栏、移动交互、批量操作、角色管理和条件编辑。
- `src/wardrobe/QuickAccess.cpp`、`src/ui/Overlay.cpp`：当前角色预设限制、打开前捕获目标。
- `tests/LifeImprovementTests.cpp`、`CMakeLists.txt`：新增独立测试及注册。
