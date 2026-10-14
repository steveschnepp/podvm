/*
 * tar.c - ustar/GNU tar reader (moved verbatim from image.c) + writer.
 * See tar.h.
 */
#define _GNU_SOURCE
#include "tar.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PATH_SIZE 2048

static int mkdir_p(const char *path, int mode) {
    char tmp[PATH_SIZE];
    char *p;
    if (strlen(path) >= sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* ───────────────────────── reader ───────────────────────── */

static unsigned long long tar_octal(const char *s, size_t n) {
    unsigned long long v = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        v = (v << 3) + (unsigned long long)(s[i] - '0');
    }
    return v;
}

/* Join destroot + entry name, rejecting absolute paths and "..". */
static int safe_path(char *out, size_t outsz, const char *root, const char *name) {
    while (*name == '/') name++;
    if (strstr(name, "../") != NULL) return -1;
    if (strcmp(name, "..") == 0) return -1;
    if (snprintf(out, outsz, "%s/%s", root, name) >= (int)outsz) return -1;
    return 0;
}

static void apply_whiteout(const char *root, const char *dirname, const char *base) {
    if (strcmp(base, ".wh..wh..opq") == 0) {
        /* Opaque dir: remove everything already present in dirname */
        char dirpath[PATH_SIZE];
        if (safe_path(dirpath, sizeof(dirpath), root, dirname) < 0) return;
        DIR *d = opendir(dirpath);
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char child[PATH_SIZE];
            if (snprintf(child, sizeof(child), "%s/%s", dirpath, de->d_name) < (int)sizeof(child))
                remove(child);
        }
        closedir(d);
        return;
    }
    /* Regular whiteout: delete "<dirname>/<name-after-.wh.>" */
    const char *target = base + 4; /* skip ".wh." */
    char path[PATH_SIZE];
    char dirbuf[PATH_SIZE];
    if (dirname[0])
        snprintf(dirbuf, sizeof(dirbuf), "%s/%s", dirname, target);
    else
        snprintf(dirbuf, sizeof(dirbuf), "%s", target);
    if (safe_path(path, sizeof(path), root, dirbuf) == 0)
        remove(path);
}

static int extract(const char *tar, size_t tarlen, const char *root, int preserve);

int tar_extract_mem(const char *tar, size_t tarlen, const char *root) {
    return extract(tar, tarlen, root, 0);
}

/* Layer unpack (native store): whiteouts become char 0:0 devices so
 * overlayfs stacking honors cross-layer deletions. Must run inside a
 * userns (mknod of a char device needs CAP_MKNOD there). */
int tar_extract_layer(const char *tar, size_t tarlen, const char *root) {
    return extract(tar, tarlen, root, 1);
}

