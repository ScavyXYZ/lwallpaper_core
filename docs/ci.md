# Continuous integration

`.github/workflows/ci.yml` runs on pushes, pull requests and manual dispatch.
Windows Server 2025 / Visual Studio 2022 builds Release and Debug separately.
The core is built without Qt; console examples are compiled but the manual
desktop-changing selftest is never run automatically.

CTest runs unit, public API and media/decoder tests. The FFmpeg CLI is taken
from the exact Gyan shared archive in `third_party/versions.json`, with its
SHA-256 checked before extraction. Git LFS objects are checked as well.
The private-window hardware GPU smoke test is excluded on hosted runners:
these machines cannot guarantee the hardware required for that test.

Successful jobs publish the static library, public header, example, matching
runtime DLLs, dependency versions and licenses as 14-day artifacts. Consumers
still need the FFmpeg/SDL development packages when linking the static library.
JUnit results and CMake/CTest diagnostics are uploaded even after failure.

Actions are pinned to commit SHAs, workflows use only `contents: read`, and
untrusted PR code never runs under `pull_request_target` or with custom secrets.
Artifacts are CI outputs, not GitHub Releases; Debug runtimes are for testing.

The same preparation/staging scripts are reused by the GUI workflows through
their pinned core submodule.
