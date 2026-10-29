# drivemap 0.4.1

`drivemap` is a small, macOS-only, **read-only physical file-extent visualizer** for mounted **APFS** and **HFS+ / HFSX** volumes.

It uses macOS `fcntl(..., F_LOG2PHYS_EXT, ...)` to translate logical file offsets into physical device offsets and contiguous run lengths. It does **not** contain code to move, rewrite, truncate, clone, punch, defragment, or otherwise modify target-file contents.

The result is a self-contained HTML report with a block-style physical map, a directory/volume fragmentation summary, and the most fragmented files found in the scan.

## Supported filesystems

- APFS
- HFS+ (`H+` signature)
- HFSX / case-sensitive HFS+ (`HX` signature)

macOS reports HFS-family mounts as `hfs`. `drivemap` asks for `ATTR_VOL_SIGNATURE` to distinguish HFS+ / HFSX from classic HFS when the mounted filesystem provides it. Classic HFS is intentionally rejected.

If the HFS driver does not return the signature, `drivemap` can still perform the extent scan and labels the report `HFS+ / HFSX (signature unavailable)`.

## Build

Install Apple's Command Line Tools if needed:

```bash
xcode-select --install
```

Then:

```bash
cd drivemap
make
```

or simply:

```bash
./build.sh
```

The result is `./drivemap`.

Optional installation:

```bash
sudo make install
```

That installs to `/usr/local/bin/drivemap` by default.

## Use

Scan a mounted APFS or HFS+ volume:

```bash
./drivemap /Volumes/MyDrive
```

The default report name contains the scan start date, local time, and the final component of the scanned path. For example:

```text
2026-08-27_134233 Archive_28TB.html
```

Scanning `/Volumes/Backup/restic-repo/data` would similarly produce a name such as `2026-08-27_134233 data.html`. Only the final folder or file name is used, not the full path.

Open it with:

```bash
open drive-map.html
```

Specify an output path:

```bash
./drivemap -o ~/Desktop/MyDrive-map.html /Volumes/MyDrive
```

Increase map resolution from the default 65,536 cells:

```bash
./drivemap --cells 262144 \
  -o ~/Desktop/MyDrive-map.html \
  /Volumes/MyDrive
```

You can also scan a directory inside a supported volume instead of its root. In that case the map and statistics represent only files reachable beneath that directory.

## Directory fragmentation summaries

Point `drivemap` at any directory to get a summary scoped to that directory tree. This is useful for repositories made of many similarly sized files, including the pack files beneath a restic repository's `data/` directory:

```bash
./drivemap \
  -o ~/Desktop/restic-data-fragmentation.html \
  /Volumes/Backup/restic-repo/data
```

Both the terminal output and HTML report include:

- files mapped and fragmented-file percentage;
- total logical size and average file size;
- average physical extents per file;
- median (P50), P90, P95, and P99 extent counts;
- distribution buckets for 1, 2–4, 5–10, and more than 10 extents; and
- the 20 files with the most physical extents, including each logical size.

Percentiles and extent buckets include successfully mapped files that have physical data extents. A fully sparse file with no physical data is reported separately and omitted from the extent distribution.

For a standard restic repository, target the `data/` directory rather than the repository root if you want statistics specifically for pack files. Traversal remains on the selected mounted filesystem and does not follow symlinks.

## Size units

Human-readable sizes use binary IEC units by default: KiB, MiB, GiB, and TiB (powers of 1024).

Use `--si` for decimal SI units: kB, MB, GB, and TB (powers of 1000):

```bash
./drivemap --si \
  -o ~/Desktop/restic-data-fragmentation-si.html \
  /Volumes/Backup/restic-repo/data
```

The selected unit system is applied consistently to terminal output, HTML summary cards, total and average sizes, the worst-file table, map address tooltips, capacity, and map-cell size.

## Watch APFS automatic defragmentation

`drivemap` can live-monitor the macOS `apfsd` daemon for automatic APFS defragmentation activity associated with a mounted APFS volume:

```bash
./drivemap --watch-defrag /Volumes/MyDrive
```

The mode first runs the read-only status command:

```text
diskutil apfs defragment <device> status
```

