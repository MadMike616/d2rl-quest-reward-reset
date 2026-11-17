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

These instructions describe v1.1.0. If you download an older DLL, use the README for its release tag.

You need an unpacked, writable mod. In the standard layout, `mods/<ModName>/<ModName>.mpq` is a directory that contains the mod's loose `data` files.

1. Check that each required file below is either in the mod's loose `data/` directory or in `mods/<ModName>/d2rloader/plugins/basefiles-and-backups/basefiles/`. Reuse any originals already in `basefiles/`. Use Ladik's Casc Viewer to extract missing originals from the installed game.
2. With the game closed, download `d2rl-quest-reward-reset.dll` from [Releases](https://github.com/MadMike616/d2rl-quest-reward-reset/releases) and copy it to `mods/<ModName>/d2rloader/plugins/`.
3. Start D2RLoader with that mod selected. After the log reports `first-run install complete`, exit and restart D2RLoader with the same mod. The token features activate on the next launch so the game can load the new data first.

| Directory under `data/` or `basefiles/` | Required files |
| --- | --- |
| `global/excel/` | `misc.txt` |
| `local/lng/strings/` | `item-names.json` |
| `hd/items/` | `items.json` |

The installer prefers the mod's loose copy of each file. When that copy is missing, it reads the original from `basefiles/` and writes the merged file to the mod's `data/` directory. It leaves `basefiles/` untouched. If a file is missing from both locations, setup stops before editing the mod and lists both paths in `mods/<ModName>/d2rloader/logs/madmike-quest-reward-reset.log`. Supply the missing files and launch again.

To upgrade an existing installation, close the game and replace the DLL. Keep `d2rloader/config/quest-reward-reset.toml`: the installer uses it as its completion marker and skips mod edits and asset copies when it already exists.

First-run data edits are serialized with other D2RLoader plugin installers that use the shared mod-install lock, preventing simultaneous backup and file-write operations.

## First-run installation details

When the marker is absent, the plugin prepares the merge and verifies a timestamped backup of each existing file it will change under `d2rloader/plugins/basefiles-and-backups/backups/quest-reward-reset/`. The backup's `created-files.txt`, if present, lists loose files the installer added, including tables and artwork. The installer then:

- Adds the three token rows to `data/global/excel/misc.txt`.
- Appends missing token names to `data/local/lng/strings/item-names.json`, assigning IDs that are unused across the mod's loose localization JSON files. Existing token names keep their IDs.
- Adds HD item-to-sprite mappings to `data/hd/items/items.json`.
- Installs each token's PNG, 98×98 sprite, and 49×49 low-end sprite under `data/hd/global/ui/items/misc/quest/`.
- Creates the TOML marker after all required changes succeed.

If a write fails, the installer restores the files it already changed and removes any files it just created. It leaves the completion marker absent so setup can retry. If a file cannot be restored, the log points to the backup for manual recovery.

The embedded token data includes its source `misc.txt` header. During installation, the plugin maps token values to the active `misc.txt` columns by exact name, so reordering columns does not shift values. Columns present only in the active file are left empty. Source columns absent from the active file are skipped; the installer requires a `code` column but does not validate missing nonempty fields against the combined weapons, armor, and misc schema. A mod that moves a populated token field to another table could therefore leave the token rows incomplete. Empty or duplicate column names are rejected.

Tooltips are supplied by the plugin at runtime through the SDK item-tooltip event. The installer does not add custom strings to `item-modifiers.json`.

To run setup again, remove `quest-reward-reset.toml` from the mod's `d2rloader/config/` folder before launching D2RLoader. The installer backs up files it will change and restores the embedded artwork if the installed files differ.

To remove the token data, first remove the tokens from characters and stashes that will continue using the mod. With the game closed, remove the DLL and restore the affected files from the original install backup, keeping any later mod edits you want to preserve. Use `created-files.txt`, if present, to identify files to remove. Remove the completion marker if you want a future reinstall to run setup again.

## Build

Build with CMake 3.29 or newer and an x64 MSVC toolchain. Download the official [D2RLoader PluginSDK](https://github.com/D2RLoader/PluginSDK) 0.4.0 separately, then set `D2RPLUGIN_SDK_ROOT` to its `SDK-0.4.0` directory:

```powershell
cmake -S . -B ../build/quest-reward-reset -A x64 -DD2RPLUGIN_SDK_ROOT="C:/path/to/PluginSDK/SDK-0.4.0"
cmake --build ../build/quest-reward-reset --config Release
```

The build produces `d2rl-quest-reward-reset.dll`. The DLL attached to GitHub releases is built with CMake's `Release` configuration. Installer assets are embedded in the DLL; no extra files are needed beside it.

## License

MIT. See [LICENSE](LICENSE).
