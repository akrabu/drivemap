#define _DARWIN_C_SOURCE 1

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/attr.h>
#include <sys/fcntl.h>
#include <fcntl.h>
#include <fts.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>

#ifndef __APPLE__
#error "drivemap is macOS-only"
#endif

#define VERSION "0.4.1"
#define DEFAULT_CELLS 65536ULL
#define MIN_CELLS 1024ULL
#define MAX_CELLS 1048576ULL
#define TOPN 20

struct extent_rec {
    uint64_t phys;
    uint64_t len;
    uint8_t fragmented_file;
    uint8_t _pad[7];
};

struct file_extent {
    uint64_t phys;
    uint64_t len;
};

struct top_file {
    uint64_t extents;
    uint64_t size;
    char *path;
};

struct stats {
    uint64_t files_seen;
    uint64_t files_mapped;
    uint64_t files_fragmented;
    uint64_t files_zero;
    uint64_t files_open_failed;
    uint64_t files_map_failed;
    uint64_t files_sparse;
    uint64_t files_with_extents;
    uint64_t total_extents;
    uint64_t logical_file_bytes;
    uint64_t logical_mapped_bytes;
    uint64_t physical_min;
    uint64_t physical_max_end;
    uint64_t extent_buckets[4];
    uint64_t *extent_counts;
    size_t extent_counts_len;
    size_t extent_counts_cap;
};

static struct top_file top_files[TOPN];
static bool g_quiet = false;
static bool g_si = false;

static void usage(FILE *f) {
    fprintf(f,
        "drivemap %s — read-only APFS/HFS+ file-extent visualizer\n\n"
        "Usage:\n"
        "  drivemap [options] <directory-or-mounted-volume>\n"
        "  drivemap --watch-defrag <mounted-apfs-volume-path>\n\n"
        "Options:\n"
        "      --watch-defrag  Live-monitor automatic APFS defrag activity\n"
        "      --si            Use decimal SI sizes (kB/MB/GB, powers of 1000)\n"
        "  -o, --output FILE   HTML output path (default: DATE_TIME NAME.html)\n"
        "  -c, --cells N       Map resolution, %llu..%llu cells (default: %llu)\n"
        "  -q, --quiet         Suppress progress output\n"
        "  -h, --help          Show this help\n"
        "      --version       Show version\n\n"
        "Scanning a directory produces a directory-specific fragmentation summary\n"
        "with extent buckets, percentiles, totals, averages, and worst files.\n\n"
        "The target is opened only for reading. The program contains no calls that\n"
        "move, rewrite, clone, truncate, punch, or otherwise alter file contents.\n",
        VERSION,
        (unsigned long long)MIN_CELLS,
        (unsigned long long)MAX_CELLS,
        (unsigned long long)DEFAULT_CELLS);
}

