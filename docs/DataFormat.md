# SkyWardrobe 数据格式（schemaVersion 1）

正式文件：`Data/SKSE/Plugins/SkyWardrobe/Wardrobe.json`，UTF-8，可含 BOM。JSON 保存用户修改，不保存完整扫描目录。

参见 `assets/examples/Wardrobe.example.json`。字段：

| 字段 | 类型 | 含义 |
|---|---|---|
| schemaVersion | integer | 固定 1，未知版本拒绝载入 |
| pluginCategories | object | 键是来源插件文件名，值含 enabled、alias、search |
| categories | object | 键是稳定分类 ID，值含 name、search、aliases |
| items | object | 键是 PluginName + 本地十六进制 FormID，值含 alias、note、favorite、categories、part、search |
| presets | object | 键是稳定预设 ID，值含 name、search、actor、rules；group 保留默认组兼容入口 |
| workingItems | string array | 玩家工作搭配的兼容镜像，逐角色内容见 actors |
| activePreset | string | 玩家当前预设的兼容镜像 |
| actualEquipmentMode | string | 玩家模式兼容镜像：off、shoes、all；逐角色模式见 actors |
| livePreview | boolean | 实时预览，缺省 true |
| directories | object | 单层目录，键为目录 ID，值含 name |
| navigation | object | directoryOrder、categoryOrder、groupOrder、placements、collapsed |
| actors | object | 角色引用键到 name、managed、workingItems、activePreset、actualEquipmentMode、automatic、manual、runtime 等配置 |
| partNames | object | 自定义部件 ID 到显示名称 |
| viewOptions | object | byPart、automatic、columns |
| quickAccess | object | 快捷浮窗的 enabled、attached、rows、columns、cellSize、cells |

FormKey 示例：`Clothes.esp|000ABC`；普通 ESP 最多 24 位本地 ID，ESL 实际使用 12 位。不要把加载顺序索引或 `FE123ABC` 写入 JSON。缺失模组时保留字符串，并在应用时给出错误。十六进制保存统一大写与至少六位；重复 FormKey 会去重，但重复的元数据键会报错。

## 导航与角色预设扩展

导航顺序数组保存带类型前缀的引用：`directory:dir-1`、`categories:daily`、`groups:casual`、`pluginCategories:Clothes.esp`。过期和重复顺序条目不显示，新条目追加在已有顺序后。`placements` 把普通服装组或移入普通分区的模组条目映射到目录 ID，空字符串为未整理。模组键不在 placements 中时放在最下方注册模组分区，仍按原有插件文件名顺序。移动不转换服装组类型，不复制成员。`collapsed` 保存分区和目录折叠状态，搜索展开不覆盖用户折叠选择。

玩家角色键固定 `player`。普通人物使用引用自身的 `PluginName|LocalFormID`，不是 NPC 基础类型；动态人物使用 `dynamic:` 标识，由 co-save 保存 FormID 绑定并在读档时调用 SKSE ResolveFormID。不同存档中未建立绑定的动态人物保持不可用，不能用同名人物自动替代。移除管理保留配置并置 managed=false；未加载时用 releasePending 延迟清理，takeoverPending 表示等待关闭该角色的 SOS 自动切换。

每个预设归属一个 actor。rules 的键为 combat、dungeon、city、town、default，值为 `{ "enabled": true, "mode": "random", "groups": ["groups:a", "groups:b"] }`。mode 支持 single、random、sequence。组内穿搭仍是随机槽位互斥选择，不递归收集引用组。actor.runtime 保存最近成功的 items、state、group、条件 signature、各条件 progress.last，以及战斗前 resume；presetRuntime 按预设保存独立进度。工作搭配和自动结果分离。

co-save 沿用 SWDE serialization ID：实际装备记录 1/2 使用版本 2，增加角色归属，并兼容版本 1 的玩家记录；记录 3 保存动态引用绑定，4 保存接管前 SOS 自动切换值，5 保存角色上下文快照。NPC 的结果、轮换进度、工作搭配、选中预设和装备/自动模式继续从该存档恢复。玩家是否参与记录 5 由 INI 中的玩家搭配存储方式决定，详见下节。目录与规则定义仍从 JSON 读取。角色管理的移除状态保持当前 JSON 决定。配置缺少新字段时渐进补全，旧预设归属玩家并迁移到默认条件，自动初始关闭。

