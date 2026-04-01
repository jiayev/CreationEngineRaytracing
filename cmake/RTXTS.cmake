add_subdirectory(extern/RTXTS-TTM)

target_compile_options(rtxts-ttm PRIVATE /W3 /WX-)

target_link_libraries(${PROJECT_NAME} PRIVATE rtxts-ttm)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/RTXTS-TTM/include
)
