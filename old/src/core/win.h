// Single place that pulls in <windows.h> with the project's preferred macros. On Linux it is the small part of the
// Win32 API the engine uses, done with POSIX calls (posix/win32.h), so the shared code compiles unchanged.
#pragma once
#ifndef _WIN32
#include "core/posix/win32.h"
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#endif
