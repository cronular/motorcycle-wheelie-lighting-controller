#include "firmware_api.h"
#include "modules/network_module.h"

namespace firmware {

String makeWriteToken() {
    char buffer[17];
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    snprintf(buffer, sizeof(buffer), "%08lX%08lX", (unsigned long)a, (unsigned long)b);
    return String(buffer);
}

bool validWriteToken() {
    if (!server.hasArg("token")) return false;
    const String supplied = server.arg("token");
    if (supplied.length() != writeToken.length()) return false;
    uint8_t difference = 0;
    for (size_t index = 0; index < writeToken.length(); ++index) {
        difference |= (uint8_t)supplied[index] ^ (uint8_t)writeToken[index];
    }
    return difference == 0;
}

bool consumeWriteRequestLimit() {
    const uint32_t clientKey = (uint32_t)server.client().remoteIP();
    return checkWriteRateLimit(
        writeRateBuckets, WRITE_RATE_BUCKET_COUNT, clientKey, millis()) ==
        WriteRateLimitDecision::ALLOW;
}

bool allowWriteRequest() {
    if (!consumeWriteRequestLimit()) {
        server.sendHeader("Retry-After", "30");
        server.send(429, "text/plain", "Too many write requests — retry in 30 seconds");
        return false;
    }
    return true;
}

bool requireWriteToken() {
    if (!allowWriteRequest()) return false;
    if (!validWriteToken()) {
        server.send(403, "text/plain", "Invalid write token");
        return false;
    }
    return true;
}

void rotateWriteToken() {
    writeToken = makeWriteToken();
    server.sendHeader("X-Write-Token", writeToken);
}

String generateUniqueWiFiPassword() {
    static constexpr char alphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    String generated;
    generated.reserve(16);
    for (uint8_t index = 0; index < 16; ++index) {
        generated += alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    return generated;
}

// =====================================================
// WEB DASHBOARD
// =====================================================

void resetFirmwareUploadState() {
    if (firmwareUpload.hashInitialized) {
        mbedtls_sha256_free(&firmwareUpload.hashContext);
    }
    if (firmwareUpload.updateStarted && Update.isRunning()) {
        Update.abort();
    }
    firmwareUpload = FirmwareUploadState{};
}

void failFirmwareUpload(const String& reason) {
    if (firmwareUpload.failed) return;
    firmwareUpload.failed = true;
    firmwareUpload.error = reason;
    if (firmwareUpload.updateStarted && Update.isRunning()) {
        Update.abort();
    }
    firmwareUpload.updateStarted = false;
    if (firmwareUpload.hashInitialized) {
        mbedtls_sha256_free(&firmwareUpload.hashContext);
        firmwareUpload.hashInitialized = false;
    }
    Serial.printf("OTA rejected: %s\n", reason.c_str());
}

String firmwareManifestValue(const char* key) {
    const String manifest(firmwareUpload.manifest);
    const String prefix = String(key) + "=";
    int start = manifest.indexOf(prefix);
    while (start >= 0 && start > 0 && manifest[start - 1] != '\n') {
        start = manifest.indexOf(prefix, start + 1);
    }
    if (start < 0) return "";
    start += prefix.length();
    int end = manifest.indexOf('\n', start);
    if (end < 0) return "";
    return manifest.substring(start, end);
}

bool isLowerHexSha256(const String& value) {
    if (value.length() != 64) return false;
    for (size_t index = 0; index < value.length(); ++index) {
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) return false;
    }
    return true;
}

String digestToHex(const uint8_t digest[32]) {
    static constexpr char hexadecimal[] = "0123456789abcdef";
    char output[65];
    for (size_t index = 0; index < 32; ++index) {
        output[index * 2] = hexadecimal[digest[index] >> 4];
        output[index * 2 + 1] = hexadecimal[digest[index] & 0x0F];
    }
    output[64] = '\0';
    return String(output);
}

bool verifyFirmwareManifestSignature() {
    uint8_t digest[32];
    mbedtls_sha256_context manifestHash;
    mbedtls_sha256_init(&manifestHash);
    if (mbedtls_sha256_starts(&manifestHash, 0) != 0 ||
        mbedtls_sha256_update(
            &manifestHash,
            reinterpret_cast<const uint8_t*>(firmwareUpload.manifest),
            firmwareUpload.manifestExpected) != 0 ||
        mbedtls_sha256_finish(&manifestHash, digest) != 0) {
        mbedtls_sha256_free(&manifestHash);
        return false;
    }
    mbedtls_sha256_free(&manifestHash);

    mbedtls_pk_context publicKey;
    mbedtls_pk_init(&publicKey);
    const int parseResult = mbedtls_pk_parse_public_key(
        &publicKey,
        reinterpret_cast<const uint8_t*>(FIRMWARE_SIGNING_PUBLIC_KEY_PEM),
        sizeof(FIRMWARE_SIGNING_PUBLIC_KEY_PEM));
    const int verifyResult = parseResult == 0 ? mbedtls_pk_verify(
        &publicKey,
        MBEDTLS_MD_SHA256,
        digest,
        sizeof(digest),
        firmwareUpload.signature,
        firmwareUpload.signatureExpected) : parseResult;
    mbedtls_pk_free(&publicKey);
    return verifyResult == 0;
}

bool prepareFirmwarePayload() {
    firmwareUpload.manifest[firmwareUpload.manifestExpected] = '\0';
    if (!verifyFirmwareManifestSignature()) {
        failFirmwareUpload("Manifest signature is invalid");
        return false;
    }

    const String format = firmwareManifestValue("format");
    const String board = firmwareManifestValue("board");
    const String chip = firmwareManifestValue("chip");
    firmwareUpload.packageVersion = firmwareManifestValue("version");
    const String channel = firmwareManifestValue("channel");
    firmwareUpload.packageCommit = firmwareManifestValue("commit");
    firmwareUpload.packageBuilt = firmwareManifestValue("built");
    const String size = firmwareManifestValue("size");
    firmwareUpload.expectedSha256 = firmwareManifestValue("sha256");

    if (format != "1") {
        failFirmwareUpload("Unsupported package format");
        return false;
    }
    if (!isFirmwareMetadataCompatible(
            board.c_str(), channel.c_str(), TARGET_BOARD_ID, otaChannel.c_str()) ||
        chip != TARGET_CHIP_ID) {
        failFirmwareUpload(
            String("Package requires board ") + board + " / " + chip +
            " on " + channel + " channel; this device is " + TARGET_BOARD_ID +
            " / " + TARGET_CHIP_ID + " on " + otaChannel);
        return false;
    }
    if ((uint32_t)size.toInt() != firmwareUpload.firmwareExpected ||
        !isLowerHexSha256(firmwareUpload.expectedSha256) ||
        firmwareUpload.packageVersion.length() == 0 ||
        firmwareUpload.packageCommit.length() == 0 ||
        firmwareUpload.packageBuilt.length() == 0) {
        failFirmwareUpload("Signed manifest metadata is incomplete or inconsistent");
        return false;
    }
    if (!Update.begin(firmwareUpload.firmwareExpected)) {
        Update.printError(Serial);
        failFirmwareUpload("Unable to reserve the OTA partition");
        return false;
    }
    firmwareUpload.updateStarted = true;
    mbedtls_sha256_init(&firmwareUpload.hashContext);
    firmwareUpload.hashInitialized = true;
    if (mbedtls_sha256_starts(&firmwareUpload.hashContext, 0) != 0) {
        failFirmwareUpload("Unable to initialize firmware SHA-256");
        return false;
    }
    return true;
}

void parseFirmwarePackageHeader() {
    if (!hasFirmwarePackageMagic(
            firmwareUpload.header, firmwareUpload.headerReceived)) {
        failFirmwareUpload("Unsigned .bin files are not accepted; choose a signed .wctrl package");
        return;
    }
    firmwareUpload.manifestExpected = readLittleEndian16(firmwareUpload.header + 8);
    firmwareUpload.signatureExpected = readLittleEndian16(firmwareUpload.header + 10);
    firmwareUpload.firmwareExpected = readLittleEndian32(firmwareUpload.header + 12);
    if (firmwareUpload.manifestExpected == 0 ||
        firmwareUpload.manifestExpected > FIRMWARE_MANIFEST_MAX_SIZE ||
        firmwareUpload.signatureExpected == 0 ||
        firmwareUpload.signatureExpected > FIRMWARE_SIGNATURE_MAX_SIZE ||
        firmwareUpload.firmwareExpected == 0) {
        failFirmwareUpload("Signed package header is invalid");
    }
}

void processFirmwarePackageBytes(uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size && !firmwareUpload.failed) {
        if (firmwareUpload.headerReceived < FIRMWARE_PACKAGE_HEADER_SIZE) {
            const size_t count = min(
                size - offset,
                FIRMWARE_PACKAGE_HEADER_SIZE - firmwareUpload.headerReceived);
            memcpy(firmwareUpload.header + firmwareUpload.headerReceived, data + offset, count);
            firmwareUpload.headerReceived += count;
            offset += count;
            if (firmwareUpload.headerReceived == FIRMWARE_PACKAGE_HEADER_SIZE) {
                parseFirmwarePackageHeader();
            }
            continue;
        }
        if (firmwareUpload.manifestReceived < firmwareUpload.manifestExpected) {
            const size_t count = min(
                size - offset,
                firmwareUpload.manifestExpected - firmwareUpload.manifestReceived);
            memcpy(firmwareUpload.manifest + firmwareUpload.manifestReceived, data + offset, count);
            firmwareUpload.manifestReceived += count;
            offset += count;
            continue;
        }
        if (firmwareUpload.signatureReceived < firmwareUpload.signatureExpected) {
            const size_t count = min(
                size - offset,
                firmwareUpload.signatureExpected - firmwareUpload.signatureReceived);
            memcpy(firmwareUpload.signature + firmwareUpload.signatureReceived, data + offset, count);
            firmwareUpload.signatureReceived += count;
            offset += count;
            if (firmwareUpload.signatureReceived == firmwareUpload.signatureExpected &&
                !prepareFirmwarePayload()) return;
            continue;
        }
        if (firmwareUpload.firmwareReceived < firmwareUpload.firmwareExpected) {
            const size_t count = min(
                size - offset,
                (size_t)(firmwareUpload.firmwareExpected - firmwareUpload.firmwareReceived));
            if (mbedtls_sha256_update(&firmwareUpload.hashContext, data + offset, count) != 0 ||
                Update.write(data + offset, count) != count) {
                Update.printError(Serial);
                failFirmwareUpload("Firmware write failed");
                return;
            }
            firmwareUpload.firmwareReceived += count;
            offset += count;
            continue;
        }
        failFirmwareUpload("Signed package contains unexpected trailing data");
    }
}

