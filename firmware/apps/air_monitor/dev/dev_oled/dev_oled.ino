
#include <Arduino.h>
#include "firmware/platform/esp32c6/pins.h"
#include "firmware/modules/oled/include/OledDriver.h"

OledDriver oled;
void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  oled.begin();
  oled.printLine(0, "OLED DEV");
}
void loop() {
  static int n=0;
  digitalWrite(LED_BUILTIN, n%2);
  oled.printLine(1, String("count: ")+n++);
  delay(300);
}
