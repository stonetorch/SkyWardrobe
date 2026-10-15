cmake_minimum_required(VERSION 3.21)

project(SkyWardrobe VERSION 0.1.0 DESCRIPTION "SKSE plugin for Skyrim SE/AE" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SKSE plugins require a 64-bit Windows build.")
endif()

set(COMMONLIB_SOURCE_DIR "${CMAKE_SOURCE_DIR}/extern/CommonLibSSE-NG" CACHE PATH "CommonLibSSE-NG source directory")
if(NOT EXISTS "${COMMONLIB_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "CommonLibSSE-NG is missing. Clone the ng branch into extern/CommonLibSSE-NG (see README.md).")
endif()

set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE unit tests" FORCE)
find_package(spdlog CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)
find_package(cpp-pinyin CONFIG REQUIRED)
add_subdirectory("${COMMONLIB_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/commonlibsse-ng" EXCLUDE_FROM_ALL)
include("${COMMONLIB_SOURCE_DIR}/cmake/CommonLibSSE.cmake")

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "SkyWardrobe"
    AUTHOR "SkyWardrobe"
    SOURCES
        src/Plugin.cpp
        src/ui/Overlay.cpp
        src/ui/InputMethod.cpp
        src/ui/UiTabs.cpp
        src/wardrobe/Store.cpp
        src/wardrobe/PinyinSearch.cpp
        src/wardrobe/ActorRuntime.cpp
        src/wardrobe/Service.cpp
        src/wardrobe/SosBackend.cpp
        src/wardrobe/SafeMode.cpp
        src/wardrobe/ActualEquipment.cpp
        src/wardrobe/Panel.cpp
        src/wardrobe/QuickAccess.cpp
        src/wardrobe/Console.cpp
    USE_ADDRESS_LIBRARY
)

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party")

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set(CAF_UI_LANGUAGE "en" CACHE STRING "CAF UI language: en or zh-CN")
set_property(CACHE CAF_UI_LANGUAGE PROPERTY STRINGS "en" "zh-CN")
if(CAF_UI_LANGUAGE STREQUAL "zh-CN")
    target_compile_definitions(${PROJECT_NAME} PRIVATE CAF_UI_ZH_CN=1)
elseif(CAF_UI_LANGUAGE STREQUAL "en")
    target_compile_definitions(${PROJECT_NAME} PRIVATE CAF_UI_ZH_CN=0)
else()
    message(FATAL_ERROR "Unsupported CAF_UI_LANGUAGE '${CAF_UI_LANGUAGE}'; use en or zh-CN.")
endif()
# 中英文常量统一使用 UTF-8，避免系统代码页影响中文与 ImGui 字形。
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
option(CAF_FREECAM_DIAGNOSTICS "Compile freecam entry diagnostics" OFF)
if(CAF_FREECAM_DIAGNOSTICS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CAF_FREECAM_DIAGNOSTICS=1)
endif()

# MSBuild 的 /m 只并行到项目级，同一 target 内的编译单元仍串行。
# 交给 cl.exe 并行编译插件源文件。
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/MP>)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui minhook::minhook cpp-pinyin::cpp-pinyin d3d11 dxgi)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Data/SKSE/Plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Data/SKSE/Plugins"
)

# 面板图标字体及其许可证随 mod 分发，必须与 DLL 一起到位，否则面板会退回纯文字。
# 运行时按相对路径 Data/SKSE/Plugins/SkyWardrobe/Fonts/ 读取，这里镜像同一结构。
set(CAF_ASSET_FILES
    "${CMAKE_SOURCE_DIR}/assets/fonts/fa-solid-900.ttf"
    "${CMAKE_SOURCE_DIR}/assets/fonts/LICENSE-FontAwesome.txt"
    "${CMAKE_SOURCE_DIR}/assets/fonts/LICENSE-IconFontCppHeaders.txt"
)
set(CAF_ASSET_DIR "SkyWardrobe/Fonts")

foreach(_asset IN LISTS CAF_ASSET_FILES)
    if(NOT EXISTS "${_asset}")
        message(FATAL_ERROR "Missing packaged asset: ${_asset}")
    endif()
endforeach()

