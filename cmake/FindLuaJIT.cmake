# Locate LuaJIT and expose the same imported target for both the bundled
# vcpkg build and a system-package build.

include(FindPackageHandleStandardArgs)

if (DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_LUAJIT_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")

    find_path(LuaJIT_INCLUDE_DIR NAMES luajit.h
        PATHS "${_LUAJIT_ROOT}/include"
        PATH_SUFFIXES luajit luajit-2.1 luajit-2.0
        NO_DEFAULT_PATH)
    find_library(LuaJIT_LIBRARY_RELEASE NAMES lua51 luajit-5.1 luajit
        PATHS "${_LUAJIT_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(LuaJIT_LIBRARY_DEBUG NAMES lua51 luajit-5.1 luajit
        PATHS "${_LUAJIT_ROOT}/debug/lib" NO_DEFAULT_PATH)

    include(SelectLibraryConfigurations)
    select_library_configurations(LuaJIT)
else ()
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(PC_LUAJIT QUIET luajit)
        if (NOT PC_LUAJIT_FOUND)
            pkg_check_modules(PC_LUAJIT QUIET luajit-5.1)
        endif ()
    endif ()

    find_path(LuaJIT_INCLUDE_DIR NAMES luajit.h
        HINTS ${PC_LUAJIT_INCLUDE_DIRS} ${PC_LUAJIT_INCLUDEDIR}
        PATH_SUFFIXES luajit luajit-2.1 luajit-2.0)
    find_library(LuaJIT_LIBRARY NAMES lua51 luajit luajit-5.1 libluajit libluajit-5.1
        HINTS ${PC_LUAJIT_LIBRARY_DIRS} ${PC_LUAJIT_LIBDIR})
    set(LuaJIT_LIBRARY_RELEASE "${LuaJIT_LIBRARY}")
endif ()

find_package_handle_standard_args(LuaJIT
    REQUIRED_VARS LuaJIT_INCLUDE_DIR LuaJIT_LIBRARY)
mark_as_advanced(LuaJIT_INCLUDE_DIR LuaJIT_LIBRARY LuaJIT_LIBRARY_RELEASE LuaJIT_LIBRARY_DEBUG)

if (LuaJIT_FOUND)
    set(LuaJIT_INCLUDE_DIRS "${LuaJIT_INCLUDE_DIR}")
    set(LuaJIT_LIBRARIES "${LuaJIT_LIBRARY}")

    if (NOT TARGET LuaJIT::LuaJIT)
        add_library(LuaJIT::LuaJIT UNKNOWN IMPORTED)
        set_target_properties(LuaJIT::LuaJIT PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${LuaJIT_INCLUDE_DIR}")

        if (LuaJIT_LIBRARY_RELEASE)
            set_property(TARGET LuaJIT::LuaJIT APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
            set_target_properties(LuaJIT::LuaJIT PROPERTIES
                IMPORTED_LOCATION_RELEASE "${LuaJIT_LIBRARY_RELEASE}")
        endif ()
        if (LuaJIT_LIBRARY_DEBUG)
            set_property(TARGET LuaJIT::LuaJIT APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
            set_target_properties(LuaJIT::LuaJIT PROPERTIES
                IMPORTED_LOCATION_DEBUG "${LuaJIT_LIBRARY_DEBUG}")
        endif ()
        if (NOT LuaJIT_LIBRARY_RELEASE AND NOT LuaJIT_LIBRARY_DEBUG)
            set_target_properties(LuaJIT::LuaJIT PROPERTIES
                IMPORTED_LOCATION "${LuaJIT_LIBRARY}")
        endif ()
    endif ()
endif ()

unset(_LUAJIT_ROOT)