服装原名、别名、备注、分类、插件别名及预设名称中的中文通过 cpp-pinyin 自动生成无声调拼音索引，无需维护拼音表。支持全拼、连续首字母和连续音节前缀组合：`裙子` 可用 `qun zi`、`qunzi`、`qz`、`qzi`、`qu zi` 搜索；`黑色连衣裙` 可用 `hslyq`、`lyq`、`lianyiqun`、`heiselyq` 搜索。忽略 ASCII 空格和大小写；匹配不会跳过音节，也不会跳过单个音节内部的字母。原文匹配排序优先于自动拼音匹配。

保留人工纠音与扩展词：`pinyin: "zhong qing"` 或 `search: ["zhong qing", "dress"]`。每条读音用空格分隔音节，人工读音与自动读音共同参与搜索；其他英文搜索词仍可直接匹配。保存或重新加载数据会清理并重建缓存。字典位于 `Data/SKSE/Plugins/SkyWardrobe/dict/`，随构建和安装分发；缺失时日志警告，原文和手工搜索词继续可用。自动转换主要支持 U+4E00–U+9FFF 常用汉字，扩展区汉字可添加人工读音。

语义部件 ID：

```text
unknown body top bottom underwear bra panties shoes stockings
gloves head hair neck cloak accessory
custom1 custom2 custom3 custom4 custom5 custom6 custom7 custom8
```

biped 槽位掩码只从游戏扫描，语义部件仅用于分类显示。部件修改不会修改 ARMO 槽位。

`actualEquipmentMode` 为逐角色搭配设置，不随预设切换；玩家字段随所选玩家搭配存储方式保存，NPC 规则保持不变。旧 JSON 缺失时自动补为 `off`，非法值拒绝载入。服装实际穿戴通过原版 `ActorEquipManager` 完成，会改变装备属性和效果。临时物品实例与衣柜实际穿戴记录保存在对应存档的 SKSE co-save 中，不写入全局 Wardrobe.json；读档重置旧世界记账，再按该存档恢复并解析 FormID。

## 别名包

在设置中勾选注册插件时，自动为该插件的全部已扫描 ARMO 生成 `AliasPacks/00_Generated.<插件文件名>.json`。每个条目以本地 FormID 为键，`name` 填游戏物品原名，`part` 填当前识别的部件 ID（含已有元数据修正，未识别时为 `unknown`）。已有生成文件会保留，避免重新注册覆盖手工编辑。

选择已注册插件后可点击“重新生成别名包模板”，覆盖该插件的生成文件；文件中的手工修改也会被替换。其他别名包与用户数据库不受影响。生成文件不会立即重载，编辑完成后点击“重新加载 JSON 与别名包”或执行 `wardrobe reload`。生成文件使用 `00_Generated.` 前缀，排序在它之后的其他包仍按原有规则覆盖同一件服装。

目录 `Data/SKSE/Plugins/SkyWardrobe/AliasPacks/`，只读取扩展名为 `.json` 的文件。格式：

```json
{
  "plugin": "Clothes.esp",
  "items": {
    "000ABC": {
      "name": "裙子",
      "pinyin": "qun zi",
      "aliases": ["黑裙", "dress"],
      "note": "短款，带 SMP",
      "part": "body"
    }
  }
}
```

`name` 映射为服装别名；也支持 `search`、`categories` 与 `favorite`。分类 ID 的定义放在用户数据库 categories 中。包提供的每个字段可以被用户覆盖；用户只改 favorite 时，其他包字段继续生效。完整删除用户元数据可恢复包默认值。

外部修改文件后，在游戏内执行 `wardrobe reload` 或点击“重新加载 JSON 与别名包”。这会重新载入已保存工作搭配和当前预设，不会自动应用外观。保存错误会保留内存中的修改，修正权限后可“重试保存”；解析错误会禁写，以保护原文件。

## 服装组

`groups` 保存其他服装组，成员为 `items` FormKey 数组。分类和注册插件也通过同一服装组接口访问，但分类成员仍使用物品的 `categories`，插件组按来源插件直接读取目录，注册只保存来源，不生成全量成员或别名模板。别名模板可手动生成。

组标识为 `categories:<分类 ID>`、`pluginCategories:<插件文件名>` 或 `groups:<组 ID>`。三类组均支持 `name`、`alias`、`note`、`search` 和 `references`（组标识数组）。别名优先于名称；插件可在元数据弹窗恢复直接显示模组名。引用用于导航，不参与组内随机选衣，也不递归应用；自动规则可将指定普通服装组的直接引用作为候选列表。