# 与 DLL 同级放置，保持构建目录下的 Data/ 自洽，可直接整体拷进游戏。
# 目标子目录在全新构建时不存在，先建目录再拷贝。
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${CAF_ASSET_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${CAF_ASSET_FILES}
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${CAF_ASSET_DIR}/"
    COMMENT "Copying panel assets to $<TARGET_FILE_DIR:${PROJECT_NAME}>/${CAF_ASSET_DIR}"
    VERBATIM
)

if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(_deploy_root "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}")
else()
    set(_deploy_root "${CMAKE_BINARY_DIR}/deploy/${PROJECT_NAME}")
endif()

set(DEPLOY_ROOT "${_deploy_root}" CACHE PATH "Mod manager staging directory for this plugin")
option(AUTO_DEPLOY "Copy the plugin to DEPLOY_ROOT after every successful build" OFF)

if(AUTO_DEPLOY)
    set(_plugin_deploy_dir "${DEPLOY_ROOT}/SKSE/Plugins")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_plugin_deploy_dir}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_plugin_deploy_dir}/${CAF_ASSET_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${_plugin_deploy_dir}/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${CAF_ASSET_FILES} "${_plugin_deploy_dir}/${CAF_ASSET_DIR}/"
        COMMENT "Copying ${PROJECT_NAME} to ${_plugin_deploy_dir}"
        VERBATIM
    )
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "${DEPLOY_ROOT}/SKSE/Plugins")
install(FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>" DESTINATION "${DEPLOY_ROOT}/SKSE/Plugins" OPTIONAL)
install(FILES ${CAF_ASSET_FILES} DESTINATION "${DEPLOY_ROOT}/SKSE/Plugins/${CAF_ASSET_DIR}")
add_custom_target(deploy
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --config Release
    DEPENDS ${PROJECT_NAME}
)

# 只分发示例，绝不在构建时覆盖用户的 Wardrobe.json 或 INI。
set(WARDROBE_EXAMPLES
    "${CMAKE_SOURCE_DIR}/assets/examples/Wardrobe.example.json"
    "${CMAKE_SOURCE_DIR}/assets/examples/AliasPack.example.json"
    "${CMAKE_SOURCE_DIR}/assets/examples/SkyWardrobe.example.ini"
    "${CMAKE_SOURCE_DIR}/docs/DataFormat.md"
    "${CMAKE_SOURCE_DIR}/docs/RuntimeChecks.md"
    "${CMAKE_SOURCE_DIR}/src/third_party/nlohmann/LICENSE.MIT"
)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SkyWardrobe/Examples"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${WARDROBE_EXAMPLES} "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SkyWardrobe/Examples/"
    VERBATIM
)
if(AUTO_DEPLOY)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_plugin_deploy_dir}/SkyWardrobe/Examples"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${WARDROBE_EXAMPLES} "${_plugin_deploy_dir}/SkyWardrobe/Examples/"
        VERBATIM
    )
endif()
install(FILES ${WARDROBE_EXAMPLES} DESTINATION "${DEPLOY_ROOT}/SKSE/Plugins/SkyWardrobe/Examples")


enable_testing()
add_executable(SkyWardrobeMultiLayerTests tests/MultiLayerTests.cpp)
target_include_directories(SkyWardrobeMultiLayerTests PRIVATE src src/third_party)
target_link_libraries(SkyWardrobeMultiLayerTests PRIVATE imgui::imgui)
target_compile_features(SkyWardrobeMultiLayerTests PRIVATE cxx_std_23)
target_compile_options(SkyWardrobeMultiLayerTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME SkyWardrobeMultiLayerTests COMMAND SkyWardrobeMultiLayerTests)