void finishFirmwarePackage() {
    if (firmwareUpload.failed) return;
    if (!firmwareUpload.updateStarted ||
        firmwareUpload.firmwareReceived != firmwareUpload.firmwareExpected) {
        failFirmwareUpload("Signed package ended before the complete firmware image arrived");
        return;
    }
    uint8_t digest[32];
    if (mbedtls_sha256_finish(&firmwareUpload.hashContext, digest) != 0) {
        failFirmwareUpload("Unable to finish firmware SHA-256");
        return;
    }
    mbedtls_sha256_free(&firmwareUpload.hashContext);
    firmwareUpload.hashInitialized = false;
    if (digestToHex(digest) != firmwareUpload.expectedSha256) {
        failFirmwareUpload("Firmware SHA-256 does not match the signed manifest");
        return;
    }
    if (!Update.end(false)) {
        Update.printError(Serial);
        failFirmwareUpload("Firmware image validation failed");
        return;
    }
    firmwareUpload.updateStarted = false;
    firmwareUpload.complete = true;
    Serial.printf(
        "Signed OTA verified: %s %s commit %s (%u bytes)\n",
        otaChannel.c_str(), firmwareUpload.packageVersion.c_str(),
        firmwareUpload.packageCommit.c_str(), firmwareUpload.firmwareReceived);
}

