cmake_minimum_required(VERSION 3.27)

project(hyprphysics
    DESCRIPTION "Gravity, bouncing, and throwable windows for Hyprland"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SRC "src/*.cpp")

add_library(hyprphysics SHARED ${SRC})

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
)
target_link_libraries(hyprphysics PRIVATE rt PkgConfig::deps)

install(TARGETS hyprphysics)
