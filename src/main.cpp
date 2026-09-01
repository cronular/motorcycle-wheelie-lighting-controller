#include <Arduino.h>
#include <esp_ota_ops.h>

#include "firmware_api.h"
#include "framework/firmware_module.h"
#include "modules/dashboard_module.h"
#include "modules/network_module.h"
#include "modules/operating_mode_module.h"
#include "modules/output_module.h"
#include "modules/sensor_module.h"
#include "modules/storage_module.h"

namespace {
firmware::ModuleRegistry<6> modules;

bool registerModules() {
    return modules.add(firmware::operatingModeModule()) &&
        modules.add(firmware::outputModule()) &&
        modules.add(firmware::storageModule()) &&
        modules.add(firmware::dashboardModule()) &&
        modules.add(firmware::sensorModule()) &&
        modules.add(firmware::networkModule());
}
}

void setup() {
    using namespace firmware;

    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("Motorcycle Wheelie Controller");
    Serial.print("Firmware ");
    Serial.println(FIRMWARE_VERSION);
    Serial.printf("Build %s at %s (%s channel)\n", BUILD_COMMIT, BUILD_DATE, RELEASE_CHANNEL);
    Serial.printf("Target %s / %s\n", TARGET_BOARD_ID, TARGET_CHIP_ID);
    Serial.println("Modular sensor, output, storage, network, dashboard, and mode runtime");
    Serial.println("--------------------------------");

    // Establish the hardware-safe output before any module can initialize.
    pinMode(OUTPUT1_PWM_PIN, OUTPUT);
    digitalWrite(OUTPUT1_PWM_PIN, LOW);

    if (!registerModules()) {
        Serial.println("FATAL: firmware module registry capacity exceeded");
        while (true) delay(1000);
    }
    modules.beginAll();

    oled.clearDisplay();
    displayDirty = true;
    Serial.println();
    Serial.println("CONTROLLER READY");
    Serial.printf("Modules: %u\n", static_cast<unsigned>(modules.size()));
    Serial.printf("Angle mode: %s\n", getAngleModeName());
    Serial.printf("Boot mode: %s\n", getModeName());
    Serial.printf("Wi-Fi: %s\n", wifiApSsid.c_str());
    Serial.println("Try: http://wheelie.local");
    Serial.println("Fallback: http://192.168.4.1");
    Serial.println("Button: 1x page, 2x angle mode, 3x Wi-Fi AP, hold ARM/STANDBY, 30s reset Wi-Fi password");
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println();
}

void loop() {
    modules.tickAll();
    delay(3);
}
