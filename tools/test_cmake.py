# Copyright (c) 2026 WD Studios LLC.
# SPDX-License-Identifier: MIT
"""Configure-only tests of CMakeLists.txt and cmake/ApertureUISamples.cmake against a fake SDK package.

Nothing is compiled. On Windows cmake picks the Visual Studio generator, which makes each configure slow, so
there the tests run only with APUI_TEST_CMAKE=1 (APUI_TEST_CMAKE_GENERATOR picks another generator).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CMAKE = shutil.which("cmake")
ENABLED = bool(CMAKE) and (sys.platform != "win32" or os.environ.get("APUI_TEST_CMAKE") == "1")

SDK_CONFIG = """\
foreach(t HTMLEngine HarrlowEngine DeveloperEngine Network Foundation)
  add_library(ApertureUI::${t} INTERFACE IMPORTED)
  set_target_properties(ApertureUI::${t} PROPERTIES INTERFACE_COMPILE_DEFINITIONS "%s;UNICODE")
endforeach()
"""

SAMPLE = """\
add_executable(stub main.cpp)
apui_sample_link(stub %s)
apui_sample_target(stub)
apui_sample_stage(stub DATA "${CMAKE_CURRENT_LIST_DIR}/assets/")
get_target_property(links stub LINK_LIBRARIES)
get_target_property(deps stub MANUALLY_ADDED_DEPENDENCIES)
message(STATUS "TEST runtime=[${CMAKE_MSVC_RUNTIME_LIBRARY}] root=[${APUI_SDK_ROOT}] in_tree=[${APUI_SAMPLES_IN_TREE}] links=[${links}] deps=[${deps}]")
"""

def write(root, rel, text=""):
    p = Path(root) / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8", newline="\n")
    return p

def make_sdk(root, name, definitions="BUILDSYSTEM_BUILDTYPE_Shipping"):
    sdk = Path(root) / name
    write(sdk, "cmake/ApertureUIConfig.cmake", SDK_CONFIG % definitions)
    # The probe names are unique: a name such as "a.pdb" also matches the end of a try-compile file's random name.
    for rel in ("bin/apui_stage_probe.dll", "bin/apui_stage_probe.pdb", "bin/resources/snapshot.bin", "shaders/s.hlsl",
                "data/d.txt"):
        write(sdk, rel, "x")
    return sdk

def make_samples(root, sample=SAMPLE % ""):
    """A copy of the repository's CMake files with stub common/ and minimal/ folders."""
    src = Path(root) / "samples"
    src.mkdir(exist_ok=True)
    shutil.copy(REPO / "CMakeLists.txt", src / "CMakeLists.txt")
    shutil.copytree(REPO / "cmake", src / "cmake", dirs_exist_ok=True)
    write(src, "common/CMakeLists.txt", "# stub\n")
    write(src, "minimal/CMakeLists.txt", sample)
    write(src, "minimal/main.cpp", "int main() { return 0; }\n")
    write(src, "minimal/assets/a.txt", "hi\n")
    return src

def configure(src, build, sdk=None, *options):
    cmd = [CMAKE, "-S", str(src), "-B", str(build), "-DAPUI_SAMPLES_APPLAYER=OFF", "-DAPUI_SAMPLES_CITYSLICE=OFF"]
    if sdk is not None:
        cmd.append(f"-DApertureUI_DIR={Path(sdk, 'cmake').as_posix()}")
    if os.environ.get("APUI_TEST_CMAKE_GENERATOR"):
        cmd += ["-G", os.environ["APUI_TEST_CMAKE_GENERATOR"]]
    return subprocess.run(cmd + list(options), capture_output=True, text=True)

def flat(text):
    """CMake wraps long error messages; compare them with the line breaks collapsed."""
    return " ".join(text.split())

def info(result):
    return dict(re.findall(r"(\w+)=\[(.*?)\]", "\n".join(l for l in result.stdout.splitlines() if "TEST " in l)))

def generated_text(build):
    """Everything configure wrote, except the cache and the try-compile scratch folder, as one string.

    CMakeFiles/CMakeScratch holds the compiler probes, whose random names (cmTC_1a2ba.pdb) are not ours to assert on.
    """
    parts = []
    for dirpath, dirnames, files in os.walk(build):
        if os.path.basename(dirpath) == "CMakeFiles":
            dirnames[:] = [d for d in dirnames if d != "CMakeScratch"]
        for f in files:
            if f != "CMakeCache.txt" and os.path.getsize(os.path.join(dirpath, f)) < 4_000_000:
                parts.append(Path(dirpath, f).read_text(encoding="utf-8", errors="replace"))
    return "\n".join(parts)

