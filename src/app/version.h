#ifndef _VERSION_H
#define _VERSION_H

// FWVERSION: "<commit date>_<git describe>[_<voice>]", written by the build into fwversion.h
// (cmake/StrisoVersion.cmake) at every build
#if !defined(FWVERSION) && defined(__has_include)
  #if __has_include("fwversion.h")
    #include "fwversion.h"
  #endif
#endif

#ifndef FWVERSION
  #define FWVERSION "v0.6 (no git)"
#endif

#endif