static void human_bytes(uint64_t n, char *buf, size_t sz) {
    static const char *iec[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    static const char *si[] = {"B", "kB", "MB", "GB", "TB", "PB"};
    const char *const *u = g_si ? si : iec;
    double base = g_si ? 1000.0 : 1024.0;
    double x = (double)n;
    int i = 0;
    while (x >= base && i < 5) { x /= base; i++; }
    if (i == 0) snprintf(buf, sz, "%.0f %s", x, u[i]);
    else snprintf(buf, sz, "%.2f %s", x, u[i]);
}

static bool push_extent_count(struct stats *st, uint64_t extents) {
    if (extents == 0) return true;
    if (st->extent_counts_len == st->extent_counts_cap) {
        size_t next = st->extent_counts_cap == 0 ? 1024 : st->extent_counts_cap * 2;
        if (next < st->extent_counts_cap || next > SIZE_MAX / sizeof(*st->extent_counts)) {
            errno = ENOMEM;
            return false;
        }
        uint64_t *p = realloc(st->extent_counts, next * sizeof(*p));
        if (!p) return false;
        st->extent_counts = p;
        st->extent_counts_cap = next;
    }
    st->extent_counts[st->extent_counts_len++] = extents;
    return true;
}

static int compare_u64(const void *a, const void *b) {
    uint64_t aa = *(const uint64_t *)a;
    uint64_t bb = *(const uint64_t *)b;
    return (aa > bb) - (aa < bb);
}

static uint64_t extent_percentile(const struct stats *st, unsigned percent) {
    if (st->extent_counts_len == 0) return 0;
    __uint128_t rank = (__uint128_t)percent * st->extent_counts_len + 99;
    size_t index = (size_t)(rank / 100);
    if (index == 0) index = 1;
    if (index > st->extent_counts_len) index = st->extent_counts_len;
    return st->extent_counts[index - 1];
}

static double percent_of(uint64_t part, uint64_t whole) {
    return whole ? 100.0 * (double)part / (double)whole : 0.0;
}


enum fs_kind {
    FS_APFS,
    FS_HFS_PLUS,
    FS_HFSX,
    FS_HFS_FAMILY_UNKNOWN
};

struct fs_info {
    enum fs_kind kind;
    const char *label;
    bool is_apfs;
};

#define HFS_SIG_CLASSIC 0x4244U  /* 'BD' */
#define HFS_SIG_PLUS    0x482BU  /* 'H+' */
#define HFS_SIG_HFSX    0x4858U  /* 'HX' */

static uint16_t swap16_local(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static bool signature_matches(uint32_t sig, uint16_t wanted) {
    uint16_t lo = (uint16_t)(sig & 0xffffU);
    uint16_t hi = (uint16_t)((sig >> 16) & 0xffffU);
    return lo == wanted || hi == wanted ||
           swap16_local(lo) == wanted || swap16_local(hi) == wanted;
}

/*
 * statfs(2) reports HFS-family mounts as "hfs". ATTR_VOL_SIGNATURE exists
 * specifically to distinguish volume formats handled by one filesystem type,
 * so use it to tell HFS+ / HFSX from classic HFS when possible.
 */
static int get_volume_signature(const char *path, uint32_t *signature) {
    struct attrlist al;
    memset(&al, 0, sizeof(al));
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.volattr = ATTR_VOL_INFO | ATTR_VOL_SIGNATURE;

    struct {
        uint32_t length;
        uint32_t signature;
    } buf;
    memset(&buf, 0, sizeof(buf));

    if (getattrlist(path, &al, &buf, sizeof(buf), 0) != 0) {
        return -1;
    }
    *signature = buf.signature;
    return 0;
}

static int detect_filesystem(const char *path, const struct statfs *sfs,
                             struct fs_info *info) {
    if (strcmp(sfs->f_fstypename, "apfs") == 0) {
        info->kind = FS_APFS;
        info->label = "APFS";
        info->is_apfs = true;
        return 0;
    }

    if (strcmp(sfs->f_fstypename, "hfs") == 0) {
        uint32_t sig = 0;
        if (get_volume_signature(path, &sig) == 0) {
            if (signature_matches(sig, HFS_SIG_PLUS)) {
                info->kind = FS_HFS_PLUS;
                info->label = "HFS+";
                info->is_apfs = false;
                return 0;
            }
            if (signature_matches(sig, HFS_SIG_HFSX)) {
                info->kind = FS_HFSX;
                info->label = "HFSX (case-sensitive HFS+)";
                info->is_apfs = false;
                return 0;
            }
            if (signature_matches(sig, HFS_SIG_CLASSIC)) {
                fprintf(stderr,
                    "drivemap: %s is classic HFS, not HFS+; classic HFS is not supported\n",
                    path);
                return -1;
            }
        }

        /*
         * If the mounted HFS driver does not return ATTR_VOL_SIGNATURE, still
         * allow extent mapping. Modern macOS normally uses this mount type for
         * HFS+ / HFSX; the report labels the uncertainty explicitly.
         */
        info->kind = FS_HFS_FAMILY_UNKNOWN;
        info->label = "HFS+ / HFSX (signature unavailable)";
        info->is_apfs = false;
        return 0;
    }

    fprintf(stderr,
        "drivemap: %s is on unsupported filesystem %s (supported: APFS, HFS+)\n",
        path, sfs->f_fstypename);
    return -1;
}

static char *html_escape(const char *s) {
    size_t cap = strlen(s) * 6 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    char *p = out;
    for (; *s; s++) {
        const char *r = NULL;
        switch (*s) {
            case '&': r = "&amp;"; break;
            case '<': r = "&lt;"; break;
            case '>': r = "&gt;"; break;
            case '"': r = "&quot;"; break;
            case '\'': r = "&#39;"; break;
            default: *p++ = *s; continue;
        }
        size_t n = strlen(r);
        memcpy(p, r, n);
        p += n;
    }
    *p = '\0';
    return out;
}

static void top_insert(const char *path, uint64_t extents, uint64_t size) {
    if (extents <= 1) return;

    int pos = TOPN;
    for (int i = 0; i < TOPN; i++) {
        if (top_files[i].path == NULL || extents > top_files[i].extents) {
            pos = i;
            break;
        }
    }
    if (pos == TOPN) return;

    if (top_files[TOPN - 1].path) {
        free(top_files[TOPN - 1].path);
        top_files[TOPN - 1].path = NULL;
    }
    for (int i = TOPN - 1; i > pos; i--) {
        top_files[i] = top_files[i - 1];
    }

    top_files[pos].path = strdup(path);
    top_files[pos].extents = extents;
    top_files[pos].size = size;
}

static bool push_extent(struct file_extent **arr, size_t *n, size_t *cap,
                        uint64_t phys, uint64_t len) {
    if (*n > 0) {
        struct file_extent *prev = &(*arr)[*n - 1];
        if (prev->phys + prev->len == phys) {
            prev->len += len;
            return true;
        }
    }

    if (*n == *cap) {
        size_t next = (*cap == 0) ? 8 : (*cap * 2);
        struct file_extent *p = realloc(*arr, next * sizeof(**arr));
        if (!p) return false;
        *arr = p;
        *cap = next;
    }

    (*arr)[*n].phys = phys;
    (*arr)[*n].len = len;
    (*n)++;
    return true;
}

static int map_file(const char *path, off_t fsize, FILE *extent_stream,
                    struct stats *st) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        st->files_open_failed++;
        return -1;
    }

    struct file_extent *exts = NULL;
    size_t n = 0, cap = 0;
    off_t logical = 0;
    bool had_hole = false;
    bool failed = false;

    while (logical < fsize) {
        struct log2phys l2p;
        memset(&l2p, 0, sizeof(l2p));

        l2p.l2p_devoffset = logical;
        l2p.l2p_contigbytes = fsize - logical;

        if (fcntl(fd, F_LOG2PHYS_EXT, &l2p) == -1) {
            failed = true;
            break;
        }

        if (l2p.l2p_contigbytes <= 0) {
            errno = EIO;
            failed = true;
            break;
        }

        off_t step = l2p.l2p_contigbytes;
        if (step > (fsize - logical)) step = fsize - logical;

        if (l2p.l2p_devoffset < 0) {
            had_hole = true;
        } else {
            if (!push_extent(&exts, &n, &cap,
                             (uint64_t)l2p.l2p_devoffset, (uint64_t)step)) {
                errno = ENOMEM;
                failed = true;
                break;
            }
        }
        logical += step;
    }

    close(fd);

    if (failed) {
        free(exts);
        st->files_map_failed++;
        return -1;
    }

    if (!push_extent_count(st, (uint64_t)n)) {
        free(exts);
        return -3;
    }

    if (had_hole) st->files_sparse++;
    st->files_mapped++;
    st->logical_file_bytes += (uint64_t)fsize;
    st->total_extents += (uint64_t)n;
    if (n > 0) {
        st->files_with_extents++;
        if (n == 1) st->extent_buckets[0]++;
        else if (n <= 4) st->extent_buckets[1]++;
        else if (n <= 10) st->extent_buckets[2]++;
        else st->extent_buckets[3]++;
    }
    if (n > 1) st->files_fragmented++;

    uint64_t file_mapped_bytes = 0;
    for (size_t i = 0; i < n; i++) {
        file_mapped_bytes += exts[i].len;
        uint64_t end = exts[i].phys + exts[i].len;
        if (exts[i].phys < st->physical_min) st->physical_min = exts[i].phys;
        if (end > st->physical_max_end) st->physical_max_end = end;
    }
    st->logical_mapped_bytes += file_mapped_bytes;

    top_insert(path, (uint64_t)n, (uint64_t)fsize);

    struct extent_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.fragmented_file = (n > 1) ? 1 : 0;

    for (size_t i = 0; i < n; i++) {
        rec.phys = exts[i].phys;
        rec.len = exts[i].len;
        if (fwrite(&rec, sizeof(rec), 1, extent_stream) != 1) {
            free(exts);
            return -2;
        }
    }

    free(exts);
    return 0;
}

