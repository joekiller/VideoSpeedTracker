# Notes for Claude sessions

- Pull requests only get one Linux CI job; the Windows/macOS/multi-distro matrix runs after merge. Before pushing a
  change to `src/`, `tests/`, `CMakeLists.txt` or `companion/`, run `.githooks/pre-commit` (or
  `git config core.hooksPath .githooks` so it runs on every commit). The C++ part needs OpenCV:
  `sudo apt-get install -y libopencv-dev cmake g++` is enough (see README "Building from source").
- Code you can't build here (Windows-only `#ifdef _WIN32` paths, `packaging/windows`, macOS signing) is checked only
  on master; say so in the PR when a change touches it, so Joe can run `gh workflow run release.yml --ref <branch>`.
