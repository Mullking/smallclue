/*
 * Minimal ustar-format tar applet: create/extract/list, with transparent
 * gzip support (either explicit -z or auto-detected via the gzip magic
 * bytes on the input stream). This is the single most-cited missing
 * capability for a self-hosted Linux guest -- without it there is no way
 * to unpack a source tarball or release archive inside the guest at all.
 *
 * Scope: regular files, directories, and symlinks (the overwhelming
 * majority of real-world tarballs). Hardlinks/devices/fifos are skipped on
 * create and ignored (with a warning) on extract.
 *
 * WRITING stays plain ustar: long names use the prefix field, for up to ~255
 * bytes total. READING also understands GNU @LongLink ('L'/'K') and PAX
 * extended headers ('x'/'g'), because refusing them was not a limitation but
 * a corruption -- an unhandled metadata entry was listed as a file called
 * "@LongLink", its data was then read as the next header, and everything
 * after it in the archive was lost with one "not a ustar archive" line. Both
 * are what busybox tar EMITS for a path over 100 bytes, so the archives this
 * could not read were the ones the same system had just written.
 */

#include "tar_app.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <zlib.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TAR_BLOCK_SIZE 512
#define TAR_NAME_SIZE 100
#define TAR_PREFIX_SIZE 155

struct UstarHeader {
    char name[TAR_NAME_SIZE];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[TAR_PREFIX_SIZE];
    char pad[12];
};

#define TAR_TYPE_REGULAR '0'
#define TAR_TYPE_REGULAR_ALT '\0'
#define TAR_TYPE_SYMLINK '2'
#define TAR_TYPE_DIRECTORY '5'
/* Metadata entries: they carry the NEXT entry's name rather than a file. */
#define TAR_TYPE_GNU_LONGLINK 'K'
#define TAR_TYPE_GNU_LONGNAME 'L'
#define TAR_TYPE_PAX_GLOBAL 'g'
#define TAR_TYPE_PAX_EXTENDED 'x'

/* A path, not a payload. Anything past this is not a name we are missing. */
#define TAR_MAX_META (1024 * 1024)

typedef struct TarStream {
    FILE *plain;
    gzFile gz;
    bool writing;
} TarStream;

static bool tarStreamOpenRead(TarStream *ts, const char *path, bool forceGzip) {
    memset(ts, 0, sizeof(*ts));
    FILE *fp = (path && strcmp(path, "-") != 0) ? fopen(path, "rb") : stdin;
    if (!fp) {
        fprintf(stderr, "tar: %s: %s\n", path, strerror(errno));
        return false;
    }
    unsigned char magic[2] = {0, 0};
    bool isGzip = forceGzip;
    if (fp != stdin) {
        size_t n = fread(magic, 1, 2, fp);
        if (n == 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
            isGzip = true;
        }
        fseek(fp, 0, SEEK_SET);
    }
    if (isGzip) {
        int fd = fileno(fp);
        int dupFd = dup(fd);
        if (dupFd < 0) {
            fprintf(stderr, "tar: failed to duplicate fd for gzip stream: %s\n", strerror(errno));
            if (fp != stdin) fclose(fp);
            return false;
        }
        if (fp != stdin) fclose(fp);
        ts->gz = gzdopen(dupFd, "rb");
        if (!ts->gz) {
            fprintf(stderr, "tar: failed to open gzip stream\n");
            close(dupFd);
            return false;
        }
    } else {
        ts->plain = fp;
    }
    ts->writing = false;
    return true;
}

static bool tarStreamOpenWrite(TarStream *ts, const char *path, bool gzip) {
    memset(ts, 0, sizeof(*ts));
    ts->writing = true;
    if (gzip) {
        if (path && strcmp(path, "-") != 0) {
            ts->gz = gzopen(path, "wb");
        } else {
            ts->gz = gzdopen(dup(STDOUT_FILENO), "wb");
        }
        if (!ts->gz) {
            fprintf(stderr, "tar: %s: %s\n", path ? path : "(stdout)", strerror(errno));
            return false;
        }
    } else {
        FILE *fp = (path && strcmp(path, "-") != 0) ? fopen(path, "wb") : stdout;
        if (!fp) {
            fprintf(stderr, "tar: %s: %s\n", path, strerror(errno));
            return false;
        }
        ts->plain = fp;
    }
    return true;
}

