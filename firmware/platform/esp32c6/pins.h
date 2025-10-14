
#pragma once
// Adjust to your wiring
#if defined(LED_BUILTIN)
  constexpr int LED_PIN = LED_BUILTIN;
#else
  constexpr int LED_PIN = 2;
#endif
constexpr int I2C_SDA_PIN = 19;
constexpr int I2C_SCL_PIN = 20;
