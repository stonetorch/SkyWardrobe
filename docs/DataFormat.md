# SkyWardrobe 数据格式（schemaVersion 1）

正式文件：`Data/SKSE/Plugins/SkyWardrobe/Wardrobe.json`，UTF-8，可含 BOM。JSON 保存用户修改，不保存完整扫描目录。

参见 `assets/examples/Wardrobe.example.json`。字段：

| 字段 | 类型 | 含义 |
|---|---|---|
| schemaVersion | integer | 固定 1，未知版本拒绝载入 |
| pluginCategories | object | 键是来源插件文件名，值含 enabled、alias、search |
| categories | object | 键是稳定分类 ID，值含 name、search、aliases |
| items | object | 键是 PluginName + 本地十六进制 FormID，值含 alias、note、favorite、categories、part、search |
| presets | object | 键是稳定预设 ID，值含 name、search、items |
| workingItems | string array | 工作搭配中的稳定 FormKey |
| activePreset | string | 当前预设 ID，空字符串表示未绑定 |
| partNames | object | 自定义部件 ID 到显示名称 |
| viewOptions | object | byPart、automatic、columns |

FormKey 示例：`Clothes.esp|000ABC`；普通 ESP 最多 24 位本地 ID，ESL 实际使用 12 位。不要把加载顺序索引或 `FE123ABC` 写入 JSON。缺失模组时保留字符串，并在应用时给出错误。十六进制保存统一大写与至少六位；重复 FormKey 会去重，但重复的元数据键会报错。

服装原名、别名、备注、分类、插件别名及预设名称中的中文通过 cpp-pinyin 自动生成无声调拼音索引，无需维护拼音表。支持全拼、连续首字母和连续音节前缀组合：`裙子` 可用 `qun zi`、`qunzi`、`qz`、`qzi`、`qu zi` 搜索；`黑色连衣裙` 可用 `hslyq`、`lyq`、`lianyiqun`、`heiselyq` 搜索。忽略 ASCII 空格和大小写；匹配不会跳过音节，也不会跳过单个音节内部的字母。原文匹配排序优先于自动拼音匹配。

保留人工纠音与扩展词：`pinyin: "zhong qing"` 或 `search: ["zhong qing", "dress"]`。每条读音用空格分隔音节，人工读音与自动读音共同参与搜索；其他英文搜索词仍可直接匹配。保存或重新加载数据会清理并重建缓存。字典位于 `Data/SKSE/Plugins/SkyWardrobe/dict/`，随构建和安装分发；缺失时日志警告，原文和手工搜索词继续可用。自动转换主要支持 U+4E00–U+9FFF 常用汉字，扩展区汉字可添加人工读音。

语义部件 ID：

```text
unknown body top bottom underwear bra panties shoes stockings
gloves head hair neck cloak accessory
custom1 custom2 custom3 custom4 custom5 custom6 custom7 custom8
```

biped 槽位掩码只从游戏扫描，语义部件仅用于分类显示。部件修改不会修改 ARMO 槽位。

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
