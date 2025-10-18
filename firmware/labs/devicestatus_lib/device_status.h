#ifndef DEVICE_STATUS_H
#define DEVICE_STATUS_H

#include <Arduino.h>

// ====== Global device status codes ======
enum DeviceStatusCode : uint8_t {
    STATUS_OK = 0,
    STATUS_WARN = 1,
    STATUS_ERROR = 2,
    STATUS_NOT_FOUND = 3,
    STATUS_INIT_FAIL = 4
};

// ====== Unified device status report ======
struct DeviceStatusReport {
    DeviceStatusCode sen66;
    DeviceStatusCode battery;
    DeviceStatusCode rtc;
    DeviceStatusCode flash;
};

// ====== Human-readable labels (for logs / OLED / serial) ======
static const char* const STATUS_LABELS[] = {
    "OK",
    "WARN",
    "ERROR",
    "NOT FOUND",
    "INIT FAIL"
};

// ====== Helper ======
inline const char* statusToString(DeviceStatusCode code) {
    if (code <= STATUS_INIT_FAIL) return STATUS_LABELS[code];
    return "UNKNOWN";
}

#endif
