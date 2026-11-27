# PS2 and Xbox extras

The PS2 and Xbox versions of the game have a few things the GameCube disc lacks or stores at lower quality. If you own
one of them, FrightWorks can take those from your own disc image. On the launcher's **Extras** page, press **Choose PS2
or Xbox disc image** and pick a plain `.iso` dump of the disc (Xbox: a trimmed "xiso" or a full dump). **Remove** takes
it all out again.

| | PS2 disc | Xbox disc |
| --- | --- | --- |
| Live story cutscenes instead of the GameCube's movies | 13 | 14, with the ending |
| Textures without the GameCube's compression | | about 1,860 (22 MB) |
| Music, voices and effects without the GameCube's hiss | | about 1,000 sounds (470 MB) |

## Cutscenes

The GameCube plays a movie for each story scene. Its levels still hold everything around the scenes (the cutscene
managers, the table naming each one, the soundtracks) but not the scenes themselves. The PS2 and Xbox levels have them,
the same files on both; only the Xbox disc has the ending (`cin_11`). The game renders them live at your resolution
instead of the movies' 640x448, and falls back to the movie for any it doesn't have.

## Textures (Xbox)

The GameCube compresses most textures (blocky CMPR, 1- or 3-bit transparency). The Xbox kept the same textures (a few
at twice the size) without that, with full 8-bit transparency. They are a texture pack, **Xbox textures** in
**Extras > Textures**: it starts at the bottom of the list, so the other packs win where they have a texture and the Xbox
ones fill the rest, and you can move it or switch it off there. With HD textures off they are the only replacements.
Textures that are smaller or shaped differently on the Xbox are left out.

## Sound (Xbox)

The GameCube stores all sound as 4-bit ADPCM, which adds a faint, even hiss, most noticeable on the voice clips. The
Xbox has the same sounds as plain 16-bit audio. The game still loads and times its own sound; each sample it plays comes
from the Xbox copy instead. 32 sounds that are a different recording on the Xbox stay the GameCube's.

## Your disc data stays yours

Everything copied comes from your own disc and stays on your PC (`UserData\ps2\cutscenes`, `UserData\texture_packs\<NN>-xbox`,
`UserData\xbox\audio`). FrightWorks never ships or shares it, and the PS2 and Xbox versions themselves can't be played.
