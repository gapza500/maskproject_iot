/** =========================================================================
 * @example{lineno} GetValues.ino
 * @author Sara Geleskie Damiano <sdamiano@stroudcenter.org>
 * @license This example is published under the BSD-3 license.
 *
 * @brief This prints basic meta-data about a sensor to the first serial port
 * and then begins taking measurements from the sensor.
 *
 * @m_examplenavigation{example_get_values,}
 * @m_footernavigation
 * ======================================================================= */

// ==========================================================================
//  Include the libraries required for any data logger
// ==========================================================================
#include <Arduino.h>
#include <ANBSensorsModbus.h>

// ==========================================================================
//  Sensor Settings
// ==========================================================================

// Define the sensor's modbus address, or SlaveID
byte modbusAddress = 0x55;  // HEX 0x55 is the ANB default modbus address.

// The Modbus baud rate the sensor uses
int32_t modbusBaud = 57600;  // 57600 is ANB default baud rate.

// Sensor Timing
// Edit these to explore
#define WARM_UP_TIME \
    30000L  // milliseconds to wait for sensor to respond to commands.

#define STABILIZATION_TIME 100  // milliseconds for readings to stabilize.

#define MEASUREMENT_TIME 300000  // milliseconds to complete a measurement.

#define TEST_POWER false

// ==========================================================================
//  Data Logger Options
// ==========================================================================
const int32_t serialBaud = 115200;  // Baud rate for serial monitor

// Define pin number variables
const int sensorPwrPin  = 22;  // The pin sending power to the sensor
const int adapterPwrPin = 22;  // The pin sending power to the RS485 adapter
const int DEREPin =
    -1;  // The pin controlling Receive Enable and Driver Enable
         // on the RS485 adapter, if applicable (else, -1)
         // Setting HIGH enables the driver (arduino) to send text
         // Setting LOW enables the receiver (sensor) to send text

// Turn on debugging outputs (i.e. raw Modbus requests & responses)
// by uncommenting next line (i.e. `#define DEBUG`)
#define DEBUG

// ==========================================================================
// Create and Assign a Serial Port for Modbus
// ==========================================================================
// Hardware serial ports are preferred when available.
// AltSoftSerial is the most stable alternative for modbus.
//   Select over alternatives with the define below.
// #define BUILD_ALTSOFTSERIAL  // Comment-out if you prefer alternatives

#if defined(BUILD_ALTSOFTSERIAL) && defined(__AVR__)
#include <AltSoftSerial.h>
AltSoftSerial modbusSerial;

#elif defined(ARDUINO_AVR_UNO) || defined(ARDUINO_AVR_FEATHER328P)
// The Uno only has 1 hardware serial port, which is dedicated to communication
// with the computer. If using an Uno, you will be restricted to using
// AltSofSerial or SoftwareSerial
#include <SoftwareSerial.h>
const int SSRxPin =
    10;  // Receive pin for software serial (Rx on RS485 adapter)
const int SSTxPin = 11;  // Send pin for software serial (Tx on RS485 adapter)
#pragma message("Using Software Serial for the Uno on pins 10 and 11")
SoftwareSerial modbusSerial(SSRxPin, SSTxPin);

#elif defined(ESP8266)
#include <SoftwareSerial.h>
#pragma message("Using Software Serial for the ESP8266")
SoftwareSerial modbusSerial;

#elif defined(NRF52832_FEATHER) || defined(ARDUINO_NRF52840_FEATHER)
#pragma message("Using TinyUSB for the NRF52")
#include <Adafruit_TinyUSB.h>
HardwareSerial& modbusSerial = Serial1;

#elif !defined(NO_GLOBAL_SERIAL1) && !defined(STM32_CORE_VERSION)
// This is just a assigning another name to the same port, for convenience
// Unless it is unavailable, always prefer hardware serial.
#pragma message("Using HardwareSerial / Serial1")
HardwareSerial& modbusSerial = Serial1;

#else
// This is just a assigning another name to the same port, for convenience
// Unless it is unavailable, always prefer hardware serial.
#pragma message("Using HardwareSerial / Serial")
HardwareSerial& modbusSerial = Serial;
#endif

