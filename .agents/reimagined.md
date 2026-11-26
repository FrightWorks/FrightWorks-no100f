# Reimagined

What an agent needs to know before working on Reimagined, the experimental new look for levels. Read `REPO.md` first;
this file only covers Reimagined. Keep it current: a change to how Reimagined works updates this file in the same PR.

## What it is

An optional new look, drawn on top of the original renderer and **only when it is on**: with it off, every level must
look and play exactly as the original. So far one level has one: **h002** (the playground, second hub area). It covers:

- relighting: the solid scene lit again from an ambient light, the moon and point lights, with the original textures'
  blue partly taken out (`deblue`);
- a night sky (gradient, stars, the moon's disc with h001's moon texture), mountains round the horizon;
- moon shadows from a shadow map of the level;
- a 3D campfire (logs, rocks, a flame, embers, flickering light) in front of the groundskeeper, who is moved there;
- 12 1960s mercury-vapour post-top lamps in place of the game's lamp posts: their globes glow and feed the bloom, their
  light casts shadows from depth maps, the posts cast moon shadows;
- texture rules: textures hidden (the old lamps, their glow sprite, the blob shadows, the painted shadow decals),
  drawn unlit (sky band, painted backdrops) or drawn solid (cypresses);
- each Scooby snack on the ground starts its spin at its own angle.

**Turning it on**: the launcher's **Extras > Game > Reimagined (experimental)** writes `[game] reimagined = true` to
`Config.toml`; the game reads it at start (`RuntimeConfigFile::Reimagined()`). In the game only the dev menu switches
it (`reimagined 0|1`): there is no hotkey, on purpose (the maintainer's rule: players switch it in the launcher). A test
run's `NO100F_REIMAGINED=<file>` turns it on and uses that file for every level.

## Where the code is

| Piece | File | What |
| --- | --- | --- |
| Light file, relighting, sky, moon, mountains, fire, props, lamps, shadows | `wiicompiled/aurora-main/lib/gfx/scene_stage.cpp` (+ `.hpp`) | Nearly everything. The keyword list is the comment block near the top |
| Frame wiring | `wiicompiled/aurora-main/lib/gfx/common.cpp` | `encode_moon_shadow`, `encode_lamp_shadows`, the sky insert, `scene_stage::encode` at the scene stage pass, the lamp globes' extra pass in `encode_bloom` |
| Per-draw texture rules, snack spin | `wiicompiled/aurora-main/lib/gx/command_processor.cpp` | `hide`/`unlit`/`opaque`/`spin` per draw; the spun position matrix round `build_uniform` |
| Shadow uniform copies | `wiicompiled/aurora-main/lib/gx/shader_info.cpp` | Each lit solid draw gets a uniform copy for the moon's map and one per lamp map; `pipeline.hpp` keeps their ranges on the draw |
| Public API | `wiicompiled/aurora-main/include/aurora/aurora.h`, `lib/aurora.cpp` | `aurora_set_reimagined(0/1/2 reload)`, `aurora_get_reimagined()` (-1 = no file for this level), `aurora_set_reimagined_file(path)`, texture sources |
| Per-level file, entity moves, dev commands | `wiicompiled/runtime/src/no100f_devmenu.cpp` | `UpdateReimagined()` (runs every frame for everyone, at the top of `Draw()`, before the dev-tools check), `ReimaginedFileFor`, `move`/`shift`/`moontex` |
| Setting | `wiicompiled/runtime/include/runtime_config.h` | `[game] reimagined` |
| Launcher option | `launcher/MainWindow.cs` | `ReimaginedBox()` in Extras > Game |
| Light files | `reimagined/<level>.txt` (repo root) | One per level; `tools/stage-assets.py` copies them into `Play/reimagined/` and the packages' `data/reimagined/` (Windows and Linux) |
| Player docs | `docs/playing.md` | "Reimagined (experimental)" |

## How a frame is drawn

The renderer's **scene stage** is the point where the solid scene is finished, before the game's see-through draws
(Scooby, footprints, particles) and the HUD. Order in a frame with Reimagined on:

1. The game's solid draws, with three changes: hidden textures skipped, lit draws lose their baked vertex colours,
   snacks spun (`command_processor.cpp`). Right after the game's frame clear the **sky** goes in (`encode_sky`: sky,
   then the props).
