# Notes for Claude sessions

- Pull requests get build.yml's Linux job plus the Windows and macOS checks (platform-windows.yml, platform-macos.yml,
  which run release.yml's own windows/macos jobs, unsigned), each path-filtered; the multi-distro .deb matrix runs
  after merge. Wait for the Windows and macOS checks before calling a change to their code done. Before pushing a
  change to `src/`, `tests/`, `CMakeLists.txt` or `companion/`, run `.githooks/pre-commit` (or
  `git config core.hooksPath .githooks` so it runs on every commit). The C++ part needs OpenCV:
  `sudo apt-get install -y libopencv-dev cmake g++` is enough (see README "Building from source").
- Code you can't build here (Windows-only `#ifdef _WIN32` paths, `packaging/windows`, `.mm` files, the .app bundle) is
  built and tested by the PR's windows and macos checks. Only signing and notarizing are left to tag builds.
- `ctest` includes `portability_windows_macros` (tests/portability_check.py): every source compiled again, syntax only,
  with windows.h's macros defined (cmake/WindowsMacros.h). Never name anything `near`, `far`, `small`, `ERROR`, `IN`,
  `OUT`, `interface` etc. The PR check also builds with `-DVST_WERROR=ON` and compiles against main's cached OpenCV 5 with
  deprecations as errors (build.yml `opencv5-headers`), so build locally with `-DVST_WERROR=ON` too.