static int extract(const char *tar, size_t tarlen, const char *root, int preserve) {
    size_t off = 0;
    char longname[PATH_SIZE] = "";

    while (off + 512 <= tarlen) {
        const char *h = tar + off;
        int empty = 1;
        for (int i = 0; i < 512; i++) if (h[i]) { empty = 0; break; }
        if (empty) break; /* end-of-archive marker */

        char name[PATH_SIZE];
        char prefix[160];
        memcpy(name, h + 0, 100);
        name[100] = '\0';
        unsigned long long size = tar_octal(h + 124, 12);
        char typeflag = h[156];
        memcpy(prefix, h + 345, 155);
        prefix[155] = '\0';

        off += 512;

        if (longname[0]) {
            /* Previous header was GNU 'L' — its data is the real name */
            snprintf(name, sizeof(name), "%s", longname);
            longname[0] = '\0';
            if (typeflag == 'L' || typeflag == 'K') { /* nested longname: skip data */ }
        }

        size_t data_blocks = (size_t)((size + 511) / 512) * 512;
        const char *data = tar + off;

        if (typeflag == 'L') {
            /* GNU long name for the NEXT header */
            size_t cp = size < sizeof(longname) - 1 ? size : sizeof(longname) - 1;
            memcpy(longname, data, cp);
            longname[cp] = '\0';
            /* strip trailing NULs */
            for (size_t k = cp; k > 0 && longname[k - 1] == '\0'; k--) longname[k - 1] = '\0';
            off += data_blocks;
            continue;
        }
        if (typeflag == 'K' || typeflag == 'x' || typeflag == 'g') {
            /* longlink / pax extended headers: skip */
            off += data_blocks;
            continue;
        }

        /* Build full name from prefix + name */
        char full[PATH_SIZE];
        {
            int fw = prefix[0]
                ? snprintf(full, sizeof(full), "%s/%s", prefix, name)
                : snprintf(full, sizeof(full), "%s", name);
            /* A truncated entry path is a wrong path -- never extract to
             * it. Skip the entry, matching the safe_path failure below. */
            if (fw < 0 || (size_t)fw >= sizeof(full)) {
                off += data_blocks;
                continue;
            }
        }

        char base[PATH_SIZE];
        { const char *sl = strrchr(full, '/'); snprintf(base, sizeof(base), "%s", sl ? sl + 1 : full); }

        /* Whiteout handling */
        if (!strncmp(base, ".wh.", 4)) {
            char dir[PATH_SIZE];
            const char *slash = strrchr(full, '/');
            if (slash) { size_t dl = (size_t)(slash - full); if (dl >= sizeof(dir)) dl = sizeof(dir) - 1; memcpy(dir, full, dl); dir[dl] = '\0'; }
            else dir[0] = '\0';
            if (preserve && strcmp(base, ".wh..wh..opq") != 0) {
                /* Materialize as a char 0:0 whiteout device (userns
                 * required). Opaque dirs (.wh..wh..opq) are not yet
                 * materialized -- a known limitation. */
                char wpath[PATH_SIZE], fpath[PATH_SIZE], parent[PATH_SIZE];
                if (dir[0])
                    snprintf(wpath, sizeof(wpath), "%s/%s", dir, base + 4);
                else
                    snprintf(wpath, sizeof(wpath), "%s", base + 4);
                if (safe_path(fpath, sizeof(fpath), root, wpath) == 0) {
                    snprintf(parent, sizeof(parent), "%s", fpath);
                    char *sl = strrchr(parent, '/');
                    if (sl) *sl = '\0';
                    mkdir_p(parent, 0755);
                    mknod(fpath, S_IFCHR | 0000, makedev(0, 0));
                }
            } else if (!preserve) {
                apply_whiteout(root, dir, base);   /* legacy flatten */
            }
            off += data_blocks;
            continue;
        }

        char path[PATH_SIZE];
        if (safe_path(path, sizeof(path), root, full) < 0) {
            off += data_blocks;
            continue;
        }

        if (typeflag == '5') { /* directory */
            mkdir_p(path, 0755);
        } else if (typeflag == '2') { /* symlink */
            char target[PATH_SIZE];
            memcpy(target, h + 157, 100);
            target[100] = '\0';
            char parent[PATH_SIZE];
            snprintf(parent, sizeof(parent), "%s", path);
            char *slash = strrchr(parent, '/');
            if (slash) *slash = '\0';
            mkdir_p(parent, 0755);
            remove(path);
            symlink(target, path);
        } else if (typeflag == '1') { /* hardlink: treat as symlink-ish copy target */
            char target[PATH_SIZE];
            memcpy(target, h + 157, 100);
            target[100] = '\0';
            char linkpath[PATH_SIZE];
            char parent[PATH_SIZE];
            snprintf(parent, sizeof(parent), "%s", path);
            char *slash = strrchr(parent, '/');
            if (slash) *slash = '\0';
            if (safe_path(linkpath, sizeof(linkpath), root, target) == 0) {
                mkdir_p(parent, 0755);
                remove(path);
                link(linkpath, path);
            }
        } else { /* regular file ('0' or '\0') */
            char parent[PATH_SIZE];
            snprintf(parent, sizeof(parent), "%s", path);
            char *slash = strrchr(parent, '/');
            if (slash) *slash = '\0';
            mkdir_p(parent, 0755);

            unsigned mode = (unsigned)tar_octal(h + 100, 8);
            if (!mode) mode = 0644;
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
            if (fd >= 0) {
                if (size) { ssize_t w = write(fd, data, (size_t)size); (void)w; }
                close(fd);
            }
        }

        off += data_blocks;
    }
    return 0;
}

