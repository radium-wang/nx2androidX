# Build an nx2androidX game pack on Mac for Retroid Pocket Duo

This workflow uses the macOS porter from `v0.3.0-alpha12` to build a data pack for the installed nx2androidX app (`com.nx.android`, player API revision 5). The working example is Super Mario Bros. Wonder, title ID `010015100B514000`, update `v327680` (1.2.1). It uses the existing game profile and its default graphics and frame pacing. The app discovers the completed pack on the removable SD card and copies its executable libraries into private storage at launch.

## 1. Prepare the Mac checkout

Use an alpha12 checkout with the local changes below applied. These changes are prerequisites of the SD-card workflow described here.

| Source | Required behavior |
| --- | --- |
| `nxnative/dm/gfx/tools/shader_split.py` | `_run_pool_item` is a module-level process-pool worker. It runs the supplied function and flushes the worker's spill file. The nested worker in the release snapshot cannot be pickled by the macOS build. |
| `nxnative/dm/host/hle_fs.c` and `nxnative/dm/host/romfs_image.h` | Validate and mount a read-only `DMROMFS1` image. On exFAT, keep a recovery directory while publishing a committed save and recover it on the next mount after an interrupted commit. |
| `nxnative/dm/android/gfx_vk_exec.c` and `nxnative/dm/android/tools/prewarm_pairs.py` | Warm recorded graphics states, compute shaders and reflected shader pairs before the first draw. Log totals, missing shaders and failures. |
| `nxnative/dm/android/haptics_android.c` | Use the latest vibration level and immediate stop for player one when `DM_RUMBLE_MODE=droidplay`. |

The Mac build used Python 3.13 with the project's dependencies, .NET SDK 10, Android NDK `28.2.13676358`, and ADB. Use a separate writable `NX2A_HOME` for build state. If using prebuilt C# tools, point `NX2A_NX_EXTRACT` at the directory containing `nx_extract` and `NX2A_DMSH_XLATE` at a separate input directory containing a matching `dmsh_xlate.dll`. The porter copies the translator into `NX2A_HOME`.

Keep the original dumps and `prod.keys` outside the checkout. Hard-link the base dump and the 1.2.1 update into `.gamedump/Wonder`. The profile uses both the base and update for its shader tables. Supply only the path to readable keys and use the player's own dumps and system font.

```sh
export NX2A_HOME="/path/to/local/build-state"
export NX2A_NDK="/path/to/android-sdk/ndk/28.2.13676358"
export NX2A_PACK_DIR="$PWD/.gamedump/Wonder/ports"
keys="/path/to/prod.keys"
driver="/path/to/turnip-duo-5.1/libvulkan_freedreno.so"
font="/path/to/CJK-system-font.ttf"

test -r "$keys" &&
python -m porter port "$PWD/.gamedump/Wonder" --app --keys "$keys" \
  --version v327680 --language zh-Hans --vulkan-driver "$driver" \
  --shared-font "$font" --jobs 8 --progress lines
```

Check the reported output path and `nx2a-data.json`. For this example, the manifest must identify title `010015100B514000`, version `v327680`, display version `1.2.1`, API revision 5, and host `host/libdmplay.so`. Preserve the matching hook libraries in `drivers/`. Record SHA-256 hashes of the host, driver, and `shaders.dmsv`. Turnip Duo 5.1 has multiple builds with the same Vulkan version string, so use the library hash to identify the intended build.

## 2. Prepare the external SD card pack

Use a fresh pack folder under `/storage/<volume-id>/Android/data/com.nx.android/files/ports/`, such as `Wonder-v327680-data`. Keep the game image in its own game-specific directory on the same card. An existing `DMROMFS1` image can be reused after its index and every file match the porter's merged 1.2.1 RomFS and its device hash matches the verified local copy. When producing a new image, pack the merged update RomFS with a compatible DMROMFS1 writer and verify it the same way.

