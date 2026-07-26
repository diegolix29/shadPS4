# Bachata Vortek Android/Bionic server: Task 4 lifecycle + Task 6 headless + Task 7 WSI.
# Reuses matched upstream request_handler + xwindow_swapchain; stubs optional Winlator features.

get_filename_component(BACHATA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../../" ABSOLUTE)
set(VORTEK_UPSTREAM "${BACHATA_ROOT}/runtime/sources/winlator-app/app/src/main/cpp/vortekrenderer")
set(WINLATOR_UPSTREAM "${BACHATA_ROOT}/runtime/sources/winlator-app/app/src/main/cpp/winlator")

if(NOT EXISTS "${VORTEK_UPSTREAM}/src/request_handler.c")
    message(FATAL_ERROR "Vortek server source missing at ${VORTEK_UPSTREAM} — run vendor-vortek.sh")
endif()

add_library(bachata_vortek_server SHARED
    bachata_vortek_server.cpp
    bachata_vortek_jni.cpp
    bachata_vortek_io.c
    bachata_vk_context.c
    bachata_ahb_present.c
    bachata_vortek_stubs.c
    # Matched upstream (pinned Winlator revision)
    ${VORTEK_UPSTREAM}/src/request_handler.c
    ${VORTEK_UPSTREAM}/src/vulkan_helper.c
    ${VORTEK_UPSTREAM}/src/vk_object.c
    ${VORTEK_UPSTREAM}/src/resource_memory.c
    ${VORTEK_UPSTREAM}/src/shader_inspector.c
    bachata_xwindow_swapchain.c
    ${VORTEK_UPSTREAM}/src/timeline_semaphore.c
    ${VORTEK_UPSTREAM}/src/async_pipeline_creator.c
    # Shared helpers
    ../winlator/src/ring_buffer.c
    ../winlator/src/sysvshared_memory.c
    ../winlator/src/arrays.c
)

target_include_directories(bachata_vortek_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../winlator/include
    ${VORTEK_UPSTREAM}/include
    ${VORTEK_UPSTREAM}/src
    ${WINLATOR_UPSTREAM}/include
)

target_compile_options(bachata_vortek_server PRIVATE
    -Wall
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-implicit-function-declaration
    -Wno-incompatible-pointer-types-discards-qualifiers
    -fvisibility=default
    # events.h (via ring_buffer) uses write/read/usleep without includes
    -include unistd.h
)

target_compile_definitions(bachata_vortek_server PRIVATE
    BACHATA_VORTEK_SERVER=1
    VT_SERVER=1
    VK_NO_PROTOTYPES=1
    VK_USE_PLATFORM_ANDROID_KHR=1
)

target_link_libraries(bachata_vortek_server
    PRIVATE
    android
    dl
    log
    nativewindow
    jnigraphics
    EGL
    GLESv2
)
