# Contributing

## Branch model

- `main` contains the current release and should always build successfully.
- `testing` receives normal development work.
- Create feature branches from `testing`, then open pull requests back into
  `testing`. Release pull requests flow from `testing` into `main`.
- Never commit generated `.pio` content or firmware binaries.

## Local checks

Before opening a pull request, run:

```powershell
pio test -e native
python tools/run_simulator.py --check
pio run -e seeed_xiao_esp32s3
```

Changes affecting sensor interpretation, GPIO, PWM, calibration, or output safety
also require physical hardware verification before release.

## Releases

1. Update `FIRMWARE_VERSION` and `CHANGELOG.md` on `testing`.
2. Merge the release pull request into `main` after CI passes.
3. Tag the merge commit using the matching semantic version, such as `v0.12.0`.
4. Push the tag. GitHub Actions builds and attaches verified firmware binaries.

## Commit and pull-request scope

Keep commits focused and explain safety-relevant behavior explicitly. Pull
requests should include test evidence, simulator screenshots when UI changes,
and hardware results when electrical or sensor behavior changes.
