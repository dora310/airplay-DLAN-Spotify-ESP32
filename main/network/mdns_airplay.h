#pragma once

/**
 * Initialize mDNS and advertise AirPlay 2 services
 *
 * This publishes:
 * - _airplay._tcp service (AirPlay 2)
 * - _raop._tcp service (Remote Audio Output Protocol)
 *
 * With all required TXT records for iOS to recognize the device
 */
#include "esp_err.h"

/** Start the AirPlay/RAOP advertisements. Never aborts the application. */
esp_err_t mdns_airplay_init(void);

/** Recreate mDNS after an interface/IP change so phones see AirPlay again. */
esp_err_t mdns_airplay_refresh(void);

/** Remove all AirPlay mDNS advertisements. */
void mdns_airplay_stop(void);
