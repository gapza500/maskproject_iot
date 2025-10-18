
#include <Arduino.h>
#include "firmware/common/config/version.h"
#include "firmware/common/config/features.h"
#include "firmware/common/util/Logging.h"
#include "firmware/platform/esp32c6/pins.h"
#include "firmware/platform/esp32c6/board_config.h"

#if __has_include("arduino_secrets.h")
  #include "arduino_secrets.h"
#else
  #define WIFI_SSID "EMPTY"
  #define WIFI_PASS "EMPTY"
#endif

#if FEAT_OLED
  #include "firmware/modules/oled/include/OledDriver.h"
  OledDriver oled;
#endif
#if FEAT_SEN66
  #include "firmware/modules/sen66/include/Sen66Driver.h"
  Sen66Driver sen;
#endif
#if FEAT_FLASH
  #include "firmware/modules/flash/include/FlashStore.h"
  FlashStore store;
#endif

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  LOGI("=== air_monitor_prod ===");
  LOGI("Board: %s", BOARD_NAME);
  LOGI("Firmware: %s", FIRMWARE_VERSION);

#if FEAT_OLED
  oled.begin();
  oled.showVersion(FIRMWARE_VERSION);
#endif
#if FEAT_SEN66
  sen.begin();
#endif
#if FEAT_FLASH
  store.begin();
#endif
}

void loop() {
  static bool led = false;
  digitalWrite(LED_BUILTIN, led); led = !led;

#if FEAT_SEN66
  if (sen.readOnce()) {
    float pm = sen.pm25();
    LOGI("PM2.5=%.1f T=%.1f RH=%.1f", pm, sen.temperature(), sen.humidity());
  #if FEAT_OLED
    oled.printLine(0, String("PM2.5: ") + pm);
  #endif
  }
#endif

  delay(500);
}