void handleFirmwareUpdateFinished() {
    const bool success = firmwareUpload.authorized && firmwareUpload.complete &&
        !firmwareUpload.failed;
    const int status = firmwareUpload.rateLimited ? 429 :
        (!firmwareUpload.authorized ? 403 : (success ? 200 : 400));
    const String response = success ?
        String("Signed firmware ") + firmwareUpload.packageVersion +
            " verified. Rebooting..." :
        (firmwareUpload.error.length() > 0 ? firmwareUpload.error : "Firmware update failed");
    if (firmwareUpload.rateLimited) server.sendHeader("Retry-After", "30");
    server.sendHeader("Connection", "close");
    server.send(status, "text/plain", response);
    if (success) {
        delay(400);
        ESP.restart();
    }
}

void handleFirmwareUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        resetFirmwareUploadState();
        firmwareUpload.rateLimited = !consumeWriteRequestLimit();
        firmwareUpload.authorized = !firmwareUpload.rateLimited && validWriteToken();
        if (!firmwareUpload.authorized) {
            firmwareUpload.error = firmwareUpload.rateLimited ?
                "Too many write requests — retry in 30 seconds" : "Invalid write token";
            return;
        }
        Serial.printf("Signed OTA upload: %s\n", upload.filename.c_str());
        setOperatingMode(OperatingMode::STANDBY);
        forceOutputOff();
    } else if (!firmwareUpload.authorized) {
        return;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        processFirmwarePackageBytes(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
        finishFirmwarePackage();
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        failFirmwareUpload("Firmware upload aborted");
    }
}

