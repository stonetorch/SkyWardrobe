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

服装、分类与预设搜索词均使用字符串数组。例如 `search: ["qun zhuang", "dress"]`。一组完整拼音用一个字符串，空格分隔音节；搜索会合并空格并允许有序子序列，所以 `qzi` 可以命中 `qun zi`。实际中文原名或别名不会自动转换拼音，需要 search / pinyin 或别名包提供读音。

语义部件 ID：

```text
unknown body top bottom underwear bra panties shoes stockings
gloves head hair neck cloak accessory
custom1 custom2 custom3 custom4 custom5 custom6 custom7 custom8
```

biped 槽位掩码只从游戏扫描，语义部件仅用于分类显示。部件修改不会修改 ARMO 槽位。

## 别名包

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