class GeneratedTextTest(unittest.TestCase):
    def test_the_try_compile_scratch_folder_is_not_read_but_the_rest_of_cmakefiles_is(self):
        with tempfile.TemporaryDirectory() as d:
            write(d, "CMakeFiles/CMakeScratch/TryCompile-x/cmTC_1a2ba.pdb.txt", "scratch-only")
            write(d, "CMakeFiles/stub.dir/build.make", "stub-rule")
            write(d, "stub.vcxproj", "project")
            write(d, "CMakeCache.txt", "cache-only")
            write(d, "sub/CMakeFiles/CMakeScratch/x.txt", "nested-scratch")
            text = generated_text(d)
            for wanted in ("stub-rule", "project"):
                self.assertIn(wanted, text)
            for unwanted in ("scratch-only", "cache-only", "nested-scratch"):
                self.assertNotIn(unwanted, text)

@unittest.skipUnless(ENABLED, "cmake is needed (on Windows also APUI_TEST_CMAKE=1)")
class StandaloneConfigureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.build = self.root / "build"

    def ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_the_same_build_folder_can_be_configured_again_and_again(self):
        src, sdk = make_samples(self.root), make_sdk(self.root, "sdk")
        for _ in range(3):  # CMAKE_PROJECT_NAME is cached, so the second configure used to lose its project()
            self.ok(configure(src, self.build, sdk))

    def test_staging_follows_the_sdk_the_build_folder_is_switched_to(self):
        src = make_samples(self.root)
        sdk_a, sdk_b = make_sdk(self.root, "sdk_a"), make_sdk(self.root, "sdk_b")
        self.assertEqual(info(self.ok(configure(src, self.build, sdk_a)))["root"], sdk_a.as_posix())
        self.assertEqual(info(self.ok(configure(src, self.build, sdk_b)))["root"], sdk_b.as_posix())
        text = generated_text(self.build)
        self.assertIn("sdk_b", text)
        self.assertNotIn("sdk_a", text)

    def test_the_cpp_runtime_follows_the_package_build_type(self):
        src = make_samples(self.root)
        for build_type, runtime in (("Shipping", "MultiThreadedDLL"), ("Dev", "MultiThreadedDLL"),
                                    ("Release", "MultiThreadedDLL"), ("Debug", "MultiThreadedDebugDLL")):
            sdk = make_sdk(self.root, f"sdk_{build_type}", f"BUILDSYSTEM_BUILDTYPE_{build_type}")
            r = self.ok(configure(src, self.root / f"build_{build_type}", sdk))
            self.assertEqual(info(r)["runtime"], runtime, build_type)

    def test_a_data_folder_with_a_trailing_slash_is_staged_under_its_name(self):
        src, sdk = make_samples(self.root), make_sdk(self.root, "sdk")
        self.ok(configure(src, self.build, sdk))
        self.assertRegex(generated_text(self.build),
                         r'copy_directory_if_different\s+"?[^\s"]*[/\\]assets"?\s+"?[^\s"]*[/\\]assets"?(?=[\s"]|$)')

    def test_a_missing_data_folder_is_an_error(self):
        src = make_samples(self.root, SAMPLE.replace("assets/", "nope/") % "")
        r = configure(src, self.build, make_sdk(self.root, "sdk"))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("does not exist", flat(r.stderr))

    def test_pdbs_are_staged_only_on_request(self):
        src, sdk = make_samples(self.root), make_sdk(self.root, "sdk")
        self.ok(configure(src, self.root / "plain", sdk))
        self.assertIn("apui_stage_probe.dll", generated_text(self.root / "plain"))
        self.assertNotIn("apui_stage_probe.pdb", generated_text(self.root / "plain"))
        self.ok(configure(src, self.root / "pdbs", sdk, "-DAPUI_SAMPLES_STAGE_PDBS=ON"))
        self.assertIn("apui_stage_probe.pdb", generated_text(self.root / "pdbs"))

    def test_a_package_that_lacks_a_folder_fails_clearly(self):
        src = make_samples(self.root)
        for missing in ("bin", "bin/resources", "shaders", "data"):
            sdk = make_sdk(self.root, "sdk_" + missing.replace("/", "_"))
            shutil.rmtree(sdk / missing)
            r = configure(src, self.root / ("b_" + missing.replace("/", "_")), sdk)
            self.assertNotEqual(r.returncode, 0, missing)
            self.assertIn(f"has no '{missing}' folder", flat(r.stderr))

    def test_a_package_without_a_build_type_fails_clearly(self):
        src, sdk = make_samples(self.root), make_sdk(self.root, "sdk", "UNICODE")
        r = configure(src, self.build, sdk)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("BUILDSYSTEM_BUILDTYPE_", flat(r.stderr))

    def test_without_applayer_sdl2_is_not_downloaded(self):
        src, sdk = make_samples(self.root), make_sdk(self.root, "sdk")
        shutil.rmtree(src / "common")
        shutil.copytree(REPO / "common", src / "common")  # the real one, not the stub
        self.ok(configure(src, self.build, sdk, "-DAPUI_SAMPLES_MINIMAL=OFF"))
        self.assertFalse((self.build / "_deps" / "sdl2-src").exists())
        text = generated_text(self.build)
        self.assertIn("apui_sample_common", text)
        self.assertNotIn("apui_sample_sdl2_input", text)

    def test_minimal_samples_configure_as_executable_targets(self):
        src, sdk = make_samples(self.root), make_sdk(self.root, "sdk")
        shutil.rmtree(src / "minimal")
        shutil.copytree(REPO / "minimal", src / "minimal")
        write(src, "common/CMakeLists.txt",
              "add_library(apui_sample_common INTERFACE)\n"
              "add_library(apui_sample_sdl2_input INTERFACE)\n")
        self.ok(configure(src, self.build, sdk))
        text = generated_text(self.build)
        for target in ("minimal_hello_view", "minimal_data_binding", "minimal_input"):
            self.assertIn(target, text)

    def test_no_sdk_says_where_to_get_it(self):
        r = configure(make_samples(self.root), self.build)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("-DApertureUI_DIR=<extracted folder>/cmake", flat(r.stderr))

