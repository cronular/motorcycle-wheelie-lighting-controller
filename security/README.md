# Firmware signing

OTA packages use ECDSA P-256 signatures. The public key in this directory is
embedded in the firmware; its private counterpart must exist only as the
repository Actions secret `FIRMWARE_SIGNING_KEY_PEM`.

Generate a replacement pair with:

```powershell
python tools/package_firmware.py keygen `
  --private-key firmware-signing-private.pem `
  --public-key firmware-signing-public.pem
```

Never commit the private key. Rotating the key requires a bridge release signed
by the old key that embeds the new public key. Devices must install that bridge
release before packages signed only by the new key can be accepted.

After generating a key, configure the repository secret without printing the
credential or key:

```powershell
python -m pip install pynacl
python tools/configure_signing_secret.py `
  --repository cronular/motorcycle-wheelie-lighting-controller `
  --private-key firmware-signing-private.pem
```

Signed `.wctrl` packages contain a manifest, its DER-encoded ECDSA signature,
and the firmware image. The manifest binds the signature to the hardware target,
release channel, version, commit, build date, image size, and SHA-256 digest.

## v0.11.0 migration image

Firmware v0.11.0 cannot parse or verify `.wctrl` packages. Stable channel and
tagged releases therefore include a versioned `v0.11-migration.bin` containing
the same application image as the signed stable package. It is a one-time bridge
for the v0.11.0 dashboard's legacy raw-binary updater, not a general unsigned OTA
path in current firmware. Its digest is included in `SHA256SUMS.txt`, and that
checksum file is signed with the release key. After installing the bridge, the
device accepts signed `.wctrl` packages only.
