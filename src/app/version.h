#ifndef _VERSION_H
#define _VERSION_H

// FWVERSION: "<commit date>_<git describe>[_<voice>]", written by the build into fwversion.h
// (cmake/StrisoVersion.cmake) at every build
#if !defined(FWVERSION) && defined(__has_include)
  #if __has_include("fwversion.h")
    #include "fwversion.h"
  #endif
#endif

// without fwversion.h (the host tests): the form StrisoVersion.cmake gives a build without git, which
// the tools recognise (tools/striso.py version(): _v<version>, R28)
#ifndef FWVERSION
  #define FWVERSION "nogit_v0.0.0"
#endif

#endif
