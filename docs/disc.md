# Which disc version do I have?

FrightWorks needs the **GameCube USA disc, version 1.01**: Game ID **GIHE78**, revision **1** (Redump: "Scooby-Doo!
Night of 100 Frights (USA) (Rev 1)").

- **The launcher checks it.** When you choose a disc image, it tells you if the file isn't a GameCube image, is another
  game or region (another Game ID, such as the European `GIHP78`), or is version 1.00 (revision 0).
- **Check it yourself:** in [Dolphin](https://dolphin-emu.org), right-click the game > **Properties** > **Info** shows
  the **Game ID** (`GIHE78`) and **Revision** (`1`).
- **Formats:** plain `.iso` or `.gcm`, or `.ciso`. Not `.rvz`, `.gcz`, `.wia`, NKit images (`.nkit.iso`,
  `.nkit.gcz`) or Wii images. Dolphin turns an `.rvz` into a plain image: right-click the game > **Convert File...** >
  **Format: ISO**. NKit images need rebuilding with the NKit tool that made them.
- **Checksum:** a plain `.iso` of the right disc is 1,459,978,240 bytes, SHA-1
  `cf5638fd61a7862e07066fb290381bdfc77d1f37` (MD5 `6dca7f5ed6b400c8216526107e83f3cd`, CRC-32 `b29a2a47`), the same as
  Redump's. A `.ciso` hashes differently but works.
- **Other versions:** USA 1.00 and the European GameCube release aren't supported yet but might be later. The PS2 and
  Xbox versions can't be played, but your own PS2 or Xbox disc image can add their cutscenes, textures and sound to this
  one: see [PS2 and Xbox extras](ps2-xbox-extras.md).
