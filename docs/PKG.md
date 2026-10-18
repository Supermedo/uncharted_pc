# PS4 .pkg files

The launcher can unpack a `.pkg` into a plain game folder. This page says exactly what that
covers, because the difference between "a `.pkg`" and "a usable dump" is where most of the
confusion sits.

## Try it

Point the launcher's **Game folder** Browse at a folder containing `.pkg` files. The launcher
notices and offers to unpack them into a new folder next to the originals, then carries that
folder over for you. From a terminal:

```bash
python scripts/pkg.py EP9000-CUSA02343_00-UNCHARTED4.pkg --out CUSA02343
python scripts/pkg.py game.pkg --out CUSA02343 --list     # inspect, write nothing
```

## What it does

A `.pkg` is a container: a header, an entry table, and a blob of file names. `scripts/pkg.py`
reads the header, walks the entry table, resolves each name, and writes the files out.

The format is awkward in two ways worth knowing, because both are silent-corruption traps if
you get them wrong:

- **Everything is big-endian** — the magic is the raw bytes `7F 43 4E 54`, and both the header
  fields and the 32-byte entry table are big-endian. This was verified against a real retail
  package: read little-endian, the entry count comes out as `0x33000000` and the table offset as
  3 GB. `tests/test_pkg.py` pins the header values observed in a real file.
- **Entry names come from the file** and are treated as untrusted input. `safe_relative()`
  refuses absolute paths, drive letters, and anything containing `..`, so a hostile package
  cannot write outside the folder you chose.

The container is read by seeking. A retail game package here is **41.5 GB**; listing it takes
about 0.1 s and 9 MB of memory, because only the header and entry table are ever read.

## What it does not do

**Encrypted packages are not decrypted.** A retail `.pkg` (`drm_type` `0x0F`) stores only its
*metadata* in the readable outer container — `sce_sys`, icons, trophies, `changeinfo`. The game
itself (`eboot.bin`, `u1data/`, `u2data/`, `u3data/`) lives in an **encrypted PFS image** inside
the package. Unpacking one yields an icons-only folder, not a game, and the tool says so rather
than reporting success. Getting the game out requires the console's keys: that is decryption,
not unpacking, and it is out of scope.

So: **if your `.pkg` is a retail package, unpacking it here will not give you a playable
game.** You need your own decrypted dump. This tool is for packages that are already
unencrypted.

**Delta patches are refused.** A delta package (content type `0x1E`, or
`content_flags & 0x41000000`) stores *differences* against a base package rather than whole
files. Merging one offline is a different problem: the PS4 normally resolves them at runtime in
firmware, and the usual community workaround is to extract the patch and **repack** it against
your own base rather than merge diffs. This tool declines rather than half-applying a patch,
because a partial merge yields a folder that passes every check and then crashes.

**Cumulative patches extract normally.** For those, and for ordinary base packages, extracting
several packages into the same `--out` folder in order leaves the newest file at each path.

## Mods still do not merge

Unchanged, and for the same underlying reason: `.dcx` and `.bnd` archives are copied whole, not
merged. See [MODS.md](MODS.md).

## Layout reference

Header (little-endian, `0x1000` bytes):

| Offset | Field |
|---|---|
| `0x00` | magic bytes `7F 43 4E 54` (`\x7fCNT`) |
| `0x10` | entry count |
| `0x18` | entry table offset |
| `0x40` | content id, 36 bytes |
| `0x70` | drm type (`0x00` none, `0x0F` PS4 = encrypted) |
| `0x74` | content type (`0x1A` game, `0x1E` delta patch) |
| `0x78` | content flags |

All big-endian. Entry table entry (32 bytes): `id`, `name_offset`, `flags1`, `flags2`,
`offset`, `size`, `pad`. File names live in the entry with id `0x200` as NUL-separated strings.
`encrypted = flags1 & 0x80000000`, `key index = (flags2 & 0xF000) >> 12`. Entry ids below
`0x1000` are the container's own metadata (digests, entry keys, the names blob), not game
files.

Primary reference: [psdevwiki.com/ps4/PKG_files](https://www.psdevwiki.com/ps4/PKG_files).

## Verified against a real package

The parser was checked against an actual retail package
(`EP9000-CUSA02344_00-UNCHARTEDTRILOGY`, base and 1.02 update), which is what caught two bugs
that the synthetic fixtures could not:

- the header endianness, which had been written little-endian by assumption, and
- `read_bytes()` on the whole file, which would have needed 41.5 GB of RAM.

Listing both packages now reads correctly: 30 entries / 19 named files for the base, 51 / 41 for
the update, with `drm_type` `0x0F` and content type `0x1A` in both.

What is **not** verified: writing a real package's files out. Both of these are retail packages,
so the files they would write are metadata only. The extraction path is exercised against
synthetic images that carry game files (`tests/test_pkg.py`), and the two code paths are the
same, but no real package containing a plaintext `eboot.bin` was available to try.