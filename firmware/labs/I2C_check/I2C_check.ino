#include <Wire.h>

// --- PIN CONFIGURATION ---
// Based on the code you were using.
const int I2C_SDA_PIN = 19;
const int I2C_SCL_PIN = 20;

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println("\nI2C Scanner");
  Serial.print("Scanning on SDA Pin ");
  Serial.print(I2C_SDA_PIN);
  Serial.print(" and SCL Pin ");
  Serial.print(I2C_SCL_PIN);
  Serial.println("...");

  // Initialize the I2C bus with our specified SDA and SCL pins
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  byte error, address;
  int deviceCount = 0;

  for (address = 1; address < 127; address++) {
    // The i2c_scanner checks for devices on each address
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) {
        Serial.print("0");
      }
      Serial.println(address, HEX);
      deviceCount++;
    } else if (error == 4) {
      Serial.print("Unknown error at address 0x");
      if (address < 16) {
        Serial.print("0");
      }
      Serial.println(address, HEX);
    }
  }

  // Final summary
  Serial.println();
  if (deviceCount == 0) {
    Serial.println("No I2C devices found. Check wiring.");
  } else {
    Serial.print(deviceCount);
    Serial.println(" device(s) found.");
    Serial.println("You can now use this address in your main sketch.");
  }
}

void loop() {
  // This sketch runs once and then stops.
}