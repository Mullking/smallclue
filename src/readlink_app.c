/*
 * readlink/realpath applet: path canonicalization is a ubiquitous shell-
 * script idiom (`cd "$(dirname "$(readlink -f "$0")")"` and friends) that
 * had no equivalent applet at all.
 */

#include "readlink_app.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Canonicalizes `path`, resolving symlinks and .././ components. libc's
 * realpath(3) already does this correctly for a fully-existing path; when
 * `allowMissing` is set and some suffix of the path doesn't exist, we
 * canonicalize the longest existing prefix via realpath(3) and manually
 * append the (nonexistent) remainder, matching GNU readlink -f/-m's more
 * lenient behavior instead of libc realpath()'s hard "all components must
 * exist" requirement. */
static bool smallclueCanonicalize(const char *path, bool allowMissing, char *out, size_t outLen) {
    char resolved[PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        snprintf(out, outLen, "%s", resolved);
        return true;
    }
    if (!allowMissing || errno != ENOENT) {
        return false;
    }

    /* Walk backwards, chopping off trailing components, until realpath()
     * succeeds on the remaining prefix. */
    char working[PATH_MAX];
    snprintf(working, sizeof(working), "%s", path);
    char suffix[PATH_MAX];
    suffix[0] = '\0';

    for (;;) {
        char *slash = strrchr(working, '/');
        char component[PATH_MAX];
        if (!slash) {
            snprintf(component, sizeof(component), "%s", working);
            working[0] = '.';
            working[1] = '\0';
        } else if (slash == working) {
            snprintf(component, sizeof(component), "%s", slash + 1);
            working[1] = '\0';
        } else {
            snprintf(component, sizeof(component), "%s", slash + 1);
            *slash = '\0';
        }
        if (component[0] != '\0') {
            char newSuffix[PATH_MAX];
            if (suffix[0] != '\0') {
                snprintf(newSuffix, sizeof(newSuffix), "%s/%s", component, suffix);
            } else {
                snprintf(newSuffix, sizeof(newSuffix), "%s", component);
            }
            snprintf(suffix, sizeof(suffix), "%s", newSuffix);
        }
        if (realpath(working, resolved) != NULL) {
            if (suffix[0] != '\0') {
                snprintf(out, outLen, "%s/%s", resolved, suffix);
            } else {
                snprintf(out, outLen, "%s", resolved);
            }
            return true;
        }
        if (errno != ENOENT || (strcmp(working, "/") == 0) || (strcmp(working, ".") == 0)) {
            return false;
        }
    }
}

