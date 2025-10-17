#ifndef SEN66_STATUS_H
#define SEN66_STATUS_H

#include <Arduino.h>
#include <SensirionI2cSen66.h>
#include "device_status.h"

// ===== SEN66 device-status bit masks =====
#define BIT(n) (1UL << (n))
const uint32_t DS_WARN_SPEED = BIT(21);
const uint32_t DS_ERR_PM     = BIT(11);
const uint32_t DS_ERR_CO2_2  = BIT(9);
const uint32_t DS_ERR_GAS    = BIT(7);
const uint32_t DS_ERR_RHT    = BIT(6);
const uint32_t DS_ERR_FAN    = BIT(4);

// ===== Functions =====

// Convert SEN66 status bits → unified DeviceStatusCode
DeviceStatusCode evaluateSen66Status(uint32_t statusValue);

// Print human-readable status messages (optional for Serial/OLED)
void printSen66Status(uint32_t statusValue);

#endif