/* ───────────────────────── writer ───────────────────────── */

/* Write an octal field: n bytes, (n-1) octal digits, NUL. */
static void hdr_octal(char *field, size_t n, unsigned long long v) {
    char tmp[32];
    int w = snprintf(tmp, sizeof(tmp), "%0*llo", (int)n - 1, v);
    if (w < 0 || (size_t)w > n - 1) w = (int)n - 1;   /* overflow: truncate */
    memset(field, '0', n - 1);
    memcpy(field, tmp, (size_t)w < n - 1 ? (size_t)w : n - 1);
    field[n - 1] = '\0';
}

/* Build a 512-byte header into h (must be 512 bytes). Returns 0, or -1 if
 * the name cannot be represented (no ustar split and too long for 'L'
 * fallback handled by the caller). */
static void hdr_build(char h[512], const char *name, const char *linkname,
                      unsigned long long size, unsigned mode, char typeflag,
                      time_t mtime, const char *prefix) {
    memset(h, 0, 512);
    const char *nm = name;
    if (prefix && prefix[0]) {
        size_t pl = strlen(prefix);
        memcpy(h + 345, prefix, pl > 155 ? 155 : pl);
        /* name field gets the part after the prefix '/' */
        nm = name + pl + 1;
    }
    strncpy(h + 0, nm, 100);
    hdr_octal(h + 100, 8, mode & 07777);
    hdr_octal(h + 108, 8, 0);            /* uid: root in the layer */
    hdr_octal(h + 116, 8, 0);            /* gid */
    hdr_octal(h + 124, 12, size);
    hdr_octal(h + 136, 12, (unsigned long long)mtime);
    memset(h + 148, ' ', 8);             /* chksum placeholder */
    h[156] = typeflag;
    if (linkname) strncpy(h + 157, linkname, 100);
    memcpy(h + 257, "ustar", 5);
    h[262] = '\0';
    h[263] = '0'; h[264] = '0';
    /* checksum over the whole block with chksum field as spaces */
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (unsigned char)h[i];
    char cks[8];
    snprintf(cks, sizeof(cks), "%06o", sum);
    memcpy(h + 148, cks, 6);
    h[154] = '\0';
    h[155] = ' ';
}

/* Split name into ustar prefix (<=155) + basename (<=100). Returns the
 * prefix length chosen, or -1 if unsplittable. */
static int name_split(const char *name, char *prefix, size_t psz, char *base, size_t bsz) {
    (void)psz;
    size_t len = strlen(name);
    if (len <= 100) { prefix[0] = '\0'; snprintf(base, bsz, "%s", name); return 0; }
    /* rightmost '/' that leaves basename <=100 and prefix <=155 */
    for (size_t i = len; i > 0; i--) {
        if (name[i - 1] != '/') continue;
        size_t pre = i - 1, bas = len - i;
        if (bas <= 100 && pre <= 155 && pre > 0) {
            memcpy(prefix, name, pre); prefix[pre] = '\0';
            memcpy(base, name + i, bas); base[bas] = '\0';
            return (int)pre;
        }
    }
    return -1;
}

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int emit_entry(int fd, const char *rel, const char *linkname,
                      unsigned long long size, unsigned mode, char typeflag,
                      const char *content) {
    char prefix[160], base[104];
    char h[512];
    if (name_split(rel, prefix, sizeof(prefix), base, sizeof(base)) >= 0) {
        hdr_build(h, rel, linkname, size, mode, typeflag, time(NULL), prefix);
        if (write_all(fd, h, 512) < 0) return -1;
    } else {
        /* GNU longname: an 'L' entry carrying the full name, then the
         * header with a truncated name field (the reader uses the 'L'). */
        char lh[512];
        unsigned long long nlen = (unsigned long long)strlen(rel);
        hdr_build(lh, "././@LongLink", NULL, nlen + 1, 0, 'L', time(NULL), NULL);
        lh[156] = 'L';
        if (write_all(fd, lh, 512) < 0) return -1;
        if (write_all(fd, rel, (size_t)nlen) < 0) return -1;
        size_t pad = (size_t)((nlen + 1 + 511) / 512) * 512 - (size_t)nlen;
        char z[512]; memset(z, 0, sizeof(z));
        if (write_all(fd, z, pad) < 0) return -1;
        hdr_build(h, rel, linkname, size, mode, typeflag, time(NULL), NULL);
        if (write_all(fd, h, 512) < 0) return -1;
    }
    if (typeflag == '0' && size && content) {
        if (write_all(fd, content, (size_t)size) < 0) return -1;
        size_t pad = (size_t)((size + 511) / 512) * 512 - (size_t)size;
        char z[512]; memset(z, 0, sizeof(z));
        if (pad) { memset(z, 0, pad); if (write_all(fd, z, pad) < 0) return -1; }
    }
    return 0;
}