static uint64_t scale_floor(uint64_t x, uint64_t span, uint64_t cells) {
    __uint128_t v = (__uint128_t)x * (__uint128_t)cells;
    return (uint64_t)(v / span);
}

static uint64_t scale_ceil(uint64_t x, uint64_t span, uint64_t cells) {
    __uint128_t v = (__uint128_t)x * (__uint128_t)cells + span - 1;
    return (uint64_t)(v / span);
}

static void mark_extent(uint8_t *map, uint64_t cells, uint64_t map_start,
                        uint64_t map_end, const struct extent_rec *r) {
    if (r->len == 0 || r->phys >= map_end) return;

    uint64_t rend = r->phys + r->len;
    if (rend < r->phys) rend = UINT64_MAX;
    if (rend <= map_start) return;

    uint64_t s = r->phys < map_start ? map_start : r->phys;
    uint64_t e = rend > map_end ? map_end : rend;
    if (e <= s) return;

    uint64_t span = map_end - map_start;
    uint64_t a = scale_floor(s - map_start, span, cells);
    uint64_t b = scale_ceil(e - map_start, span, cells);

    if (a >= cells) return;
    if (b > cells) b = cells;
    if (b <= a) b = a + 1;

    uint8_t val = r->fragmented_file ? 2 : 1;
    for (uint64_t i = a; i < b; i++) {
        if (val > map[i]) map[i] = val;
    }
}

static int write_rle_js(FILE *out, const uint8_t *map, uint64_t cells) {
    fprintf(out, "const rle=[");
    uint64_t i = 0;
    bool first = true;

    while (i < cells) {
        uint8_t v = map[i];
        uint64_t j = i + 1;
        while (j < cells && map[j] == v) j++;

        if (!first) fputc(',', out);
        fprintf(out, "[%u,%" PRIu64 "]", (unsigned)v, j - i);
        first = false;
        i = j;
    }

    fprintf(out, "];\n");
    return ferror(out) ? -1 : 0;
}

