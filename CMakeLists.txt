cmake_minimum_required(VERSION 4.2)

project(libotoworm
    VERSION 0.1.0
    LANGUAGES C CXX)

option(LIBOTOWORM_BUILD_SHARED "Build libotoworm as a shared library" OFF)

find_package(ZLIB REQUIRED)
find_package(Iconv REQUIRED)

set(LIBOTOWORM_SOURCES
    src/bms.cpp
    src/conversion.cpp
    src/note_loader_bms.cpp
    src/note_loader_bmson.cpp
    src/note_loader_ftb.cpp
    src/note_loader_ojn.cpp
    src/note_loader_om.cpp
    src/note_loader_sm.cpp
    src/nps_graph.cpp
    src/note_writer_bms.cpp
    src/note_writer_osumania.cpp
        src/ProcessedChart.cpp
    src/rowified_difficulty_7k.cpp
    src/song.cpp
    src/song_timing.cpp
    src/text_and_file_util.cpp
    src/track_note.cpp
    src-thirdparty/minizip/ioapi.c
    src-thirdparty/minizip/unzip.c
    src-thirdparty/sha256/sha256.cpp)

if(LIBOTOWORM_BUILD_SHARED)
    add_library(otoworm SHARED ${LIBOTOWORM_SOURCES})
else()
    add_library(otoworm STATIC ${LIBOTOWORM_SOURCES})
endif()

add_library(otoworm::otoworm ALIAS otoworm)

target_compile_features(otoworm PUBLIC cxx_std_20)

target_include_directories(otoworm
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src-thirdparty)

target_link_libraries(otoworm PUBLIC ZLIB::ZLIB Iconv::Iconv)

if(APPLE)
    target_compile_definitions(otoworm PRIVATE DARWIN)
endif()

set_target_properties(otoworm PROPERTIES
    EXPORT_NAME otoworm
    OUTPUT_NAME otoworm)
