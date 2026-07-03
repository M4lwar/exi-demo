from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class ExiDemoConan(ConanFile):
    """Consumer recipe for exi-demo.

    Builds the demo with CMake against the prebuilt `exificient` Conan package.
    Restore that package into your local cache first (see README), then:

        conan install .
        conan build .

    A schema-baked build of the same package/version is selected per-install
    with the `baked_schema` option (no recipe pin change needed):

        conan install . -o "exificient/*:baked_schema=uci-2.5.0"
    """

    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeToolchain", "CMakeDeps", "VirtualRunEnv"

    def requirements(self):
        self.requires("exificient/1.0.0")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
