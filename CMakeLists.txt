cmake_minimum_required(VERSION 3.24)

# This project intentionally has a checked-in vcpkg baseline and overlay
# registry. Pick it automatically for a fresh clone, while still allowing a
# caller to provide a different toolchain explicitly.
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(SIMPLEGRAPHIC_BUNDLED_VCPKG_TOOLCHAIN
        "${CMAKE_CURRENT_LIST_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    if (NOT EXISTS "${SIMPLEGRAPHIC_BUNDLED_VCPKG_TOOLCHAIN}")
        message(FATAL_ERROR
            "The bundled vcpkg submodule is required. Run: git submodule update --init --recursive")
    endif ()
    set(CMAKE_TOOLCHAIN_FILE "${SIMPLEGRAPHIC_BUNDLED_VCPKG_TOOLCHAIN}"
        CACHE FILEPATH "CMake toolchain file")
endif ()

# The shipped Lua extension modules must import one shared LuaJIT runtime.
# Keep our explicit dynamic Unix triplets discoverable by both CMake and
# vcpkg; callers can still supply a larger overlay list themselves.
if (NOT DEFINED VCPKG_OVERLAY_TRIPLETS)
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_LIST_DIR}/vcpkg-triplets"
        CACHE PATH "Additional vcpkg triplet directories")
