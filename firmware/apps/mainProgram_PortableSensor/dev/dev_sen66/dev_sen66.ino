
#include <Arduino.h>
#include "firmware/modules/sen66/include/Sen66Driver.h"

Sen66Driver sen;
void setup(){ Serial.begin(115200); sen.begin(); }
void loop(){
  if (sen.readOnce()) {
    Serial.printf("PM2.5=%.1f T=%.1f RH=%.1f\n", sen.pm25(), sen.temperature(), sen.humidity());
  }
  delay(1000);
}