预设的 `group` 字段绑定要使用的服装组。旧预设首次加载会自动迁移到独立服装组；原物品元数据和分类保持不变。应用预设或使用服装组会打乱直接成员顺序，选择真实 biped 槽位互不覆盖的服装，再应用结果。每次使用都重新随机；部件标记不参与冲突判定。旧 `items` 字段保留兼容信息，应用以 `group` 为准。

衣柜中分类、注册插件和其他组分段展示。浏览组时可编辑元数据、使用组、创建子组。创建子组扫描不重复名称，建立双向引用，随后在父组中快捷点选成员。快捷添加高亮表示目标组成员，部件标记高亮表示目标部件，移除高亮表示当前组成员；橙色边框与工作搭配高亮区分。快捷状态不会穿脱工作搭配，结束后恢复普通点击。

## 快捷浮窗

打开 `wa` 菜单时默认显示 3 × 3 快捷浮窗，采用无标题栏、圆角布局，尺寸按网格自动计算，四边留白与横纵间距对称，图标在方格中居中。浮窗位于主面板右侧并与主面板垂直居中；空间不足时尝试左侧，并限制在可见视口内。点击空格子或右键任意格子打开绑定编辑器，选择操作、目标和图标后保存。可设置悬停名称；留空则跟随目标当前名称。右键浮窗留白或进入主面板“界面设置”，可调整显示开关、跟随模式、行列数和方格大小；关闭跟随后可按住浮窗留白拖动，位置在本次游戏进程中保留。

`quickAccess` 缺省使用默认配置，不改变旧数据库的 schemaVersion。`enabled`、`attached` 为布尔值；`rows`、`columns` 为 1–12 的整数；`cellSize` 为 32–128，默认 64，按界面缩放调整实际像素大小。大网格提供横向和纵向滚动。`cells` 最多 144 项，按从左到右、从上到下排列；更改列数会按新列数换行。缩小网格保留超出显示范围的绑定，重新扩大后恢复。

每项格子为对象，含 `action`、`target`、`icon`、`label` 字符串；空对象为空格子。操作与目标：

| action | 点击行为 | target |
|---|---|---|
| none | 编辑空格子 | 空字符串 |
| category | 主面板切到衣柜并跳转分类，清除搜索过滤 | `categories:<分类 ID>` |
| group | 主面板切到衣柜并查看服装组，清除搜索过滤 | `groups:<组 ID>` |
| preset | 选择预设并展示工作搭配；实时预览开启时沿用自动应用行为 | 预设 ID |
| applyPreset | 选择并立即应用预设，展示工作搭配 | 预设 ID |
| useGroup | 一键使用指定组并立即应用，展示工作搭配 | `groups:`、`categories:` 或 `pluginCategories:` 组标识 |

预设与使用组操作沿用现有随机选衣和槽位冲突规则。目标使用稳定 ID，重命名无需重绑；删除目标或预设绑定组后格子以橙色提示失效，点击不会应用旧搭配，右键可重新绑定。

