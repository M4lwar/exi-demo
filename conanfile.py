from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class ExiDemoConan(ConanFile):
    """Consumer recipe for exi-demo.

    Builds the demo with CMake against the prebuilt `exificient` Conan package.
    Restore that package into your local cache first (see README), then:

        conan install .
        conan build .
    """

    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeToolchain", "CMakeDeps", "VirtualRunEnv"

    def requirements(self):
        self.requires("exificient/0.3.0-rc")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