and then streams unified-log messages from `apfsd`. Historical macOS versions expose per-volume automatic-defrag activity names such as:

```text
com.apple.apfs.defrag.disk5s1
```

`drivemap` labels a line **`[target]`** only when it explicitly contains the activity marker for the selected volume. A defrag-related `apfsd` line that does not identify another volume is labeled **`[possible]`**. Lines that explicitly identify a different APFS volume are suppressed.

Stop the watch with **Control-C**. This mode does not enable or disable defragmentation and does not touch files merely to provoke defrag; it only reads the current setting and observes unified logging.

### Watch-mode limitation

Apple does not document a stable per-file defragmentation event API, and `apfsd` logging can change between macOS releases. The unified log may omit or redact filenames, so `drivemap` does **not** claim that a log event identifies a particular file unless macOS itself prints that information. If Tahoe changes the activity-marker format, the raw `apfsd` process may still be active while this filter shows fewer events; that would require adjusting the matcher based on Tahoe's actual log output.

## What the map means

- **Green** — the cell contains mapped data from a file that is physically contiguous after adjacent runs are merged.
- **Red** — the cell contains at least one physical extent belonging to a file that has multiple non-adjacent physical extents.
- **Dark** — no regular-file data extent from this scan mapped into the cell.

A file is counted as fragmented when its data fork resolves to more than one non-adjacent physical extent.

## HFS+ interpretation

For HFS+, the map is a fairly direct picture of the physical scatter of the **regular-file data forks** found by the directory traversal. Unlike APFS, an HFS+ volume owns its allocation space rather than sharing a copy-on-write container with sibling volumes.

However, **dark does not strictly mean free space**. This version does not separately enumerate:

- HFS+ filesystem metadata files and B-trees
- resource forks / named streams
- files the current process cannot open
- allocations not represented by the mounted namespace

Hard-linked files can also make the `mapped logical data` total count the same underlying data more than once.

So the HFS+ view is an excellent fragmentation/scatter map of visible data forks, but it is not a replacement for parsing the HFS+ allocation bitmap directly.

## APFS interpretation

APFS is copy-on-write and supports clones/shared extents. APFS volumes also share physical space inside a container. Consequently:

- a file with multiple returned physical extents is meaningfully physically non-contiguous;
- the percentage of multi-extent files is useful as a fragmentation indicator;
- the map shows physical addresses returned for visible file data;
- dark cells do **not** mean free space;
- snapshot-only blocks and filesystem/container metadata are not represented;
- the `mapped logical data` total can double-count clone/shared extents and hard-linked data.

For a spinning APFS HDD, the physical scatter and per-file extent counts are the useful parts.

## Read-only behavior

The target tree is traversed with:

```text
FTS_PHYSICAL | FTS_XDEV | FTS_NOCHDIR
```

Symlinks are not followed, and traversal does not cross onto another mounted filesystem. Regular files are opened with:

```text
O_RDONLY | O_NOFOLLOW | O_CLOEXEC
```

and queried with `F_LOG2PHYS_EXT`.

The program itself writes only:

1. the HTML report at the path you request; and
2. an anonymous temporary extent stream created by `tmpfile()`.

If you do not want the report written to the drive being scanned, choose an output path on another volume, such as:

```bash
./drivemap -o ~/Desktop/map.html /Volumes/MyDrive
```

Opening files read-only is not equivalent to putting the physical disk behind a forensic hardware write blocker; ordinary macOS filesystem/accounting behavior remains outside this program's control.

## Permissions

The tool normally does not require root. macOS privacy/TCC restrictions can prevent Terminal from opening some files. If the report shows many open failures, grant the terminal application **Full Disk Access** if appropriate and rerun the scan.

## Options

```text
drivemap [options] <directory-or-mounted-volume>

    --si            Use decimal SI sizes (kB/MB/GB, powers of 1000)
-o, --output FILE   HTML output path (default: DATE_TIME NAME.html)
-c, --cells N       Map resolution, 1024..1048576 cells
-q, --quiet         Suppress progress output
-h, --help          Show help
    --version       Show version
    --watch-defrag  Live-monitor automatic APFS defrag activity
```

The target may be a mounted volume root or a directory/file within a mounted APFS or HFS+ volume.
