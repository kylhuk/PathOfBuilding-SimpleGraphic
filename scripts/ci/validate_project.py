#!/usr/bin/env python3
"""Fast, dependency-free checks for the reproducible build contract."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(path: Path) -> str:
    if not path.is_file():
        raise RuntimeError(f"required file is missing: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


def main() -> int:
    try:
        cmake = require(ROOT / "CMakeLists.txt")
        presets = json.loads(require(ROOT / "CMakePresets.json"))
        configuration = json.loads(require(ROOT / "vcpkg-configuration.json"))
        build_runtime = require(ROOT / ".github/workflows/build-runtime.yml")
        runtime_verifier = require(ROOT / "scripts/ci/verify_runtime.py")
        require(ROOT / ".github/workflows/dev-build.yml")
        release = require(ROOT / ".github/workflows/release.yml")
        validate = require(ROOT / ".github/workflows/validate.yml")
        require(ROOT / ".github/dependabot.yml")
        require(ROOT / "scripts/ci/test_launcher_paths.py")
        if "test_launcher_paths.py" not in validate:
            raise RuntimeError("source validation does not exercise the standalone launcher")

        baseline = configuration["default-registry"]["baseline"]
        if not re.fullmatch(r"[0-9a-f]{40}", baseline):
            raise RuntimeError("vcpkg baseline must be an immutable 40-character commit")
        if "vcpkg-triplets" not in configuration.get("overlay-triplets", []):
            raise RuntimeError("vcpkg dynamic triplet overlay is not configured")
        if "RUNTIME_DEPENDENCY_SET simplegraphic_runtime_dependencies" not in cmake:
            raise RuntimeError("CMake does not stage transitive runtime dependencies")
        if "SIMPLEGRAPHIC_RUNTIME_DEPENDENCY_ARGUMENTS" not in cmake:
            raise RuntimeError("platform runtime dependency staging is not configured")
        if "if (UNIX AND NOT APPLE)\n    target_compile_definitions(SimpleGraphic PRIVATE _POSIX_C_SOURCE=200809L)" not in cmake:
            raise RuntimeError("Darwin must not receive Linux POSIX feature macros")
        if "MACOSX_RPATH ON" not in cmake or 'INSTALL_NAME_DIR "@rpath"' not in cmake:
            raise RuntimeError("macOS dylibs do not have a relocatable install name")
        if "GLM_ENABLE_EXPERIMENTAL" not in cmake:
            raise RuntimeError("CMake does not opt in to GLM extensions required by GLI")
        gli_portfile = require(ROOT / "vcpkg-ports/ports/gli/2021-07-06_3/portfile.cmake")
        gli_patch = require(ROOT / "vcpkg-ports/ports/gli/2021-07-06_3/qualify-make-vec4.patch")
        if "qualify-make-vec4.patch" not in gli_portfile or gli_patch.count("gli::make_vec4") != 4:
            raise RuntimeError("GLI is not patched for the current GLM make_vec4 API")
        if "#include <fmt/format.h>" not in require(ROOT / "engine/core/core_config.cpp"):
            raise RuntimeError("core config does not include fmt's formatting API")
        common = require(ROOT / "engine/common/common.cpp")
        if "VFormatString" not in common or "vasprintf" in common:
            raise RuntimeError("shared printf formatting is not portable")
        if any("vasprintf" in require(path) for path in (
            ROOT / "engine/common/console.cpp",
            ROOT / "engine/system/win/sys_main.cpp",
        )):
            raise RuntimeError("GNU-only vasprintf remains in a portable source path")
        system_main = require(ROOT / "engine/system/win/sys_main.cpp")
        if system_main.count("std::nullopt, std::string{") != 2:
            raise RuntimeError("user-path errors do not use portable optional construction")
        if 'Error("%s", threadError);' not in system_main:
            raise RuntimeError("thread errors are passed as an unsafe format string")
        if (
            "bool tokenStarted = false;" not in system_main
            or "if (tokenStarted) {\n\t\t\t\tresult.push_back" not in system_main
            or "if (tokenStarted) {\n\t\tresult.push_back" not in system_main
        ):
            raise RuntimeError("POSIX process launching does not preserve explicitly empty arguments")
        ui_main = require(ROOT / "ui_main.cpp")
        if (
            'root + "/?.lua;" + root + "/?/init.lua"' not in ui_main
            or "ConfigureLuaScriptSearchPath(L, scriptWorkDir);" not in ui_main
        ):
            raise RuntimeError("the selected script directory is not in Lua package.path")
        ui_api = require(ROOT / "ui_api.cpp")
        launch_subscript_start = ui_api.find("SG_LUA_CPP_FUN_BEGIN(LaunchSubScript)")
        launch_subscript_end = ui_api.find("SG_LUA_CPP_FUN_END()", launch_subscript_start)
        launch_subscript = ui_api[launch_subscript_start:launch_subscript_end]
        if (
            launch_subscript_start < 0
            or launch_subscript_end < 0
            or "const std::string error = subScript->StartError();" not in launch_subscript
            or 'ui->LExpect(L, false, "LaunchSubScript(): %s", error.c_str());' not in launch_subscript
            or "luaL_error" in launch_subscript
            or "ui->LAssert" in launch_subscript
        ):
            raise RuntimeError("LaunchSubScript can bypass C++ cleanup before reporting a Lua error")
        subscript = require(ROOT / "ui_subscript.cpp")
        if (
            "static void ssWipeCalls(ssCall_s* calls)" not in subscript
            or subscript.count("ssWipeCalls(calls);") < 2
            or "std::atomic_size_t luaMemoryKilobytes" not in subscript
            or "CacheScriptMemory(L);" not in subscript
            or "return luaMemoryKilobytes.load(std::memory_order_relaxed);" not in subscript
            or "return running ? 0" in subscript
        ):
            raise RuntimeError("subscript abort handling or active Lua memory accounting is incomplete")
        if "sys->Sleep(1);" not in require(ROOT / "engine/render/r_main.cpp"):
            raise RuntimeError("renderer does not use the platform-neutral sleep API")
        # LuaSocket intentionally has both socket.core and mime.core. Their
        # identical filenames need separate build directories, not only
        # separate install destinations, or Ninja rejects the generated graph.
        for output_dir in ("${CMAKE_CURRENT_BINARY_DIR}/lua/socket", "${CMAKE_CURRENT_BINARY_DIR}/lua/mime"):
            if output_dir not in cmake:
                raise RuntimeError("LuaSocket core modules do not have distinct build output directories")
        if "runtime_smoke.cpp" not in cmake or "SimpleGraphicRuntimeSmoke" not in require(ROOT / "runtime_smoke.cpp") or "--smoke-modules" not in require(ROOT / "launcher/main.cpp"):
            raise RuntimeError("staged runtime smoke coverage is missing")
        launcher = require(ROOT / "launcher/main.cpp")
        executable_path = require(ROOT / "engine/system/executable_path.h")
        if (
            "SimpleGraphicExecutablePath(std::error_code& error)" not in executable_path
            or "GetModuleFileNameW" not in executable_path
            or 'readlink("/proc/self/exe"' not in executable_path
            or "proc_pidpath" not in executable_path
            or "std::filesystem::weakly_canonical" not in executable_path
        ):
            raise RuntimeError("the shared executable-image resolver is incomplete")
        if '"engine/system/executable_path.h"' not in cmake:
            raise RuntimeError("the executable-image resolver is not tracked by CMake")
        if (
            "std::filesystem::absolute(" not in launcher
            or "std::vector<char*> runtimeArgs" not in launcher
            or "SimpleGraphicExecutablePath(error)" not in launcher
            or 'std::filesystem::absolute(std::filesystem::u8path(argv[0])' in launcher
        ):
            raise RuntimeError("standalone launcher does not preserve caller-relative script paths")
        if (
            "#define SIMPLEGRAPHIC_VERSION CFG_VERSION_NUM" not in launcher
            or 'SIMPLEGRAPHIC_VERSION="${PROJECT_VERSION}"' in cmake
        ):
            raise RuntimeError("standalone launcher does not report the complete SemVer")
        debug = require(ROOT / "ui_debug.cpp")
        if "lua_getstack(state, 1, &caller)" not in debug or "AddLineHit(call->lineHits, caller)" not in debug:
            raise RuntimeError("profiler hot-call lines are not attributed to callers")
        release_manifest = require(ROOT / "scripts/ci/release_manifest.py")
        if "filename = archive.name" not in release_manifest:
            raise RuntimeError("release metadata does not name flattened release assets")
        check_release = require(ROOT / "scripts/ci/check_release.py")
        strict_semver_fragments = (
            r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)",
            r"(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)",
            r"(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*",
            r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?",
        )
        if (
            "numeric_version = args.version.split" not in check_release
            or "SEMVER_PATTERN = re.compile(" not in check_release
            or "SEMVER_PATTERN.fullmatch(args.version)" not in check_release
            or any(fragment not in check_release for fragment in strict_semver_fragments)
            # A nonnumeric prerelease identifier must contain a letter or
            # hyphen; otherwise values such as "01" could bypass the numeric
            # leading-zero rule. The + quantifiers reject empty dot segments.
            or r"[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*" not in check_release
            or r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?" not in check_release
            or r"(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?" in check_release
        ):
            raise RuntimeError("release metadata does not enforce strict SemVer prerelease and build identifiers")
        runtime_dependency_start = cmake.index(
            "install(RUNTIME_DEPENDENCY_SET simplegraphic_runtime_dependencies"
        )
        runtime_dependency_end = cmake.index(
            "install(FILES README.md LICENSE", runtime_dependency_start
        )
        runtime_dependency_install = cmake[runtime_dependency_start:runtime_dependency_end]
        if 'LIBRARY DESTINATION "."' not in runtime_dependency_install or 'RUNTIME DESTINATION "."' not in runtime_dependency_install:
            raise RuntimeError("runtime dependency install must declare library and runtime destinations")
        if runtime_dependency_install.index('LIBRARY DESTINATION "."') < runtime_dependency_install.index("POST_EXCLUDE_REGEXES"):
            raise RuntimeError("runtime dependency filters must precede artifact destinations")
        if 'install(DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/"' not in cmake:
            raise RuntimeError("Windows runtime does not stage the vcpkg DLL closure")
        if build_runtime.count("if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }") < 6:
            raise RuntimeError("Windows native command failures are not propagated")
        if "actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c" not in build_runtime or "gh run download" in build_runtime:
            raise RuntimeError("Windows 10 package smoke tests must use the pinned artifact downloader")
        luajit_current = ROOT / "vcpkg-ports/ports/luajit/2026-07-20_2"
        luajit_configure = require(luajit_current / "configure")
        if "'LJ_TARGET_ARM 1'" not in luajit_configure or "'LJ_TARGET_X86 1'" not in luajit_configure:
            raise RuntimeError("LuaJIT's 32-bit manual buildvm architecture tokens are incomplete")
        if '"-DCMAKE_INSTALL_PREFIX=$stage"' not in build_runtime:
            raise RuntimeError("Windows runtime staging does not expand its CMake install prefix")
        try:
            x86_container_script = build_runtime.split("linux32 bash -lc '\n", 1)[1].split("\n            '\n", 1)[0]
        except IndexError as error:
            raise RuntimeError("Linux x86 container command is not a single quoted script") from error
        if "'" in x86_container_script:
            raise RuntimeError("Linux x86 container script contains an unsafe single quote")
        if "cmake==3.31.1" not in x86_container_script or "--only-binary=:all:" not in x86_container_script:
            raise RuntimeError("Linux x86 does not provision its pinned binary CMake")
        if 'grep -q "Class:.*ELF32"' not in x86_container_script:
            raise RuntimeError("Linux x86 build does not assert a 32-bit staged payload")
        if (
            "deployment_target: '10.15'" not in build_runtime
            or "deployment_target: '11.0'" not in build_runtime
            or "export MACOSX_DEPLOYMENT_TARGET" not in build_runtime
            or "-DCMAKE_OSX_DEPLOYMENT_TARGET" not in build_runtime
            or "--macos-deployment-target" not in build_runtime
        ):
            raise RuntimeError("macOS deployment target is not propagated through CI")
        if (
            "def staged_native_binaries" not in runtime_verifier
            or "*staged_native_binaries(runtime, args.platform)" not in runtime_verifier
            or "exceeding the macOS" not in runtime_verifier
        ):
            raise RuntimeError("runtime verification does not inspect the staged dependency closure")
        luajit_port = require(luajit_current / "portfile.cmake")
        if (
            "MACOSX_DEPLOYMENT_TARGET=" not in luajit_port
            or "$ENV{MACOSX_DEPLOYMENT_TARGET}" not in luajit_port
            or "MACOSX_DEPLOYMENT_TARGET for a Darwin build" not in luajit_port
        ):
            raise RuntimeError("LuaJIT does not receive the macOS deployment target")
        if 'set(ENV{MACOSX_DEPLOYMENT_TARGET} "${CMAKE_OSX_DEPLOYMENT_TARGET}")' not in cmake:
            raise RuntimeError("CMake does not export its effective macOS deployment target before loading vcpkg")
        luajit_makefile = require(luajit_current / "configure")
        if (
            "LUAJIT_MACOSX_DEPLOYMENT_TARGET" not in luajit_makefile
            or "export MACOSX_DEPLOYMENT_TARGET" not in luajit_makefile
        ):
            raise RuntimeError("LuaJIT's generated Makefile does not export its deployment target")
        if (luajit_current / "003-do-not-set-macosx-deployment-target.patch").exists():
            raise RuntimeError("LuaJIT still suppresses missing macOS deployment-target errors")
        luajit_versions = json.loads(require(ROOT / "vcpkg-ports/versions/l-/luajit.json"))
        luajit_baselines = json.loads(require(ROOT / "vcpkg-ports/versions/baseline.json"))
        for baseline_name, packages in luajit_baselines.items():
            selected = packages.get("luajit")
            if selected is None:
                continue
            matches = [
                version for version in luajit_versions["versions"]
                if version["version-date"] == selected["baseline"]
                and version.get("port-version", 0) == selected.get("port-version", 0)
            ]
            if len(matches) != 1 or not matches[0]["path"].startswith("$/"):
                raise RuntimeError(f"LuaJIT registry baseline {baseline_name} is not immutable")
            manifest = json.loads(require(ROOT / "vcpkg-ports" / matches[0]["path"][2:] / "vcpkg.json"))
            if (
                manifest.get("version-date") != selected["baseline"]
                or manifest.get("port-version", 0) != selected.get("port-version", 0)
            ):
                raise RuntimeError(f"LuaJIT registry baseline {baseline_name} points to the wrong port revision")
        if "workflow_dispatch:" not in release or "release:" in release.split("on:", 1)[1].split("permissions:", 1)[0]:
            raise RuntimeError("release workflow must be manual-only")
        if (
            "prerelease: ${{ steps.release_request.outputs.prerelease }}" not in release
            or 'version_without_build="${RELEASE_VERSION%%+*}"' not in release
            or 'if [[ "$version_without_build" == *-* ]]; then' not in release
            or "IS_PRERELEASE: ${{ needs.validate_release.outputs.prerelease }}" not in release
            or "release_options+=(--prerelease)" not in release
            or '"${release_options[@]}"' not in release
        ):
            raise RuntimeError("release workflow does not mark SemVer prereleases as GitHub prereleases")
        if (ROOT / ".github/workflows/main.yml").exists():
            raise RuntimeError("legacy automatic release workflow remains")
        if "lukka/run-vcpkg" in "\n".join(
            path.read_text(encoding="utf-8") for path in (ROOT / ".github/workflows").glob("*.yml")
        ):
            raise RuntimeError("mutable run-vcpkg action remains")
        preset_by_name = {
            preset["name"]: preset for preset in presets["configurePresets"] if not preset.get("hidden")
        }
        names = set(preset_by_name)
        expected = {
            "windows-x86", "windows-x64", "windows-arm64",
            "macos-x64", "macos-arm64",
            "linux-x86", "linux-x64", "linux-arm64",
        }
        if names != expected:
            raise RuntimeError(f"CMake preset matrix is incomplete: expected {sorted(expected)}, got {sorted(names)}")
        x86_cache = preset_by_name["linux-x86"]["cacheVariables"]
        for flag in (
            "CMAKE_C_FLAGS",
            "CMAKE_CXX_FLAGS",
            "CMAKE_EXE_LINKER_FLAGS",
            "CMAKE_SHARED_LINKER_FLAGS",
            "CMAKE_MODULE_LINKER_FLAGS",
        ):
            if x86_cache.get(flag) != "-m32":
                raise RuntimeError(f"linux-x86 preset does not set {flag}=-m32")
        for name, architecture, deployment_target, triplet in (
            ("macos-x64", "x86_64", "10.15", "x64-osx-dynamic"),
            ("macos-arm64", "arm64", "11.0", "arm64-osx-dynamic"),
        ):
            preset = preset_by_name[name]
            cache = preset["cacheVariables"]
            if (
                cache.get("CMAKE_OSX_ARCHITECTURES") != architecture
                or cache.get("CMAKE_OSX_DEPLOYMENT_TARGET") != deployment_target
                or cache.get("VCPKG_TARGET_TRIPLET") != triplet
                or preset.get("environment", {}).get("MACOSX_DEPLOYMENT_TARGET") != deployment_target
            ):
                raise RuntimeError(f"{name} does not preserve its macOS architecture and deployment target")
            triplet_file = require(ROOT / "vcpkg-triplets" / f"{triplet}.cmake")
            if f'set(VCPKG_OSX_DEPLOYMENT_TARGET "{deployment_target}")' not in triplet_file:
                raise RuntimeError(f"{triplet} does not propagate its macOS deployment target to vcpkg")
        project_index = cmake.index("project(PathOfBuildingSimpleGraphic")
        if cmake.index("CMAKE_OSX_DEPLOYMENT_TARGET") > project_index:
            raise RuntimeError("CMake's direct macOS deployment-target default is set too late")
        preproject_cmake = cmake[:project_index]
        if (
            "CMAKE_HOST_SYSTEM_PROCESSOR MATCHES" in preproject_cmake
            or "CMAKE_APPLE_SILICON_PROCESSOR" not in preproject_cmake
            or "execute_process(" not in preproject_cmake
            or "COMMAND uname -m" not in preproject_cmake
        ):
            raise RuntimeError("CMake does not determine native macOS architecture before project()")
        print("source, presets, package staging, and manual-release contract are valid")
        return 0
    except (KeyError, RuntimeError, json.JSONDecodeError) as error:
        print(f"validation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