int smallclueReadlinkCommand(int argc, char **argv) {
    static const char *usage =
        "usage: readlink [-f|-e|-m] [-nqsvz] PATH...\n"
        "  -f canonicalize, all but the last component must exist\n"
        "  -e canonicalize, every component must exist\n"
        "  -m canonicalize, no component need exist\n"
        "  -n omit the trailing newline    -z terminate with NUL instead\n"
        "  -q, -s suppress error messages (the default)\n"
        "  -v report error messages\n";
    bool canonicalize = false;
    bool requireExisting = false;
    bool allowMissing = false;
    bool noNewline = false;
    /* The real readlink is quiet by default -- -s/-q are documented as "on by
     * default" and -v is what turns diagnostics on. This used to report every
     * failure, so `readlink --silent /proc/self/fd/0 || true` in Devuan's udev
     * init script printed an error where the real tool prints nothing. */
    bool verbose = false;
    bool nulTerminated = false;

    int argi = 1;
    for (; argi < argc; ++argi) {
        const char *arg = argv[argi];
        if (arg[0] != '-' || strcmp(arg, "-") == 0) {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            argi++;
            break;
        }
        if (arg[1] == '-') {
            const char *lopt = arg + 2;
            if (strcmp(lopt, "canonicalize") == 0) {
                canonicalize = true;
            } else if (strcmp(lopt, "canonicalize-existing") == 0) {
                canonicalize = true;
                requireExisting = true;
            } else if (strcmp(lopt, "canonicalize-missing") == 0) {
                canonicalize = true;
                allowMissing = true;
            } else if (strcmp(lopt, "no-newline") == 0) {
                noNewline = true;
            } else if (strcmp(lopt, "silent") == 0 || strcmp(lopt, "quiet") == 0) {
                verbose = false;
            } else if (strcmp(lopt, "verbose") == 0) {
                verbose = true;
            } else if (strcmp(lopt, "zero") == 0) {
                nulTerminated = true;
            } else if (strcmp(lopt, "help") == 0) {
                fputs(usage, stdout);
                return 0;
            } else {
                fprintf(stderr, "readlink: unrecognized option '%s'\n", arg);
                fputs(usage, stderr);
                return 1;
            }
            continue;
        }
        for (const char *p = arg + 1; *p; ++p) {
            switch (*p) {
                case 'f': canonicalize = true; break;
                case 'e': canonicalize = true; requireExisting = true; break;
                case 'm': canonicalize = true; allowMissing = true; break;
                case 'n': noNewline = true; break;
                case 's': case 'q': verbose = false; break;
                case 'v': verbose = true; break;
                case 'z': nulTerminated = true; break;
                default:
                    fprintf(stderr, "readlink: unsupported option '%c'\n", *p);
                    fputs(usage, stderr);
                    return 1;
            }
        }
    }
    if (argi >= argc) {
        fprintf(stderr, "readlink: missing operand\n");
        return 1;
    }

    int status = 0;
    for (int i = argi; i < argc; ++i) {
        const char *path = argv[i];
        char resolved[PATH_MAX];
        if (canonicalize) {
            bool lenient = allowMissing || !requireExisting;
            if (!smallclueCanonicalize(path, lenient, resolved, sizeof(resolved))) {
                if (verbose) {
                    fprintf(stderr, "readlink: %s: %s\n", path, strerror(errno));
                }
                status = 1;
                continue;
            }
            fputs(resolved, stdout);
        } else {
            ssize_t n = readlink(path, resolved, sizeof(resolved) - 1);
            if (n < 0) {
                if (verbose) {
                    fprintf(stderr, "readlink: %s: %s\n", path, strerror(errno));
                }
                status = 1;
                continue;
            }
            resolved[n] = '\0';
            fputs(resolved, stdout);
        }
        if (nulTerminated) {
            putchar('\0');
        } else if (!noNewline) {
            putchar('\n');
        }
    }
    return status;
}

int smallclueRealpathCommand(int argc, char **argv) {
    bool allowMissing = true; /* GNU realpath's default: missing final component OK */

    int argi = 1;
    for (; argi < argc; ++argi) {
        const char *arg = argv[argi];
        if (arg[0] != '-' || strcmp(arg, "-") == 0) {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            argi++;
            break;
        }
        if (strcmp(arg, "-m") == 0 || strcmp(arg, "--canonicalize-missing") == 0) {
            allowMissing = true;
        } else if (strcmp(arg, "-e") == 0 || strcmp(arg, "--canonicalize-existing") == 0) {
            allowMissing = false;
        } else {
            fprintf(stderr, "realpath: unsupported option '%s'\n", arg);
            return 1;
        }
    }
    if (argi >= argc) {
        fprintf(stderr, "realpath: missing operand\n");
        return 1;
    }

    int status = 0;
    for (int i = argi; i < argc; ++i) {
        const char *path = argv[i];
        char resolved[PATH_MAX];
        /* GNU realpath defaults to allowing a missing final component;
         * -e demands every component (including the last) exist. */
        if (!smallclueCanonicalize(path, allowMissing, resolved, sizeof(resolved))) {
            fprintf(stderr, "realpath: %s: %s\n", path, strerror(errno));
            status = 1;
            continue;
        }
        printf("%s\n", resolved);
    }
    return status;
}