static void tarStreamClose(TarStream *ts) {
    if (ts->gz) {
        gzclose(ts->gz);
    } else if (ts->plain && ts->plain != stdin && ts->plain != stdout) {
        fclose(ts->plain);
    }
}

static size_t tarStreamRead(TarStream *ts, void *buf, size_t len) {
    if (ts->gz) {
        int n = gzread(ts->gz, buf, (unsigned)len);
        return n > 0 ? (size_t)n : 0;
    }
    return fread(buf, 1, len, ts->plain);
}

static bool tarStreamWrite(TarStream *ts, const void *buf, size_t len) {
    if (ts->gz) {
        return gzwrite(ts->gz, buf, (unsigned)len) == (int)len;
    }
    return fwrite(buf, 1, len, ts->plain) == len;
}

static void tarWritePadding(TarStream *ts, size_t dataLen) {
    size_t rem = dataLen % TAR_BLOCK_SIZE;
    if (rem == 0) return;
    char zeros[TAR_BLOCK_SIZE];
    memset(zeros, 0, sizeof(zeros));
    tarStreamWrite(ts, zeros, TAR_BLOCK_SIZE - rem);
}

static void tarSkipPadding(TarStream *ts, size_t dataLen) {
    size_t rem = dataLen % TAR_BLOCK_SIZE;
    if (rem == 0) return;
    char buf[TAR_BLOCK_SIZE];
    tarStreamRead(ts, buf, TAR_BLOCK_SIZE - rem);
}

static unsigned tarComputeChecksumReal(const struct UstarHeader *hdr) {
    struct UstarHeader copy = *hdr;
    memset(copy.chksum, ' ', sizeof(copy.chksum));
    const unsigned char *p = (const unsigned char *)&copy;
    unsigned sum = 0;
    for (size_t i = 0; i < sizeof(copy); ++i) {
        sum += p[i];
    }
    return sum;
}

static void tarSetOctal(char *field, size_t fieldLen, unsigned long long value) {
    snprintf(field, fieldLen, "%0*llo", (int)(fieldLen - 1), value);
}

/* Consumes an entry's data and its record padding, returning it NUL-terminated.
 * Metadata only -- the cap is what keeps a corrupt size field from asking for
 * the archive's worth of memory. NULL means the caller should stop: the data
 * either ran out or was never a name. */
static char *tarReadMetaEntry(TarStream *ts, unsigned long long size) {
    if (size > TAR_MAX_META) {
        return NULL;
    }
    char *buf = (char *) malloc((size_t) size + 1);
    if (!buf) {
        return NULL;
    }
    unsigned long long got = 0;
    while (got < size) {
        size_t n = tarStreamRead(ts, buf + got, (size_t) (size - got));
        if (n == 0) {
            free(buf);
            return NULL;
        }
        got += n;
    }
    buf[size] = '\0';
    tarSkipPadding(ts, (size_t) size);
    return buf;
}

/* PAX records are "<len> <key>=<value>\n", where <len> counts its own digits
 * too. Only path and linkpath matter here; everything else (mtime, uid, the
 * SCHILY.* and LIBARCHIVE.* namespaces) is metadata this tar does not carry
 * anyway, and is skipped by advancing over the record rather than by parsing
 * it. A malformed length stops the walk rather than guessing where the next
 * record begins. */
