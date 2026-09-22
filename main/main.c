#include "audio_output.h"
#include "audio_receiver.h"
#include "buttons.h"
#include "spiram_task.h"
#include "display.h"
#include "dns_server.h"
#include "dlna_renderer.h"
#include "ethernet.h"
#include "led.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "nvs_flash.h"
#include "playback_control.h"
#include "ptp_clock.h"
#include "rtsp_server.h"
#include "rtsp_events.h"
#include "settings.h"
#include "web_server.h"
#include "log_stream.h"
#include "wifi.h"
#include "spiffs_storage.h"
#include "source_manager.h"
#ifdef CONFIG_SPOTIFY_CONNECT_ENABLE
#include "spotify_connect.h"
#endif
#include "system_monitor.h"
#include "mqtt_control.h"
#include "optional_backends.h"
#include "software_dsp.h"
#include "maintenance.h"
#include "recovery.h"
#include "audio_test.h"

#ifdef CONFIG_BT_A2DP_ENABLE
#include "a2dp_sink.h"
#endif

#include "iot_board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static bool s_airplay_started = false;
static bool s_airplay_infrastructure_ready = false;
#ifdef CONFIG_SPOTIFY_CONNECT_ENABLE
static bool s_spotify_started = false;
#endif

static void on_airplay_dlna_event(rtsp_event_t event,
                                  const rtsp_event_data_t *data,
                                  void *user_data) {
  (void)data;
  (void)user_data;
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    source_manager_acquire(SOURCE_MANAGER_AIRPLAY);
    playback_control_set_source(PLAYBACK_SOURCE_AIRPLAY);
    dlna_renderer_set_airplay_active(true);
    break;
  case RTSP_EVENT_PLAYING:
  case RTSP_EVENT_PAUSED:
    dlna_renderer_set_airplay_active(true);
    break;
  case RTSP_EVENT_DISCONNECTED:
    dlna_renderer_set_airplay_active(false);
    source_manager_release(SOURCE_MANAGER_AIRPLAY);
    playback_control_set_source(PLAYBACK_SOURCE_NONE);
    break;
  default:
    break;
  }
}

static esp_err_t start_airplay_services(void) {
  if (recovery_is_safe_mode()) {
    ESP_LOGW(TAG, "Safe mode: AirPlay/DLNA audio services remain disabled");
    return ESP_ERR_INVALID_STATE;
  }
  if (s_airplay_started) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Starting AirPlay services...");

  // One-time infrastructure init (PTP, HAP, audio receiver/output)
  if (!s_airplay_infrastructure_ready) {
    esp_err_t err;
#ifndef CONFIG_AIRPLAY_FORCE_V1
    err = ptp_clock_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Failed to init PTP clock: %s", esp_err_to_name(err));
      s_airplay_started = false;
      return err;
    }
#endif

    err = hap_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "HAP initialization failed: %s", esp_err_to_name(err));
      return err;
    }
    err = audio_receiver_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Audio receiver initialization failed: %s",
               esp_err_to_name(err));
      return err;
    }
    err = audio_output_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Audio output initialization failed: %s",
               esp_err_to_name(err));
      return err;
    }
    audio_test_set_output_ready(true);
    err = mdns_airplay_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "AirPlay advertisement failed: %s", esp_err_to_name(err));
      return err;
    }
    s_airplay_infrastructure_ready = true;
  }

  audio_output_start();

  esp_err_t rtsp_err = rtsp_server_start();
  if (rtsp_err != ESP_OK && rtsp_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "RTSP server failed to start: %s",
             esp_err_to_name(rtsp_err));
    audio_output_stop();
    return rtsp_err;
  }

  s_airplay_started = true;
  ESP_LOGI(TAG, "AirPlay ready");

#ifdef CONFIG_SPOTIFY_CONNECT_ENABLE
  if (!s_spotify_started && web_server_get_handle()) {
    char spotify_name[65];
    settings_get_device_name(spotify_name, sizeof(spotify_name));
    esp_err_t spotify_err = spotify_connect_start(
        web_server_get_handle(), 80, spotify_name);
    if (spotify_err == ESP_OK || spotify_err == ESP_ERR_INVALID_STATE) {
      s_spotify_started = true;
      ESP_LOGI(TAG, "Spotify Connect ready");
    } else {
      ESP_LOGW(TAG, "Spotify Connect unavailable: %s",
               esp_err_to_name(spotify_err));
    }
  }
#endif
  return ESP_OK;
}

/* Rebuild network-facing services after DHCP/interface recovery. A link can
 * disconnect and reconnect entirely between two network-monitor polls, so the
 * Wi-Fi disconnect generation is also watched below. */
static esp_err_t recover_airplay_services(void) {
  if (recovery_is_safe_mode()) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!s_airplay_started) {
    return start_airplay_services();
  }

  ESP_LOGW(TAG, "Network changed; refreshing AirPlay services");
  rtsp_server_stop();
  s_airplay_started = false;

