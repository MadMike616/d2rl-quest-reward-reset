# Hook Inventory — Quest Reward Reset

## Plugin metadata

| Field | Value |
|---|---|
| Plugin name | Quest Reward Reset |
| Plugin ID | madmike-quest-reward-reset |
| Release version | 1.0.24 |
| Inventory updated | 2026-11-08 |
| Source revision | v1.0.24 release tag |
| D2R build(s) audited | Not validated |
| D2RLoader SDK / Plugin ABI | PluginSDK 0.4.0 / Plugin ABI 4 |
| Audit status | Complete |

## Scope and method

Inspected all files under `src/`, `CMakeLists.txt`, and `README.md` for plugin-owned hooks or patches that intercept or modify native or operating-system targets. The only direct hook site is the thread-scoped Win32 `WH_GETMESSAGE` hook in `src/plugin.cpp`. Quest-state and synchronization RVAs are validated helper calls and are not installed as hook targets. No hook-site changes were made from v1.0.23; this release changes version metadata and the build configuration only.

Numeric native addresses are reported as RVAs. This Win32 hook has no native address, so its identifier is N/A.

## Hook sites

| Site ID | Type | Target | Address / identifier | Operation | Guard / validation | Source reference | Notes |
|---|---|---|---|---|---|---|---|
| H-01 | Win32 input-message | Active D2R game-window message queue, `WH_GETMESSAGE` | N/A | `SetWindowsHookExW` installs `GameMessageHookCallback` on the game-window thread | Resolves an active window belonging to the current process, obtains its thread ID, and pins the callback module | `src/plugin.cpp` — `TryInstallGameMessageHook` | Retries when a token tooltip needs right-click handling; forwards via `CallNextHookEx` and removes the hook during plugin shutdown. |

## Known collision overlaps

| Other plugin | Shared target | Relationship | Evidence / notes |
|---|---|---|---|
| Shared Stash Page Text | `WH_GETMESSAGE` on the D2R host thread | Possible shared thread message-hook chain | Both source inventories register `WH_GETMESSAGE` hooks; active window and thread lifetime determine whether they coexist. |

## Audit notes

This is a source-level hook-site audit. No specific D2R build was audited or runtime-validated for this inventory. Quest helper RVAs and their expected-byte witnesses are build-specific and are guarded in source, but those helper calls are outside the hook-site inventory.