static void tarParsePaxRecords(const char *data, size_t len,
                               char **name, char **link) {
    size_t pos = 0;
    while (pos < len) {
        size_t start = pos;
        unsigned long long reclen = 0;
        while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
            reclen = reclen * 10 + (unsigned long long) (data[pos] - '0');
            pos++;
            if (reclen > len) {
                return;
            }
        }
        if (pos == start || pos >= len || data[pos] != ' ') {
            return;
        }
        if (reclen <= (pos - start) || start + reclen > len) {
            return;
        }
        pos++; /* the space after the length */
        size_t recEnd = start + (size_t) reclen;
        size_t valueEnd = recEnd;
        if (valueEnd > pos && data[valueEnd - 1] == '\n') {
            valueEnd--;
        }
        const char *eq = (const char *) memchr(data + pos, '=', valueEnd - pos);
        if (eq) {
            size_t klen = (size_t) (eq - (data + pos));
            const char *value = eq + 1;
            size_t vlen = (size_t) ((data + valueEnd) - value);
            char **slot = NULL;
            if (klen == 4 && memcmp(data + pos, "path", 4) == 0) {
                slot = name;
            } else if (klen == 8 && memcmp(data + pos, "linkpath", 8) == 0) {
                slot = link;
            }
            if (slot) {
                char *copy = (char *) malloc(vlen + 1);
                if (copy) {
                    memcpy(copy, value, vlen);
                    copy[vlen] = '\0';
                    free(*slot);
                    *slot = copy;
                }
            }
        }
        pos = recEnd;
    }
}

static unsigned long long tarParseOctal(const char *field, size_t fieldLen) {
    unsigned long long value = 0;
    for (size_t i = 0; i < fieldLen && field[i]; ++i) {
        if (field[i] < '0' || field[i] > '7') break;
        value = value * 8 + (unsigned long long)(field[i] - '0');
    }
    return value;
}

static bool tarSplitPathIntoHeader(struct UstarHeader *hdr, const char *path) {
    size_t len = strlen(path);
    if (len < TAR_NAME_SIZE) {
        memset(hdr->name, 0, sizeof(hdr->name));
        memcpy(hdr->name, path, len);
        memset(hdr->prefix, 0, sizeof(hdr->prefix));
        return true;
    }
    if (len >= TAR_NAME_SIZE + TAR_PREFIX_SIZE) {
        fprintf(stderr, "tar: %s: name too long (max %d bytes)\n", path, TAR_NAME_SIZE + TAR_PREFIX_SIZE);
        return false;
    }
    /* Find a '/' split point so the suffix fits in name[] and the prefix
     * fits in prefix[], per POSIX ustar. */
    size_t splitAt = (size_t)-1;
    for (size_t i = len - 1; i > 0; --i) {
        if (path[i] == '/' && (len - i - 1) < TAR_NAME_SIZE && i < TAR_PREFIX_SIZE) {
            splitAt = i;
            break;
        }
    }
    if (splitAt == (size_t)-1) {
        fprintf(stderr, "tar: %s: name too long to split for ustar format\n", path);
        return false;
    }
    memset(hdr->prefix, 0, sizeof(hdr->prefix));
    memcpy(hdr->prefix, path, splitAt);
    memset(hdr->name, 0, sizeof(hdr->name));
    memcpy(hdr->name, path + splitAt + 1, len - splitAt - 1);
    return true;
}

static void tarJoinPathFromHeader(const struct UstarHeader *hdr, char *out, size_t outLen) {
    char name[TAR_NAME_SIZE + 1];
    char prefix[TAR_PREFIX_SIZE + 1];
    memcpy(name, hdr->name, TAR_NAME_SIZE);
    name[TAR_NAME_SIZE] = '\0';
    memcpy(prefix, hdr->prefix, TAR_PREFIX_SIZE);
    prefix[TAR_PREFIX_SIZE] = '\0';
    if (prefix[0]) {
        snprintf(out, outLen, "%s/%s", prefix, name);
    } else {
        snprintf(out, outLen, "%s", name);
    }
}