// Construct the anbSensor instance
anbSensor sensor;
bool      success;

// ==========================================================================
// Working Functions
// ==========================================================================
// A function for pretty-printing the Modbuss Address in Hexadecimal notation,
// from ANBSensorsModbus `sensorLocation()`
String prettyprintAddressHex(byte _modbusAddress) {
    String addressHex = F("0x");
    if (_modbusAddress < 0x10) { addressHex += "0"; }
    addressHex += String(_modbusAddress, HEX);
    return addressHex;
}

void setSensorPower(bool power) {
    if (sensorPwrPin >= 0) { digitalWrite(sensorPwrPin, power ? HIGH : LOW); }
    if (adapterPwrPin >= 0 && adapterPwrPin != sensorPwrPin) {
        digitalWrite(adapterPwrPin, power ? HIGH : LOW);
    }
}

// ==========================================================================
//  Arduino Setup Function
// ==========================================================================
void setup() {
// Wait for USB connection to be established by PC
// NOTE:  Only use this when debugging - if not connected to a PC, this
// could prevent the script from starting
#if defined(SERIAL_PORT_USBVIRTUAL)
    while (!SERIAL_PORT_USBVIRTUAL && (millis() < 10000)) {
        // wait
    }
#endif

    // Turn on the "main" serial port for debugging via USB Serial Monitor
    Serial.begin(serialBaud);

    // Set pin modes
    if (sensorPwrPin >= 0) { pinMode(sensorPwrPin, OUTPUT); }
    if (adapterPwrPin >= 0 && adapterPwrPin != sensorPwrPin) {
        pinMode(adapterPwrPin, OUTPUT);
    }
    if (DEREPin >= 0) { pinMode(DEREPin, OUTPUT); }
#if defined(LED_BUILTIN)
    if (LED_BUILTIN >= 0) { pinMode(LED_BUILTIN, OUTPUT); }
#endif

    // Turn on your modbus serial port
#if defined(ESP8266)
    const int SSRxPin =
        13;  // Receive pin for software serial (Rx on RS485 adapter)
    const int SSTxPin =
        14;  // Send pin for software serial (Tx on RS485 adapter)
    modbusSerial.begin(modbusBaud, SWSERIAL_8N1, SSRxPin, SSTxPin, false);
#else  // For Hardware Serial
    modbusSerial.begin(modbusBaud);
#endif

    // Start up the connection to the ANB pH sensor
    sensor.begin(modbusAddress, &modbusSerial, DEREPin);

// Turn on debugging
#ifdef DEBUG
    sensor.setDebugStream(&Serial);
#endif

    // Start up note
    Serial.print(F("\nTesting ANB pH Sensor"));
    Serial.println();

    // Confirm Modbus Address
    Serial.println(F("Selected modbus address:"));
    Serial.print(F("  Decimal: "));
    Serial.print(modbusAddress, DEC);
    Serial.print(F(", Hexidecimal: "));
    Serial.println(prettyprintAddressHex(modbusAddress));
    Serial.println();

    Serial.println(F("Holding with power off for 15s"));
    setSensorPower(false);
    for (size_t i = 0; i < 15; i++) {
        delay(1000L);
        Serial.print(F("."));
    }
    Serial.println();

    // Turn on power pins
    Serial.println(F("Powering on"));
    setSensorPower(true);

    // Allow the sensor and converter to warm up
    Serial.print(F("Waiting up to "));
    Serial.print(WARM_UP_TIME);
    Serial.print(F(" ms for the sensor to be ready... "));
    bool     isReady   = false;
    uint32_t startTime = millis();
    do {
        delay(250);
        isReady = sensor.isSensorReady();
    } while (!isReady && millis() - startTime <= WARM_UP_TIME);
    if (isReady) {
        Serial.print(F("Sensor ready after "));
        Serial.print(millis() - startTime);
        Serial.println(F(" ms"));
    } else {
        Serial.print(F("Timed out waiting for ready after "));
        Serial.print(millis() - startTime);
        Serial.println(F(" ms"));
    }

    // // Enable modbus
    // Serial.println(F("Enabling Modbus... "));
    // bool isEnabled = sensor.enableModbus();
    // Serial.print(F(" ..."));
    // Serial.println(isEnabled ? F("success") : F("failed"));

    // Try to force modbus if the enable failed
    if (!isReady) {
        Serial.println(F("Trying to get any modbus response... "));
        bool gotModbusResponse = sensor.gotModbusResponse();
        if (!gotModbusResponse) {
            Serial.println(F("Did not get a modbus response, trying to force "
                             "Modbus enable... "));
            sensor.forceModbus();
            Serial.println(F("Trying again get a modbus response... "));
            gotModbusResponse = sensor.gotModbusResponse();
            Serial.print(F(" ..."));
            Serial.println(gotModbusResponse ? F("success") : F("failed"));
        }
    }

    // Get Sensor Information
    Serial.print(F("\n\n\nGet sensor information...\n"));

    // Get the sensor serial number
    Serial.println(F("Getting sensor serial number."));
    String SN = sensor.getSerialNumber();
    Serial.print(F("    Serial Number: "));
    Serial.println(SN);

    // Get the sensor manufacturer
    Serial.println(F("Getting sensor manufacturer."));
    String manufacturer = sensor.getManufacturer();
    Serial.print(F("    Manufacturer: "));
    Serial.println(manufacturer);

    // Get the sensor name
    Serial.println(F("Getting sensor name."));
    String name = sensor.getName();
    Serial.print(F("    Name: "));
    Serial.println(name);

    // Get the sensor sub-name
    Serial.println(F("Getting sensor sub-name."));
    String subname = sensor.getSubName();
    Serial.print(F("    Subname: "));
    Serial.println(subname);

    // Get the sensor interface version
    Serial.println(F("Getting sensor interface version."));
    String interfaceVersion = sensor.getInterfaceVersion();
    Serial.print(F("    Interface Version: "));
    Serial.println(interfaceVersion);

    // Get the sensor driver version
    Serial.println(F("Getting sensor driver version."));
    String driverVersion = sensor.getDriverVersion();
    Serial.print(F("    Driver Version: "));
    Serial.println(driverVersion);

    // Get the sensor RTC
    Serial.println(F("Getting sensor RTC."));
    uint16_t seconds = -1;
    uint16_t minutes = -1;
    uint16_t hours   = -1;
    uint16_t day     = -1;
    uint16_t month   = -1;
    uint16_t year    = -1;
    sensor.getRTC(seconds, minutes, hours, day, month, year);
    Serial.print(F("    RTC: "));
    Serial.print(month);
    Serial.print(F("/"));
    Serial.print(day);
    Serial.print(F("/"));
    Serial.print(year);
    Serial.print(F(" "));
    Serial.print(hours);
    Serial.print(F(":"));
    Serial.print(minutes);
    Serial.print(F(":"));
    Serial.print(seconds);
    Serial.println();

    // Get Readable Sensor Configuration
    Serial.print(F("\n\nGet sensor configuration...\n"));

#if 0
    // Get Sensor Modbus Baud
    Serial.println(F("Get sensor modbus baud setting."));
    ANBSensorBaud sensorBaud = sensor.getBaud();
    Serial.print(F("  Baud: "));
    Serial.println(static_cast<uint16_t>(sensorBaud));
    Serial.println();

    // Get Sensor Control Mode
    Serial.println(F("Get sensor control mode."));
    ANBSensorMode controlMode = sensor.getControlMode();
    Serial.print(F("  Control Mode: "));
    Serial.println(static_cast<uint16_t>(controlMode));
    Serial.println();

    // Get Sensor Salinity Mode
    Serial.println(F("Get sensor salinity mode."));
    ANBSalinityMode salinityMode = sensor.getSalinityMode();
    Serial.print(F("  Salinity Mode: "));
    Serial.println(static_cast<uint16_t>(salinityMode));
    Serial.println();

    // Get Sensor Power Style
    Serial.println(F("Get sensor power style."));
    ANBPowerStyle powerStyle = sensor.getPowerStyle();
    Serial.print(F("  Power Style: "));
    Serial.println(static_cast<uint16_t>(powerStyle));
    Serial.println();
#endif

    // Get Sensor Sampling Interval
    Serial.println(F("Get sensor sampling interval."));
    uint8_t samplingInterval = sensor.getIntervalTime();
    Serial.print(F("  Sampling Interval: "));
    Serial.println(samplingInterval);
    Serial.println();

    // Get current immersion rule settings
    Serial.println(F("Get current immersion rule settings."));
    bool immersionRule = sensor.isImmersionSensorEnabled();
    Serial.print(F("  Immersion sensor is "));
    Serial.println(immersionRule ? "enabled" : "disabled");

    // Configure sensor
    Serial.print(F("\n\nConfigure sensor...\n"));

    // Set Sensor Control Mode
    Serial.print(F("Set sensor control mode to controlled... "));
    bool modeSet = sensor.setControlMode(ANBSensorMode::CONTROLLED);
    Serial.print(F(" ..."));
    Serial.println(modeSet ? F("success") : F("failed"));

    // Set Sensor Salinity Mode
    Serial.print(F("Set sensor salinity mode to low salinity... "));
    bool salinitySet = sensor.setSalinityMode(ANBSalinityMode::LOW_SALINITY);
    Serial.print(F(" ..."));
    Serial.println(salinitySet ? F("success") : F("failed"));

// Set Sensor Power Style
#if TEST_POWER
    Serial.print(F("Set sensor power style to on measurement... "));
    bool powerStyleSet = sensor.setPowerStyle(ANBPowerStyle::ON_MEASUREMENT);
#else
    Serial.print(F("Set sensor power style to always powered... "));
    bool powerStyleSet = sensor.setPowerStyle(ANBPowerStyle::ALWAYS_POWERED);
#endif
    Serial.print(F(" ..."));
    Serial.println(powerStyleSet ? F("success") : F("failed"));

    // Set Sampling Interval Time
    Serial.print(F("Set sensor sampling interval to 0 (continuous)... "));
    bool intervalSet = sensor.setIntervalTime(0);
    Serial.print(F(" ..."));
    Serial.println(intervalSet ? F("success") : F("failed"));

    // Set Immersion Rule
    Serial.print(F("Set sensor immersion rule to enabled... "));
    bool immersionSet = sensor.enableImmersionSensor();
    Serial.print(F(" ..."));
    Serial.println(immersionSet ? F("success") : F("failed"));

#if 0
    // Reboot the sensor after configuration to save and apply settings
    Serial.print(F("\n\nRebooting sensor... "));
    bool rebooted = sensor.reboot();
    Serial.println(rebooted ? F("success") : F("failed"));

    Serial.println(F("\n\nForcing modbus after reboot... "));
    sensor.forceModbus();
#endif

#if TEST_POWER
    Serial.println(F("\n\n\nHolding with power off for 15 seconds..."));
    setSensorPower(false);
    for (size_t i = 0; i < 15; i++) {
        delay(1000L);
        Serial.print(F("."));
    }
    Serial.println();
#endif
}

