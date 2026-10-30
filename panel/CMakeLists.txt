add_library(panel SHARED
    dllmain.cpp
    Menu.cpp
    ESP.cpp
    Aimbot.cpp
    Silent.cpp)

target_include_directories(panel PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/minhook/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

target_sources(panel PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

target_link_libraries(panel PRIVATE
    minhook d3d11 dxgi d3dcompiler gdiplus dwmapi user32)