static bool tarMakeDirsRecursive(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static bool tarWriteEntry(TarStream *ts, const char *path, bool verbose) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "tar: %s: %s\n", path, strerror(errno));
        return false;
    }

    struct UstarHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    char entryPath[4096];
    if (S_ISDIR(st.st_mode)) {
        snprintf(entryPath, sizeof(entryPath), "%s/", path);
    } else {
        snprintf(entryPath, sizeof(entryPath), "%s", path);
    }
    if (!tarSplitPathIntoHeader(&hdr, entryPath)) {
        return false;
    }

    tarSetOctal(hdr.mode, sizeof(hdr.mode), st.st_mode & 07777);
    tarSetOctal(hdr.uid, sizeof(hdr.uid), st.st_uid);
    tarSetOctal(hdr.gid, sizeof(hdr.gid), st.st_gid);
    tarSetOctal(hdr.mtime, sizeof(hdr.mtime), (unsigned long long)st.st_mtime);
    memcpy(hdr.magic, "ustar", 5);
    memcpy(hdr.version, "00", 2);

    off_t dataSize = 0;
    char linkTarget[PATH_MAX];
    if (S_ISLNK(st.st_mode)) {
        hdr.typeflag = TAR_TYPE_SYMLINK;
        ssize_t n = readlink(path, linkTarget, sizeof(linkTarget) - 1);
        if (n < 0) {
            fprintf(stderr, "tar: %s: %s\n", path, strerror(errno));
            return false;
        }
        linkTarget[n] = '\0';
        strncpy(hdr.linkname, linkTarget, sizeof(hdr.linkname) - 1);
    } else if (S_ISDIR(st.st_mode)) {
        hdr.typeflag = TAR_TYPE_DIRECTORY;
    } else if (S_ISREG(st.st_mode)) {
        hdr.typeflag = TAR_TYPE_REGULAR;
        dataSize = st.st_size;
    } else {
        fprintf(stderr, "tar: %s: unsupported file type, skipping\n", path);
        return true;
    }
    tarSetOctal(hdr.size, sizeof(hdr.size), (unsigned long long)dataSize);

    unsigned sum = tarComputeChecksumReal(&hdr);
    snprintf(hdr.chksum, sizeof(hdr.chksum), "%06o", sum);
    hdr.chksum[6] = '\0';
    hdr.chksum[7] = ' ';

    if (!tarStreamWrite(ts, &hdr, sizeof(hdr))) {
        fprintf(stderr, "tar: write error\n");
        return false;
    }

    if (verbose) {
        printf("%s\n", entryPath);
    }

    if (S_ISREG(st.st_mode)) {
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "tar: %s: %s\n", path, strerror(errno));
            return false;
        }
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
            if (!tarStreamWrite(ts, buf, n)) {
                fclose(fp);
                return false;
            }
        }
        fclose(fp);
        tarWritePadding(ts, (size_t)dataSize);
    }
    return true;
}

static bool tarWriteEntryRecursive(TarStream *ts, const char *path, bool verbose) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "tar: %s: %s\n", path, strerror(errno));
        return false;
    }
    if (!tarWriteEntry(ts, path, verbose)) {
        return false;
    }
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            fprintf(stderr, "tar: %s: %s\n", path, strerror(errno));
            return false;
        }
        struct dirent *entry;
        bool ok = true;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char childPath[4096];
            snprintf(childPath, sizeof(childPath), "%s/%s", path, entry->d_name);
            if (!tarWriteEntryRecursive(ts, childPath, verbose)) {
                ok = false;
            }
        }
        closedir(dir);
        if (!ok) return false;
    }
    return true;
}

static void tarWriteEndOfArchive(TarStream *ts) {
    char zeros[TAR_BLOCK_SIZE];
    memset(zeros, 0, sizeof(zeros));
    tarStreamWrite(ts, zeros, sizeof(zeros));
    tarStreamWrite(ts, zeros, sizeof(zeros));
}

