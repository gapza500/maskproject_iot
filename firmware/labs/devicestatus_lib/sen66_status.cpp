#include "sen66_status.h"

DeviceStatusCode evaluateSen66Status(uint32_t v) {
    bool warn = (v & DS_WARN_SPEED);
    bool err  = (v & (DS_ERR_PM | DS_ERR_CO2_2 | DS_ERR_GAS | DS_ERR_RHT | DS_ERR_FAN));

    if (err)  return STATUS_ERROR;
    if (warn) return STATUS_WARN;
    return STATUS_OK;
}

void printSen66Status(uint32_t v) {
    Serial.print(F("SEN66 Status = 0x"));
    Serial.println(v, HEX);

    if (v == 0) {
        Serial.println(F("  OK: No errors/warnings"));
        return;
    }

    if (v & DS_WARN_SPEED) Serial.println(F("  WARN: Fan speed out of range (±10%)"));
    if (v & DS_ERR_PM)     Serial.println(F("  ERR : PM sensor error"));
    if (v & DS_ERR_CO2_2)  Serial.println(F("  ERR : CO2 sensor error (SEN66)"));
    if (v & DS_ERR_GAS)    Serial.println(F("  ERR : GAS (VOC/NOx) sensor error"));
    if (v & DS_ERR_RHT)    Serial.println(F("  ERR : RH&T sensor error"));
    if (v & DS_ERR_FAN)    Serial.println(F("  ERR : FAN error (stalled/blocked)"));
}