FAKE_ENGINE = """\
cmake_minimum_required(VERSION 3.26)
project(FakeEngine LANGUAGES CXX)
function(ns_set_default_target_output_dirs t)
  set_property(GLOBAL APPEND PROPERTY OUTPUT_DIRS_SET_FOR ${t})
endfunction()
foreach(t apertureuihtmlengine apertureuiharrlowengine apertureuideveloperengine apertureuinetwork)
  add_library(${t} SHARED e.cpp)
endforeach()
add_subdirectory(samples)
# Defined after the samples, as in the engine: Foundation follows the Applications folder.
add_library(Foundation SHARED e.cpp)
%s
get_property(dirs GLOBAL PROPERTY OUTPUT_DIRS_SET_FOR)
message(STATUS "TEST output_dirs=[${dirs}]")
# Scheduled after the samples' own deferred call, so it sees what that call added.
function(print_deps)
  get_target_property(deps stub MANUALLY_ADDED_DEPENDENCIES)
  message(STATUS "TEST final_deps=[${deps}]")
endfunction()
cmake_language(DEFER CALL print_deps)
"""

@unittest.skipUnless(ENABLED, "cmake is needed (on Windows also APUI_TEST_CMAKE=1)")
class InTreeConfigureTest(unittest.TestCase):
    def configure_in_tree(self, d, engine_extra):
        make_samples(d, SAMPLE % "VULKAN_SHADERS")
        write(d, "CMakeLists.txt", FAKE_ENGINE % engine_extra)
        write(d, "e.cpp", "void e() {}\n")
        r = subprocess.run([CMAKE, "-S", d, "-B", str(Path(d) / "build")] +
                           (["-G", os.environ["APUI_TEST_CMAKE_GENERATOR"]] if os.environ.get("APUI_TEST_CMAKE_GENERATOR") else []) +
                           ["-DAPUI_SAMPLES_APPLAYER=OFF", "-DAPUI_SAMPLES_CITYSLICE=OFF"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    def test_in_tree_links_core_targets_defined_later_and_names_the_shader_target(self):
        with tempfile.TemporaryDirectory() as d:
            r = self.configure_in_tree(d, "add_custom_target(apertureui_vulkan_shaders)")
            got = info(r)
            self.assertEqual(got["in_tree"], "ON")
            self.assertEqual(got["links"].split(";")[:5],
                             ["apertureuihtmlengine", "apertureuiharrlowengine", "Foundation", "apertureuideveloperengine", "apertureuinetwork"])
            self.assertEqual(got["final_deps"], "apertureui_vulkan_shaders")
            self.assertEqual(got["output_dirs"], "stub")
            self.assertNotIn("apertureui_vulkan_shaders", flat(r.stderr))
            text = generated_text(Path(d) / "build")
            self.assertNotIn("/Shaders", text.replace("\\", "/"))  # in-tree only the sample data is copied
            self.assertRegex(text, r"copy_directory_if_different")

    def test_in_tree_without_the_shader_target_warns_instead_of_failing(self):
        with tempfile.TemporaryDirectory() as d:  # the engine defines the target only when slangc is installed
            r = self.configure_in_tree(d, "")
            self.assertEqual(info(r)["final_deps"], "deps-NOTFOUND")
            self.assertIn("CMake Warning", r.stderr)
            self.assertIn("no apertureui_vulkan_shaders target", flat(r.stderr))
            self.assertIn("for stub", flat(r.stderr))

if __name__ == "__main__":
    unittest.main()
