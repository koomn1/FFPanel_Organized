cmake_minimum_required(VERSION 3.20)
project(FFPanel CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4)
FetchContent_MakeAvailable(imgui)

set(MINHOOK_DIR "${CMAKE_SOURCE_DIR}/third_party/minhook")
if(NOT EXISTS "${MINHOOK_DIR}/src/hook.c")
    message(FATAL_ERROR "MinHook not found. Run setup.bat first.")
endif()

add_library(minhook STATIC
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c)

set_target_properties(minhook PROPERTIES LINKER_LANGUAGE C)
target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/third_party)

add_subdirectory(external)
add_subdirectory(panel)
add_subdirectory(injector)
add_subdirectory(loader)
add_subdirectory(driver)
