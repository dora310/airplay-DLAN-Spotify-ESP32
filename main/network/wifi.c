#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

#include "wifi.h"
#include "led.h"
#include "settings.h"

static const char *TAG = "wifi";

// Event group to signal WiFi connection
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Re-enable AP after this many consecutive failures
#define AP_REENABLE_THRESHOLD 5
// Keep the provisioning network visible long enough for a phone to discover
// and join it, even when the station reconnects to saved Wi-Fi quickly.
#define SETUP_AP_GRACE_SECONDS 120
// lwIP DHCP hostnames are limited to 31 characters plus the trailing NUL.
#define DHCP_HOSTNAME_MAX_LEN 31

static int s_retry_num = 0;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_wifi_initialized = false;
static bool s_sta_connected = false;
static bool s_has_credentials = false;
static bool s_pending_credential_test = false;
static uint8_t s_last_disconnect_reason = 0;
static volatile uint32_t s_disconnect_count = 0;
static esp_timer_handle_t s_retry_timer = NULL;
static esp_timer_handle_t s_ap_shutdown_timer = NULL;
static SemaphoreHandle_t s_scan_mutex = NULL;

// Saved AP config from init, used to re-enable AP without duplication
static wifi_config_t s_ap_config;

static void wifi_select_best_ap(const char *ssid);
static void scan_and_connect_task(void *arg);

static void configure_provisioning_subnet(void) {
  if (!s_ap_netif) {
    return;
  }

  /* The ESP-IDF default SoftAP address (192.168.4.1/24) conflicts with LANs
   * such as 192.168.4.0/22. Use a separate, uncommon /24 so the STA DHCP
   * client can install its LAN address and gateway without an overlapping
   * SoftAP route. */
  esp_netif_ip_info_t info = {0};
  IP4_ADDR(&info.ip, 192, 168, 240, 1);
  IP4_ADDR(&info.gw, 192, 168, 240, 1);
  IP4_ADDR(&info.netmask, 255, 255, 255, 0);

  /* The default AP netif owns the DHCP server. Stop it before changing the
   * interface address, then restart it with the new subnet. */
  esp_netif_dhcps_stop(s_ap_netif);
  esp_err_t err = esp_netif_set_ip_info(s_ap_netif, &info);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure provisioning subnet: %s",
             esp_err_to_name(err));
    return;
  }
  err = esp_netif_dhcps_start(s_ap_netif);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Provisioning DHCP start returned: %s",
             esp_err_to_name(err));
  }
  ESP_LOGI(TAG, "Provisioning AP address: " WIFI_PROVISIONING_IP_STR "/24");
}

static void sanitize_hostname(const char *name, char *out, size_t out_len) {
  size_t j = 0;
  for (size_t i = 0; name[i] && j < out_len - 1; i++) {
    char c = name[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
      out[j++] = c;
    } else if (j > 0 && out[j - 1] != '-') {
      out[j++] = '-';
    }
  }
  while (j > 0 && out[j - 1] == '-') {
    j--;
  }
  if (j == 0) {
    strlcpy(out, "esp32-airplay", out_len);
    return;
  }
  out[j] = '\0';
}

void wifi_set_hostname(const char *device_name) {
  if (!s_sta_netif || !device_name) {
    return;
  }
  char hostname[DHCP_HOSTNAME_MAX_LEN + 1];
  sanitize_hostname(device_name, hostname, sizeof(hostname));
  esp_err_t err = esp_netif_set_hostname(s_sta_netif, hostname);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set hostname '%s': %s", hostname,
             esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Hostname set to: %s", hostname);
  }
}

static void retry_timer_callback(void *arg) {
  (void)arg;
  if (s_has_credentials && !s_sta_connected) {
    ESP_LOGI(TAG, "Retry timer fired, reconnecting (attempt %d)...",
             s_retry_num + 1);
    led_set_wifi_state(LED_WIFI_CONNECTING);
    esp_wifi_connect();
  }
}

static void ap_shutdown_timer_callback(void *arg) {
  (void)arg;
  if (!s_sta_connected) {
    return;
  }

  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
    ESP_LOGI(TAG, "Setup window expired; disabling AP mode");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Could not disable setup AP: %s", esp_err_to_name(err));
    }
  }
}