static int pack_walk(int fd, const char *root, const char *rel) {
    char dirpath[PATH_SIZE];
    if (rel[0])
        snprintf(dirpath, sizeof(dirpath), "%s/%s", root, rel);
    else
        snprintf(dirpath, sizeof(dirpath), "%s", root);

    DIR *d = opendir(dirpath);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char childrel[PATH_SIZE];
        if (rel[0])
            snprintf(childrel, sizeof(childrel), "%s/%s", rel, de->d_name);
        else
            snprintf(childrel, sizeof(childrel), "%s", de->d_name);
        char childpath[PATH_SIZE];
        {
            int cw = snprintf(childpath, sizeof(childpath), "%s/%s", root, childrel);
            /* A truncated child path is a wrong path -- skip the entry
             * rather than lstat/open something else. */
            if (cw < 0 || (size_t)cw >= sizeof(childpath)) continue;
        }

        struct stat st;
        if (lstat(childpath, &st) < 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (emit_entry(fd, childrel, NULL, 0, (unsigned)st.st_mode & 07777,
                           '5', NULL) < 0) { closedir(d); return -1; }
            if (pack_walk(fd, root, childrel) < 0) { closedir(d); return -1; }
        } else if (S_ISREG(st.st_mode)) {
            int f = open(childpath, O_RDONLY);
            if (f < 0) { closedir(d); return -1; }
            char *content = NULL;
            if (st.st_size) {
                content = malloc((size_t)st.st_size);
                if (!content) { close(f); closedir(d); return -1; }
                ssize_t rd = 0, got = 0;
                while (got < st.st_size &&
                       (rd = read(f, content + got, (size_t)st.st_size - got)) > 0)
                    got += rd;
                if (got != st.st_size) { free(content); close(f); closedir(d); return -1; }
            }
            close(f);
            int rc = emit_entry(fd, childrel, NULL, (unsigned long long)st.st_size,
                                (unsigned)st.st_mode & 07777, '0', content);
            free(content);
            if (rc < 0) { closedir(d); return -1; }
        } else if (S_ISLNK(st.st_mode)) {
            char target[PATH_SIZE];
            ssize_t n = readlink(childpath, target, sizeof(target) - 1);
            if (n < 0) { closedir(d); return -1; }
            target[n] = '\0';
            if (emit_entry(fd, childrel, target, 0, 0777, '2', NULL) < 0) {
                closedir(d); return -1;
            }
        } else if (S_ISCHR(st.st_mode) &&
                   major(st.st_mode) == 0 && minor(st.st_mode) == 0) {
            /* Overlay whiteout: emit a portable .wh.<name> entry. */
            char whrel[PATH_SIZE];
            const char *slash = strrchr(childrel, '/');
            if (slash)
                snprintf(whrel, sizeof(whrel), "%.*s/.wh.%s",
                         (int)(slash - childrel), childrel, slash + 1);
            else
                snprintf(whrel, sizeof(whrel), ".wh.%s", childrel);
            if (emit_entry(fd, whrel, NULL, 0, 0644, '0', NULL) < 0) {
                closedir(d);
                return -1;
            }
        }
        /* other node types (devices, sockets, non-whiteout char devs) skipped */
    }
    closedir(d);
    return 0;
}

int tar_pack_dir(const char *root, const char *out_path) {
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int rc = pack_walk(fd, root, "");
    /* end-of-archive: two zero blocks */
    char z[1024]; memset(z, 0, sizeof(z));
    if (rc == 0) rc = write_all(fd, z, sizeof(z));
    close(fd);
    return rc;
}
