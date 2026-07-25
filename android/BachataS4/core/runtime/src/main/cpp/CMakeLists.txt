cmake_minimum_required(VERSION 3.22.1)
project(BachataRuntime LANGUAGES C CXX)

get_filename_component(BACHATA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../" ABSOLUTE)
set(ADRENOTOOLS_ROOT "${BACHATA_ROOT}/externals/winlator-app/app/src/main/cpp/libadrenotools")

# Reproducible native builds (F-Droid): strip absolute build paths and GNU build-ids that
# embed machine-specific NDK/project paths.
add_compile_options(
    "-ffile-prefix-map=${BACHATA_ROOT}=."
    "-fdebug-prefix-map=${BACHATA_ROOT}=."
)
add_link_options("LINKER:--build-id=none")

set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${ADRENOTOOLS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/libadrenotools")

add_library(bachata_custom_vulkan SHARED custom_vulkan_bridge.cpp)
target_link_libraries(bachata_custom_vulkan PRIVATE adrenotools android dl log)

add_subdirectory(pkg)
add_subdirectory(winlator)
add_subdirectory(vortek)
