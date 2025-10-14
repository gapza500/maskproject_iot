
#include <Arduino.h>
#include "OledDriver.h"

OledDriver oled;
void setup() {
  Serial.begin(115200);
  oled.begin();
  oled.printLine(0, "min_oled example");
}
void loop() {}
