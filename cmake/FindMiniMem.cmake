# FindMiniMem — CMake find module for libminimem
#
# Finds the MiniMem transparent memory compression library.
#
# This will define:
#   MiniMem_FOUND        — True if the system has the MiniMem library
#   MiniMem_INCLUDE_DIRS  — Include directories needed to use MiniMem
#   MiniMem_LIBRARIES    — Libraries needed to link to MiniMem
#   MiniMem_VERSION       — Version of MiniMem found

find_path(MiniMem_INCLUDE_DIR
    NAMES minimem/minimem.h
    PATHS
        /usr/include
        /usr/local/include
        ${CMAKE_PREFIX_PATH}/include
)

find_library(MiniMem_LIBRARY
    NAMES minimem
    PATHS
        /usr/lib
        /usr/local/lib
        /usr/lib/x86_64-linux-gnu
        ${CMAKE_PREFIX_PATH}/lib
)

find_library(MiniMem_STATIC_LIBRARY
    NAMES minimem.a
    PATHS
        /usr/lib
        /usr/local/lib
        /usr/lib/x86_64-linux-gnu
        ${CMAKE_PREFIX_PATH}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MiniMem
    FOUND_VAR MiniMem_FOUND
    REQUIRED_VARS MiniMem_LIBRARY MiniMem_INCLUDE_DIR
    VERSION_VAR MiniMem_VERSION
)

if(MiniMem_FOUND AND NOT TARGET MiniMem::MiniMem)
    add_library(MiniMem::MiniMem UNKNOWN IMPORTED)
    set_target_properties(MiniMem::MiniMem PROPERTIES
        IMPORTED_LOCATION "${MiniMem_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MiniMem_INCLUDE_DIR}"
    )
endif()

if(MiniMem_FOUND AND MiniMem_STATIC_LIBRARY AND NOT TARGET MiniMem::MiniMem_static)
    add_library(MiniMem::MiniMem_static STATIC IMPORTED)
    set_target_properties(MiniMem::MiniMem_static PROPERTIES
        IMPORTED_LOCATION "${MiniMem_STATIC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MiniMem_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(MiniMem_INCLUDE_DIR MiniMem_LIBRARY MiniMem_STATIC_LIBRARY)