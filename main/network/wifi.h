#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>
#include <stdint.h>

/* Keep the provisioning AP away from common home LAN ranges. The previous
 * 192.168.4.1 address overlapped networks using 192.168.4.0/22 and prevented
 * the station interface from completing DHCP after authentication. */
#define WIFI_PROVISIONING_IP_STR  "192.168.240.1"
#define WIFI_PROVISIONING_IP_ADDR 0x01F0A8C0UL

/**
 * Initialize WiFi in both AP and STA modes
 * @param ap_ssid AP SSID (if NULL, uses default)
 * @param ap_password AP password (if NULL, uses default or open)
 */
void wifi_init_apsta(const char *ap_ssid, const char *ap_password);

/**
 * Block until WiFi is connected and has an IP address
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return true if connected, false if timeout
 */
bool wifi_wait_connected(uint32_t timeout_ms);

/**
 * Get the device MAC address as a string (XX:XX:XX:XX:XX:XX)
 */
void wifi_get_mac_str(char *mac_str, size_t len);

/**
 * Check if WiFi STA is connected
 */
bool wifi_is_connected(void);

/**
 * Get current IP address as string
 * @param ip_str Output buffer
 * @param len Buffer size
 * @return ESP_OK on success
 */
esp_err_t wifi_get_ip_str(char *ip_str, size_t len);

/**
 * Scan for available WiFi networks
 * @param ap_list Output array of AP info (caller must free)
 * @param ap_count Output: number of APs found
 * @return ESP_OK on success
 */
esp_err_t wifi_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count);

typedef struct {
  bool initialized;
  bool connected;
  bool setup_ap_enabled;
  bool pending_credential_test;
  int retry_count;
  uint8_t last_disconnect_reason;
  int8_t rssi;
  uint8_t channel;
  char ssid[33];
  char bssid[18];
  char ip[16];
} wifi_diagnostics_t;

typedef struct {
  bool network_visible;
  bool password_format_valid;
  bool already_connected;
  int8_t rssi;
  uint8_t channel;
  wifi_auth_mode_t authmode;
  char message[128];
} wifi_credential_check_t;

void wifi_get_diagnostics(wifi_diagnostics_t *diagnostics);

/** Non-disruptive scan and credential-format validation before staging. */
esp_err_t wifi_check_credentials(const char *ssid, const char *password,
                                 wifi_credential_check_t *result);

/**
 * Disconnect and stop WiFi
 */
void wifi_stop(void);

/**
 * Set the DHCP hostname from the given device name.
 * Sanitizes to a valid DNS label (lowercase, hyphens for spaces/symbols).
 * Takes effect on the next DHCP transaction.
 */
void wifi_set_hostname(const char *device_name);
