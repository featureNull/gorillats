from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy
import os


class GorillatsConan(ConanFile):
    name = "gorillats"
    version = "0.1.0"
    license = "MIT"
    author = "Asoss GmbH"
    url = "https://github.com/asoss/gorillats"
    homepage = "https://github.com/asoss/gorillats"
    description = (
        "Gorilla time series compression (Pelkonen et al., VLDB 2015): "
        "delta-of-delta timestamps + XOR float encoding."
    )
    topics = ("compression", "timeseries", "gorilla", "tsdb")

    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "include/*",
        "src/*",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.variables["GORILLATS_BUILD_TESTS"] = False
        tc.variables["GORILLATS_BUILD_PYTHON"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "gorillats")
        self.cpp_info.set_property("cmake_target_name", "gorillats::cpp")
        self.cpp_info.libs = ["gorillats_core"]
