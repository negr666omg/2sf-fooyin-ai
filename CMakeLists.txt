cmake_minimum_required(VERSION 3.19)

project(
    fooyin-plugin-2sf
    VERSION 0.1.0
    DESCRIPTION "Nintendo DS 2SF/MINI2SF input plugin for fooyin"
    HOMEPAGE_URL "https://github.com/negr666omg/2sf-fooyin-ai"
    LANGUAGES C CXX
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD) # FetchContent_Populate is needed: upstream root requires Kodi
endif()
find_package(Fooyin REQUIRED)
find_package(ZLIB REQUIRED)

# Vio2SF and PSFLib are taken from Kodi's maintained 2SF decoder addon.
# Pin an exact upstream commit so builds are reproducible.
FetchContent_Declare(
    audiodecoder_2sf
    GIT_REPOSITORY https://github.com/xbmc/audiodecoder.2sf.git
    GIT_TAG        232abe97f202bda4deef214989bb9d99a0730540
    GIT_SHALLOW    FALSE
)
FetchContent_GetProperties(audiodecoder_2sf)
if(NOT audiodecoder_2sf_POPULATED)
    FetchContent_Populate(audiodecoder_2sf)
endif()

add_subdirectory("${audiodecoder_2sf_SOURCE_DIR}/lib/vio2sf" "${CMAKE_CURRENT_BINARY_DIR}/vio2sf")
add_subdirectory("${audiodecoder_2sf_SOURCE_DIR}/lib/psflib" "${CMAKE_CURRENT_BINARY_DIR}/psflib")

target_include_directories(vio2sf PUBLIC
    "${audiodecoder_2sf_SOURCE_DIR}/lib/vio2sf/src/vio2sf/desmume"
)
target_include_directories(psflib PUBLIC
    "${audiodecoder_2sf_SOURCE_DIR}/lib/psflib"
)

# Vio2SF is a legacy emulator core with generic C symbol names. Keep its
# implementation symbols local to this module so they cannot interpose with
# another plugin embedding an emulator core.
set_target_properties(vio2sf psflib PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

create_fooyin_plugin(
    twosf
    JSON_IN src/twosf.json.in
    DEPENDS Fooyin::Core Fooyin::Gui vio2sf psflib ZLIB::ZLIB
    SOURCES
        src/twosfdefs.h
        src/twosfinput.cpp
        src/twosfinput.h
        src/twosfplugin.cpp
        src/twosfplugin.h
        src/twosfsettings.cpp
        src/twosfsettings.h
)

if(UNIX AND NOT APPLE)
    target_link_libraries(twosf PRIVATE m)
endif()

# Keep the emulator's C symbols private to this plugin: other input plugins may
# bundle different emulator cores with overlapping legacy symbol names.
set_target_properties(twosf PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

option(BUILD_PLUGIN_BUNDLE "Create an installable .fyplugin bundle" ON)
if(BUILD_PLUGIN_BUNDLE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bundle-licenses")
    configure_file(
        "${audiodecoder_2sf_SOURCE_DIR}/lib/vio2sf/src/vio2sf/desmume/COPYING"
        "${CMAKE_CURRENT_BINARY_DIR}/bundle-licenses/Vio2SF-GPL-2.0.txt" COPYONLY
    )
    configure_file(
        "${audiodecoder_2sf_SOURCE_DIR}/LICENSE.md"
        "${CMAKE_CURRENT_BINARY_DIR}/bundle-licenses/Kodi-2SF-decoder-GPL-2.0.txt" COPYONLY
    )
    create_fooyin_plugin_bundle(
        twosf
        NAME fooyin-2sf
        FILES README.md COPYING
        LICENSE_DIRECTORIES LICENSES "${CMAKE_CURRENT_BINARY_DIR}/bundle-licenses"
    )
endif()
