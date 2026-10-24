/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Force-included into every C and C++ file with XENIA_PORTABLE_LINUX. glibc
// 2.43 gave these math functions new symbol versions, which an older glibc,
// such as Batocera's 2.40, doesn't have, so a binary calling them wouldn't
// load there. The versions every x86-64 glibc has behave the same for the
// values Xenia passes.

#ifndef XENIA_BASE_GLIBC_COMPAT_H_
#define XENIA_BASE_GLIBC_COMPAT_H_

// Not checking __GLIBC__: it comes from <features.h>, which this runs before,
// and including that here would fix the feature macros before a file sets
// _GNU_SOURCE. XENIA_PORTABLE_LINUX only targets glibc.
#if defined(__linux__) && defined(__x86_64__)
__asm__(".symver acosf,acosf@GLIBC_2.2.5");
__asm__(".symver log10f,log10f@GLIBC_2.2.5");
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
#endif

#endif  // XENIA_BASE_GLIBC_COMPAT_H_
