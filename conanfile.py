import os

from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout
from conan.tools.files import copy
from conan.tools.microsoft import is_msvc


class ExiDemoConan(ConanFile):
    """Consumer recipe for exi-demo.

    Builds the demo with CMake against the prebuilt `exificient` Conan package.
    Restore that package into your local cache first (see README), then:

        conan install .
        conan build .

    The exificient shared library is copied next to the built executable, so
    it runs straight from the build tree -- no conanrun.sh/.bat step.

    A schema-baked build of the same package/version is selected per-install
    with the `baked_schema` option (no recipe pin change needed):

        conan install . -o "exificient/*:baked_schema=uci-2.5.0"
    """

    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeToolchain", "CMakeDeps"

    def requirements(self):
        self.requires("exificient/1.0.1")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        # Place each dependency's shared libraries next to the demo
        # executable. Windows finds DLLs in the exe's own directory; on
        # Linux the exe searches $ORIGIN via BUILD_RPATH (see CMakeLists).
        # MSVC is a multi-config generator, so the exe lands in a per-config
        # subdirectory of the build folder; single-config generators put it
        # in the build folder itself.
        exe_dir = self.build_folder
        if is_msvc(self):
            exe_dir = os.path.join(self.build_folder, str(self.settings.build_type))
        for dep in self.dependencies.values():
            for bindir in dep.cpp_info.bindirs:
                copy(self, "*.dll", bindir, exe_dir)
            for libdir in dep.cpp_info.libdirs:
                copy(self, "*.so*", libdir, exe_dir)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