#ifndef CONFIG_AIRPLAY_FORCE_V1
  ptp_clock_stop();
  esp_err_t ptp_err = ptp_clock_init();
  if (ptp_err != ESP_OK && ptp_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "PTP restart failed: %s", esp_err_to_name(ptp_err));
  }
#endif

  esp_err_t mdns_err = mdns_airplay_refresh();
  if (mdns_err != ESP_OK) {
    ESP_LOGE(TAG, "AirPlay mDNS refresh failed: %s",
             esp_err_to_name(mdns_err));
    return mdns_err;
  }

  audio_output_start();
  esp_err_t rtsp_err = rtsp_server_start();
  if (rtsp_err != ESP_OK && rtsp_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "RTSP recovery failed: %s", esp_err_to_name(rtsp_err));
    return rtsp_err;
  }
  s_airplay_started = true;
  ESP_LOGI(TAG, "AirPlay recovered after network change");
  return ESP_OK;
}
#ifdef CONFIG_BT_A2DP_ENABLE
static void stop_airplay_services(void) {
  if (!s_airplay_started) {
    return;
  }

  ESP_LOGI(TAG, "Stopping AirPlay services...");

  rtsp_server_stop();
  audio_output_stop();

  s_airplay_started = false;
  playback_control_set_source(PLAYBACK_SOURCE_NONE);
  ESP_LOGI(TAG, "AirPlay stopped");
}
#endif

static void network_monitor_task(void *pvParameters) {
  (void)pvParameters;
  bool had_network = ethernet_is_connected() || wifi_is_connected();
  bool dns_running = !had_network;
  bool wifi_started = wifi_is_connected() || !ethernet_is_connected();
  bool had_eth = ethernet_is_connected();
  uint32_t last_wifi_disconnect_count = wifi_get_disconnect_count();
  bool airplay_recovery_pending = false;

  // Start captive portal DNS if no network yet
  if (dns_running) {
    dns_server_start(WIFI_PROVISIONING_IP_ADDR);
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool eth_up = ethernet_is_connected();
    bool wifi_up = wifi_is_connected();
    bool has_network = eth_up || wifi_up;
    uint32_t wifi_disconnect_count = wifi_get_disconnect_count();
    if (wifi_disconnect_count != last_wifi_disconnect_count) {
      last_wifi_disconnect_count = wifi_disconnect_count;
      airplay_recovery_pending = true;
    }

    // Ethernet just came up — stop WiFi entirely
    if (eth_up && !had_eth && wifi_started) {
      ESP_LOGI(TAG, "Ethernet connected — stopping WiFi");
      wifi_stop();
      wifi_started = false;
      wifi_up = false;
    }

    // Ethernet dropped — bring up WiFi (AP + STA)
    if (!eth_up && had_eth) {
      ESP_LOGI(TAG, "Ethernet down — starting WiFi as fallback");
      wifi_init_apsta(NULL, NULL);
      wifi_started = true;
    }

    had_eth = eth_up;
    has_network = eth_up || wifi_is_connected();

    if (has_network == had_network &&
        !(has_network && airplay_recovery_pending)) {
      continue;
    }

    if (has_network) {
      ESP_LOGI(TAG, "Network up (eth=%s, wifi=%s)", eth_up ? "yes" : "no",
               wifi_up ? "yes" : "no");
      if (airplay_recovery_pending) {
        recover_airplay_services();
        airplay_recovery_pending = false;
      } else {
        start_airplay_services();
      }
      if (dns_running) {
        dns_server_stop();
        dns_running = false;
      }
    } else {
      if (!dns_running) {
        dns_server_start(WIFI_PROVISIONING_IP_ADDR);
        dns_running = true;
      }
    }

    had_network = has_network;
  }
}

#ifdef CONFIG_BT_A2DP_ENABLE
static void on_bt_state_changed(bool connected) {
  if (connected) {
    ESP_LOGI(TAG, "BT connected — disabling AirPlay");
    stop_airplay_services();
    playback_control_set_source(PLAYBACK_SOURCE_BLUETOOTH);
  } else {
    ESP_LOGI(TAG, "BT disconnected — re-enabling AirPlay");
    playback_control_set_source(PLAYBACK_SOURCE_NONE);
    if (ethernet_is_connected() || wifi_is_connected()) {
      start_airplay_services();
    }
  }
}

static void on_airplay_client_event(rtsp_event_t event,
                                    const rtsp_event_data_t *data,
                                    void *user_data) {
  (void)data;
  (void)user_data;
  if (bt_a2dp_sink_is_connected()) {
    return;
  }
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    ESP_LOGI(TAG, "AirPlay client connected — disabling BT");
    bt_a2dp_sink_set_discoverable(false);
    break;
  case RTSP_EVENT_PAUSED:
    // V1 grace period active — keep BT hidden so the phone reconnects
    // to AirPlay rather than falling back to BT.
    ESP_LOGI(TAG, "AirPlay paused — keeping BT hidden");
    break;
  case RTSP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "AirPlay client disconnected — enabling BT");
    bt_a2dp_sink_set_discoverable(true);
    break;
  default:
    break;
  }
}
#endif