Change the manifest's `romfs_at` to the **directory containing the image**. Alpha12 checks that this path is a directory before enabling Run. Set `DM_ROMFS` to the **image file** so the host opens that file read-only. Preserve the profile's other environment rows and replace any Mac paths with their SD-card equivalents, including all font paths. The following example uses symbolic paths. Write the resolved Android paths in the actual `env.txt`.

```dotenv
DM_ROMFS=/storage/<volume-id>/GameData/Switch/Wonder/Wonder.romfs
DM_SAVE=/storage/<volume-id>/Android/data/com.nx.android/files/ports/Wonder-v327680-data/save-test
DM_TEMP=/storage/<volume-id>/Android/data/com.nx.android/files/ports/Wonder-v327680-data/guest-temp
DM_VK_DRIVER=libvulkan_freedreno.so
DM_VK_PREWARM_BEFORE_DRAW=1
DM_VK_WARM_SHADER_PAIRS=1
DM_RUMBLE_MODE=droidplay
MESA_SHADER_CACHE_DISABLE=true
```

The driver basename must match the bundled library the app copies beside the host. An existing driver in nx2androidX's driver manager can also be selected by its verified absolute path. The runtime log must show that adrenotools loaded the selected driver. Keep the RP Duo's existing GPU policy and performance mode and the Wonder profile's graphics and frame-rate settings.

Before deploying, inspect the foreground, current pack folders, SD free space and available committed save backups. Stage the completed pack in a new sibling directory, verify the uploaded files against the local hashes, and rename it into the library folder on the same volume. Refresh the nx2androidX library and confirm that it recognizes the title and version. Keep existing packs and save trees separate from this staging operation.

## 3. Warm the Vulkan pipelines

Generate the pair manifest from the newly built shader bundle and the game's own source pairs.

```sh
pack="$NX2A_PACK_DIR/SMBWMobile-data"
python nxnative/dm/android/tools/prewarm_pairs.py \
  "$pack/shaders.dmsv" \
  nxnative/dm/games/010015100B514000/shader_pairs.tsv \
  "$pack/vk_shader_pairs.bin"
```

The Mac build translated 16,268 shaders, including 71 compute shaders, and validated 14,603 source pairs. Pair reflection describes representative pipelines. Exact graphics states must come from actual replay by the matching alpha12 host. Keep `vk_pipelines.bin` and `vk_pipeline_cache.bin` in the SD-card pack. The standalone build's `.keys4` records use a different format from alpha12's PPE2 `vk_pipelines.bin` and serve as evidence of previously visited scenes.

Start with an empty Vulkan cache when the title build, shader bundle, host pipeline format or driver changes. If a matching alpha12 graphics recording is unavailable, use the test save and record startup, title and map states first, then exit cleanly and restart before selecting a course. Record any newly encountered course states for the next launch. Before the course, check the log for the graphics, compute and pair totals and their failure counts. After restart, confirm the same driver loaded and the runtime reports a nonzero number of loaded cache bytes.

## 4. Verify saves, rendering and vibration

1. Use a copy of an exported committed save for this title and version. Preserve its file names and metadata. With an active standalone session, wait until the player releases the upper screen before launching, pressing controls or restarting either app. If the committed save is private and an export is unavailable, a fresh test profile can exercise the exFAT save path. Leave the standalone migration pending until its committed save is available.
2. Open the pack from the nx2androidX library on logical display `0`. Check that the image is mounted read-only, the test save is in the SD-card directory, and a playable course renders. Record the scene screenshot and the relevant log excerpts.
3. Trigger an actual game vibration and check the player-one route and `first Droidplay rumble` line. The player confirms the physical feel.
4. Trigger an in-game save, verify that the committed test tree changed, then restart from the library. Check that the saved progress returns and that the 5.1 pipeline cache loads. Verify the original backup hash before using a separately copied production save as `DM_SAVE`.

Keep logs, images, extracted data, caches, saves and keys in local build storage. The Mac build totals above describe artifact validation. Record device results from the exact pack and save copy that were exercised.