static bool tarHeaderIsZero(const struct UstarHeader *hdr) {
    const unsigned char *p = (const unsigned char *)hdr;
    for (size_t i = 0; i < sizeof(*hdr); ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

static const char *tarJoinExtractPath(const char *destDir, const char *entryPath, char *buf, size_t bufLen) {
    if (destDir && *destDir) {
        snprintf(buf, bufLen, "%s/%s", destDir, entryPath);
    } else {
        snprintf(buf, bufLen, "%s", entryPath);
    }
    return buf;
}

static void tarStripTrailingSlash(char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

static void tarEnsureParentDir(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    char *slash = strrchr(buf, '/');
    if (slash) {
        *slash = '\0';
        if (*buf) {
            tarMakeDirsRecursive(buf);
        }
    }
}

static int tarExtractOrList(const char *archivePath, bool doExtract, bool verbose,
                            bool forceGzip, const char *destDir, int filterc, char **filterv) {
    TarStream ts;
    if (!tarStreamOpenRead(&ts, archivePath, forceGzip)) {
        return 1;
    }

    int status = 0;
    /* Set by an 'L'/'K'/'x' entry, consumed by the entry after it. */
    char *pendingName = NULL;
    char *pendingLink = NULL;
    for (;;) {
        struct UstarHeader hdr;
        size_t n = tarStreamRead(&ts, &hdr, sizeof(hdr));
        if (n == 0) break;
        if (n != sizeof(hdr)) {
            fprintf(stderr, "tar: truncated archive\n");
            status = 1;
            break;
        }
        if (tarHeaderIsZero(&hdr)) {
            break;
        }
        if (memcmp(hdr.magic, "ustar", 5) != 0) {
            fprintf(stderr, "tar: not a ustar archive (unsupported format)\n");
            status = 1;
            break;
        }

        unsigned long long size = tarParseOctal(hdr.size, sizeof(hdr.size));
        unsigned long long mode = tarParseOctal(hdr.mode, sizeof(hdr.mode));

        if (hdr.typeflag == TAR_TYPE_GNU_LONGNAME ||
            hdr.typeflag == TAR_TYPE_GNU_LONGLINK ||
            hdr.typeflag == TAR_TYPE_PAX_EXTENDED ||
            hdr.typeflag == TAR_TYPE_PAX_GLOBAL) {
            char *data = tarReadMetaEntry(&ts, size);
            if (!data) {
                /* Reading it is what keeps the stream aligned, so failing to
                 * is not something to warn about and continue past: the next
                 * read would land mid-data and every later entry would be
                 * lost. */
                fprintf(stderr, "tar: unreadable %s header, stopping\n",
                        hdr.typeflag == TAR_TYPE_PAX_EXTENDED ||
                        hdr.typeflag == TAR_TYPE_PAX_GLOBAL ? "extended" : "long-name");
                status = 1;
                break;
            }
            if (hdr.typeflag == TAR_TYPE_GNU_LONGNAME) {
                free(pendingName);
                pendingName = data;
            } else if (hdr.typeflag == TAR_TYPE_GNU_LONGLINK) {
                free(pendingLink);
                pendingLink = data;
            } else {
                /* A global header applies to the rest of the archive rather
                 * than the next entry, but the only keys read here are
                 * per-entry ones, so treating both alike costs nothing. */
                if (hdr.typeflag == TAR_TYPE_PAX_EXTENDED) {
                    tarParsePaxRecords(data, (size_t) size, &pendingName, &pendingLink);
                }
                free(data);
            }
            continue;
        }

        char entryPath[4096];
        char linkTarget[4096];
        if (pendingName) {
            snprintf(entryPath, sizeof(entryPath), "%s", pendingName);
            free(pendingName);
            pendingName = NULL;
        } else {
            tarJoinPathFromHeader(&hdr, entryPath, sizeof(entryPath));
        }
        if (pendingLink) {
            snprintf(linkTarget, sizeof(linkTarget), "%s", pendingLink);
            free(pendingLink);
            pendingLink = NULL;
        } else {
            memcpy(linkTarget, hdr.linkname, sizeof(hdr.linkname));
            linkTarget[sizeof(hdr.linkname)] = '\0';
        }

        bool matches = (filterc == 0);
        for (int i = 0; i < filterc; ++i) {
            size_t flen = strlen(filterv[i]);
            if (strncmp(entryPath, filterv[i], flen) == 0 &&
                (entryPath[flen] == '\0' || entryPath[flen] == '/')) {
                matches = true;
                break;
            }
        }

        if (!doExtract) {
            if (matches) {
                if (verbose) {
                    char typeCh = hdr.typeflag == TAR_TYPE_DIRECTORY ? 'd' :
                                  hdr.typeflag == TAR_TYPE_SYMLINK ? 'l' : '-';
                    printf("%c%s %10llu %s\n", typeCh, "rw-r--r--", size, entryPath);
                } else {
                    printf("%s\n", entryPath);
                }
            }
            if (hdr.typeflag == TAR_TYPE_REGULAR || hdr.typeflag == TAR_TYPE_REGULAR_ALT) {
                char discard[65536];
                unsigned long long remaining = size;
                while (remaining > 0) {
                    size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : (size_t)remaining;
                    size_t got = tarStreamRead(&ts, discard, chunk);
                    if (got == 0) break;
                    remaining -= got;
                }
                tarSkipPadding(&ts, (size_t)size);
            }
            continue;
        }

        char destPath[4096];
        tarJoinExtractPath(destDir, entryPath, destPath, sizeof(destPath));
        tarStripTrailingSlash(destPath);

        if (!matches) {
            if (hdr.typeflag == TAR_TYPE_REGULAR || hdr.typeflag == TAR_TYPE_REGULAR_ALT) {
                char discard[65536];
                unsigned long long remaining = size;
                while (remaining > 0) {
                    size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : (size_t)remaining;
                    size_t got = tarStreamRead(&ts, discard, chunk);
                    if (got == 0) break;
                    remaining -= got;
                }
                tarSkipPadding(&ts, (size_t)size);
            }
            continue;
        }

        if (verbose) {
            printf("%s\n", entryPath);
        }

        switch (hdr.typeflag) {
            case TAR_TYPE_DIRECTORY:
                tarMakeDirsRecursive(destPath);
                chmod(destPath, (mode_t)mode);
                break;
            case TAR_TYPE_SYMLINK: {
                tarEnsureParentDir(destPath);
                unlink(destPath);
                if (symlink(linkTarget, destPath) != 0) {
                    fprintf(stderr, "tar: %s: %s\n", destPath, strerror(errno));
                    status = 1;
                }
                break;
            }
            case TAR_TYPE_REGULAR:
            case TAR_TYPE_REGULAR_ALT: {
                tarEnsureParentDir(destPath);
                FILE *out = fopen(destPath, "wb");
                if (!out) {
                    fprintf(stderr, "tar: %s: %s\n", destPath, strerror(errno));
                    status = 1;
                    char discard[65536];
                    unsigned long long remaining = size;
                    while (remaining > 0) {
                        size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : (size_t)remaining;
                        size_t got = tarStreamRead(&ts, discard, chunk);
                        if (got == 0) break;
                        remaining -= got;
                    }
                    tarSkipPadding(&ts, (size_t)size);
                    break;
                }
                unsigned long long remaining = size;
                char buf[65536];
                while (remaining > 0) {
                    size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
                    size_t got = tarStreamRead(&ts, buf, chunk);
                    if (got == 0) break;
                    fwrite(buf, 1, got, out);
                    remaining -= got;
                }
                fclose(out);
                chmod(destPath, (mode_t)mode);
                tarSkipPadding(&ts, (size_t)size);
                break;
            }
            default:
                fprintf(stderr, "tar: %s: unsupported entry type '%c', skipping\n", entryPath, hdr.typeflag);
                if (size > 0) {
                    char discard[65536];
                    unsigned long long remaining = size;
                    while (remaining > 0) {
                        size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : (size_t)remaining;
                        size_t got = tarStreamRead(&ts, discard, chunk);
                        if (got == 0) break;
                        remaining -= got;
                    }
                    tarSkipPadding(&ts, (size_t)size);
                }
                break;
        }
    }

    /* An archive that ends on a long-name header names nothing. */
    free(pendingName);
    free(pendingLink);
    tarStreamClose(&ts);
    return status;
}

static int tarCreate(const char *archivePath, bool verbose, bool gzip, int filterc, char **filterv) {
    TarStream ts;
    if (!tarStreamOpenWrite(&ts, archivePath, gzip)) {
        return 1;
    }
    int status = 0;
    for (int i = 0; i < filterc; ++i) {
        char clean[4096];
        snprintf(clean, sizeof(clean), "%s", filterv[i]);
        tarStripTrailingSlash(clean);
        if (!tarWriteEntryRecursive(&ts, clean, verbose)) {
            status = 1;
        }
    }
    tarWriteEndOfArchive(&ts);
    tarStreamClose(&ts);
    return status;
}

static void tarUsage(void) {
    fprintf(stderr,
        "usage: tar -c|-x|-t -f archive [-v] [-z] [-C dir] [file...]\n"
        "       tar c|x|t [vzf] archive [file...]   (bundled-flag form)\n");
}

int smallclueTarCommand(int argc, char **argv) {
    if (argc < 2) {
        tarUsage();
        return 1;
    }

    bool doCreate = false, doExtract = false, doList = false;
    bool verbose = false, gzip = false;
    const char *archivePath = NULL;
    const char *destDir = NULL;
    int firstOperand = -1;

    /* Accept both "tar -xzf archive.tar.gz" and the traditional
     * bundled-without-dash "tar xzf archive.tar.gz" form. */
    int argi = 1;
    const char *modeArg = argv[argi];
    bool bundled = (modeArg[0] != '-');
    const char *flags = bundled ? modeArg : modeArg + 1;

    for (const char *p = flags; *p; ++p) {
        switch (*p) {
            case 'c': doCreate = true; break;
            case 'x': doExtract = true; break;
            case 't': doList = true; break;
            case 'v': verbose = true; break;
            case 'z': gzip = true; break;
            case 'f':
                /* 'f' consumes the NEXT argument as the archive path,
                 * whether bundled or not -- matches traditional tar. */
                break;
            default:
                fprintf(stderr, "tar: unsupported option '%c'\n", *p);
                tarUsage();
                return 1;
        }
    }
    argi++;

    bool wantsFileArg = strchr(flags, 'f') != NULL;
    if (wantsFileArg) {
        if (argi >= argc) {
            fprintf(stderr, "tar: option 'f' requires an archive path\n");
            return 1;
        }
        archivePath = argv[argi++];
    }

    if (!bundled) {
        for (; argi < argc; ++argi) {
            if (strcmp(argv[argi], "-C") == 0) {
                if (argi + 1 >= argc) {
                    fprintf(stderr, "tar: -C requires a directory argument\n");
                    return 1;
                }
                destDir = argv[++argi];
            } else if (strcmp(argv[argi], "-f") == 0) {
                if (argi + 1 >= argc) {
                    fprintf(stderr, "tar: -f requires an archive path\n");
                    return 1;
                }
                archivePath = argv[++argi];
            } else if (strcmp(argv[argi], "-v") == 0) {
                verbose = true;
            } else if (strcmp(argv[argi], "-z") == 0) {
                gzip = true;
            } else {
                break;
            }
        }
    } else {
        /* Traditional bundled form also allows a following "-C dir". */
        if (argi < argc && strcmp(argv[argi], "-C") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "tar: -C requires a directory argument\n");
                return 1;
            }
            destDir = argv[argi + 1];
            argi += 2;
        }
    }
    firstOperand = argi;

    if (!doCreate && !doExtract && !doList) {
        fprintf(stderr, "tar: one of -c/-x/-t is required\n");
        tarUsage();
        return 1;
    }
    if ((doCreate ? 1 : 0) + (doExtract ? 1 : 0) + (doList ? 1 : 0) > 1) {
        fprintf(stderr, "tar: only one of -c/-x/-t may be given\n");
        return 1;
    }
    if (!archivePath) {
        archivePath = "-";
    }

    int filterc = argc - firstOperand;
    char **filterv = argv + firstOperand;

    if (doCreate) {
        if (filterc == 0) {
            fprintf(stderr, "tar: -c requires at least one file or directory\n");
            return 1;
        }
        if (destDir && chdir(destDir) != 0) {
            fprintf(stderr, "tar: -C %s: %s\n", destDir, strerror(errno));
            return 1;
        }
        return tarCreate(archivePath, verbose, gzip, filterc, filterv);
    }

    return tarExtractOrList(archivePath, doExtract, verbose, gzip, destDir, filterc, filterv);
}
