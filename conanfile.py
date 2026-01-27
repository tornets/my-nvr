from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps


class CompressorRecipe(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"

    def requirements(self):
        self.requires("ffmpeg/8.0.1")
        self.requires("spdlog/1.17.0")
        self.requires("zlib/1.3.1")

    def build_requirements(self):
        self.tool_requires("gsoap/2.8.139")
        self.tool_requires("cmake/3.27.9")

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()