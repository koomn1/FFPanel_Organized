add_executable(loader main.cpp)
target_include_directories(loader PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/injector)
target_link_libraries(loader PRIVATE injector winhttp iphlpapi)
target_compile_options(loader PRIVATE /W3)

add_custom_command(TARGET loader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config.json $<TARGET_FILE_DIR:loader>)
