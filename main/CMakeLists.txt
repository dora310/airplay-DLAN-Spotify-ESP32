set( SRC_FILES
    "main.c"
    "alac_magic_cookie.c"
    "settings.c"
    "audio/audio_receiver.c"
    "audio/audio_stream.c"
    "audio/audio_stream_realtime.c"
    "audio/audio_stream_buffered.c"
    "audio/audio_decoder.c"
    "audio/audio_buffer.c"
    "audio/audio_timing.c"
    "audio/audio_crypto.c"
    "audio/audio_resample.c"
    "rtsp/rtsp_server.c"
    "rtsp/rtsp_conn.c"
    "rtsp/rtsp_message.c"
    "rtsp/rtsp_handlers.c"
    "rtsp/rtsp_fairplay.c"
    "rtsp/rtsp_crypto.c"
    "rtsp/rtsp_rsa.c"
    "rtsp/rtsp_events.c"
    "plist/plist_xml.c"
    "plist/bplist_parser.c"
    "plist/bplist_builder.c"
    "plist/base64.c"
    "hap/hap.c"
    "hap/hap_pair_verify.c"
    "hap/hap_pair_setup.c"
    "hap/hap_crypto.c"
    "hap/srp.c"
    "hap/tlv8.c"
    "network/wifi.c"
    "network/mdns_airplay.c"
    "network/ptp_clock.c"
    "network/ntp_clock.c"
    "network/socket_utils.c"
    "network/web_server.c"
    "network/ota.c"
    "network/dns_server.c"
    "network/log_stream.c"
    "network/dlna_renderer.c"
    "led.c"
    "audio/eq_events.c"
    "buttons.c"
    "playback_control.c"
    "source_manager.c"
    "system_monitor.c"
    "maintenance.c"
    "recovery.c"
    "optional_backends.c"
    "dacp_client.c"
    "audio/software_dsp.c"
    "audio/audio_test.c"
    "network/web_api_v3.c"
    "network/mqtt_control.c"
    "network/github_ota.c"
)

if(CONFIG_SPOTIFY_CONNECT_ENABLE)
    list(APPEND SRC_FILES "spotify_connect.cpp")
endif()

# Select audio output implementation at compile time
if(CONFIG_AUDIO_OUTPUT_SPDIF)
    list(APPEND SRC_FILES "audio/audio_output_spdif.c")
elseif(CONFIG_AUDIO_OUTPUT_USB)
    list(APPEND SRC_FILES "audio/audio_output_usb.c")
else()
    list(APPEND SRC_FILES "audio/audio_output.c")
endif()

# Bluetooth A2DP sink (only available on ESP32 with Classic BT)
if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND SRC_FILES "audio/a2dp_sink.c")
endif()

# W5500 SPI Ethernet
if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND SRC_FILES "network/ethernet.c")
endif()

set( INC_DIRS
    "." 
    "audio" 
    "rtsp" 
    "plist" 
    "hap" 
    "network"
)

if(CONFIG_SPOTIFY_CONNECT_ENABLE)
    list(APPEND INC_DIRS
        "${PROJECT_DIR}/components/cspot_embedded/cspot/include"
        "${PROJECT_DIR}/components/cspot_embedded/cspot/bell/main/utilities/include"
        "${PROJECT_DIR}/components/cspot_embedded/cspot/bell/main/platform"
        "${PROJECT_DIR}/components/cspot_embedded/cspot/bell/external/nanopb"
        "${PROJECT_DIR}/components/cspot_embedded/cspot/bell/external/nlohmann_json/include"
        "${PROJECT_DIR}/components/cspot_embedded/cspot/bell/external/tremor"
        "${CMAKE_BINARY_DIR}/esp-idf/cspot_embedded/cspot")
endif()

set( DEPS
    driver
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_ringbuf
    esp_driver_i2s
    esp_timer
    esp_http_server
    esp_http_client
    esp_https_ota
    app_update
    json
    mbedtls
    esp_driver_ledc
    boards
    dac
    dac_tas58xx
    display
    audio-resampler
    esp_eth
    spiffs_storage
    esp_audio_codec
    mqtt
)

if(CONFIG_SPOTIFY_CONNECT_ENABLE)
    list(APPEND DEPS cspot_embedded)
    # ESP-IDF validates every INCLUDE_DIRS entry during initial configure,
    # before cspot's nanopb custom command creates its generated header tree.
    # A clean GitHub Actions runner therefore needs the parent directory to
    # exist at configure time (incremental/PlatformIO builds already have it).
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/esp-idf/cspot_embedded/cspot")
endif()

# TODO: why these fall over in CI and fine locally...
# Bluetooth component only needed when A2DP is enabled
#if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND DEPS bt)
#endif()

# Ethernet component only needed when W5500 is enabled
#if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND DEPS esp_eth)
#endif()

idf_component_register(
    SRCS ${SRC_FILES}
    INCLUDE_DIRS ${INC_DIRS}
    PRIV_REQUIRES ${DEPS}
)

if(CONFIG_SPOTIFY_CONNECT_ENABLE)
    target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        BELL_DISABLE_FMT BELL_DISABLE_REGEX BELL_ONLY_CJSON)
    add_dependencies(${COMPONENT_LIB} generate_proto_sources)
endif()

# Enable function wrapping for abort() to intercept panics and mute GPIO
if(CONFIG_BOARD_SQUEEZEAMP_COMMON)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=abort")
endif()