endif ()
if (UNIX AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    if (APPLE)
        if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
            set(_SIMPLEGRAPHIC_UNIX_TRIPLET arm64-osx-dynamic)
        else ()
            set(_SIMPLEGRAPHIC_UNIX_TRIPLET x64-osx-dynamic)
        endif ()
    elseif (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
        set(_SIMPLEGRAPHIC_UNIX_TRIPLET x86-linux-dynamic)
    elseif (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(_SIMPLEGRAPHIC_UNIX_TRIPLET arm64-linux-dynamic)
    else ()
        set(_SIMPLEGRAPHIC_UNIX_TRIPLET x64-linux-dynamic)
    endif ()
    set(VCPKG_TARGET_TRIPLET "${_SIMPLEGRAPHIC_UNIX_TRIPLET}" CACHE STRING "vcpkg target triplet")
    unset(_SIMPLEGRAPHIC_UNIX_TRIPLET)
endif ()

project(PathOfBuildingSimpleGraphic VERSION 2.6.0 LANGUAGES C CXX)

if (UNIX AND DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET MATCHES "-dynamic$")
    message(FATAL_ERROR
        "Unix package builds require a dynamic LuaJIT triplet so all Lua modules share one runtime. "
        "Use one of the checked-in *-dynamic triplets (for example x64-linux-dynamic).")
endif ()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CTest)

option(SIMPLEGRAPHIC_BUILD_HOST "Build the standalone Lua host executable" ON)
option(SIMPLEGRAPHIC_BUILD_TESTS "Build and register runtime smoke tests" ${BUILD_TESTING})
option(SIMPLEGRAPHIC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if (APPLE)
    enable_language(OBJCXX)
endif ()

if (WIN32)
    # Ship the matching MSVC/UCRT runtime with the staged payload. This keeps
    # x86, x64 and ARM64 packages runnable on a clean Windows 10/11 system.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
    set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
    include(InstallRequiredSystemLibraries)
endif ()

if (MSVC)
    add_compile_options("/Zi" "/W4")
    add_link_options("/DEBUG:FULL")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options("-Wall" "-Wextra" "-Wpedantic")
endif ()

if (SIMPLEGRAPHIC_WARNINGS_AS_ERRORS)
    if (MSVC)
        add_compile_options("/WX")
    else ()
        add_compile_options("-Werror")
    endif ()
endif ()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

if (UNIX AND NOT APPLE)
    set(SIMPLEGRAPHIC_RUNTIME_RPATH "$ORIGIN")
    set(SIMPLEGRAPHIC_LUA_RPATH "$ORIGIN/..")
    set(SIMPLEGRAPHIC_NESTED_LUA_RPATH "$ORIGIN/../..")
elseif (APPLE)
    set(SIMPLEGRAPHIC_RUNTIME_RPATH "@loader_path")
    set(SIMPLEGRAPHIC_LUA_RPATH "@loader_path/..")
    set(SIMPLEGRAPHIC_NESTED_LUA_RPATH "@loader_path/../..")
endif ()

add_subdirectory(dep/glm EXCLUDE_FROM_ALL)

set(SIMPLEGRAPHIC_SOURCES
    "config.h"
    "simplegraphic.h"
    "lua_runtime.h"
    "runtime_smoke.cpp"
    "dep/stb/stb_image.h"
    "dep/stb/stb_image_resize.h"
    "dep/stb/stb_image_write.h"
    "engine/common.h"
    "engine/common/base64.c"
    "engine/common/base64.h"
    "engine/common/common.cpp"
    "engine/common/console.cpp"
    "engine/common/console.h"
    "engine/common/keylist.h"
    "engine/common/memtrak3.cpp"
    "engine/common/memtrak3.h"
    "engine/common/streams.cpp"
    "engine/common/streams.h"
    "engine/core/core_compress.cpp"
    "engine/core/core_compress.h"
    "engine/core/core_config.cpp"
    "engine/core/core_config.h"
    "engine/core/core_image.cpp"
    "engine/core/core_image.h"
    "engine/core/core_main.cpp"
    "engine/core/core_main.h"
    "engine/core/core_tex_manipulation.cpp"
    "engine/core/core_tex_manipulation.h"
    "engine/core/core_video.cpp"
    "engine/core/core_video.h"
    "engine/render/r_font.cpp"
    "engine/render/r_font.h"
    "engine/render/r_main.cpp"
    "engine/render/r_main.h"
    "engine/render/r_texture.cpp"
    "engine/render/r_texture.h"
    "engine/system/win/sys_local.h"
    "engine/system/win/sys_main.cpp"
    "engine/system/win/sys_opengl.cpp"
    "engine/system/win/sys_video.cpp"
    "engine/system/sys_console.h"
    "engine/system/sys_main.h"
    "engine/system/sys_opengl.h"
    "engine/system/sys_video.h"
    "win/entry.cpp"
    "ui.h"
    "ui_api.cpp"
    "ui_console.cpp"
    "ui_console.h"
    "ui_debug.cpp"
    "ui_debug.h"
    "ui_local.h"
    "ui_main.cpp"
    "ui_main.h"
    "ui_subscript.cpp"
    "ui_subscript.h"
)

set(SIMPLEGRAPHIC_PLATFORM_SOURCES)
if (WIN32)
    list(APPEND SIMPLEGRAPHIC_PLATFORM_SOURCES
        "engine/system/win/sys_console.cpp"
        "SimpleGraphic.rc"
    )
else ()
    list(APPEND SIMPLEGRAPHIC_PLATFORM_SOURCES
        "engine/system/win/sys_console_unix.cpp"
    )
endif ()

if (APPLE)
    list(APPEND SIMPLEGRAPHIC_PLATFORM_SOURCES "engine/system/win/sys_macos.mm")
endif ()

find_package(unofficial-angle CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(gli CONFIG REQUIRED)
find_package(LuaJIT REQUIRED)
find_package(Microsoft.GSL CONFIG REQUIRED)
find_package(re2 CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)

if (TARGET zstd::libzstd_shared)
    set(SIMPLEGRAPHIC_ZSTD_TARGET zstd::libzstd_shared)
elseif (TARGET zstd::libzstd_static)
    set(SIMPLEGRAPHIC_ZSTD_TARGET zstd::libzstd_static)
elseif (TARGET zstd::libzstd)
    set(SIMPLEGRAPHIC_ZSTD_TARGET zstd::libzstd)
else ()
    message(FATAL_ERROR "zstd did not export a usable CMake target")
endif ()

add_library(cmp_core STATIC
    dep/compressonator/cmp_core/source/cmp_core.cpp
    dep/compressonator/cmp_core/source/cmp_core.h
    dep/compressonator/cmp_core/shaders/bc3_encode_kernel.cpp
    dep/compressonator/cmp_core/shaders/bc7_encode_kernel.cpp
)
target_include_directories(cmp_core PRIVATE dep/compressonator/applications/_libs/cmp_math)
target_include_directories(cmp_core PUBLIC
    dep/compressonator/cmp_core/shaders
    dep/compressonator/cmp_core/source
)
set_target_properties(cmp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(imgui STATIC
    dep/imgui/imconfig.h
    dep/imgui/imgui.cpp
    dep/imgui/imgui.h
    dep/imgui/imgui_demo.cpp
    dep/imgui/imgui_draw.cpp
    dep/imgui/imgui_internal.h
    dep/imgui/imgui_tables.cpp
    dep/imgui/imgui_widgets.cpp
    dep/imgui/imstb_rectpack.h
    dep/imgui/imstb_textedit.h
    dep/imgui/imstb_truetype.h
    dep/imgui/backends/imgui_impl_glfw.cpp
    dep/imgui/backends/imgui_impl_glfw.h
    dep/imgui/backends/imgui_impl_opengl3.cpp
    dep/imgui/backends/imgui_impl_opengl3.h
    dep/imgui/backends/imgui_impl_opengl3_loader.h
    dep/imgui/misc/cpp/imgui_stdlib.cpp
    dep/imgui/misc/cpp/imgui_stdlib.h
)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
target_include_directories(imgui PUBLIC dep/imgui dep/imgui/backends dep/imgui/misc/cpp)
target_link_libraries(imgui PUBLIC unofficial::angle::libGLESv2 glfw)
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(SimpleGraphic SHARED ${SIMPLEGRAPHIC_SOURCES} ${SIMPLEGRAPHIC_PLATFORM_SOURCES})
target_compile_definitions(SimpleGraphic PRIVATE
    UNICODE
    _CRT_SECURE_NO_DEPRECATE
    _CRT_SECURE_NO_WARNINGS
    _SCL_SECURE_NO_DEPRECATE
    _SCL_SECURE_NO_WARNINGS
    GLFW_INCLUDE_NONE
    GLM_ENABLE_EXPERIMENTAL
    GL_SILENCE_DEPRECATION
    SIMPLEGRAPHIC_EXPORTS
)
if (UNIX AND NOT APPLE)
    target_compile_definitions(SimpleGraphic PRIVATE _POSIX_C_SOURCE=200809L)
endif ()
target_include_directories(SimpleGraphic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/dep/glad/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/dep/stb"
)
target_link_libraries(SimpleGraphic PRIVATE
    unofficial::angle::libEGL
    unofficial::angle::libGLESv2
    cmp_core
    fmt::fmt
    glfw
    gli
    glm::glm
    imgui
    LuaJIT::LuaJIT
    Microsoft.GSL::GSL
    re2::re2
    Threads::Threads
    WebP::webpdecoder
    ZLIB::ZLIB
    ${SIMPLEGRAPHIC_ZSTD_TARGET}
)
set_target_properties(SimpleGraphic PROPERTIES PREFIX "")

if (WIN32)
    target_compile_options(SimpleGraphic PRIVATE "/EHa")
    target_link_libraries(SimpleGraphic PRIVATE winmm)
endif ()

if (APPLE)
    find_library(CORE_FOUNDATION_LIBRARY CoreFoundation REQUIRED)
    find_library(APPLICATION_SERVICES_LIBRARY ApplicationServices REQUIRED)
    target_link_libraries(SimpleGraphic PRIVATE ${CORE_FOUNDATION_LIBRARY} ${APPLICATION_SERVICES_LIBRARY})
    # A staged executable must link the installed dylib through its own rpath,
    # never through CMake's build-directory install name.
    set_target_properties(SimpleGraphic PROPERTIES
        MACOSX_RPATH ON
        INSTALL_NAME_DIR "@rpath"
    )
endif ()

if (SIMPLEGRAPHIC_RUNTIME_RPATH)
    set_target_properties(SimpleGraphic PROPERTIES
        BUILD_RPATH "${SIMPLEGRAPHIC_RUNTIME_RPATH}"
        INSTALL_RPATH "${SIMPLEGRAPHIC_RUNTIME_RPATH}"
    )
endif ()

if (SIMPLEGRAPHIC_BUILD_HOST)
    add_executable(SimpleGraphicHost launcher/main.cpp)
    target_link_libraries(SimpleGraphicHost PRIVATE SimpleGraphic)
    target_compile_definitions(SimpleGraphicHost PRIVATE SIMPLEGRAPHIC_VERSION="${PROJECT_VERSION}")
    set_target_properties(SimpleGraphicHost PROPERTIES OUTPUT_NAME "PathOfBuilding-SimpleGraphic")
    if (SIMPLEGRAPHIC_RUNTIME_RPATH)
        set_target_properties(SimpleGraphicHost PROPERTIES
            BUILD_RPATH "${SIMPLEGRAPHIC_RUNTIME_RPATH}"
            INSTALL_RPATH "${SIMPLEGRAPHIC_RUNTIME_RPATH}"
        )
    endif ()
endif ()

# Lua extension modules. Build canonical Lua module paths: for example,
# require("socket.core") resolves lua/socket/core.[dll|so].
set(LCURL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/Lua-cURLv3")
file(GLOB_RECURSE LCURL_SOURCES CONFIGURE_DEPENDS "${LCURL_SOURCE_DIR}/src/*.c")
list(FILTER LCURL_SOURCES EXCLUDE REGEX "l52util\\.c$")
list(APPEND LCURL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compat/lcurl_l52util.c")
add_library(lcurl MODULE ${LCURL_SOURCES})
target_include_directories(lcurl PRIVATE "${LCURL_SOURCE_DIR}/src")
target_compile_definitions(lcurl PRIVATE LCURL_LUAJIT_HAS_LUA52_AUX=1)
target_link_libraries(lcurl PRIVATE CURL::libcurl LuaJIT::LuaJIT)

# Lua-cURL exposes a separate luaopen_lcurl_safe entry point.  Keeping it at
# lua/lcurl/safe makes require("lcurl.safe") use LuaJIT's canonical loader.
add_library(lcurl_safe MODULE ${LCURL_SOURCES})
target_include_directories(lcurl_safe PRIVATE "${LCURL_SOURCE_DIR}/src")
target_compile_definitions(lcurl_safe PRIVATE LCURL_LUAJIT_HAS_LUA52_AUX=1)
target_link_libraries(lcurl_safe PRIVATE CURL::libcurl LuaJIT::LuaJIT)
set_target_properties(lcurl_safe PROPERTIES OUTPUT_NAME "safe")

add_library(lua_utf8 MODULE libs/luautf8/lutf8lib.c)
target_link_libraries(lua_utf8 PRIVATE LuaJIT::LuaJIT)
set_target_properties(lua_utf8 PROPERTIES OUTPUT_NAME "lua-utf8")
if (WIN32)
    # lutf8lib defines luaopen_utf8 itself.  Do not mark it as a LuaJIT DLL
    # import; ask CMake to emit the module export table instead.
    set_target_properties(lua_utf8 PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

set(LUASOCKET_SOURCES
    libs/luasocket/src/auxiliar.c
    libs/luasocket/src/buffer.c
    libs/luasocket/src/compat.c
    libs/luasocket/src/except.c
    libs/luasocket/src/inet.c
    libs/luasocket/src/io.c
    libs/luasocket/src/luasocket.c
    libs/luasocket/src/options.c
    libs/luasocket/src/select.c
    libs/luasocket/src/tcp.c
    libs/luasocket/src/timeout.c
    libs/luasocket/src/udp.c
)
if (WIN32)
    list(APPEND LUASOCKET_SOURCES libs/luasocket/src/wsocket.c)
else ()
    list(APPEND LUASOCKET_SOURCES libs/luasocket/src/usocket.c)
endif ()
add_library(luasocket MODULE ${LUASOCKET_SOURCES})
target_include_directories(luasocket PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/luasocket/src")
target_link_libraries(luasocket PRIVATE LuaJIT::LuaJIT)
if (WIN32)
    target_link_libraries(luasocket PRIVATE wsock32 ws2_32)
endif ()
# socket.core and mime.core share their filename by Lua convention. Keep
# their build outputs in the same hierarchy used by the installed payload so
# single-config generators never emit two rules for one core.so/core.dll.
set_target_properties(luasocket PROPERTIES
    OUTPUT_NAME "core"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua/socket"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua/socket"
)

add_library(luasocket_mime MODULE
    libs/luasocket/src/mime.c
    libs/luasocket/src/compat.c
)
target_include_directories(luasocket_mime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/luasocket/src")
target_link_libraries(luasocket_mime PRIVATE LuaJIT::LuaJIT)
set_target_properties(luasocket_mime PROPERTIES
    OUTPUT_NAME "core"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua/mime"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua/mime"
)

set(SIMPLEGRAPHIC_LUA_NESTED_MODULE_TARGETS luasocket luasocket_mime)
if (UNIX)
    add_library(luasocket_unix MODULE
        libs/luasocket/src/auxiliar.c
        libs/luasocket/src/buffer.c
        libs/luasocket/src/compat.c
        libs/luasocket/src/io.c
        libs/luasocket/src/options.c
        libs/luasocket/src/timeout.c
        libs/luasocket/src/unix.c
        libs/luasocket/src/unixdgram.c
        libs/luasocket/src/unixstream.c
        libs/luasocket/src/usocket.c
    )
    target_include_directories(luasocket_unix PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/luasocket/src")
    target_link_libraries(luasocket_unix PRIVATE LuaJIT::LuaJIT)
    set_target_properties(luasocket_unix PROPERTIES
        OUTPUT_NAME "unix"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua/socket"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua/socket"
    )
    if (APPLE)
        target_compile_definitions(luasocket_unix PRIVATE UNIX_HAS_SUN_LEN)
    endif ()
    list(APPEND SIMPLEGRAPHIC_LUA_NESTED_MODULE_TARGETS luasocket_unix)
endif ()

add_library(lzip MODULE libs/LZip/lzip.cpp)
target_link_libraries(lzip PRIVATE LuaJIT::LuaJIT ZLIB::ZLIB)
if (UNIX AND NOT APPLE)
    target_compile_definitions(lzip PRIVATE _FILE_OFFSET_BITS=64 _POSIX_C_SOURCE=200809L)
elseif (UNIX)
    target_compile_definitions(lzip PRIVATE _FILE_OFFSET_BITS=64)
endif ()

set(SIMPLEGRAPHIC_LUA_ROOT_MODULE_TARGETS lcurl lua_utf8 lzip)
set(SIMPLEGRAPHIC_LUA_LCURL_MODULE_TARGETS lcurl_safe)
set(SIMPLEGRAPHIC_RUNTIME_TARGETS SimpleGraphic)
if (SIMPLEGRAPHIC_BUILD_HOST)
    list(APPEND SIMPLEGRAPHIC_RUNTIME_TARGETS SimpleGraphicHost)
endif ()
foreach(runtime_target IN LISTS SIMPLEGRAPHIC_RUNTIME_TARGETS)
    set_target_properties(${runtime_target} PROPERTIES PREFIX "")
    if (SIMPLEGRAPHIC_RUNTIME_RPATH)
        set_target_properties(${runtime_target} PROPERTIES
            BUILD_RPATH "${SIMPLEGRAPHIC_RUNTIME_RPATH}"
            INSTALL_RPATH "${SIMPLEGRAPHIC_RUNTIME_RPATH}"
        )
    endif ()
endforeach ()

foreach(lua_module IN LISTS SIMPLEGRAPHIC_LUA_ROOT_MODULE_TARGETS)
    set_target_properties(${lua_module} PROPERTIES PREFIX "")
    if (SIMPLEGRAPHIC_LUA_RPATH)
        set_target_properties(${lua_module} PROPERTIES
            BUILD_RPATH "${SIMPLEGRAPHIC_LUA_RPATH}"
            INSTALL_RPATH "${SIMPLEGRAPHIC_LUA_RPATH}"
        )
    endif ()
endforeach ()

foreach(lua_module IN LISTS SIMPLEGRAPHIC_LUA_LCURL_MODULE_TARGETS)
    set_target_properties(${lua_module} PROPERTIES PREFIX "")
    if (SIMPLEGRAPHIC_NESTED_LUA_RPATH)
        set_target_properties(${lua_module} PROPERTIES
            BUILD_RPATH "${SIMPLEGRAPHIC_NESTED_LUA_RPATH}"
            INSTALL_RPATH "${SIMPLEGRAPHIC_NESTED_LUA_RPATH}"
        )
    endif ()
endforeach ()

foreach(lua_module IN LISTS SIMPLEGRAPHIC_LUA_NESTED_MODULE_TARGETS)
    set_target_properties(${lua_module} PROPERTIES PREFIX "")
    if (SIMPLEGRAPHIC_NESTED_LUA_RPATH)
        set_target_properties(${lua_module} PROPERTIES
            BUILD_RPATH "${SIMPLEGRAPHIC_NESTED_LUA_RPATH}"
            INSTALL_RPATH "${SIMPLEGRAPHIC_NESTED_LUA_RPATH}"
        )
    endif ()
endforeach ()

# LuaJIT's native package path on Unix uses ?.so, including on macOS.  The
# payload is still a Mach-O shared library; only its loader-facing suffix is
# conventional rather than CMake's default .dylib.
if (APPLE)
    foreach(lua_module IN LISTS SIMPLEGRAPHIC_LUA_ROOT_MODULE_TARGETS SIMPLEGRAPHIC_LUA_LCURL_MODULE_TARGETS SIMPLEGRAPHIC_LUA_NESTED_MODULE_TARGETS)
        set_target_properties(${lua_module} PROPERTIES SUFFIX ".so")
    endforeach ()
endif ()

install(TARGETS ${SIMPLEGRAPHIC_RUNTIME_TARGETS}
    RUNTIME_DEPENDENCY_SET simplegraphic_runtime_dependencies
    RUNTIME DESTINATION "."
    LIBRARY DESTINATION "."
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(TARGETS ${SIMPLEGRAPHIC_LUA_ROOT_MODULE_TARGETS}
    RUNTIME_DEPENDENCY_SET simplegraphic_runtime_dependencies
    RUNTIME DESTINATION "lua"
    LIBRARY DESTINATION "lua"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(TARGETS ${SIMPLEGRAPHIC_LUA_LCURL_MODULE_TARGETS}
    RUNTIME_DEPENDENCY_SET simplegraphic_runtime_dependencies
    RUNTIME DESTINATION "lua/lcurl"
    LIBRARY DESTINATION "lua/lcurl"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(TARGETS luasocket
    RUNTIME_DEPENDENCY_SET simplegraphic_runtime_dependencies
    RUNTIME DESTINATION "lua/socket"
    LIBRARY DESTINATION "lua/socket"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(TARGETS luasocket_mime
    RUNTIME_DEPENDENCY_SET simplegraphic_runtime_dependencies
    RUNTIME DESTINATION "lua/mime"
    LIBRARY DESTINATION "lua/mime"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
if (TARGET luasocket_unix)
    install(TARGETS luasocket_unix
        RUNTIME_DEPENDENCY_SET simplegraphic_runtime_dependencies
        RUNTIME DESTINATION "lua/socket"
        LIBRARY DESTINATION "lua/socket"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif ()

install(FILES
    libs/luasocket/src/ltn12.lua
    libs/luasocket/src/mime.lua
    libs/luasocket/src/socket.lua
    DESTINATION "lua"
)
install(FILES libs/Lua-cURLv3/src/lua/cURL.lua DESTINATION "lua")
install(DIRECTORY "${LCURL_SOURCE_DIR}/src/lua/cURL"
    DESTINATION "lua"
    FILES_MATCHING PATTERN "*.lua"
)
install(FILES
    libs/luasocket/src/ftp.lua
    libs/luasocket/src/headers.lua
    libs/luasocket/src/http.lua
    libs/luasocket/src/mbox.lua
    libs/luasocket/src/smtp.lua
    libs/luasocket/src/tp.lua
    libs/luasocket/src/url.lua
    DESTINATION "lua/socket"
)

set(SIMPLEGRAPHIC_RUNTIME_SEARCH_DIRS)
set(SIMPLEGRAPHIC_RUNTIME_PRE_EXCLUDE_REGEXES
    "^(api-ms-win|ext-ms-win)-.*"
)
if (DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND SIMPLEGRAPHIC_RUNTIME_SEARCH_DIRS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
    )
endif ()
if (WIN32 AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    # Visual Studio copies this one dependency beside every Lua module. CMake
    # then sees several equally named candidates while collecting runtime
    # dependencies. Stage the canonical vcpkg DLL once instead.
    list(APPEND SIMPLEGRAPHIC_RUNTIME_PRE_EXCLUDE_REGEXES "^lua51\\.dll$")
    install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/lua51.dll" DESTINATION ".")
endif ()
install(RUNTIME_DEPENDENCY_SET simplegraphic_runtime_dependencies
    DESTINATION "."
    DIRECTORIES ${SIMPLEGRAPHIC_RUNTIME_SEARCH_DIRS}
    PRE_EXCLUDE_REGEXES ${SIMPLEGRAPHIC_RUNTIME_PRE_EXCLUDE_REGEXES}
    POST_EXCLUDE_REGEXES
        "^/System/Library/.*"
        "^/usr/lib/.*"
        "^/lib/.*"
        ".*/ld-linux[^/]*\\.so.*"
        ".*/lib(c|m|dl|pthread|rt|stdc\\+\\+|gcc_s)\\.so.*"
)
install(FILES README.md LICENSE DESTINATION ".")

if (BUILD_TESTING AND SIMPLEGRAPHIC_BUILD_TESTS AND SIMPLEGRAPHIC_BUILD_HOST)
    add_test(NAME simplegraphic-host-version COMMAND SimpleGraphicHost --version)
    set_tests_properties(simplegraphic-host-version PROPERTIES LABELS "smoke")
endif ()
