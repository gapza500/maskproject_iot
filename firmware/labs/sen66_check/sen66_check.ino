#include <Wire.h>
#include <SensirionI2cSen66.h>
#include <SensirionErrors.h>   // Use the official errorToString()

// ===== User pins (ESP32-C6 example; adjust if needed) =====
const int I2C_SDA_PIN = 19;
const int I2C_SCL_PIN = 20;

// ===== Constants =====
const uint8_t SEN66_ADDR = 0x6B;        // SEN6x I2C address (7-bit)
const uint32_t I2C_SPEED_HZ = 100000;   // SEN6x supports 100 kHz only

// ===== Objects =====
SensirionI2cSen66 sen;

// ===== Device-status bit masks (from datasheet 4.3.1) =====
#define BIT(n) (1UL << (n))
const uint32_t DS_WARN_SPEED = BIT(21);
const uint32_t DS_ERR_PM     = BIT(11);
const uint32_t DS_ERR_CO2_2  = BIT(9);
const uint32_t DS_ERR_GAS    = BIT(7);
const uint32_t DS_ERR_RHT    = BIT(6);
const uint32_t DS_ERR_FAN    = BIT(4);

// ===== Forward decls =====
bool scanI2C(uint8_t addr);
void printDeviceInfo();
void printDeviceStatus(uint32_t value);
void printFloat2(const char* label, float v);
void printLine();

// ===== Setup =====
void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }
    Serial.println();
    Serial.println(F("=== SEN66 Checker ==="));

    // --- I2C init ---
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(I2C_SPEED_HZ);
    Serial.printf("I2C started on SDA=%d, SCL=%d @ %lu Hz\n",
                  I2C_SDA_PIN, I2C_SCL_PIN, (unsigned long)I2C_SPEED_HZ);

    // --- Probe address ---
    if (!scanI2C(SEN66_ADDR)) {
        Serial.println(F("FATAL: SEN66 not found at 0x6B. Check wiring/power."));
        while (true) { delay(1000); }
    }
    Serial.println(F("SEN66 ACKed at 0x6B"));

    // --- Bind driver ---
    sen.begin(Wire, SEN66_ADDR);

    // --- Reset device and wait ---
    {
        int16_t err = sen.deviceReset();
        if (err) {
            char msg[128];
            errorToString((uint16_t)err, msg, sizeof(msg));
            Serial.print(F("deviceReset() error: "));
            Serial.println(msg);
            while (true) { delay(1000); }
        }
        delay(200); // Datasheet: ~100 ms startup after reset
    }

    // --- Print device info ---
    printDeviceInfo();

    // --- Start measurement ---
    {
        int16_t err = sen.startContinuousMeasurement();
        if (err) {
            char msg[128];
            errorToString((uint16_t)err, msg, sizeof(msg));
            Serial.print(F("startContinuousMeasurement() error: "));
            Serial.println(msg);
            while (true) { delay(1000); }
        }
        Serial.println(F("Measurement started. Waiting for data..."));
    }
}

// ===== Loop =====
void loop() {
    // 1) Poll data-ready
    {
        uint8_t padding = 0;
        bool ready = false;
        int16_t err = sen.getDataReady(padding, ready);
        if (err) {
            char msg[128];
            errorToString((uint16_t)err, msg, sizeof(msg));
            Serial.print(F("getDataReady() error: "));
            Serial.println(msg);
            delay(500);
            return;
        }
        if (!ready) {
            delay(200);
            return; // Not ready yet
        }
    }

    // 2) Read measured values
    float pm1, pm25, pm4, pm10, rh, tC, vocIdx, noxIdx;
    uint16_t co2ppm;
    {
        int16_t err = sen.readMeasuredValues(pm1, pm25, pm4, pm10, rh, tC, vocIdx, noxIdx, co2ppm);
        if (err) {
            char msg[128];
            errorToString((uint16_t)err, msg, sizeof(msg));
            Serial.print(F("readMeasuredValues() error: "));
            Serial.println(msg);
            delay(500);
            return;
        }
    }

    // 3) Read & clear device status
    {
        SEN66DeviceStatus ds{};
        int16_t err = sen.readAndClearDeviceStatus(ds);
        if (err) {
            char msg[128];
            errorToString((uint16_t)err, msg, sizeof(msg));
            Serial.print(F("readAndClearDeviceStatus() error: "));
            Serial.println(msg);
        } else {
            printDeviceStatus(ds.value);
        }
    }

    // 4) Print results
    printLine();
    Serial.println(F("New measurement:"));
    printFloat2("PM1.0 (µg/m³): ", pm1);
    printFloat2("PM2.5 (µg/m³): ", pm25);
    printFloat2("PM4.0 (µg/m³): ", pm4);
    printFloat2("PM10  (µg/m³): ", pm10);
    printFloat2("RH       (%RH): ", rh);
    printFloat2("Temp       (°C): ", tC);
    printFloat2("VOC Index     : ", vocIdx);
    printFloat2("NOx Index     : ", noxIdx);
    Serial.print  (F("CO2     (ppm): ")); Serial.println(co2ppm);
    printLine();

    delay(1000);
}

// ===== Helpers =====
bool scanI2C(uint8_t addr) {
    Wire.beginTransmission(addr);
    uint8_t rc = Wire.endTransmission();
    return (rc == 0);
}

void printDeviceInfo() {
    int8_t prodNameBuf[49] = {0};
    int8_t serialBuf[49]   = {0};
    uint8_t fwMaj = 0, fwMin = 0;

    if (sen.getProductName(prodNameBuf, 48) == 0) {
        Serial.print(F("Product: "));
        Serial.println((const char*)prodNameBuf);
    } else Serial.println(F("Product: <read error>"));

    if (sen.getSerialNumber(serialBuf, 48) == 0) {
        Serial.print(F("Serial : "));
        Serial.println((const char*)serialBuf);
    } else Serial.println(F("Serial : <read error>"));

    if (sen.getVersion(fwMaj, fwMin) == 0) {
        Serial.print(F("FW ver : "));
        Serial.print(fwMaj); Serial.print('.');
        Serial.println(fwMin);
    } else Serial.println(F("FW ver : <read error>"));
}

void printDeviceStatus(uint32_t v) {
    Serial.print(F("DeviceStatus=0x"));
    Serial.println(v, HEX);

    bool any = false;
    if (v & DS_WARN_SPEED) { Serial.println(F("  WARN: Fan speed out of range (±10%)")); any = true; }
    if (v & DS_ERR_PM   )  { Serial.println(F("  ERR : PM sensor error")); any = true; }
    if (v & DS_ERR_CO2_2)  { Serial.println(F("  ERR : CO2 sensor error (SEN66)")); any = true; }
    if (v & DS_ERR_GAS  )  { Serial.println(F("  ERR : GAS (VOC/NOx) sensor error")); any = true; }
    if (v & DS_ERR_RHT  )  { Serial.println(F("  ERR : RH&T sensor error")); any = true; }
    if (v & DS_ERR_FAN  )  { Serial.println(F("  ERR : FAN error (stalled/blocked)")); any = true; }
    if (!any) Serial.println(F("  OK  : No errors/warnings"));
}

void printFloat2(const char* label, float v) {
    Serial.print(label);
    if (isnan(v)) Serial.println(F("nan"));
    else          Serial.println(v, 2);
}

void printLine() {
    Serial.println(F("------------------------------"));
}