include(FindPackageHandleStandardArgs)

# Add vcpkg installation path to search paths
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows")
    set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows" ${CMAKE_PREFIX_PATH})
endif()

find_library(CryptoPP_LIBRARY NAMES cryptopp)
find_path(CryptoPP_INCLUDE_PATH NAMES cryptopp/cryptlib.h)

find_package_handle_standard_args(
    CryptoPP
    REQUIRED_VARS CryptoPP_LIBRARY CryptoPP_INCLUDE_PATH
)

if(CryptoPP_FOUND AND NOT TARGET cryptopp::cryptopp)
    add_library(cryptopp::cryptopp UNKNOWN IMPORTED)
    set_property(TARGET cryptopp::cryptopp PROPERTY IMPORTED_LOCATION "${CryptoPP_LIBRARY}")
    set_property(TARGET cryptopp::cryptopp PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${CryptoPP_INCLUDE_PATH}")
endif()