2. **Props** (campfire logs and rocks, lamp posts) are real geometry drawn in that pass with depth, so the game's draws
   hide them and the stage lights them.
3. At the scene stage: **moon shadow map** (the solid passes replayed from the moon with their shadow uniform copies,
   then the props), then the **lamp shadow maps** (the same per lamp, without that lamp's own post).
4. `scene_stage::encode`: **relight** pass (`fs_relight`: ambient + moon x shadow + each point light x its lamp shadow,
   the campfire's scorch), the **flame** (`fs_fire`, added) and its core into the depth buffer (`fs_fire_depth`, so the
   late footprints behind it are hidden but Scooby in front still draws over it), the **lamp globes** (unlit, own pass
   with the scene depth), then AO and ink.
5. The game's see-through draws and HUD.
6. **Bloom**: its light-texture draws again over black, then the lamp globes again (extra last pass in `encode_bloom`,
   which then resolves), so the globes glow.

Conventions: reversed Z (depth compare GreaterEqual, clear 0, the viewport range mapped as `1 - zfar .. 1 - znear`);
GX matrices are used as `vec4f(p, 1) * proj`; the level is drawn in world space with the camera's view matrix, which
the stage finds by **votes** (the position matrix most solid perspective draws use, `note_world_matrix`).

## The light file

`reimagined/<level>.txt`, one item per line, `#` comments, world units (Scooby is about 1.3 tall). Read by aurora
(`load_relight_scene`) and by the runtime (`LoadReimaginedMoves`: only `move`, `shift`, `moontex`). Which file: the
runtime picks `reimagined/<level>.txt` beside the game when a new scene id appears (between levels the last one stays)
and passes it with `aurora_set_reimagined_file`; no file means the level is untouched and `aurora_get_reimagined()`
is -1. `NO100F_REIMAGINED` (or the older `NO100F_RELIGHT`) overrides that for every level.

| Item | Meaning |
| --- | --- |
| `ambient R G B` | Light everywhere |
| `moon DX DY DZ R G B` | Direction toward the moon, its colour |
| `light X Y Z RADIUS R G B` | A point light (up to 32 lights in all, lamps included; the last slot is kept for the fire) |
| `scale RADIUS_SCALE POINT_INTENSITY` | Scales every point light |
| `far NEAR FAR` | The new light fades out from NEAR to FAR units from the camera (backdrops stay as painted) |
| `deblue S` | 0..1: how much of the textures' blue (positive Cb) goes |
| `shadow RADIUS SIZE BIAS` | Moon shadow map: RADIUS units round the view, SIZE texels; RADIUS 0 = none |
| `moonshadow S` | How dark the moon's shadows are (1 = all its light blocked) |
| `lampshadow COUNT [SIZE [BIAS]]` | The COUNT (0..4) lamps nearest the view get shadow maps of SIZE texels |
| `unlit KEY` | Texture drawn as the game draws it |
| `hide KEY` | Texture not drawn |
| `opaque KEY` | Texture drawn solid, never blended (trees the game fades near the camera) |
| `spin KEY` | Models with this texture start their spin at an angle from where they stand |
| `sky ZR ZG ZB HR HG HB` | Night sky: zenith and horizon colours (hide the game's sky band) |
| `moondisc RADIUS_DEG R G B` | The moon in the sky at the moon's direction |
| `moontex KEY [FILE]` | The moon's picture: a texture name looked up like the game's (a pack wins); FILE = a disc level file whose textures are searched too (runtime) |
| `stars DENSITY` | 0 to about 0.1 |
| `mountains LOW_DEG HIGH_DEG R G B` | Ridges round the horizon between those heights |
| `fire X Y Z [SIZE [INTENSITY]]` | Campfire on the ground there (SIZE = scorch radius, default 0.6) |
| `firemask 0/1` | The flame core into the depth buffer (default 1) |
| `lamp X GROUND_Y Z` | A post-top lamp standing there (4 units tall; its light sits 3.5 above the ground) |
| `lampcolor R G B [RADIUS]` | Every lamp's light (default 0.5 0.85 1.0, radius 5) |
| `move ID X Y Z [YAW]` | Runtime: entity ID (hex) moved there |
| `shift ID DX DY DZ` | Runtime: a trigger moved by an offset (its asset's position and box, and the entity's bound) |

Keys are the texture names the dev menu shows (`tex1_<w>x<h>_<hash>_<format>`). In h002's file each hide/unlit line
says what the texture is; keep doing that.

## Working on it

Read `docs/testing.md` first. Game runs follow `REPO.md`: short, muted, rumble off, never from your own `Play/`, and
the debug kit puts the player's setup back (`tools/debug/manual-session.ps1 stop`).

- **Build**: `tools/build-no100f.ps1` (add `-SkipTools -SkipTranslate` when only aurora or the runtime changed). It
  stages `reimagined/` into `Play/`.
- **Launch with a file**: set `NO100F_REIMAGINED` to the repo's `reimagined/h002.txt` and run
  `manual-session.ps1 start -Boot h002`. The log line `[devmenu] reimagined: <file> for h002` and
  `Reimagined: N lights, ...` confirm it. Pick the new log folder by creation time *after* the game has made it (the
  newest folder at launch is the previous run's).
- **Edit and reload without a rebuild**: change the light file, then the dev command `reimagined reload`.
- **Dev commands** (write one per line to `Play/devmenu.txt`; wait about a second between lines, two writes in a row
  can lose the first): `reimagined 0|1|reload`, `cam X Y Z YAW PITCH` (free camera; yaw 180 looks toward -z, pitch > 0
  looks down) and `freecam off`, `tp X Y Z [yaw]`, `scene <level>`, `ents`, `stageview N`, `bloom <percent>`,
  `bloomview N`, `textures [all]`, `texhl [KEY]`.
- **Stage views** (`stageview`): 1 tint, 2 depth, 3 AO alone, 4 ink alone, 5 the new light alone, 6 the moon's shadow
  alone, 0 off.
- **Bloom is 0% in test sessions**: `bloom 100` before judging the globes.
- **Screenshots**: `manual-session.ps1 shot -Label X`.
- **A/B diffs**: to prove an effect, shoot the same `cam` pose with it on and off (edit the file, `reimagined reload`)
  and diff the two images; mark the changed pixels in a copy. That's how the lamp shadows and the posts' moon shadows
  were checked.
- **h002 poses**: lamp 12 close `cam 35.5 3.0 8.2 180 -5`; lamps and fire mid view `cam 30 3 2 -150 -2`; fire
  `cam 24.7 1.6 -2.0 180 14`; above lamp 4 (shadows) `cam 11.8 10 15 180 55`.

### Finding a texture's key

1. `textures all` lists every key drawn this frame (to the dev menu log).
2. `texhl KEY` paints that texture magenta. Diff a shot against one without it to see where it is.
3. When nothing turns magenta, it may be a **darkening (multiply) draw**, like the pickups' blob shadows: highlighted
   it turns into dark *squares*, not magenta. Sweep every listed key (a shot per key) and rank the region's change
   against the median of all shots instead of counting magenta pixels.
4. Highlighting a texture and seeing a darkening stay on top of it means the darkening is a separate draw (or the new
   light), not that texture.

## Notes by feature

- **Relighting** only touches solid perspective draws that write depth ("lit" draws, `ShaderInfo.relight`). The normal
  comes from the depth buffer; the light fades out past `far`.
- **Sky and props**: drawn right after the game's frame clear (the last colour clear before the stage). The sky is
  also what turns `encode_props` on, so `sky_wanted` includes fire and lamps.
- **Campfire**: flame and embers are a full-screen pass on a camera-facing sheet (`flame_hit`, `flame_shape`). Its
  depth core exists because the game draws footprints late, after the stage: without it they showed through the flame;
  drawing the flame after the scene pass instead covered Scooby.
- **Lamps**: `push_lamp_metal` (plinth, fluted pole, ring, collar, cap, finial; lit by the stage) and the globe
  (`push_lamp_globe`, own WGSL: hot core facing the camera, mercury blue-green rim, 16 ribs; unlit). A lamp's light
  is added after the file is read (`lamp` lines, `lampcolor`), flagged to cast shadows.
- **Moon shadow map**: orthographic box round a point ahead of the camera, snapped to texels; depth reversed. Each lit
  draw's uniform copy gets the moon's matrix written at the stage (`write_shadow_uniforms`).
- **Lamp shadow maps**: up to 4 (`kMaxLampShadows`), for the shadowing lights nearest the camera (within the moon
  shadow radius). Each looks straight down from the light (perspective, `kLampShadowTanHalf` 2.75 each way, near 0.3),
  depth stored as near/distance, compared in distance. Each lit draw gets one more uniform copy per slot (memory is
  fine: the uniform buffer is 24 MB). The maps are MSAA like the game's pipelines, so they can't be a texture array:
  four bindings (5 to 8) and a `switch` in `lamp_depth`.
- **Snack spin**: `spin_angle` hashes the model's world x, z (from the voted view matrix) to an angle. It refuses
  models not upright in the level or drawn with another projection: the HUD's snack counter has its own camera and spun
  wildly before that check.
- **Entity moves**: the runtime moves the groundskeeper (`move`) and his talk trigger (`shift`) while Reimagined is
  on and puts them back when it is off.

## Gotchas

- WGSL reserves more words than you'd think: `from` broke a shader. Dawn aborts the game on a WGSL or bind group
  error; the message is in `console.log` (`WebGPU error`).
- The stage pipelines use auto layouts: bind only what that entry point uses (the flame doesn't use the moon map).
- Windows headers define `near` and `far` as macros: don't name C++ variables that.
- The scene stage replays the solid passes for every shadow map; a new draw type that should cast shadows needs its
  own shadow pass (see `encode_props_shadow`).
- Light file changes don't need a rebuild; shader and C++ changes do.
- Never commit game data: texture keys and coordinates are fine; pictures, models or disc files are not
  (`tools/check-assets.py`).

## Open and known limits

- Only h002 has a file. Other levels: copy the approach, not h002's values (its keys and positions are h002's).
- Lamp shadows: only the 4 nearest lamps, so a lamp further away can switch its shadows on or off as you walk; each
  map sees about 70 degrees each way from straight down.
- Not checked yet: Scooby's footprints behind the fire in play; snacks spinning in motion; a release package and a
  Linux build with the light files.
- Later, as discussed: the treeline; the moon placed relative to the gate.

## Adding a level

1. Boot it with a new empty `reimagined/<level>.txt` (or `NO100F_REIMAGINED`) and find its keys (`textures all`,
   `texhl`) and its light positions (the level's LITE assets; h002's came from `h002.HIP`).
2. Start from `ambient`, `moon`, `shadow`, `deblue`, `far`; mark the sky band and painted backdrops `unlit`, the painted
   shadow decals and blob shadows `hide`.
3. Check with the game as made: with Reimagined off the level must be unchanged.
4. Commit the file; `stage-assets.py` ships it. Update this file's list of levels and `docs/playing.md`.