static int write_html(const char *output, const char *root,
                      const struct statfs *sfs, const struct fs_info *fsi,
                      const struct stats *st, FILE *extent_stream,
                      uint64_t cells, uint64_t map_start, uint64_t map_end) {
    uint8_t *map = calloc((size_t)cells, 1);
    if (!map) {
        fprintf(stderr, "drivemap: cannot allocate map (%" PRIu64 " cells)\n", cells);
        return -1;
    }

    rewind(extent_stream);
    struct extent_rec rec;
    while (fread(&rec, sizeof(rec), 1, extent_stream) == 1) {
        mark_extent(map, cells, map_start, map_end, &rec);
    }

    if (ferror(extent_stream)) {
        fprintf(stderr, "drivemap: failed reading temporary extent stream\n");
        free(map);
        return -1;
    }

    FILE *out = fopen(output, "w");
    if (!out) {
        fprintf(stderr, "drivemap: cannot write %s: %s\n", output, strerror(errno));
        free(map);
        return -1;
    }

    char *eroot = html_escape(root);
    char *edev = html_escape(sfs->f_mntfromname);
    if (!eroot || !edev) {
        fclose(out);
        free(map);
        free(eroot);
        free(edev);
        return -1;
    }

    char capbuf[64], totalbuf[64], avgbuf[64], cellbuf[64], minbuf[64], maxbuf[64];
    uint64_t capacity = (uint64_t)sfs->f_blocks * (uint64_t)sfs->f_bsize;
    uint64_t span = map_end - map_start;
    uint64_t percell = (span + cells - 1) / cells;
    uint64_t average_file_size = st->files_mapped
        ? st->logical_file_bytes / st->files_mapped : 0;
    double average_extents = st->files_with_extents
        ? (double)st->total_extents / (double)st->files_with_extents : 0.0;
    uint64_t p50 = extent_percentile(st, 50);
    uint64_t p90 = extent_percentile(st, 90);
    uint64_t p95 = extent_percentile(st, 95);
    uint64_t p99 = extent_percentile(st, 99);
    bool volume_root = strcmp(root, sfs->f_mntonname) == 0;
    const char *scope = volume_root ? "Volume" : "Directory";
    const char *unit_description = g_si
        ? "decimal SI (kB/MB/GB/TB, powers of 1000)"
        : "binary IEC (KiB/MiB/GiB/TiB, powers of 1024)";

    human_bytes(capacity, capbuf, sizeof(capbuf));
    human_bytes(st->logical_file_bytes, totalbuf, sizeof(totalbuf));
    human_bytes(average_file_size, avgbuf, sizeof(avgbuf));
    human_bytes(percell, cellbuf, sizeof(cellbuf));
    human_bytes(st->physical_min == UINT64_MAX ? 0 : st->physical_min,
                minbuf, sizeof(minbuf));
    human_bytes(st->physical_max_end, maxbuf, sizeof(maxbuf));

    double fragpct = st->files_with_extents
        ? 100.0 * (double)st->files_fragmented / (double)st->files_with_extents
        : 0.0;

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S %Z", &tmv);

    fprintf(out,
        "<!doctype html>\n"
        "<html><head><meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
        "<title>%s extent map — %s</title>\n"
        "<style>\n"
        ":root{color-scheme:light dark;font-family:-apple-system,BlinkMacSystemFont,\"SF Pro Text\",system-ui,sans-serif}\n"
        "body{max-width:1180px;margin:32px auto;padding:0 20px;line-height:1.45}\n"
        "h1{margin-bottom:.2rem}.sub{opacity:.72;margin-top:0}\n"
        ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin:22px 0}\n"
        ".card{border:1px solid color-mix(in srgb,currentColor 20%%,transparent);border-radius:12px;padding:14px}\n"
        ".big{font-size:1.45rem;font-weight:650}.label{opacity:.7;font-size:.85rem}\n"
        "canvas{width:100%%;height:auto;image-rendering:pixelated;border:1px solid color-mix(in srgb,currentColor 25%%,transparent);border-radius:8px;background:#262626}\n"
        ".legend{display:flex;flex-wrap:wrap;gap:16px;margin:10px 0 18px;font-size:.9rem}.sw{display:inline-block;width:12px;height:12px;border-radius:2px;margin-right:6px;vertical-align:-1px}\n"
        ".u{background:#2a2a2a}.m{background:#49a66f}.f{background:#e05a47}\n"
        ".note{padding:12px 14px;border-left:4px solid #999;background:color-mix(in srgb,currentColor 6%%,transparent);margin:18px 0}\n"
        "table{width:100%%;border-collapse:collapse}th,td{text-align:left;padding:7px;border-bottom:1px solid color-mix(in srgb,currentColor 14%%,transparent)}td.num,th.num{text-align:right}.path{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.86rem;word-break:break-all}\n"
        "#tip{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.86rem;min-height:1.4em;margin-top:8px}\n"
        "details{margin-top:20px}.muted{opacity:.68}\n"
        "</style></head><body>\n"
        "<h1>%s physical file-extent map</h1>\n"
        "<p class=\"sub\">%s &nbsp;·&nbsp; %s &nbsp;·&nbsp; generated %s</p>\n",
        fsi->label, eroot, fsi->label, eroot, edev, timestr);

    fprintf(out,
        "<div class=\"cards\">\n"
        "<div class=\"card\"><div class=\"big\">%" PRIu64 "</div><div class=\"label\">files mapped</div></div>\n"
        "<div class=\"card\"><div class=\"big\">%" PRIu64 "</div><div class=\"label\">fragmented files (%.2f%%)</div></div>\n"
        "<div class=\"card\"><div class=\"big\">%" PRIu64 "</div><div class=\"label\">physical extents</div></div>\n"
        "<div class=\"card\"><div class=\"big\">%s</div><div class=\"label\">filesystem capacity</div></div>\n"
        "<div class=\"card\"><div class=\"big\">%s</div><div class=\"label\">total logical file size*</div></div>\n"
        "<div class=\"card\"><div class=\"big\">%s</div><div class=\"label\">approx. bytes per map cell</div></div>\n"
        "</div>\n",
        st->files_mapped, st->files_fragmented, fragpct, st->total_extents,
        capbuf, totalbuf, cellbuf);

    fprintf(out,
        "<h2>%s fragmentation summary</h2>\n"
        "<p class=\"muted\">Statistics for files beneath <span class=\"path\">%s</span>. Size units: %s.</p>\n"
        "<div class=\"cards\">\n"
        "<div class=\"card\"><div class=\"big\">%s</div><div class=\"label\">average file size</div></div>\n"
        "<div class=\"card\"><div class=\"big\">%.2f</div><div class=\"label\">average extents per file</div></div>\n"
        "<div class=\"card\"><div class=\"big\">%" PRIu64 "</div><div class=\"label\">median extents (P50)</div></div>\n"
        "<div class=\"card\"><div class=\"big\">%" PRIu64 "</div><div class=\"label\">P90 extents</div></div>\n"
        "<div class=\"card\"><div class=\"big\">%" PRIu64 "</div><div class=\"label\">P95 extents</div></div>\n"
        "<div class=\"card\"><div class=\"big\">%" PRIu64 "</div><div class=\"label\">P99 extents</div></div>\n"
        "</div>\n"
        "<table><thead><tr><th>Physical extents per file</th><th class=\"num\">Files</th><th class=\"num\">Share</th></tr></thead><tbody>\n"
        "<tr><td>1 extent</td><td class=\"num\">%" PRIu64 "</td><td class=\"num\">%.2f%%</td></tr>\n"
        "<tr><td>2–4 extents</td><td class=\"num\">%" PRIu64 "</td><td class=\"num\">%.2f%%</td></tr>\n"
        "<tr><td>5–10 extents</td><td class=\"num\">%" PRIu64 "</td><td class=\"num\">%.2f%%</td></tr>\n"
        "<tr><td>More than 10 extents</td><td class=\"num\">%" PRIu64 "</td><td class=\"num\">%.2f%%</td></tr>\n"
        "</tbody></table>\n",
        scope, eroot, unit_description, avgbuf, average_extents,
        p50, p90, p95, p99,
        st->extent_buckets[0], percent_of(st->extent_buckets[0], st->files_with_extents),
        st->extent_buckets[1], percent_of(st->extent_buckets[1], st->files_with_extents),
        st->extent_buckets[2], percent_of(st->extent_buckets[2], st->files_with_extents),
        st->extent_buckets[3], percent_of(st->extent_buckets[3], st->files_with_extents));

    if (st->files_mapped > st->files_with_extents) {
        fprintf(out,
            "<p class=\"muted\">%" PRIu64 " successfully mapped sparse file(s) had no physical data extents and are omitted from the extent distribution.</p>\n",
            st->files_mapped - st->files_with_extents);
    }

    fprintf(out,
        "<h2>Drive map</h2>\n"
        "<div class=\"legend\"><span><i class=\"sw u\"></i>not mapped by scan</span><span><i class=\"sw m\"></i>mapped file data</span><span><i class=\"sw f\"></i>contains data belonging to a fragmented file</span></div>\n"
        "<canvas id=\"map\" aria-label=\"%s physical file extent map\"></canvas><div id=\"tip\"></div>\n",
        fsi->label);

    if (fsi->is_apfs) {
        fprintf(out,
            "<div class=\"note\"><strong>Important:</strong> dark cells do <em>not</em> mean free space. F_LOG2PHYS_EXT maps visible file data, not APFS metadata, snapshot-only allocations, or every other allocation in the shared APFS container. APFS clones may cause the same physical extent to be referenced by multiple files. Red means the cell contains at least one extent from a file that has multiple physical extents; it does not mean the entire cell is fragmented.</div>\n");
    } else {
        fprintf(out,
            "<div class=\"note\"><strong>Important:</strong> this is a physical extent map of regular-file <em>data forks</em>. Dark cells are not guaranteed to be free: HFS+ metadata files/B-trees, resource forks, inaccessible files, and other allocations may not be represented by the namespace scan. Because HFS+ does not use APFS-style shared container allocation, the physical scatter of the mapped data is a more direct view of this volume's file layout. Red means the cell contains at least one extent from a file that has multiple physical extents.</div>\n");
    }

    fprintf(out,
        "<p class=\"muted\">Map address range: 0x%016" PRIx64 "–0x%016" PRIx64 ". Observed file extents: %s–%s. Resolution: %" PRIu64 " cells.</p>\n",
        map_start, map_end, minbuf, maxbuf, cells);

    fprintf(out,
        "<h2>Most fragmented files</h2>"
        "<table><thead><tr><th>File</th><th class=\"num\">Extents</th><th class=\"num\">Logical size</th></tr></thead><tbody>\n");

    bool any_top = false;
    for (int i = 0; i < TOPN; i++) {
        if (!top_files[i].path) break;
        any_top = true;
        char *ep = html_escape(top_files[i].path);
        char sb[64];
        human_bytes(top_files[i].size, sb, sizeof(sb));
        if (ep) {
            fprintf(out,
                "<tr><td class=\"path\">%s</td><td class=\"num\">%" PRIu64 "</td><td class=\"num\">%s</td></tr>\n",
                ep, top_files[i].extents, sb);
            free(ep);
        }
    }

    if (!any_top) {
        fprintf(out,
            "<tr><td colspan=\"3\">No multi-extent files were successfully mapped.</td></tr>\n");
    }
    fprintf(out, "</tbody></table>\n");

    fprintf(out,
        "<details><summary>Scan details and caveats</summary><ul>\n"
        "<li>Filesystem: %s</li>\n"
        "<li>Files encountered: %" PRIu64 "</li>\n"
        "<li>Mapped files with physical data extents: %" PRIu64 "</li>\n"
        "<li>Zero-length files: %" PRIu64 "</li>\n"
        "<li>Files that could not be opened: %" PRIu64 "</li>\n"
        "<li>Files for which extent mapping failed: %" PRIu64 "</li>\n"
        "<li>Sparse files encountered: %" PRIu64 "</li>\n"
        "<li>Traversal stays on the selected mounted filesystem (FTS_XDEV) and does not follow symlinks.</li>\n"
        "<li>Permissions/TCC can make a scan incomplete. On protected locations, grant Terminal Full Disk Access if you need a fuller view.</li>\n",
        fsi->label, st->files_seen, st->files_with_extents, st->files_zero,
        st->files_open_failed, st->files_map_failed, st->files_sparse);

    if (fsi->is_apfs) {
        fprintf(out,
            "<li>*Total logical file size can double-count APFS clone/shared extents and hard-linked files; it is not a unique-physical-bytes figure.</li>\n");
    } else {
        fprintf(out,
            "<li>*Total logical file size can double-count hard-linked files. Resource-fork data is not scanned in this version.</li>\n");
    }
    fprintf(out, "</ul></details>\n");

    fprintf(out, "<script>\n");
    if (write_rle_js(out, map, cells) != 0) {
        fprintf(stderr, "drivemap: error writing map data\n");
    }

    fprintf(out,
        "const N=%" PRIu64 ", start=%" PRIu64 "n, end=%" PRIu64 "n;\n"
        "const a=new Uint8Array(N);let k=0;for(const [v,n] of rle){a.fill(v,k,k+n);k+=n}\n"
        "const side=Math.ceil(Math.sqrt(N));const rows=Math.ceil(N/side);\n"
        "const c=document.getElementById('map');c.width=side;c.height=rows;\n"
        "const x=c.getContext('2d',{alpha:false});const im=x.createImageData(side,rows);\n"
        "for(let i=0;i<side*rows;i++){let v=i<N?a[i]:0;let r=38,g=38,b=38;if(v===1){r=73;g=166;b=111}else if(v===2){r=224;g=90;b=71}let q=i*4;im.data[q]=r;im.data[q+1]=g;im.data[q+2]=b;im.data[q+3]=255}x.putImageData(im,0,0);\n"
        "const tip=document.getElementById('tip');const span=end-start;\n"
        "const byteBase=%u,byteUnits=%s;\n"
        "function hb(n){let i=0,d=Number(n);while(d>=byteBase&&i<byteUnits.length-1){d/=byteBase;i++}return(i?d.toFixed(2):d.toFixed(0))+' '+byteUnits[i]}\n"
        "c.addEventListener('mousemove',e=>{const r=c.getBoundingClientRect(),cx=Math.floor((e.clientX-r.left)*c.width/r.width),cy=Math.floor((e.clientY-r.top)*c.height/r.height),i=cy*side+cx;if(i>=N){tip.textContent='';return}const s=start+(span*BigInt(i))/BigInt(N),z=start+(span*BigInt(i+1))/BigInt(N);const label=a[i]===2?'fragmented-file extent present':a[i]===1?'mapped file data':'not mapped by scan';tip.textContent='#'+i+'  '+hb(s)+' – '+hb(z)+'  ·  '+label});\n"
        "</script></body></html>\n",
        cells, map_start, map_end,
        g_si ? 1000U : 1024U,
        g_si ? "['B','kB','MB','GB','TB','PB']"
             : "['B','KiB','MiB','GiB','TiB','PiB']");

    free(eroot);
    free(edev);
    free(map);

    if (fclose(out) != 0) {
        fprintf(stderr, "drivemap: error closing output %s: %s\n",
                output, strerror(errno));
        return -1;
    }

    return 0;
}


