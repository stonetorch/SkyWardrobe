#pragma once

#include <string>
#include <string_view>

// 构建未显式指定时保留英文；业务 UI 只引用常量，不直接判断语言。
#ifndef CAF_UI_ZH_CN
#define CAF_UI_ZH_CN 0
#endif
#if CAF_UI_ZH_CN != 0 && CAF_UI_ZH_CN != 1
#error CAF_UI_ZH_CN must be 0 or 1
#endif

namespace CAF::Ui::Text
{
#if CAF_UI_ZH_CN
#define CAF_TEXT(name, english, chinese) inline constexpr char name[] = chinese;
#else
#define CAF_TEXT(name, english, chinese) inline constexpr char name[] = english;
#endif
#define CAF_LABEL(name, english, chinese) CAF_TEXT(name, english, chinese)
#define CAF_DIAGNOSTIC(name, english, chinese) CAF_TEXT(name, english, chinese)
#include "ui/Text.def"
#undef CAF_DIAGNOSTIC
#undef CAF_LABEL
#undef CAF_TEXT

    // 仅转换展示名称，调用方继续使用原始脚本代码/通道名称保存与解析。
    std::string_view DisplayName(std::string_view name);
    // 旧诊断仍携带英文文本身份；在绘制边界转换，避免改变运行时比较与等待计时。
    std::string Diagnostic(std::string_view message);
}
