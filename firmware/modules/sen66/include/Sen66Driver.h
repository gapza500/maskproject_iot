
#pragma once
#include <Arduino.h>

class Sen66Driver {
 public:
  bool begin();
  bool readOnce();
  float pm25() const;
  float temperature() const;
  float humidity() const;
 private:
  float pm25_ = 0, t_ = 0, rh_ = 0;
};