图标库使用项目已有的 Font Awesome Free Solid 字体，包含 34 个常见图标；支持中英文搜索。`icon` 标识包括 `folder`、`shirt`、`dress`、`shoes`、`socks`、`gloves`、`hat`、`crown`、`gem`、`ring`、`glasses`、`mask`、`star`、`heart`、`shield`、`magic`、`dragon`、`bolt`、`fire`、`snow`、`sun`、`moon`、`leaf`、`feather`、`palette`、`user`、`home`、`bookmark`、`layers`、`tag`、`archive`、`dice`、`music`、`check`。未知标识回退为文件夹图标；字体缺失时显示文字。图标来源：[Font Awesome](https://fontawesome.com/)，许可证随现有字体分发。


## 默认预设与规则候选列表

顶层 `multiplePresets` 为布尔值，缺省 `false`。角色的 `defaultPreset` 保存固定默认预设 ID；首次迁移优先采用该角色当前活动预设，没有时采用已有的角色预设或创建空规则预设。关闭增强功能时 `activePreset` 固定为 `defaultPreset`，额外预设保留。新建服装组只保存工作搭配的 `items` 快照，不改变预设绑定或规则。

规则仍包含 `enabled`、`mode` 和 `groups`。可选 `listGroup` 为 `groups:<组 ID>` 字符串：在 `random` / `sequence` 模式下，从该组的 `references` 动态读取直接候选，保持顺序、去重并跳过自身；后续按原有规则过滤无效或不可用组。列表源丢失或为空时回落到更低优先级条件，不使用手工 `groups` 替代。`single` 模式使用 `groups`。

可选 `useWorking` 为布尔值，默认 `false`。启用条件命中且 `useWorking=true` 时，直接采用该角色的 `workingItems`，忽略候选组并停止匹配后续条件。即使工作搭配为空也保持此优先级，后备与原版透出按角色现有分层设置合成。该选项不会改写服装组或工作搭配。

```json
{
  "enabled": true,
  "mode": "sequence",
  "groups": [],
  "listGroup": "groups:城市穿搭列表",
  "useWorking": false
}
```

关闭多预设增强功能时，预设快捷格子的绑定保留但按钮隐藏，快捷编辑器不提供预设操作，控制台预设切换也被禁用。


## 玩家搭配存储方式

全局配置文件 `Data/SKSE/Plugins/SkyWardrobe.ini` 使用 `player_outfit_storage=global`（缺省值）或 `player_outfit_storage=save_local`。设置位于面板「设置」中的「玩家搭配存储方式」，不写入 JSON 或 co-save，不随读档变化。无法写入 INI 时界面报告错误并恢复原模式。

玩家搭配状态作为一个整体选择来源，完整字段为：`workingItems`、`backupItems`、`backupEnabled`、`backupGroup`、`originalEnabled`、`hiddenSlots`、`activePreset`、`baseline`、`automatic`、`actualEquipmentMode`、`runtime`、`presetRuntime`、`manual`、`manualState`、`releasePending`、`takeoverPending`、`appearanceActive`。自动结果、战斗前恢复记录与轮换进度包含在 `runtime` / `presetRuntime` 中。`takeoverPending` 在世界重置时按当前自动模式重新安排引擎接管。

`name`、`managed`、`defaultPreset` 保留在 JSON：它们分别是角色显示信息、管理标记和默认预设定义的固定绑定；预设定义及其规则、物品元数据、服装组、多预设增强开关、目录与界面设置始终共享。关闭多预设增强功能时，活动预设仍遵守固定默认预设规则。

- **全局**：玩家搭配从 JSON 读取并异步保存，不必保存游戏。新 co-save 记录 5 只保留 NPC；旧记录中的玩家条目在解析验证前丢弃。加载旧游戏存档保留当前全局玩家搭配与自动进度，不将旧玩家快照写回 JSON。
- **存档局部**：玩家搭配保留在内存并写入 co-save 记录 5，需要保存游戏才能持久化。JSON 写入经过正常校验后，再从输出快照移除上述玩家字段及根级兼容镜像，避免校验补默认值后重新落盘。内存中的编辑不被清理。重载 JSON 保留当前内存玩家搭配，只重载共享数据。
- **局部旧档与新游戏**：没有玩家记录时，以空工作搭配、空后备搭配、后备启用、原版透出关闭、隐藏槽位 0、自动关闭、实际装备 off、外观未激活初始化；基线、手动覆盖、自动结果及轮换进度全部清空。读取局部快照时先重置整组字段，再恢复快照内字段，缺失字段不会沿用之前存档的值。
- **切换模式**：保留当前内存搭配，不立即迁移或清理文件。切到全局后首次衣柜保存将当前搭配写入 JSON，下一次读取游戏存档忽略其玩家快照；切到局部后首次游戏保存将当前搭配写入 co-save，下一次游戏读档使用所读存档的快照（缺少记录则初始化为空）。重启时全局从 JSON 恢复，局部等待所加载存档恢复。

局部模式的普通衣柜保存及游戏保存都会在各自保存路径处理遗留 JSON。游戏保存直接读取磁盘并清除上述字段，没有遗留字段时不改写文件；所有 JSON 写入与模式切换共用互斥锁，后台旧快照不能把局部玩家搭配重新写回。清理保留其他角色与共享配置；失败时保留原文件并记录日志，面板错误区可查看并重试。全局 JSON 写入失败也通过现有错误区报告。

此模式不影响记录 1/2 的临时物品与实际穿戴记账、记录 3 的动态引用绑定、记录 4 的 SOS 原自动切换值。安全模式暂停应用，存储语义保持不变：局部模式中修复搭配后必须保存游戏，读取尚未修复的旧存档仍会恢复其旧搭配。历史实施报告中的无条件玩家 co-save 恢复描述已由本节取代。
