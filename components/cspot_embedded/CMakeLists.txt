cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS "component_anchor.c"
    INCLUDE_DIRS "include" "cspot/include" "cspot/bell/external/nanopb"
                 "cspot/bell/main/utilities/include"
                 "cspot/bell/main/platform"
                 "cspot/bell/main/io/include"
                 "cspot/bell/external/nlohmann_json/include"
                 "cspot/bell/external/tremor"
    REQUIRES mbedtls espressif__mdns pthread driver lwip json esp_timer
)

if(CONFIG_SPOTIFY_CONNECT_ENABLE)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # cspot only needs its protocol, networking and Tremor/Vorbis decoder.
    # Disable the unrelated Bell codecs, sinks, MQTT and web server to keep
    # the ESP32-S3 image and RAM footprint under control.
    set(BELL_DISABLE_CODECS ON CACHE BOOL "" FORCE)
    set(BELL_DISABLE_SINKS ON CACHE BOOL "" FORCE)
    set(BELL_DISABLE_FMT ON CACHE BOOL "" FORCE)
    set(BELL_DISABLE_REGEX ON CACHE BOOL "" FORCE)
    set(BELL_ONLY_CJSON ON CACHE BOOL "" FORCE)
    set(BELL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
    set(BELL_DISABLE_WEBSERVER ON CACHE BOOL "" FORCE)
    set(BELL_EXTERNAL_CJSON "idf::json" CACHE STRING "" FORCE)
    set(BELL_EXTERNAL_VORBIS "" CACHE STRING "" FORCE)
    set(CSPOT_TARGET_ESP32 ON CACHE BOOL "" FORCE)

    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/cspot"
                     "${CMAKE_CURRENT_BINARY_DIR}/cspot")
    # spotify_connect.cpp includes cspot headers that themselves include the
    # generated nanopb headers, so make generation an explicit prerequisite of
    # this component as well as of the cspot archive.
    add_dependencies(${COMPONENT_LIB} generate_proto_sources)
    target_link_libraries(${COMPONENT_LIB} PUBLIC cspot)
endif()