static void schedule_retry(void) {
  if (!s_has_credentials) {
    return;
  }
  // Exponential backoff: 5s, 10s, 20s, 30s (max)
  int delay_s = 5;
  if (s_retry_num > AP_REENABLE_THRESHOLD) {
    int backoff_count = s_retry_num - AP_REENABLE_THRESHOLD;
    delay_s = 5 * (1 << (backoff_count > 3 ? 3 : backoff_count));
    if (delay_s > 30) {
      delay_s = 30;
    }
  }
  ESP_LOGI(TAG, "Scheduling retry in %d seconds", delay_s);
  esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000);
}

static void enable_ap_mode(void) {
  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_APSTA) {
    ESP_LOGI(TAG, "Re-enabling AP mode for configuration access");
    if (!s_ap_netif) {
      s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &s_ap_config);
  }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    if (!s_has_credentials) {
      led_set_wifi_state(LED_WIFI_DISCONNECTED);
      ESP_LOGI(TAG,
               "No saved WiFi credentials; setup AP remains available");
      return;
    }
    led_set_wifi_state(LED_WIFI_CONNECTING);
    // Defer scan+connect to a separate task — the blocking scan uses too
    // much stack to run inside the sys_evt event loop (2–4 KB).
    if (xTaskCreate(scan_and_connect_task, "wifi_scan", 4096, NULL, 3,
                    NULL) != pdPASS) {
      ESP_LOGW(TAG, "Could not start best-AP scan; connecting directly");
      esp_wifi_connect();
    }
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_sta_connected = false;
    led_set_wifi_state(LED_WIFI_DISCONNECTED);
    if (s_ap_shutdown_timer) {
      (void)esp_timer_stop(s_ap_shutdown_timer);
    }
    s_disconnect_count++;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    wifi_event_sta_disconnected_t *disconnected =
        (wifi_event_sta_disconnected_t *)event_data;
    s_last_disconnect_reason = disconnected->reason;
    /* Persist the real 802.11 disconnect reason. HTTP socket resets are not
       Wi-Fi link disconnects and must not be confused with this event. */
    ESP_LOGW(TAG, "WiFi link disconnected: reason=%d, count=%lu",
             disconnected->reason, (unsigned long)s_disconnect_count);

    s_retry_num++;

    if (s_retry_num < AP_REENABLE_THRESHOLD) {
      // Fast retries — reconnect immediately
      ESP_LOGI(TAG, "Retrying connection (%d/%d)...", s_retry_num,
               AP_REENABLE_THRESHOLD);
      led_set_wifi_state(LED_WIFI_CONNECTING);
      esp_wifi_connect();
    } else {
      if (s_retry_num == AP_REENABLE_THRESHOLD) {
        if (s_pending_credential_test) {
          ESP_LOGE(TAG, "WiFi credential test failed; restoring previous network");
          settings_clear_pending_wifi_credentials();
          vTaskDelay(pdMS_TO_TICKS(250));
          esp_restart();
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGW(TAG,
                 "WiFi connection failed after %d attempts, switching to "
                 "backoff retries",
                 AP_REENABLE_THRESHOLD);
        enable_ap_mode();
      }
      // Delayed retries with backoff
      schedule_retry();
    }
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_CONNECTED) {
    // Associated with the access point; authentication/DHCP is in progress.
    led_set_wifi_state(LED_WIFI_CONNECTING);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    s_sta_connected = true;
    led_set_wifi_state(LED_WIFI_CONNECTED);
    xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
    if (s_pending_credential_test) {
      esp_err_t promote = settings_promote_pending_wifi_credentials();
      if (promote == ESP_OK) {
        s_pending_credential_test = false;
      } else {
        ESP_LOGE(TAG, "Could not commit tested WiFi credentials: %s",
                 esp_err_to_name(promote));
      }
    }
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    // Leave the setup AP discoverable for a bounded grace period. Previously
    // it was disabled immediately after DHCP completed, often before a phone
    // had time to show the SSID in its Wi-Fi list.
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
      if (s_ap_shutdown_timer) {
        (void)esp_timer_stop(s_ap_shutdown_timer);
        esp_err_t timer_err = esp_timer_start_once(
            s_ap_shutdown_timer,
            (uint64_t)SETUP_AP_GRACE_SECONDS * 1000000ULL);
        if (timer_err == ESP_OK) {
          ESP_LOGI(TAG,
                   "STA connected; setup AP remains available for %d seconds "
                   "at http://" WIFI_PROVISIONING_IP_STR,
                   SETUP_AP_GRACE_SECONDS);
        } else {
          ESP_LOGW(TAG, "Could not start setup AP timer: %s",
                   esp_err_to_name(timer_err));
        }
      }
    }
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
    ESP_LOGI(TAG, "AP started");
  }
}

