#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "firmware_runtime.h"
#include "framework/firmware_module.h"

namespace firmware {

const char* getStateName();
const char* getModeName();
const char* getAngleModeName();
const char* getAdaptiveFreezeReasonName();
const char* getModelOutcomeName(ModelEventOutcome outcome);
const char* getModelLabelName(ModelEventLabel label);
const char* getRotationAxisName();
const char* getAxisName(RotationAxis axis);
const char* getAxisJsonName(RotationAxis axis);

void validateSettings();
void loadSettings();
void saveSettings();

void writeMPURegister(uint8_t reg, uint8_t value);
bool readMPU(MPUData& data);
float getGyroRateForAxis(const MPUData& data, RotationAxis axis);
float getSelectedGyroRate(const MPUData& data);
float calculateAccelAngleRelative(RotationAxis axis, const MPUData& data);
bool runOrientationWizard();
bool calibrateMPU();
void resetAdaptiveBaselineToCurrent();
void updateAngleProcessing(float dt);
bool updateIMUAndPitch(float& dt);

void writePWM(uint8_t brightness);
void initializePWM();
void forceOutputOff();
void flashCalibrationComplete();
void setOutputTarget(uint8_t target, bool immediate = false);
void updateOutputFade();
void updateManualOutputTest();

void oledPrintRow(uint8_t row, const char* text);
void drawBootScreen();
void drawCalibrationScreen(const char* line5 = "Please wait...");
void drawStandbyScreen();
void drawStatusPage();
void drawSettingsPage();
void drawNetworkPage();
void drawDiagnosticsPage();
void drawWiFiPasswordResetScreen();
void updateDisplay();
void updateSerialStatus();

void loadRiderModel();
void saveRiderModel();
float recommendedRiderFreezeRate();
void addCurrentModelSample(ModelEventAccumulator& accumulator, uint32_t now);
void beginCapturedModelEvent(uint32_t now);
void finalizeCapturedModelEvent(uint32_t now);
void updateRiderModel();
String ridePathForSlot(uint8_t slot);
void copyRideText(char* destination, size_t size, const char* value);
bool readRideHeader(uint8_t slot, RideLogHeader& header);
void writeActiveRideHeader();
void recoverRideFile(uint8_t slot);
void initializeRideLogging();
void startRideSession();
void finishRideSession(bool capacityReached = false);
void updateRideLogging();

void setOperatingMode(OperatingMode mode);
void toggleOperatingMode();
void cycleDisplayPage();
void toggleAngleModeFromButton();
void registerShortTap(unsigned long now);
void resolveShortTapGesture();
void resetWiFiPasswordToDefault();
void updateButton();
void updateHighAngleWarning(float triggerPitch);
void updateController(float triggerPitch);

String makeWriteToken();
bool validWriteToken();
bool consumeWriteRequestLimit();
bool allowWriteRequest();
bool requireWriteToken();
void rotateWriteToken();
String generateUniqueWiFiPassword();
void handleRoot();
void handleSettingsPage();
void handleCapturePage();
void handleCaptivePortalRedirect();
void handleStatus();
void handleRiderModel();
void handleRiderModelCsv();
void handleRiderModelFeedback();
void handleRiderModelReset();
String rideDigestHex(const uint8_t digest[32]);
uint8_t collectRideHeaders(RideLogHeader headers[RIDE_LOG_MAX_SESSIONS], uint8_t slots[RIDE_LOG_MAX_SESSIONS]);
bool findRideSession(uint32_t sessionId, RideLogHeader& header, uint8_t& slot);
void handleRideList();
bool requestedRide(RideLogHeader& header, uint8_t& slot);
void handleRideCsv();
void handleRideReport();
void handleSettings();
void handleMode();
void handleCalibration();
void handleManualOutput();
void handlePeakReset();
void handleWiFiPassword();
void resetFirmwareUploadState();
void failFirmwareUpload(const String& reason);
String firmwareManifestValue(const char* key);
bool isLowerHexSha256(const String& value);
String digestToHex(const uint8_t digest[32]);
bool verifyFirmwareManifestSignature();
bool prepareFirmwarePayload();
void parseFirmwarePackageHeader();
void processFirmwarePackageBytes(uint8_t* data, size_t size);
void finishFirmwarePackage();
void handleFirmwareUpdateFinished();
void handleFirmwareUpload();
void handleOrientationWizard();
void handleFirmwareRollback();
void registerWebRoutes();
void startAccessPoint();
void stopAccessPoint();
void scheduleAccessPointRestart(unsigned long delayMs = 750);
void updateAccessPointRestart();
void toggleAccessPointFromButton();
void initializeNetwork();

} // namespace firmware
