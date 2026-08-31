#pragma once

// Single source of truth for the release version. Deliberately kept without
// quotes so build scripts can extract it (`for /f "tokens=3"`) and bake it
// into output file names; C++ code stringifies it with STR().
#define IMAGEPOLISH_VERSION 2.0.0
