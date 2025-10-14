
#include "Sen66Driver.h"
#include <Arduino.h>
// Replace with real Sensirion lib wiring later

bool Sen66Driver::begin() {
  // pretend sensor init ok
  return true;
}
bool Sen66Driver::readOnce() {
  // stub values for structure; swap with real read
  pm25_ = 12.3f; t_ = 27.5f; rh_ = 58.0f;
  return true;
}
float Sen66Driver::pm25() const { return pm25_; }
float Sen66Driver::temperature() const { return t_; }
float Sen66Driver::humidity() const { return rh_; }
