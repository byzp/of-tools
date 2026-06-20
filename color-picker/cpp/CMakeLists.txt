cmake_minimum_required(VERSION 3.15)
project(of_color_picker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimized build for single-config generators (Ninja/NMake).
# For multi-config generators (Visual Studio), build with --config Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(picker
    app.cpp
    ui.cpp
    colordlg.cpp
    sniffer.cpp
    search.cpp
    swirl.cpp
    texture.cpp
)

if(MSVC)
    # /O2 on every non-Debug config (avoids the /RTC1+/O2 conflict in Debug).
    target_compile_options(picker PRIVATE /EHsc $<$<NOT:$<CONFIG:Debug>>:/O2>)
endif()

# wpcap is loaded dynamically at runtime (no link dependency).
target_link_libraries(picker PRIVATE gdiplus comdlg32 ws2_32 user32 gdi32 ole32 comctl32)