void app_main(void) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_ERROR_CHECK(settings_init());
  ESP_ERROR_CHECK(recovery_init());
  spiffs_storage_init();
  log_stream_init();
  ESP_ERROR_CHECK(playback_control_init());
  led_init();

  // Initialize board-specific hardware (includes I2C/SPI bus for display and
  // DAC)
  ESP_LOGI(TAG, "Board: %s", iot_board_get_info());
  esp_err_t err = iot_board_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(err));
  }

  // Pass the board-owned bus to the display so it reuses it rather than
  // creating a duplicate bus on the same pins.
#if defined(CONFIG_DISPLAY_BUS_SPI)
  display_init(iot_board_get_handle(BOARD_SPI_DISP_ID));
#else
  display_init(iot_board_get_handle(BOARD_I2C_DISP_ID));
#endif

  // Initialize LVGL-dependent board resources (e.g., touch input) after
  // display/LVGL port is ready.
  iot_board_init_lvgl_resources();

  // Try ethernet first
  bool eth_available = false;
  err = ethernet_init();
  if (err == ESP_OK) {
    // Wait for ethernet link + DHCP (up to 5s for link, then 10s more for DHCP)
    ESP_LOGI(TAG, "Waiting for ethernet...");
    for (int i = 0; i < 25 && !ethernet_is_link_up(); i++) {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ethernet_is_link_up() && !ethernet_is_connected()) {
      ESP_LOGI(TAG, "Ethernet link up, waiting for DHCP...");
      for (int i = 0; i < 50 && !ethernet_is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
      }
    }
    eth_available = ethernet_is_connected();
    if (eth_available) {
      ESP_LOGI(TAG, "Ethernet connected");
    } else {
      ESP_LOGI(TAG, "Ethernet not connected (cable?), will use WiFi");
    }
  } else if (err != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(err));
  }

  // Start WiFi only if ethernet is not available
  if (!eth_available) {
    wifi_init_apsta(NULL, NULL);

    // Wait for initial WiFi connection if credentials exist
    if (settings_has_wifi_credentials()) {
      if (!wifi_wait_connected(30000)) {
        ESP_LOGI(TAG, "Connect to 'AirPlay and DLNA Setup' -> http://"
                      WIFI_PROVISIONING_IP_STR);
      }
    } else {
      ESP_LOGI(TAG, "Connect to 'AirPlay and DLNA Setup' -> http://"
                    WIFI_PROVISIONING_IP_STR);
    }
  } else {
    ESP_LOGI(TAG, "Ethernet connected — skipping WiFi");
  }

  /* WiFi/setup AP is boot-critical.  Optional v0.3 services are deliberately
     initialized only after networking has started, and must never abort the
     captive-portal boot path. */
  err = software_dsp_init(44100);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Software DSP unavailable: %s", esp_err_to_name(err));
  }
  err = source_manager_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Source manager unavailable: %s", esp_err_to_name(err));
  }
  err = maintenance_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Maintenance supervisor unavailable: %s",
             esp_err_to_name(err));
  }
  err = system_monitor_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "System monitor unavailable: %s", esp_err_to_name(err));
  }

  // Start services that work on any interface
  ESP_ERROR_CHECK(web_server_start(80));
  maintenance_mark_services_ready();
  recovery_mark_services_ready();
  if (!recovery_is_safe_mode()) {
    esp_err_t dlna_err =
        dlna_renderer_register(web_server_get_handle(), 80);
    if (dlna_err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to register DLNA: %s", esp_err_to_name(dlna_err));
    }
    if (mqtt_control_start() != ESP_OK) {
      ESP_LOGW(TAG, "MQTT integration did not start");
    }
    rtsp_events_register(on_airplay_dlna_event, NULL);
  } else {
    ESP_LOGW(TAG, "Recovery web panel is available; audio services skipped");
  }
  task_create_spiram(network_monitor_task, "net_mon", 4096, NULL, 5, NULL,
                     NULL);

  bool connected = eth_available || wifi_is_connected();
  if (connected && !recovery_is_safe_mode()) {
    start_airplay_services();
    err = optional_backends_init();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Optional backends unavailable: %s", esp_err_to_name(err));
    }
  }

#ifdef CONFIG_BT_A2DP_ENABLE
  // Initialize Bluetooth A2DP Sink
  {
    char bt_name[65];
    settings_get_device_name(bt_name, sizeof(bt_name));
    esp_err_t bt_err = bt_a2dp_sink_init(bt_name, on_bt_state_changed);
    if (bt_err != ESP_OK) {
      ESP_LOGE(TAG, "BT A2DP init failed: %s", esp_err_to_name(bt_err));
    } else {
      rtsp_events_register(on_airplay_client_event, NULL);
    }
  }
#endif

  buttons_init();

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
