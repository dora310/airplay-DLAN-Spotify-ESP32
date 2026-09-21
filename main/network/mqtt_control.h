#pragma once
#include "esp_err.h"
#include <stdbool.h>

typedef struct {
  bool enabled;
  bool connected;
  int last_error;
  char broker_uri[128];
  char topic_prefix[96];
} mqtt_control_status_t;

esp_err_t mqtt_control_start(void);
esp_err_t mqtt_control_reload(void);
void mqtt_control_stop(void);
void mqtt_control_get_status(mqtt_control_status_t *status);