// One-shot task: scan for best AP then connect — runs outside the event loop
// to avoid overflowing the sys_evt stack.
static void scan_and_connect_task(void *arg) {
  (void)arg;
  wifi_config_t cfg;
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK ||
      strlen((char *)cfg.sta.ssid) == 0) {
    ESP_LOGI(TAG, "Station connection skipped: SSID is not configured");
    vTaskDelete(NULL);
    return;
  }
  wifi_select_best_ap((char *)cfg.sta.ssid);
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Initial WiFi connection failed to start: %s",
             esp_err_to_name(err));
  }
  vTaskDelete(NULL);
}

// Scan matching APs for diagnostics, then let ESP-IDF select/roam dynamically.
static void wifi_select_best_ap(const char *ssid) {
  wifi_scan_config_t scan_config = {
      .ssid = (uint8_t *)ssid,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time = {.active = {.min = 0,
                               .max = 0}}, // 0, 0 needed for BT co-exist
  };

  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Best-AP scan failed: %s", esp_err_to_name(err));
    return;
  }

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) {
    ESP_LOGW(TAG, "Best-AP scan: no APs found for SSID %s", ssid);
    esp_wifi_scan_get_ap_records(&ap_count, NULL);
    return;
  }

  wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
  if (!ap_list) {
    esp_wifi_scan_get_ap_records(&ap_count, NULL);
    return;
  }

  esp_wifi_scan_get_ap_records(&ap_count, ap_list);

  // Find AP with strongest signal
  int best_idx = 0;
  for (int i = 1; i < ap_count; i++) {
    if (ap_list[i].rssi > ap_list[best_idx].rssi) {
      best_idx = i;
    }
  }

  ESP_LOGI(TAG, "Found %d APs for SSID '%s', best: " MACSTR " (rssi=%d, ch=%d)",
           ap_count, ssid, MAC2STR(ap_list[best_idx].bssid),
           ap_list[best_idx].rssi, ap_list[best_idx].primary);

  for (int i = 0; i < ap_count; i++) {
    if (i != best_idx) {
      ESP_LOGI(TAG, "  Other AP: " MACSTR " (rssi=%d, ch=%d)",
               MAC2STR(ap_list[i].bssid), ap_list[i].rssi, ap_list[i].primary);
    }
  }

  /* Do not lock to the BSSID found during the startup scan. Mesh networks can
     move a client between nodes under one SSID; a pinned BSSID then prevents
     recovery. Clear both the BSSID and channel so the Wi-Fi driver can select
     any matching access point. */
  wifi_config_t sta_cfg;
  if (esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK) {
    sta_cfg.sta.bssid_set = false;
    memset(sta_cfg.sta.bssid, 0, sizeof(sta_cfg.sta.bssid));
    sta_cfg.sta.channel = 0;
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.failure_retry_cnt = 3;
    esp_err_t config_err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (config_err != ESP_OK) {
      ESP_LOGW(TAG, "Could not enable automatic AP selection: %s",
               esp_err_to_name(config_err));
    }
  }
  free(ap_list);
}

static void wifi_init_base(void) {
  if (s_wifi_initialized) {
    return;
  }

  s_wifi_event_group = xEventGroupCreate();
  s_scan_mutex = xSemaphoreCreateMutex();
  if (!s_wifi_event_group) {
    ESP_LOGE(TAG, "Failed to allocate WiFi event group");
  }
  if (!s_scan_mutex) {
    ESP_LOGW(TAG, "WiFi scan mutex unavailable; web scans will be disabled");
  }

  esp_err_t ret = esp_netif_init();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(ret);
  }

  // Create event loop if it doesn't exist
  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(ret);
  }

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  // Create one-shot retry timer (no background task needed)
  const esp_timer_create_args_t timer_args = {
      .callback = retry_timer_callback,
      .name = "wifi_retry",
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

  const esp_timer_create_args_t ap_shutdown_timer_args = {
      .callback = ap_shutdown_timer_callback,
      .name = "setup_ap",
  };
  ESP_ERROR_CHECK(
      esp_timer_create(&ap_shutdown_timer_args, &s_ap_shutdown_timer));

  s_wifi_initialized = true;
}

