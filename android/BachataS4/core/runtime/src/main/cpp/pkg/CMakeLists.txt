add_library(bachata_pkg SHARED
    bachata_pkg_jni.cpp
    pkg_extractor.cpp
    pkg_crypto.cpp
    pkg_type.cpp
    aes.c
)

target_include_directories(bachata_pkg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bachata_pkg PRIVATE cxx_std_20)
target_compile_options(bachata_pkg PRIVATE -fexceptions -frtti)
find_library(log-lib log)
find_library(z-lib z)
target_link_libraries(bachata_pkg PRIVATE ${log-lib} ${z-lib})
