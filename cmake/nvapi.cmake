vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/NVIDIA/nvapi
    REF 9296d671e71608d6d6b7749ed93989af4ada8858
    HEAD_REF main
)

set(NVAPI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/extern/nvapi")
set(NVAPI_LIBRARY "${CMAKE_SOURCE_DIR}/extern/nvapi/amd64/nvapi64.lib")