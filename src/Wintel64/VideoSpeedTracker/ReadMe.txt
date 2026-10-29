VideoSpeedTracker sources
=========================

The Visual Studio AppWizard notes that used to be here described project files
(VideoSpeedTracker.vcxproj, StdAfx.h/.cpp) that were never committed. The
programs now build with CMake from the top-level CMakeLists.txt; see
"Building from source" in README.md.

videoSpeedTracker.cpp   main(): command-line options, interactive setup, the
                        frame-differencing loop, csv/trace/highlights output
Globals.cpp/.h          VST.cfg settings, their defaults and the config parser
VehicleDynamics.cpp/.h  per-vehicle predictive tracker and speed computation
Projection.cpp/.h       a predicted vehicle box, direction state and velocity
Snapshot.cpp/.h         one observed vehicle box at a frame number
VST.cfg                 copy of bin/VST.cfg, the sample config that is
                        installed and used by the tests
../Common/UserPaths.*   per-user config and data folders shared with
                        ProcessHiLites
