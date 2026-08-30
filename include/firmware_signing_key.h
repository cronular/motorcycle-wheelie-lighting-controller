#pragma once

// ECDSA P-256 public key trusted for signed .wctrl OTA packages. The matching
// private key is stored only in the FIRMWARE_SIGNING_KEY_PEM GitHub secret.
constexpr const char FIRMWARE_SIGNING_PUBLIC_KEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEjk5eZXzFmu9Wd4qK5EZv1+w/GdbT\n"
    "X2ljV3fBUr9jxCr5unG9ZKpBTz9dGkvxHcMT3nm0E9SC3dfDfjesGemXtQ==\n"
    "-----END PUBLIC KEY-----\n";
