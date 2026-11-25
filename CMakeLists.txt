# Build of Liquinix's Linux side (Qt 6 + layer-shell-qt; see docs/linux-roadmap.md and docs/linux-dev.md).
# Windows keeps using build.cmd: this file only builds the Linux liquinix-* programs.
#   cmake -B build -G Ninja && ninja -C build
#   tools/linux/nested-kwin.sh ./build/liquinix-dock
cmake_minimum_required(VERSION 3.22)
project(Liquinix LANGUAGES C CXX)  # C: the Wayland protocol glue the scanner generates

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "CMakeLists.txt builds the Linux side only. On Windows, use build.cmd.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui WaylandClient)
find_package(PlasmaWaylandProtocols REQUIRED)  # Plasma's window-management protocol (the Dock's "running" dots)
find_package(LayerShellQt REQUIRED)

# platform-free code (src/core) and the Linux platform backend (src/platform/linux): the same code Windows builds
# through build.cmd, with the other backend
add_library(liquinix-core STATIC
    src/core/curves.cpp
    src/core/dock_drawing.cpp
    src/core/dock_layout.cpp
    src/platform/linux/platform_linux.cpp)
target_include_directories(liquinix-core PUBLIC src)

# the painter over QPainter (drawing::Painter on Linux)
add_library(liquinix-painter-qt STATIC src/platform/linux/painter_qt.cpp)
target_link_libraries(liquinix-painter-qt PUBLIC liquinix-core Qt6::Gui)

# the Dock (#20): pinned apps from .desktop files, theme icons, magnification, running dots; the layout is the Windows one
add_executable(liquinix-dock src/linux/dock/main.cpp src/linux/dock/apps.cpp src/linux/dock/window_list.cpp src/linux/dock/window_list.h)
set_target_properties(liquinix-dock PROPERTIES AUTOMOC ON)
qt6_generate_wayland_protocol_client_sources(liquinix-dock FILES ${PLASMA_WAYLAND_PROTOCOLS_DIR}/plasma-window-management.xml)
target_link_libraries(liquinix-dock PRIVATE liquinix-core liquinix-painter-qt Qt6::Core Qt6::Gui Qt6::WaylandClient LayerShellQt::Interface)