void wifi_init_apsta(const char *ap_ssid, const char *ap_password) {
  wifi_init_base();

  if (!s_sta_netif) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    char dev_name[65];
    settings_get_device_name(dev_name, sizeof(dev_name));
    wifi_set_hostname(dev_name);
  }
  if (!s_ap_netif) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
    configure_provisioning_subnet();
  }

  // Configure STA
  char ssid[33] = {0};
  char password[65] = {0};
  s_has_credentials = false;

  s_pending_credential_test = settings_has_pending_wifi_credentials();
  if (s_pending_credential_test &&
      settings_get_pending_wifi_credentials(ssid, sizeof(ssid), password,
                                            sizeof(password)) == ESP_OK) {
    s_has_credentials = true;
    ESP_LOGI(TAG, "Testing staged WiFi credentials for: %s", ssid);
  } else if (settings_get_wifi_ssid(ssid, sizeof(ssid)) == ESP_OK &&
      settings_get_wifi_password(password, sizeof(password)) == ESP_OK &&
      strlen(ssid) > 0) {
    s_has_credentials = true;
  }

  wifi_config_t sta_config = {0};
  strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
  strlcpy((char *)sta_config.sta.password, password,
          sizeof(sta_config.sta.password));
  sta_config.sta.threshold.authmode =
      s_has_credentials && strlen(password) > 0 ? WIFI_AUTH_WPA2_PSK
                                                : WIFI_AUTH_OPEN;
  /* Choose the strongest matching mesh/extender node instead of accepting
     the first scan match, and retry it after a temporary WPA3 refusal. */
  sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  sta_config.sta.failure_retry_cnt = 3;
  sta_config.sta.bssid_set = false;
  sta_config.sta.channel = 0;
  sta_config.sta.pmf_cfg.capable = true;
  sta_config.sta.pmf_cfg.required = false;

  // Configure AP and save for later re-enable
  const char *default_ssid = ap_ssid ? ap_ssid : CONFIG_DEFAULT_AP_SSID;
  const char *default_password =
      ap_password ? ap_password : CONFIG_DEFAULT_AP_PASSWORD;

  memset(&s_ap_config, 0, sizeof(s_ap_config));
  strncpy((char *)s_ap_config.ap.ssid, default_ssid,
          sizeof(s_ap_config.ap.ssid) - 1);
  s_ap_config.ap.ssid_len = strlen(default_ssid);
  s_ap_config.ap.channel = CONFIG_DEFAULT_AP_CHANNEL;
  s_ap_config.ap.max_connection = 4;

  if (strlen(default_password) == 0) {
    s_ap_config.ap.authmode = WIFI_AUTH_OPEN;
  } else {
    strncpy((char *)s_ap_config.ap.password, default_password,
            sizeof(s_ap_config.ap.password) - 1);
    s_ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  }

  s_retry_num = 0;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "AP+STA mode started: AP SSID=%s", default_ssid);
  if (s_has_credentials) {
    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
  } else {
    ESP_LOGI(TAG, "Provisioning mode active at http://"
                  WIFI_PROVISIONING_IP_STR);
  }
}

bool wifi_wait_connected(uint32_t timeout_ms) {
  if (!s_wifi_event_group) {
    return false;
  }

  TickType_t timeout_ticks =
      timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, timeout_ticks);

  if (bits & WIFI_CONNECTED_BIT) {
    return true;
  }
  if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "Failed to connect to WiFi");
  }
  return false;
}

void wifi_get_mac_str(char *mac_str, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(mac_str, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

bool wifi_is_connected(void) {
  return s_sta_connected;
}

uint32_t wifi_disconnect_count(void) { return s_disconnect_count; }

esp_err_t wifi_get_ip_str(char *ip_str, size_t len) {
  if (!s_sta_netif || !ip_str || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_netif_ip_info_t ip_info;
  esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
  if (err == ESP_OK) {
    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
  }
  return err;
}

esp_err_t wifi_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count) {
  if (!ap_list || !ap_count) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!s_scan_mutex || xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  // Scan in place. Disconnecting the STA here made the web request lose its
  // own TCP connection and could leave the receiver without a DHCP address.
  // ESP-IDF supports a blocking scan while the station remains associated.

  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = true,
  };

  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
    xSemaphoreGive(s_scan_mutex);
    return err;
  }

  uint16_t number = 0;
  err = esp_wifi_scan_get_ap_num(&number);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get AP count: %s", esp_err_to_name(err));
    xSemaphoreGive(s_scan_mutex);
    return err;
  }

  if (number == 0) {
    *ap_list = NULL;
    *ap_count = 0;
    xSemaphoreGive(s_scan_mutex);
    return ESP_OK;
  }

  wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * number);
  if (!aps) {
    xSemaphoreGive(s_scan_mutex);
    return ESP_ERR_NO_MEM;
  }

  err = esp_wifi_scan_get_ap_records(&number, aps);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(err));
    free(aps);
    xSemaphoreGive(s_scan_mutex);
    return err;
  }

  *ap_list = aps;
  *ap_count = number;
  xSemaphoreGive(s_scan_mutex);
  return ESP_OK;
}

