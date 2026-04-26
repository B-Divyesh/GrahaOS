// user/libtls-mg/mongoose_tls_core.c
//
// Phase 22 closeout (G1.3): instead of slicing TLS-only code via extract.py,
// we compile the entire vendored mongoose.c with a TLS-only config.
// Mongoose's #ifdef gates strip TCP/IP, sockets, FS, IPv6, mDNS, MQTT, etc.
// What's left is the TLS handshake + record layer + crypto primitives.
//
// vendor/mongoose.c is a self-contained amalgamation that includes mongoose.h
// at the top.  mongoose.h's MG_ARCH_CUSTOM path then includes our
// vendor/mongoose_config.h which carries the libc / allocator / errno hooks
// and disables every non-TLS subsystem.

#include "vendor/mongoose.c"
