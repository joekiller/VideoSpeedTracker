// Force-included by tests/portability_check.py on Linux and macOS: the object-like macros windows.h (and the rpcndr.h
// and wingdi.h it pulls in) defines, so code that would break on Windows breaks here first.
//
// On Windows, App.cpp and Server.cpp see windows.h before any of our headers (portable-file-dialogs and cpp-httplib
// include it), so a variable called `far` became `bool = ...` and stopped the MSVC build (PR #43) while every Linux
// check passed.  Defining the same names here and compiling every source with -fsyntax-only finds that on a Linux PR.
//
// Only macros that change how code parses are listed.  min/max are left out: every Windows target builds with NOMINMAX
// (CMakeLists.txt).  Function-name macros such as DrawText -> DrawTextW are left out too: they only rename, and break
// only declarations seen before windows.h, which a syntax check can't tell apart.
#pragma once

// minwindef.h
#define near
#define far
#define NEAR
#define FAR
#define PASCAL __stdcall
#define CALLBACK __stdcall
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0
#define OPTIONAL
#define IN
#define OUT
// rpcndr.h
#define small char
#define hyper __int64
// objbase.h
#define interface struct
// winnt.h
#define DELETE (0x00010000L)
// wingdi.h
#define ERROR 0
#define TRANSPARENT 1
#define OPAQUE 2
#define ABSOLUTE 1
#define RELATIVE 2
#define ALTERNATE 1
#define WINDING 2
// winerror.h, winbase.h
#define NO_ERROR 0L
#define INFINITE 0xFFFFFFFF
#define IGNORE 0
// wincon.h
#define MOUSE_MOVED 0x0001
#define DOUBLE_CLICK 0x0002
#define KEY_EVENT 0x0001
#define FOCUS_EVENT 0x0010
