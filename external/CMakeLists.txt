add_executable(external main.cpp Overlay.cpp)
target_include_directories(external PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/third_party)
target_compile_options(external PRIVATE /W3)
target_link_libraries(external PRIVATE user32 gdiplus)

add_custom_command(TARGET external POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config.json $<TARGET_FILE_DIR:external>)
