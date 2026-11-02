# Quest Reward Reset Tokens

A D2RLoader plugin that adds three right-click tokens for reusing quest rewards.

| Item code | Reward reset |
| --- | --- |
| `rt1` | Akara's free character respec |
| `rt2` | Charsi's imbue |
| `rt3` | Larzuk's socket reward |

Use a token from your inventory or Horadric Cube. It resets the reward for the character's current difficulty and is consumed only when the reset succeeds. Each token displays a matching right-click tooltip. A success sound plays when the item is consumed.

## Compatibility

- Windows x64
- D2RLoader with Plugin ABI 4 support
- Built with the official D2RLoader PluginSDK 0.4.0 (ABI 4)
- Tested with D2RLoader 1.3.1-beta

## Install

1. Download `d2rl-quest-reward-reset.dll` from the [Releases](https://github.com/MadMike616/d2rl-quest-reward-reset/releases) page.
2. Copy it into `mods/<ModName>/d2rloader/plugins/`, alongside the active mod's `<ModName>.mpq` folder.
3. Start D2RLoader with that mod. On the first run, the plugin adds the token data and artwork to the mod. Restart D2RLoader and the game once afterward so they load the new data.

The installer uses `d2rloader/config/quest-reward-reset.toml` as its completion marker. If the marker already exists, it skips all mod-file edits and asset copies.

## First-run installation details

When the marker is absent, the plugin creates a timestamped backup under `d2rloader/backups/quest-reward-reset/`, then:

- Adds the three token rows to `data/global/excel/misc.txt`.
- Appends the token names to `data/local/lng/strings/item-names.json`, assigning unused IDs.
- Adds HD item-to-sprite mappings to `data/hd/items/items.json`.
- Installs each token's PNG, 98×98 sprite, and 49×49 low-end sprite under `data/hd/global/ui/items/misc/quest/`.
- Creates the TOML marker after all required changes succeed.

The embedded token data includes its source `misc.txt` header. During installation, the plugin maps token values to the active `misc.txt` columns by exact name, so reordering columns does not shift values. Columns present only in the active file are left empty. Source columns absent from the active file are skipped; the installer requires a `code` column but does not validate missing nonempty fields against the combined weapons, armor, and misc schema. A mod that moves a populated token field to another table could therefore leave the token rows incomplete. Empty or duplicate column names are rejected.

Tooltips are supplied by the plugin at runtime through the SDK item-tooltip event. The installer does not add custom strings to `item-modifiers.json`.

To run first-time installation again, remove `quest-reward-reset.toml` from the mod's `d2rloader/config/` folder before launching D2RLoader. The installer makes a fresh backup and reapplies the embedded defaults, including the original token artwork.

## Build

Build with CMake 3.29 or newer and an x64 MSVC toolchain. Download the official [D2RLoader PluginSDK](https://github.com/D2RLoader/PluginSDK) 0.4.0 separately, then set `D2RPLUGIN_SDK_ROOT` to its `SDK-0.4.0` directory:

```powershell
cmake -S . -B build -A x64 -DD2RPLUGIN_SDK_ROOT="C:/path/to/PluginSDK/SDK-0.4.0"
cmake --build build --config Release
```

The build produces `d2rl-quest-reward-reset.dll`. Installer assets are embedded in the DLL; no extra files are needed beside it.

## License

MIT. See [LICENSE](LICENSE).
