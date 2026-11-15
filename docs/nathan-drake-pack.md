# Nathan Drake Pack

The offline AppContent provider can report the metadata-only Nathan Drake Pack
as installed. Put your own extracted add-on metadata at:

```
<user>/addcont/<TITLE_ID>/NATHANDRAKEPACK1/sce_sys/param.sfo
```

`<user>` is the configured `--user` / `BB_USER_DIR` folder. For example, the EU
collection uses `CUSA02343`; the US collection uses `CUSA02320`. The metadata
must have matching TITLE_ID, CATEGORY `ac`, and CONTENT_ID ending with
`-<TITLE_ID>_00-NATHANDRAKEPACK1`. No metadata, packages or game assets are supplied.

This collection's existing profile generator uses the numeric CUSA title as
parameter 1 when USER_DEFINED_PARAM_1 is absent. The pack reader relies on that
collection-specific convention; it is not a general add-on loader. SKU and
user parameters remain unchanged. The legacy BBCONT01 profile and its JSON
`mounted_addons: []` field do not encode this separately detected entitlement.
The pack has no separate game-data files to mount.

Initialization logs distinguish missing/unreadable, oversized/read-error and
invalid metadata from an installed entitlement. The first four list queries
log their capacity and entries written. Registration alone does not establish
which rewards the game actually uses.

In a campaign, check Options > Bonuses > Rewards > Skins / Weapon Select.
The pack contains the Drake's Fortune, Among Thieves, Drake's Deception,
Wetsuit and Nepal Drake skins, plus Golden AK-47 and Golden 92FS. The golden
weapons may require collecting the first treasure (documented for Uncharted 2).

Local validation used EU CUSA02343 update 1.02 on Windows with a v0.3.13 runtime:
the game queried and received the installed entitlement, and a user screenshot
showed Golden 92FS enabled and Golden AK-47 listed. This does not establish
every skin, weapon firing, all three campaigns or US gameplay behavior.

Run the standalone generic tests with a Clang-compatible compiler on PATH:

```
python -m unittest discover -s tests -p test_content_addons.py -v
```

The runner compiles both API harnesses with NDEBUG defined, verifies existing
base-profile behavior, and executes 52 metadata/API fixtures. Fixtures are
synthetic and need no game files, real metadata, saves or GPU libraries.
