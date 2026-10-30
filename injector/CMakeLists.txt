add_library(injector STATIC Injector.cpp)
target_include_directories(injector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(injector PRIVATE /W3)