static bool is_hex_password(const char *password) {
  if (strlen(password) != 64) return false;
  for (const char *p = password; *p; p++) {
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
          (*p >= 'A' && *p <= 'F'))) return false;
  }
  return true;
}

esp_err_t wifi_check_credentials(const char *ssid, const char *password,
                                 wifi_credential_check_t *result) {
  if (!ssid || !ssid[0] || strlen(ssid) > 32 || !password || !result)
    return ESP_ERR_INVALID_ARG;
  memset(result, 0, sizeof(*result));
  wifi_ap_record_t *aps = NULL;
  uint16_t count = 0;
  esp_err_t err = wifi_scan(&aps, &count);
  if (err != ESP_OK) {
    snprintf(result->message, sizeof(result->message), "Scan failed: %s",
             esp_err_to_name(err));
    return err;
  }
  wifi_ap_record_t *best = NULL;
  for (uint16_t i = 0; i < count; i++) {
    if (strcmp((char *)aps[i].ssid, ssid) == 0 &&
        (!best || aps[i].rssi > best->rssi)) best = &aps[i];
  }
  if (best) {
    result->network_visible = true;
    result->rssi = best->rssi;
    result->channel = best->primary;
    result->authmode = best->authmode;
    char current[33] = {0};
    wifi_config_t cfg;
    if (s_sta_connected && esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
      strlcpy(current, (char *)cfg.sta.ssid, sizeof(current));
      char saved_password[65] = {0};
      settings_get_wifi_password(saved_password, sizeof(saved_password));
      result->already_connected = strcmp(current, ssid) == 0 &&
                                  strcmp(saved_password, password) == 0;
    }
    size_t password_len = strlen(password);
    result->password_format_valid =
        best->authmode == WIFI_AUTH_OPEN ? password_len == 0
                                        : ((password_len >= 8 && password_len <= 63) ||
                                           is_hex_password(password));
    snprintf(result->message, sizeof(result->message), "%s",
             result->already_connected
                 ? "Already connected: credentials are working"
                 : result->password_format_valid
                       ? "Network found; ready for transactional connection test"
                       : "Password format does not match this network");
  } else {
    snprintf(result->message, sizeof(result->message),
             "Network was not found during the scan");
  }
  free(aps);
  return ESP_OK;
}

void wifi_get_diagnostics(wifi_diagnostics_t *diagnostics) {
  if (!diagnostics) return;
  memset(diagnostics, 0, sizeof(*diagnostics));
  diagnostics->initialized = s_wifi_initialized;
  diagnostics->connected = s_sta_connected;
  diagnostics->pending_credential_test = s_pending_credential_test;
  diagnostics->retry_count = s_retry_num;
  diagnostics->last_disconnect_reason = s_last_disconnect_reason;
  wifi_mode_t mode;
  diagnostics->setup_ap_enabled =
      esp_wifi_get_mode(&mode) == ESP_OK &&
      (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
  wifi_ap_record_t ap;
  if (s_sta_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    diagnostics->rssi = ap.rssi;
    diagnostics->channel = ap.primary;
    strlcpy(diagnostics->ssid, (char *)ap.ssid, sizeof(diagnostics->ssid));
    snprintf(diagnostics->bssid, sizeof(diagnostics->bssid), MACSTR,
             MAC2STR(ap.bssid));
  }
  wifi_get_ip_str(diagnostics->ip, sizeof(diagnostics->ip));
}

uint32_t wifi_get_disconnect_count(void) { return s_disconnect_count; }

void wifi_stop(void) {
  if (s_wifi_initialized) {
    esp_timer_stop(s_retry_timer);
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_initialized = false;
    s_sta_connected = false;
    s_has_credentials = false;
    s_retry_num = 0;
    led_set_wifi_state(LED_WIFI_DISCONNECTED);
    if (s_wifi_event_group) {
      xEventGroupClearBits(s_wifi_event_group,
                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
  }
}
