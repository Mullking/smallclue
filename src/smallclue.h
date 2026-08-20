#ifndef SMALLCLUE_SMALLCLUE_H
#define SMALLCLUE_SMALLCLUE_H

#include <stddef.h>
#include <stdbool.h>
#if defined(PSCAL_TARGET_IOS)
#include "common/path_virtualization.h"
/* Ensure vproc syscall shims apply in app builds that skip global -include. */
#include "ios/vproc_shim.h"
#endif

#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define SMALLCLUE_HAS_IFADDRS 1
#else
#define SMALLCLUE_HAS_IFADDRS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*SmallclueAppletEntry)(int argc, char **argv);

typedef bool (*SmallclueAppletAvailable)(void);

typedef struct SmallclueApplet {
    const char *name;
    SmallclueAppletEntry entry;
    const char *description;
    /* NULL means "always". Otherwise the applet is left out of the listing
     * when this returns false. Some applets are fronts for a host runtime
     * that not every build has, and one that can only answer "not available
     * on this platform" is worse in the list than absent from it: it reads
     * as a capability the build has, and the only way to find out otherwise
     * is to run it. Still dispatchable by name, so invoking one directly
     * gives the real explanation rather than "unknown applet". */
    SmallclueAppletAvailable available;
} SmallclueApplet;

int smallclueMain(int argc, char **argv);

const SmallclueApplet *smallclueGetApplets(size_t *count);
const SmallclueApplet *smallclueFindApplet(const char *name);
const char *smallclueLookupAppletUsage(const char *name);
int smallclueDispatchApplet(const SmallclueApplet *applet, int argc, char **argv);

void smallclueRegisterBuiltins(void);
bool smallclueIsRegisteredBuiltinName(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SMALLCLUE_SMALLCLUE_H */
