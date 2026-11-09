# Patch mods: adding things to a level without sharing game data

A mod that replaces a level has to contain that whole level, and the level is game data, so it can't be shared. A patch
mod holds only a recipe and the author's own models and textures. The launcher builds the changed level on the player's
PC, from the player's own disc.

    mods/my-mod/
      patch.json
      assets/thing.modl
      assets/thing.rwtx

Players put the folder in the mods folder, or press **Add a mod (zip)** on the launcher's Mods page. The game needs no
new code: the launcher writes the built level to `mods/!patched`, and the game reads that like any other mod. A folder
can have `files/` and `textures/` as well as `patch.json`.

## The recipe

```json
{
  "format": 1,
  "name": "My mod",
  "levels": {
    "h002": {
      "base_sha256": "SHA-256 of the level file this was made for",
      "add": [
        { "type": "RWTX", "name": "thing.RW3", "file": "assets/thing.rwtx" },
        { "type": "MODL", "name": "thing", "file": "assets/thing.modl" },
        { "type": "RWTX", "from": { "level": "l011", "id": "32345294", "sha256": "SHA-256 of that asset" } },
        { "type": "MODL", "from": { "level": "l011", "id": "50721989", "sha256": "SHA-256 of that asset" } }
      ],
      "objects": [
        { "name": "MY THING", "model": "thing", "pos": [-24, 1.8, 1.25], "yaw": 90 },
        { "name": "MY PROP", "model": "crab_barrel", "pos": [-24, 0.12, 1.25], "yaw": 30,
          "scale": [0.8, 0.8, 0.8], "tint": [0.85, 0.95, 0.92], "solid": false, "see_through": 1.0 }
      ]
    }
  }
}
```

Top level: `format` (only 1 exists), `levels`, and optional `name`, `author` and `description`. Unknown settings are
refused, so a typing mistake is reported instead of being ignored.

`add` lists assets to put in the level, either your own file or one copied from another level of the player's disc
(`from`, by level and asset ID, checked by hash, so no game bytes are in your mod).

| Asset | What it has to be |
| --- | --- |
| `MODL` | A little-endian RenderWare 3.1 (0x310) clump with plain geometry and a material mesh list, up to 13000 triangles (the largest model on the disc has 12159). The launcher counts them from the file, in both the geometry header and the mesh list. |
| `RWTX` | A RenderWare texture dictionary holding GameCube native textures. Its name must end in `.RW3`: materials find a texture by the hash of that name, so name it what your model's material asks for. |

`objects` places things in the level:

| Setting | Meaning | Default |
| --- | --- | --- |
| `name` | Unique within the level; the object's ID is the hash of it | required |
| `model` | Name of a `MODL` in this recipe's `add`, or one the level already has | required |
| `pos` | x, y, z | required |
| `yaw` | Degrees | 0 |
| `scale` | x, y, z | 1, 1, 1 |
| `tint` | Red, green, blue, multiplied onto the model | 1, 1, 1 |
| `solid` | Scooby stands on it and can't walk through it | false |
| `see_through` | The camera's fade when the object is in the way; 1 means it never fades | 1 |

Format 1 only adds. It can't move, change or remove what's in the level.

## What the game does with an added object

Measured in hub level h002:

- **Lighting:** none at run time. A model shows as texture x its vertex colours x `tint`, so bake the shading into the
  vertex colours and pick them to match what's around it.
- **Collision:** with `solid`, Scooby lands on it. It is one-sided: he isn't pushed out if he ends up inside.
- **Saves:** an added object isn't in the save file. A save made with the mod loads without it and the other way round.
- **Interpolation at 60 fps and above:** the object is handled like any other in the level.
- **Size of one material:** the game's draw count is 16 bits. A material with more than 65,535 vertices (21,845 triangles)
  **crashes the game**, because the count wraps and the rest of the vertices are run as commands. The 13000 triangle limit
  keeps a model well under this. To go past it, use more than one model.
- **Memory:** models come out of the OS heap at about 0.9 of their file size, not out of the main heap. Each level has a
  budget for added bytes, in `launcher/ModPatches/PatchBuilder.cs`. The hubs' budgets are sizes that ran there, not the
  most that fits. Other levels haven't been measured and get the smallest budget. What happens when memory really runs out
  hasn't been separated from the limit above: don't rely on it failing gently.

## When things go wrong

A problem costs that mod and never the game. The Mods page shows the reason under the mod.

| Situation | Result |
| --- | --- |
| The player's level or a `from` asset has a different hash | Mod skipped: "made for a different copy of the game" |
| Bad recipe, missing or malformed asset, a link, a path out of the folder | Mod skipped, reason shown |
| Over a level's budget | Mod skipped |
| Two mods add the same ID with the same bytes | Shared, both apply |
| Two mods add the same ID with different bytes | The later name is skipped, and the other mod is named |
| Two mods change one level | Both apply, in name order |
| A mod with `files/` replaces the same level | That mod wins, and the patch mod's line says so |
| A mod fails in one of its levels | None of that mod is applied |
| Something unexpected in the launcher | `!patched` is removed, the player is told, and the game starts unmodded |

## When the level gets built

The launcher checks when it opens, on a tick or Refresh, and before Play. It rebuilds only if something changed: the
enabled mods, their files, or the disc levels they use. The work happens in `UserData/Cache/mod-build`, the result is
read back and checked, then it replaces `mods/!patched` in one move. So the game never sees a half-written level, and
the disc and the saves are never touched. A game started without the launcher uses the last build.

`!patched` holds game data from the player's own disc. **Don't share it**, and don't publish a mod with a changed level
in it.

## Zips are untrusted

**Add a mod (zip)** unpacks into the cache, and the mods folder changes only in the final move. A zip is refused for
any path that leaves the folder or that means something else on Windows or Linux, links, devices, duplicate names,
oversized contents, or a recipe or asset that fails the checks above.

## Tests

`dotnet run --project tools/hip-patch-tests` needs only dotnet. Set `DOTNET_CLI_TELEMETRY_OPTOUT=1` first (PowerShell: `$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'`; bash: `export DOTNET_CLI_TELEMETRY_OPTOUT=1`): the project has a no-telemetry rule. On a real disc, with results kept in a git-ignored folder:

    dotnet run --project tools/hip-patch-tests -- --disc DISC
    dotnet run --project tools/hip-patch-tests -- --export DISC WORK
    python tools/hip-patch-tests/reference.py DISC WORK
    dotnet run --project tools/hip-patch-tests -- --recipe MODFOLDER --disc DISC --expect SHA256

They check that every level on the disc rebuilds byte for byte, that the launcher's code agrees byte for byte with
`tools/debug/game/hipedit.py`, and that a mod builds to a known level.