void handleOrientationWizard() {
    if (!requireWriteToken()) return;

    setOperatingMode(OperatingMode::STANDBY);
    forceOutputOff();
    const bool orientationSuccess = runOrientationWizard();
    bool calibrationSuccess = false;

    if (orientationSuccess) {
        for (uint8_t seconds = INITIAL_CALIBRATION_DELAY_SECONDS; seconds > 0; --seconds) {
            char countdown[17];
            snprintf(countdown, sizeof(countdown), "Starting in %u...", seconds);
            drawCalibrationScreen(countdown);
            delay(1000);
        }
        calibrationSuccess = calibrateMPU();
    }

    setOperatingMode(OperatingMode::STANDBY);
    lastMicros = micros();
    if (orientationSuccess) server.sendHeader("X-Write-Token", writeToken);
    server.send(
        orientationSuccess && calibrationSuccess ? 200 : 500,
        "text/plain",
        !orientationSuccess ? "Orientation not detected — previous setup preserved" :
        (calibrationSuccess ? "Orientation saved and calibrated; controller left in STANDBY" :
         "Orientation saved, but calibration failed — controller left in STANDBY")
    );
}

void handleFirmwareRollback() {
    if (!requireWriteToken()) return;

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* alternate = esp_ota_get_next_update_partition(running);
    if (alternate == nullptr || alternate == running) {
        server.send(409, "text/plain", "No alternate OTA partition is available");
        return;
    }

    esp_app_desc_t description;
    if (esp_ota_get_partition_description(alternate, &description) != ESP_OK) {
        server.send(409, "text/plain", "No valid previous firmware image is available");
        return;
    }

    if (esp_ota_set_boot_partition(alternate) != ESP_OK) {
        server.send(500, "text/plain", "Unable to select the previous firmware image");
        return;
    }

    setOperatingMode(OperatingMode::STANDBY);
    forceOutputOff();
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", String("Returning to firmware ") + description.version + ". Rebooting...");
    delay(400);
    ESP.restart();
}

// =====================================================
// NETWORK
// =====================================================

void registerWebRoutes() {
    if (webRoutesRegistered) {
        return;
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/settings", HTTP_GET, handleSettingsPage);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/model", HTTP_GET, handleRiderModel);
    server.on("/api/model/events.csv", HTTP_GET, handleRiderModelCsv);
    server.on("/api/model/feedback", HTTP_POST, handleRiderModelFeedback);
    server.on("/api/model/reset", HTTP_POST, handleRiderModelReset);
    server.on("/api/rides", HTTP_GET, handleRideList);
    server.on("/api/ride/report", HTTP_GET, handleRideReport);
    server.on("/api/ride/csv", HTTP_GET, handleRideCsv);
    server.on("/api/settings", HTTP_POST, handleSettings);
    server.on("/api/mode", HTTP_POST, handleMode);
    server.on("/api/calibrate", HTTP_POST, handleCalibration);
    server.on("/api/orientation", HTTP_POST, handleOrientationWizard);
    server.on("/api/output", HTTP_POST, handleManualOutput);
    server.on("/api/peak/reset", HTTP_POST, handlePeakReset);
    server.on("/api/wifi", HTTP_POST, handleWiFiPassword);
    server.on("/api/update", HTTP_POST, handleFirmwareUpdateFinished, handleFirmwareUpload);
    server.on("/api/rollback", HTTP_POST, handleFirmwareRollback);

    // Common captive-portal probes must redirect to our actual local origin.
    // Serving dashboard HTML at a Microsoft probe URL can cause Windows to
    // continue into its MSN fallback rather than remain on the controller.
    server.on("/generate_204", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/library/test/success.html", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/connecttest.txt", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/ncsi.txt", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/redirect", HTTP_GET, handleCaptivePortalRedirect);
    server.on("/fwlink", HTTP_GET, handleCaptivePortalRedirect);

    server.onNotFound([]() {
        server.sendHeader("Location", "http://192.168.4.1/", true);
        server.send(302, "text/plain", "");
    });

    webRoutesRegistered = true;
}