static bool contains_ci(const char *haystack, const char *needle) {
    if (!*needle) return true;
    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i] &&
               tolower((unsigned char)h[i]) ==
               tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static const char *final_path_component(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    return *name ? name : "root";
}

static int show_defrag_status(const char *device) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "drivemap: fork for diskutil failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl("/usr/sbin/diskutil", "diskutil", "apfs", "defragment",
              device, "status", (char *)NULL);
        fprintf(stderr, "drivemap: cannot execute /usr/sbin/diskutil: %s\n",
                strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "drivemap: waitpid(diskutil): %s\n", strerror(errno));
        return -1;
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/*
 * apfsd's public man page says it performs automatic file defragmentation.
 * Apple does not publish a stable per-file event API for it. Historical
 * unified-log output identifies per-volume defrag activity with activity
 * names such as com.apple.apfs.defrag.disk1s1. We therefore monitor apfsd's
 * unified log and classify only what the log itself exposes.
 */
static int watch_apfs_defrag(const char *resolved, const struct statfs *sfs,
                             const struct fs_info *fsi) {
    if (!fsi->is_apfs) {
        fprintf(stderr,
            "drivemap: --watch-defrag is APFS-only; %s is %s\n",
            resolved, fsi->label);
        return 1;
    }

    const char *device = final_path_component(sfs->f_mntfromname);
    char activity[256];
    if (snprintf(activity, sizeof(activity), "com.apple.apfs.defrag.%s", device)
            >= (int)sizeof(activity)) {
        fprintf(stderr, "drivemap: device identifier is unexpectedly long\n");
        return 1;
    }

    printf("APFS defrag watch\n");
    printf("  Volume:          %s\n", resolved);
    printf("  Device:          %s\n", device);
    printf("  Activity marker: %s\n", activity);
    printf("\nCurrent diskutil setting:\n");
    fflush(stdout);
    (void)show_defrag_status(device);

    printf("\nWatching apfsd unified logs. Press Control-C to stop.\n");
    printf("[target]   = log line explicitly names this volume's defrag activity\n");
    printf("[possible] = generic defrag-related apfsd line with no other volume marker\n\n");
    fflush(stdout);

    int fds[2];
    if (pipe(fds) != 0) {
        fprintf(stderr, "drivemap: pipe: %s\n", strerror(errno));
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "drivemap: fork for log stream failed: %s\n", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) {
            fprintf(stderr, "drivemap: dup2: %s\n", strerror(errno));
            _exit(127);
        }
        close(fds[1]);
        execl("/usr/bin/log", "log", "stream", "--style", "compact",
              "--predicate", "process == \"apfsd\"", (char *)NULL);
        fprintf(stderr, "drivemap: cannot execute /usr/bin/log: %s\n",
                strerror(errno));
        _exit(127);
    }

    close(fds[1]);
    FILE *stream = fdopen(fds[0], "r");
    if (!stream) {
        fprintf(stderr, "drivemap: fdopen(log stream): %s\n", strerror(errno));
        close(fds[0]);
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stream) >= 0) {
        if (strstr(line, activity)) {
            printf("[target]   %s", line);
            continue;
        }

        if (contains_ci(line, "defrag")) {
            const char *other = strstr(line, "com.apple.apfs.defrag.");
            if (other != NULL) {
                /* It explicitly names a different APFS volume. */
                continue;
            }
            printf("[possible] %s", line);
        }
    }
    free(line);
    fclose(stream);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "drivemap: waitpid(log): %s\n", strerror(errno));
        return 1;
    }

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGINT || sig == SIGTERM) return 0;
        fprintf(stderr, "drivemap: log stream terminated by signal %d\n", sig);
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "drivemap: log stream exited unsuccessfully\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *output = NULL;
    char default_output[PATH_MAX];
    const char *target = NULL;
    uint64_t cells = DEFAULT_CELLS;
    bool watch_defrag = false;
    bool output_set = false;
    bool cells_set = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(argv[i], "--version")) {
            printf("drivemap %s\n", VERSION);
            return 0;
        } else if (!strcmp(argv[i], "--watch-defrag")) {
            watch_defrag = true;
        } else if (!strcmp(argv[i], "--si")) {
            g_si = true;
        } else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
            g_quiet = true;
        } else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
            if (++i >= argc) {
                usage(stderr);
                return 2;
            }
            output = argv[i];
            output_set = true;
        } else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--cells")) {
            if (++i >= argc) {
                usage(stderr);
                return 2;
            }
            char *endp = NULL;
            unsigned long long v = strtoull(argv[i], &endp, 10);
            if (!endp || *endp || v < MIN_CELLS || v > MAX_CELLS) {
                fprintf(stderr,
                    "drivemap: --cells must be between %llu and %llu\n",
                    (unsigned long long)MIN_CELLS,
                    (unsigned long long)MAX_CELLS);
                return 2;
            }
            cells = (uint64_t)v;
            cells_set = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "drivemap: unknown option: %s\n", argv[i]);
            usage(stderr);
            return 2;
        } else if (!target) {
            target = argv[i];
        } else {
            fprintf(stderr, "drivemap: only one target path may be scanned\n");
            return 2;
        }
    }

    if (!target) {
        usage(stderr);
        return 2;
    }

    if (watch_defrag && (output_set || cells_set || g_quiet)) {
        fprintf(stderr,
            "drivemap: --watch-defrag cannot be combined with --output, --cells, or --quiet\n");
        return 2;
    }

    char resolved[PATH_MAX];
    if (!realpath(target, resolved)) {
        fprintf(stderr, "drivemap: cannot resolve %s: %s\n",
                target, strerror(errno));
        return 1;
    }

    if (!output_set) {
        time_t started = time(NULL);
        struct tm local_started;
        char stamp[32];
        if (localtime_r(&started, &local_started) == NULL ||
            strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S",
                     &local_started) == 0 ||
            snprintf(default_output, sizeof(default_output), "%s %s.html",
                     stamp, final_path_component(resolved))
                >= (int)sizeof(default_output)) {
            fprintf(stderr, "drivemap: cannot construct default output filename\n");
            return 1;
        }
        output = default_output;
    }

    struct statfs sfs;
    if (statfs(resolved, &sfs) != 0) {
        fprintf(stderr, "drivemap: statfs(%s): %s\n",
                resolved, strerror(errno));
        return 1;
    }

    struct fs_info fsi;
    memset(&fsi, 0, sizeof(fsi));
    if (detect_filesystem(resolved, &sfs, &fsi) != 0) {
        return 1;
    }

    if (watch_defrag) {
        return watch_apfs_defrag(resolved, &sfs, &fsi);
    }

    uint64_t capacity = (uint64_t)sfs.f_blocks * (uint64_t)sfs.f_bsize;
    char capbuf[64];
    human_bytes(capacity, capbuf, sizeof(capbuf));

    if (!g_quiet) {
        fprintf(stderr,
            "%s volume: %s\n"
            "Backing device: %s\n"
            "Filesystem capacity: %s\n"
            "Scanning file extents (read-only)...\n",
            fsi.label, resolved, sfs.f_mntfromname, capbuf);
    }

    FILE *extent_stream = tmpfile();
    if (!extent_stream) {
        fprintf(stderr,
            "drivemap: cannot create anonymous temporary file: %s\n",
            strerror(errno));
        return 1;
    }

    struct stats st;
    memset(&st, 0, sizeof(st));
    st.physical_min = UINT64_MAX;

    char *paths[] = { resolved, NULL };
    FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_XDEV | FTS_NOCHDIR, NULL);
    if (!fts) {
        fprintf(stderr, "drivemap: fts_open: %s\n", strerror(errno));
        fclose(extent_stream);
        return 1;
    }

    FTSENT *ent;
    for (;;) {
        errno = 0;
        ent = fts_read(fts);
        if (ent == NULL) break;
        if (ent->fts_info == FTS_F) {
            st.files_seen++;

            off_t size = ent->fts_statp->st_size;
            if (size <= 0) {
                st.files_zero++;
                continue;
            }

            int r = map_file(ent->fts_path, size, extent_stream, &st);
            if (r == -2) {
                fprintf(stderr,
                    "drivemap: temporary extent stream write failed: %s\n",
                    strerror(errno));
                fts_close(fts);
                fclose(extent_stream);
                free(st.extent_counts);
                return 1;
            }
            if (r == -3) {
                fprintf(stderr,
                    "drivemap: cannot allocate fragmentation summary: %s\n",
                    strerror(errno));
                fts_close(fts);
                fclose(extent_stream);
                free(st.extent_counts);
                return 1;
            }

            if (!g_quiet && (st.files_seen % 10000ULL) == 0) {
                fprintf(stderr,
                    "\rFiles: %" PRIu64
                    "  mapped: %" PRIu64
                    "  fragmented: %" PRIu64
                    "  extents: %" PRIu64,
                    st.files_seen, st.files_mapped,
                    st.files_fragmented, st.total_extents);
                fflush(stderr);
            }
        }
    }

    int traversal_errno = errno;
    if (fts_close(fts) != 0 && traversal_errno == 0) {
        traversal_errno = errno;
    }

    if (!g_quiet) fputc('\n', stderr);
    if (traversal_errno != 0) {
        fprintf(stderr, "drivemap: traversal warning: %s\n",
                strerror(traversal_errno));
    }

    if (st.extent_counts_len > 1) {
        qsort(st.extent_counts, st.extent_counts_len,
              sizeof(*st.extent_counts), compare_u64);
    }

    uint64_t map_start = 0;
    uint64_t map_end = capacity;
    if (st.physical_max_end > map_end) map_end = st.physical_max_end;
    if (map_end == 0) map_end = 1;

    if (write_html(output, resolved, &sfs, &fsi, &st, extent_stream,
                   cells, map_start, map_end) != 0) {
        fclose(extent_stream);
        free(st.extent_counts);
        return 1;
    }

    fclose(extent_stream);

    double fragpct = st.files_with_extents
        ? 100.0 * (double)st.files_fragmented / (double)st.files_with_extents
        : 0.0;

    char totalbuf[64], avgbuf[64];
    uint64_t average_file_size = st.files_mapped
        ? st.logical_file_bytes / st.files_mapped : 0;
    human_bytes(st.logical_file_bytes, totalbuf, sizeof(totalbuf));
    human_bytes(average_file_size, avgbuf, sizeof(avgbuf));
    double average_extents = st.files_with_extents
        ? (double)st.total_extents / (double)st.files_with_extents : 0.0;

    printf("%s extent scan complete\n", fsi.label);
    printf("  Files mapped:        %" PRIu64 "\n", st.files_mapped);
    printf("  Fragmented files:    %" PRIu64 " (%.2f%%)\n",
           st.files_fragmented, fragpct);
    printf("  Physical extents:    %" PRIu64 "\n", st.total_extents);
    printf("  Total logical size:  %s\n", totalbuf);
    printf("  Average file size:   %s\n", avgbuf);
    printf("  Average extents:     %.2f per file\n", average_extents);
    printf("  Extent percentiles:  P50=%" PRIu64 "  P90=%" PRIu64
           "  P95=%" PRIu64 "  P99=%" PRIu64 "\n",
           extent_percentile(&st, 50), extent_percentile(&st, 90),
           extent_percentile(&st, 95), extent_percentile(&st, 99));
    printf("  Extent distribution:\n");
    printf("    1 extent:          %" PRIu64 " (%.2f%%)\n",
           st.extent_buckets[0], percent_of(st.extent_buckets[0], st.files_with_extents));
    printf("    2-4 extents:       %" PRIu64 " (%.2f%%)\n",
           st.extent_buckets[1], percent_of(st.extent_buckets[1], st.files_with_extents));
    printf("    5-10 extents:      %" PRIu64 " (%.2f%%)\n",
           st.extent_buckets[2], percent_of(st.extent_buckets[2], st.files_with_extents));
    printf("    >10 extents:       %" PRIu64 " (%.2f%%)\n",
           st.extent_buckets[3], percent_of(st.extent_buckets[3], st.files_with_extents));
    printf("  Open failures:       %" PRIu64 "\n", st.files_open_failed);
    printf("  Mapping failures:    %" PRIu64 "\n", st.files_map_failed);
    printf("  HTML map:            %s\n", output);

    for (int i = 0; i < TOPN; i++) {
        free(top_files[i].path);
    }
    free(st.extent_counts);

    return 0;
}
