cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0116 NEW)
set(CMAKE_POLICY_WARNING_CMP0116 OFF)

project(
    CreationEngineRaytracing
    VERSION 0.0.1
    LANGUAGES CXX
)

# default install path
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set_property(
        CACHE CMAKE_INSTALL_PREFIX
        PROPERTY VALUE "${CMAKE_CURRENT_BINARY_DIR}/aio"
    )
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# ########################################################################################################################
# ## Build options
# ########################################################################################################################
message("Options:")

option(
    AUTO_PLUGIN_DEPLOYMENT
    "Copy the build output and addons to env:CommunityShadersOutputDir."
    OFF
)

# #######################################################################################################################
# # Add CMake features
# #######################################################################################################################
include(Module)

# #######################################################################################################################
# # Find dependencies
# #######################################################################################################################
find_path(BSHOSHANY_THREAD_POOL_INCLUDE_DIRS "BS_thread_pool.hpp")
find_package(magic_enum CONFIG REQUIRED)
find_package(xbyak CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(EASTL CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_path(CLIB_UTIL_INCLUDE_DIRS "ClibUtil/utils.hpp")
find_package(cppwinrt CONFIG REQUIRED)
find_package(directx-dxc CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
	Microsoft::DirectXTK
	Microsoft::DirectX-Headers
    Microsoft::DirectXShaderCompiler
    EASTL
	d3d12.lib
	magic_enum::magic_enum
	d3dcompiler.lib
	nvrhi_d3d12
	nvrhi
    xbyak::xbyak
)

include(NVRHI)

target_compile_definitions(
    ${PROJECT_NAME}
    PRIVATE 
    CERT_EXPORTS
)