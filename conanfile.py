from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps


class CompressorRecipe(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"

    def requirements(self):
        if self.settings.os == "Windows":
            self.requires("ffmpeg/8.0.1")
        self.requires("spdlog/1.15.1")
        self.requires("zlib/1.3.1")
        self.requires("yaml-cpp/0.8.0")
        self.requires("argparse/2.9")
        self.requires("cpp-httplib/0.30.1", options={"with_openssl": True})
        self.requires("nlohmann_json/3.11.3")

    def build_requirements(self):
        self.tool_requires("cmake/3.27.9")

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
