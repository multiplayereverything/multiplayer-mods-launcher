// GDI+ needs min/max, which NOMINMAX removes; give it std's versions.
#pragma once
#include "core/win.h"

#include <algorithm>
#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