add_executable(SkyWardrobeLifeImprovementTests tests/LifeImprovementTests.cpp)
target_include_directories(SkyWardrobeLifeImprovementTests PRIVATE src src/third_party)
target_compile_features(SkyWardrobeLifeImprovementTests PRIVATE cxx_std_23)
target_compile_options(SkyWardrobeLifeImprovementTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME SkyWardrobeLifeImprovementTests COMMAND SkyWardrobeLifeImprovementTests)
add_executable(SkyWardrobeQuickAccessLayoutTests tests/QuickAccessLayoutTests.cpp)
target_include_directories(SkyWardrobeQuickAccessLayoutTests PRIVATE src)
target_link_libraries(SkyWardrobeQuickAccessLayoutTests PRIVATE imgui::imgui)
target_compile_features(SkyWardrobeQuickAccessLayoutTests PRIVATE cxx_std_23)
target_compile_options(SkyWardrobeQuickAccessLayoutTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME SkyWardrobeQuickAccessLayoutTests COMMAND SkyWardrobeQuickAccessLayoutTests)
add_executable(SkyWardrobeQuickAccessTests tests/QuickAccessTests.cpp)
target_include_directories(SkyWardrobeQuickAccessTests PRIVATE src src/third_party)
target_compile_features(SkyWardrobeQuickAccessTests PRIVATE cxx_std_23)
target_compile_options(SkyWardrobeQuickAccessTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME SkyWardrobeQuickAccessTests COMMAND SkyWardrobeQuickAccessTests)
add_executable(SkyWardrobeActualEquipmentTests tests/ActualEquipmentTests.cpp)
target_include_directories(SkyWardrobeActualEquipmentTests PRIVATE src src/third_party)
target_compile_features(SkyWardrobeActualEquipmentTests PRIVATE cxx_std_23)
target_compile_options(SkyWardrobeActualEquipmentTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME SkyWardrobeActualEquipmentTests COMMAND SkyWardrobeActualEquipmentTests)
add_executable(SkyWardrobeInputMethodTests tests/InputMethodTests.cpp)
target_include_directories(SkyWardrobeInputMethodTests PRIVATE src)
target_compile_features(SkyWardrobeInputMethodTests PRIVATE cxx_std_23)
target_compile_options(SkyWardrobeInputMethodTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME SkyWardrobeInputMethodTests COMMAND SkyWardrobeInputMethodTests)
add_executable(SkyWardrobeModelTests tests/WardrobeModelTests.cpp src/wardrobe/Store.cpp)
target_include_directories(SkyWardrobeModelTests PRIVATE src src/third_party)
target_compile_features(SkyWardrobeModelTests PRIVATE cxx_std_23)
target_compile_definitions(SkyWardrobeModelTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SkyWardrobeModelTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME SkyWardrobeModelTests COMMAND SkyWardrobeModelTests)
add_executable(SkyWardrobeSafeModeTests tests/SafeModeTests.cpp src/wardrobe/SafeMode.cpp)
target_include_directories(SkyWardrobeSafeModeTests PRIVATE src)
target_compile_features(SkyWardrobeSafeModeTests PRIVATE cxx_std_23)
target_compile_definitions(SkyWardrobeSafeModeTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SkyWardrobeSafeModeTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME SkyWardrobeSafeModeTests COMMAND SkyWardrobeSafeModeTests)
target_sources(SkyWardrobeModelTests PRIVATE src/wardrobe/PinyinSearch.cpp)
target_link_libraries(SkyWardrobeModelTests PRIVATE cpp-pinyin::cpp-pinyin)

# 字典随 DLL 和安装包分发，运行时不依赖开发机器上的 vcpkg 路径。
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${cpp-pinyin_DIR}/dict"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SkyWardrobe/dict"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${cpp-pinyin_DIR}/copyright"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SkyWardrobe/LICENSE-cpp-pinyin.txt"
    VERBATIM
)
if(AUTO_DEPLOY)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${cpp-pinyin_DIR}/dict"
                "${_plugin_deploy_dir}/SkyWardrobe/dict"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${cpp-pinyin_DIR}/copyright"
                "${_plugin_deploy_dir}/SkyWardrobe/LICENSE-cpp-pinyin.txt"
        VERBATIM
    )
endif()
install(DIRECTORY "${cpp-pinyin_DIR}/dict" DESTINATION "${DEPLOY_ROOT}/SKSE/Plugins/SkyWardrobe")
install(FILES "${cpp-pinyin_DIR}/copyright" DESTINATION "${DEPLOY_ROOT}/SKSE/Plugins/SkyWardrobe" RENAME "LICENSE-cpp-pinyin.txt")

add_executable(SkyWardrobePinyinSearchTests tests/PinyinSearchTests.cpp src/wardrobe/PinyinSearch.cpp)
target_include_directories(SkyWardrobePinyinSearchTests PRIVATE src src/third_party)
target_compile_features(SkyWardrobePinyinSearchTests PRIVATE cxx_std_23)
target_compile_options(SkyWardrobePinyinSearchTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(SkyWardrobePinyinSearchTests PRIVATE cpp-pinyin::cpp-pinyin)
add_test(NAME SkyWardrobePinyinSearchTests COMMAND SkyWardrobePinyinSearchTests "${cpp-pinyin_DIR}/dict")