// ==========================================================================
//  Arduino Loop Function
// ==========================================================================
void loop() {
#if TEST_POWER
    // Turn on power pins
    Serial.println(F("Powering on"));
    setSensorPower(true);
#endif

    // Allow the sensor and converter to warm up
    Serial.print(F("Waiting up to "));
    Serial.print(WARM_UP_TIME);
    Serial.print(F(" ms for the sensor to be ready... "));
    bool     isReady   = false;
    uint32_t startTime = millis();
    do {
        delay(250);
        isReady = sensor.isSensorReady();
    } while (!isReady && millis() - startTime <= WARM_UP_TIME);
    if (isReady) {
        Serial.print(F("Sensor ready after "));
        Serial.print(millis() - startTime);
        Serial.println(F(" ms"));
    } else {
        Serial.print(F("Timed out waiting for ready after "));
        Serial.print(millis() - startTime);
        Serial.println(F(" ms"));
    }

    Serial.println(F("\n\nStarting a scan... "));
    bool scanStarted = sensor.start();
    Serial.print(F(" ..."));
    Serial.println(scanStarted ? F("success") : F("failed"));
    if (!scanStarted) {
        // Wait before the next attempt
        Serial.println(F("Waiting before the next attempt..."));
        for (size_t i = 0; i < 20; i++) {
            delay(1000L);
            Serial.print(F("."));
        }
        Serial.println('\n');
        return;
    } else {
#if defined(LED_BUILTIN)
        digitalWrite(LED_BUILTIN, HIGH);
#endif
    }

    startTime = millis();
    Serial.println(F("\n\nWaiting for measurement to be ready... "));
    delay(6000L);
    while (!sensor.isMeasurementComplete() &&
           millis() - startTime <= MEASUREMENT_TIME) {
        if (millis() - startTime > MEASUREMENT_TIME) {
            Serial.print(F("Measurement timed out after "));
            Serial.print(MEASUREMENT_TIME / 60000);
            Serial.println(F(" minutes."));
            return;
        }
        for (size_t i = 0; i < 15; i++) {
            delay(1000L);
            Serial.print(F("."));
        }
    }
    Serial.print(F("Measurement took "));
    Serial.print(millis() - startTime);
    Serial.println(F(" milliseconds."));

    Serial.println(F("\n\nGetting results in bulk..."));
    float             pH2, temperature2, salinity2, spcond2, raw_cond2;
    ANBHealthCode     health2;
    ANBStatusCode     status2;
    ANBDiagnosticCode diagStatus2;
    sensor.getValues(pH2, temperature2, salinity2, spcond2, raw_cond2, health2,
                     status2, diagStatus2);
    Serial.print(F("  pH: "));
    Serial.println(pH2);
    Serial.print(F("  Temperature: "));
    Serial.println(temperature2);
    Serial.print(F("  Salinity: "));
    Serial.println(salinity2);
    Serial.print(F("  Specific Conductance: "));
    Serial.println(spcond2);
    Serial.print(F("  Raw Conductivity: "));
    Serial.println(raw_cond2);
    Serial.print(F("  Health: "));
    Serial.println(static_cast<uint16_t>(health2));
    Serial.print(F("  Status: "));
    Serial.println(static_cast<uint16_t>(status2));
    Serial.print(F("  Diagnostic Status: "));
    Serial.println(static_cast<uint16_t>(diagStatus2));

    Serial.println(F("\n\nGetting results individually..."));
    float             pH          = sensor.getpH();
    float             temperature = sensor.getTemperature();
    float             salinity    = sensor.getSalinity();
    float             spcond      = sensor.getSpecificConductance();
    float             raw_cond    = sensor.getRawConductivity();
    ANBHealthCode     health      = sensor.getHealthCode();
    ANBStatusCode     status      = sensor.getStatusCode();
    ANBDiagnosticCode diagStatus  = sensor.getDiagnosticCode();
    Serial.print(F("  pH: "));
    Serial.println(pH);
    Serial.print(F("  Temperature: "));
    Serial.println(temperature);
    Serial.print(F("  Salinity: "));
    Serial.println(salinity);
    Serial.print(F("  Specific Conductance: "));
    Serial.println(spcond);
    Serial.print(F("  Raw Conductivity: "));
    Serial.println(raw_cond);
    Serial.print(F("  Health: "));
    Serial.println(static_cast<uint16_t>(health));
    Serial.print(F("  Status: "));
    Serial.println(static_cast<uint16_t>(status));
    Serial.print(F("  Diagnostic Status: "));
    Serial.println(static_cast<uint16_t>(diagStatus));


    Serial.println(F("\n\nStopping scan... "));
    bool scanStopped = sensor.stop();
    Serial.print(F(" ..."));
    Serial.println(scanStopped ? F("success") : F("failed"));
    if (scanStopped) {
#if defined(LED_BUILTIN)
        digitalWrite(LED_BUILTIN, LOW);
#endif
    }

// Wait for the next reading
#if TEST_POWER
    Serial.println(F("\n\nHolding with power off before the next reading..."));
    setSensorPower(false);
#else
    Serial.println(F("\n\nWaiting with power on for the next reading..."));
#endif
    for (size_t i = 0; i < 15; i++) {
        delay(1000L);
        Serial.print(F("."));
    }
    Serial.println(F("\n\n"));
}

// cspell: ignore DEREPin SWSERIAL spcond