void startAccessPoint() {
    if (accessPointEnabled) {
        return;
    }

    Serial.println("Starting Wi-Fi AP...");

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAPsetHostname(MDNS_HOSTNAME);

    wifiHealthy = WiFi.softAP(wifiApSsid.c_str(), wifiApPassword.c_str());

    if (!wifiHealthy) {
        accessPointEnabled = false;
        Serial.println("ERROR: Wi-Fi AP failed");
        displayDirty = true;
        return;
    }

    accessPointEnabled = true;
    delay(100);

    // Captive DNS resolves ordinary hostnames to the controller even on
    // clients that do not cooperate with .local/mDNS.
    dnsHealthy = dnsServer.start(DNS_PORT, "*", AP_IP);

    // mDNS remains the preferred friendly wheelie.local name. Give the AP
    // interface a moment and retry startup because some ESP32-S3/core
    // combinations are timing-sensitive immediately after softAP().
    mdnsHealthy = false;
    for (uint8_t attempt = 0; attempt < 3 && !mdnsHealthy; ++attempt) {
        mdnsHealthy = MDNS.begin(MDNS_HOSTNAME);
        if (!mdnsHealthy) delay(120);
    }
    if (mdnsHealthy) {
        MDNS.addService("http", "tcp", 80);
    }

    server.begin();

    Serial.printf("SSID: %s\n", wifiApSsid.c_str());
    Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("mDNS: %s\n", mdnsHealthy ? "http://wheelie.local" : "unavailable");
    Serial.printf("Captive DNS: %s\n", dnsHealthy ? "ON" : "FAILED");
    Serial.println("HTTP server started");

    displayDirty = true;
}

void stopAccessPoint() {
    if (!accessPointEnabled) {
        return;
    }

    // Stop accepting HTTP/DNS clients before dropping the AP.
    server.stop();
    dnsServer.stop();
    dnsHealthy = false;

    if (mdnsHealthy) {
        MDNS.end();
    }

    mdnsHealthy = false;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    wifiHealthy = false;
    accessPointEnabled = false;

    Serial.println("Wi-Fi AP -> OFF");
    displayDirty = true;
}

void scheduleAccessPointRestart(unsigned long delayMs) {
    accessPointRestartPending = true;
    accessPointRestartAtMs = millis() + delayMs;
}

void updateAccessPointRestart() {
    if (!accessPointRestartPending) return;
    if ((long)(millis() - accessPointRestartAtMs) < 0) return;

    accessPointRestartPending = false;
    stopAccessPoint();
    delay(75);
    startAccessPoint();
}

void toggleAccessPointFromButton() {
    if (accessPointEnabled) {
        stopAccessPoint();
    } else {
        startAccessPoint();
        Serial.println(accessPointEnabled ? "Wi-Fi AP -> ON" : "Wi-Fi AP failed to start");
    }

    oled.clearDisplay();
    displayDirty = true;
}

void initializeNetwork() {
    registerWebRoutes();
    startAccessPoint();
}

// =====================================================
// IMU HEALTH / MAIN SENSOR UPDATE
// =====================================================

namespace {
void beginNetwork() {
    writeToken = makeWriteToken();
    initializeNetwork();
}

void tickNetwork() {
    if (accessPointEnabled) {
        dnsServer.processNextRequest();
        server.handleClient();
    }
    updateAccessPointRestart();
}

const FirmwareModule MODULE = {"network", beginNetwork, tickNetwork, 60, 10};
}

const FirmwareModule& networkModule() { return MODULE; }

} // namespace firmware
