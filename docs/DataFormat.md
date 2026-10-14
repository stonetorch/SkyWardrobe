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

co-save 沿用 SWDE serialization ID：实际装备记录 1/2 使用版本 2，增加角色归属，并兼容版本 1 的玩家记录；记录 3 保存动态引用绑定，4 保存接管前 SOS 自动切换值，5 保存角色上下文快照。载入存档时，结果、轮换进度、工作搭配、选中预设和装备/自动模式从该存档恢复，目录与规则定义仍从 JSON 读取。角色管理的移除状态保持当前 JSON 决定。配置缺少新字段时渐进补全，旧预设归属玩家并迁移到默认条件，自动初始关闭。

服装原名、别名、备注、分类、插件别名及预设名称中的中文通过 cpp-pinyin 自动生成无声调拼音索引，无需维护拼音表。支持全拼、连续首字母和连续音节前缀组合：`裙子` 可用 `qun zi`、`qunzi`、`qz`、`qzi`、`qu zi` 搜索；`黑色连衣裙` 可用 `hslyq`、`lyq`、`lianyiqun`、`heiselyq` 搜索。忽略 ASCII 空格和大小写；匹配不会跳过音节，也不会跳过单个音节内部的字母。原文匹配排序优先于自动拼音匹配。

保留人工纠音与扩展词：`pinyin: "zhong qing"` 或 `search: ["zhong qing", "dress"]`。每条读音用空格分隔音节，人工读音与自动读音共同参与搜索；其他英文搜索词仍可直接匹配。保存或重新加载数据会清理并重建缓存。字典位于 `Data/SKSE/Plugins/SkyWardrobe/dict/`，随构建和安装分发；缺失时日志警告，原文和手工搜索词继续可用。自动转换主要支持 U+4E00–U+9FFF 常用汉字，扩展区汉字可添加人工读音。

语义部件 ID：

```text
unknown body top bottom underwear bra panties shoes stockings
gloves head hair neck cloak accessory
custom1 custom2 custom3 custom4 custom5 custom6 custom7 custom8
```

biped 槽位掩码只从游戏扫描，语义部件仅用于分类显示。部件修改不会修改 ARMO 槽位。

`actualEquipmentMode` 为全局衣柜设置，不随预设切换；旧 JSON 缺失时自动补为 `off`，非法值拒绝载入。服装实际穿戴通过原版 `ActorEquipManager` 完成，会改变装备属性和效果。临时物品实例与衣柜实际穿戴记录保存在对应存档的 SKSE co-save 中，不写入全局 Wardrobe.json；读档重置旧世界记账，再按该存档恢复并解析 FormID。

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

组标识为 `categories:<分类 ID>`、`pluginCategories:<插件文件名>` 或 `groups:<组 ID>`。三类组均支持 `name`、`alias`、`note`、`search` 和 `references`（组标识数组）。别名优先于名称；插件可在元数据弹窗恢复直接显示模组名。引用只用于导航，不参与随机搭配，也不递归应用。

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
