# Build do lado Linux do Liquinix (Qt 6 + layer-shell-qt; ver docs/linux-roadmap.md e docs/linux-dev.md).
# O Windows continua no build.cmd: este arquivo só monta os programas liquinix-* do Linux.
#   cmake -B build -G Ninja && ninja -C build
#   tools/linux/nested-kwin.sh ./build/liquinix-dock
cmake_minimum_required(VERSION 3.22)
project(Liquinix LANGUAGES CXX)

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

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui)
find_package(LayerShellQt REQUIRED)

# código sem nada de plataforma (src/nucleo) e o backend Linux da plataforma (src/plataforma/linux): o mesmo
# código que o Windows usa pelo build.cmd, com o outro backend
add_library(liquinix-nucleo STATIC
    src/nucleo/curvas.cpp
    src/nucleo/dock_desenho.cpp
    src/nucleo/dock_layout.cpp
    src/plataforma/linux/plataforma_linux.cpp)
target_include_directories(liquinix-nucleo PUBLIC src)

# o pintor sobre QPainter (desenho::Pintor no Linux)
add_library(liquinix-pintor-qt STATIC src/plataforma/linux/pintor_qt.cpp)
target_link_libraries(liquinix-pintor-qt PUBLIC liquinix-nucleo Qt6::Gui)

# o Dock (#20). Por enquanto, a janela de layer-shell na borda de baixo, sem ícones (#18).
add_executable(liquinix-dock src/linux/dock/main.cpp)
target_link_libraries(liquinix-dock PRIVATE liquinix-nucleo liquinix-pintor-qt Qt6::Core Qt6::Gui LayerShellQt::Interface)
