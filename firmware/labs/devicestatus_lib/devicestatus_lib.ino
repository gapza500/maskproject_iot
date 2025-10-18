#include "device_status.h"
#include "sen66_status.h"

DeviceStatusReport report;

void loop() {
    // after you read SEN66 status value:
    SEN66DeviceStatus ds{};
    sen.readAndClearDeviceStatus(ds);

    report.sen66 = evaluateSen66Status(ds.value);
    printSen66Status(ds.value);

    Serial.print("Summary -> SEN66: ");
    Serial.println(statusToString(report.sen66));
}
