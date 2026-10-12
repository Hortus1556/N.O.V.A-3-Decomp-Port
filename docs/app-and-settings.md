# Application and settings

`nova3.exe` (`host/main.cpp`) is the Windows program around the translated game: it opens the
window, shows the launcher, replays the Android lifecycle and runs the frame loop.

## Window and renderer

- **SDL3** (3.4.18, `extern/sdl`, `SDL3.dll` copied next to the executable) provides the window,
  the GLES 2 context, input, gamepads and audio.
- On Windows the GLES context comes from **ANGLE** (GLES on Direct3D 11): `libEGL.dll` /
  `libGLESv2.dll` next to `nova3.exe`, or else the copies inside the installed Microsoft Edge.
  SDL is pointed at them with `SDL_HINT_VIDEO_FORCE_EGL`, the EGL library hint and a DLL search
  directory. Elsewhere SDL uses the system's EGL / GLES.
- The game renders at the window's pixel size; the menus scale correctly.
- The frame loop runs on its own thread (the game thread), with the window and events on the
  main thread.

## Starting the game

- **Packaged folder** (`python tools\package.py`, default `dist\`): `nova3.exe`, `SDL3.dll`,
  ANGLE (`libEGL.dll`, `libGLESv2.dll`, `d3dcompiler_47.dll`, taken from Microsoft Edge), and
  `nova3_data\` with the game files in one folder: `libGloftNAHM.so` and the OBBs' contents,
  extracted (56 files: the `.gla` archives, `GameOptions.json`, ...; 1.6 GB). When an archive
  is not in an OBB, the game opens it as a loose file in the OBB folder
  (`CCustomFileSystem::getFileInOBB` falls back to the plain file system), and only notes
  "could not find main.obb pack!" at start. With `--obb`, the two OBB files go there
  instead. The library and the OBB files are hard links to the originals when on the same
  drive (no extra space). The APK is not needed: its only reader was an analytics event
  (`Application::Tracking_AddEventZoro`, values read from the APK through libzip), now a stub. Each build copies the new `nova3.exe` into `dist\`
  when it exists. Like the build itself, the folder is made from the player's own game files
  and is for their own use.
- Lookup: a `nova3_data` folder next to the executable is used first; otherwise the
  development tree (`game\libGloftNAHM.so`, the OBB folder named after the package, the APK)
  is searched in the executable's folder and its parents. The working directory
  becomes that folder, so double-clicking works from anywhere.
- **Icon**: the game's own (the largest `res/drawable*/icon.png` in the APK), made into a
  multi-size `.ico` at build time (`tools/make_icon.py`) and linked in as resource 1; the
  launcher and game windows use it too (`SDL_HINT_WINDOWS_INTRESOURCE_ICON`).
- `nova3.exe` is a GUI program (no console window). Started from Explorer or a shortcut it
  logs to `nova3.log` next to `nova3.exe`, or to a console window with
  `[launcher] console = true`. A shell that redirects stderr keeps the log there.
- Fatal errors are also shown in a message box (not in scripted test runs).

## Launcher

Before the game starts, a launcher window (`host/launcher.cpp`, Dear ImGui 1.92.9b on SDL's
renderer) shows:

| Section | Settings |
|---|---|
| Display | Monitor, mode (windowed / borderless fullscreen / exclusive fullscreen), resolution and refresh rate (the monitor's real modes), VSync (off / on / adaptive), frame limit |
| Graphics | Texture filtering (anisotropy), HD textures (when `hd_textures.pak` is there), field of view (slider, with Default) |
| Audio | Master volume |
| Controls | Sensitivity, aim sensitivity, invert Y, gamepad on/off, stick dead zone, stick look speed, aim assist, auto-leveling and rumble with its strength (controller only), toggle sprint, "Edit in nova3.ini..." for key bindings |
| Multiplayer | Nickname (your name in local Wi-Fi matches) |
| Discord | Rich Presence on/off, application ID |
| Other | "Open folder..." (the save and settings folder), log console |

- **Play** (or Enter) saves everything and starts the game; **Quit** (or Esc) exits. The
  hotkeys are disabled while typing in a field.
- Key bindings are changed in the game (Options → Controls → Keyboard & Mouse / Controller,
  see [Input](input.md#the-controls-page)) or in `nova3.ini`. Play re-reads them from the file
  first, so edits made while the launcher is open are kept.
- The window is capped at 90% of the usable screen height and scrolls; Play / Quit stay in
  the footer.
- **`nova3.exe -silent`** skips the launcher and boots straight into the game with the saved
  settings. Scripted test runs skip it too and are always windowed at `--size` (default
  1280x720).

## The Graphics page

The game's own Options (pause menu and title menu) have a **Graphics** page, where Advanced
was (`host/controls_menu.cpp`; the Switch has no launcher, so this is where its video
settings live). It is built like the Controls page (see [Input](input.md#the-controls-page)):
the in-game menu's page 9 with a panel of its own, opened by the Options page's Advanced
button (renamed; `PushPage(8)` becomes page 9) and by the title menu's Advanced screen
(150516, through `Menu_GoToMenu`). Each row is a wide button showing "Name: value"; a click
(mouse, touch, Enter, pad A) takes the next value, left / right (arrows, D-pad) the previous
or next; up / down, the mouse wheel and Back work as on the Controls page.

| Row | Values (PC) | Switch |
|---|---|---|
| Display Mode | Windowed, Borderless, Fullscreen (exclusive) | (automatic: docked / handheld) |
| Resolution | windowed: 1280x720 … 3840x2160 and the monitor's modes that fit; fullscreen: the monitor's modes; borderless: the desktop's (fixed) | — |
| VSync | Off, On, Adaptive | On, Adaptive |
| Frame Cap | Unlimited, 30, 60, 120, 144, 165, 240 fps | 30, 60 fps |
| Field of View | Default, 60 … 110 (steps of 5) | same |
| Anisotropic Filtering | Off, 2x, 4x, 8x, 16x | same |
| HD Textures (after restart) | Off, On | — |

A value set in `nova3.ini` that is not in the list is shown as it is, and left / right step
to its neighbours. Every change applies at once through `display::apply` (`host/display.h`,
implemented by each platform's main) and is saved to `nova3.ini` (`display::save`). On the PC
(`host/main.cpp`, called on the game thread):

- VSync: `SDL_GL_SetSwapInterval` on the game thread, which holds the GL context (adaptive
  falls back to on); the frame cap: the frame limiter reads it every frame.
- Field of view: `hooks::g_fov_scale`, at once (the `getFOV` hook also undoes a scaled value
  after a change back to Default).
- Anisotropic filtering: `rt::gl_set_anisotropy`; textures get it when the game next sets their
  filter, so in practice from the next level load.
- Display mode and resolution: the window is changed on the main thread
  (`SDL_RunOnMainThread`: `SDL_SetWindowFullscreen`, `SDL_SetWindowFullscreenMode` with the
  closest exclusive mode or none for borderless, `SDL_SetWindowSize`, centred); its new pixel
  size (also from `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`) reaches the game at the start of the
  next frame as on Android, `GL2JNILib.resize` (`glf::AndroidResizeScreen`), which glitch's
  device picks up (`CGlfDevice::runImpl` polls the window size: video driver `onResize`, a
  `CDeviceResizeEvent`). Screenshots and mouse coordinates follow the new size. The menu that
  is open keeps the layout it was made with until it is opened again.
- HD textures: saved, used at the next start.

Scripted test runs start windowed at `--size` whatever the file says; the page still shows the
file's values, and only a change made on it acts on the window.

## The player's folder

Saves and settings live in `Saved Games\N.O.V.A. 3` (`SDL_GetUserFolder(SDL_FOLDER_SAVEDGAMES)`;
on other systems the SDL preferences folder, e.g. `~/.local/share/nova3`). `--user DIR`
chooses another folder, `--config FILE` another settings file.

- `nova3.ini`: the launcher settings and key bindings.
- `save\`: the game's private data (the guest's `/data/data/<package>` except `cache/`):
  `files\a1.dat` is the campaign save, `files\a9.dat` the checkpoint, the other `a*.dat` are
  profile, upgrades and statistics; `shared_prefs\` holds the Java-side preferences and
  `data_sharing.txt`, the game's account ID that every save is tied to (see
  [Runtime](runtime.md#fake-java)).
- **Migration**: earlier builds kept `nova3.ini` and the saves in the game folder. On the first
  start, while the player's folder has none, `nova3.ini` and
  `userdata\root\data\data\<package>\{files, shared_prefs}` are copied over. The old files stay
  where they were. (Bug fixed on the way: the copy failed silently at first because the
  `save\` folder did not exist yet; it is now created first and copy errors are logged.)
- **Non-ASCII paths**: the executable's manifest (`host/nova3.manifest`) sets the process code
  page to UTF-8, so the narrow-path file functions (CRT, `fopen`, `std::filesystem` from UTF-8
  strings) accept a Saved Games folder under a user name with accented letters. Tested with a
  folder named `tëst ü`.

## `nova3.ini`

Written with defaults when missing, and again when the game's Controls page changes a setting
(half a second after the last change) or the Graphics page does (at once). Unknown settings are reported in the log (and not written back); out-of-range values are clamped (`fov` 50–120, `anisotropy` 1–16, at least 320x240).

| Section | Keys |
|---|---|
| `[video]` | `mode` (`windowed`, `borderless`, `fullscreen`), `display`, `width` / `height` (window size or exclusive resolution; borderless uses the desktop's), `refresh` (0 = highest), `vsync` (`off`, `on`, `adaptive`; adaptive falls back to on without driver support), `max_fps` (0 = unlimited), `anisotropy` (1–16), `fov` (horizontal degrees at 16:9, 0 = the game's own ~58), `hd_textures` (the [HD texture pack](hd-textures.md), when present) |
| `[audio]` | `volume` (master, percent) |
| `[controls]` | `sensitivity` (look speed factor: 0.04 degrees per mouse count, `look_speed` on the stick), `aim_sensitivity` (the same while aiming down the sights; missing = `sensitivity`), `invert_y` (mouse and stick), `aim_assist` and `auto_leveling` (applied with a controller only), `toggle_sprint` (any device: a press of sprint runs, the next walks). Also set on the game's Controls page. Older files' `[mouse]` keys are read too. |
| `[gamepad]` | `enabled`, `deadzone`, `look_speed` (degrees/s at full right stick at sensitivity 1, squared response) |
| `[multiplayer]` | `nickname` (up to 16 characters; empty = the game's own) |
| `[discord]` | `enabled`, `app_id` (digits; empty = no Rich Presence) |
| `[launcher]` | `console` |
| `[keys]` | one comma-separated list per action: keys, `Mouse1`–`Mouse5`, `Pad*` buttons (see [Input](input.md)) |

## Discord Rich Presence

`host/discord.cpp` shows what the player is doing on their Discord profile, through the
Discord client's local IPC (`\\.\pipe\discord-ipc-0` .. `9` on Windows, a Unix socket
`discord-ipc-N` in `$XDG_RUNTIME_DIR` / `$TMPDIR` / `/tmp` elsewhere): frames of opcode, length
and JSON; a handshake with the application ID, then `SET_ACTIVITY` commands. No Discord
library is involved. A background thread connects (again every 10 s while Discord is closed,
every 60 s after a refused ID) and sends the activity when it changes; the game thread reads
the state once a second:

| When | Shown |
|---|---|
| title menu, loading | In the menus |
| a campaign level | Campaign / Chapter N: title (`CLevelsManager` +0x18, `GetLevelTitleStr`) |
| a local match | Local multiplayer: mode / map (`GameMpManager` +0x3c00, the game's mode names; `CLevelsManager` +0x28, `GetMPLevelTitleStr`) |
| between matches | Local multiplayer / In the lobby |

Titles and mode names are the game's strings in its current language, read from the menus'
string table (MAIN) even inside a level. The elapsed time restarts with each new activity
(not with the next chapter or map). Discord shows the application's name ("Playing ...") and
its icon, so the player creates one at discord.com/developers (Applications → New
Application, named e.g. N.O.V.A. 3, with an icon) and enters its Application ID in the
launcher (`[discord] app_id`). Discord's own list of detectable games has no N.O.V.A. 3.

Test runs only log the activity (`discord: Campaign / Chapter 1: HOMECOMING`) and do not
connect; `NOVA3_DISCORD_TEST=1` connects anyway. Checked: the states above in test runs, and
the handshake with Discord running (an unknown ID is refused with code 4000).

## Display

- Tested: borderless at 3840x2160 (the game renders natively at that size), exclusive
  1920x1080 at 240 Hz, the menus at 2560x1440.
- Gameplay holds 236–240 fps with VSync on a 240 Hz display (1280x720 test runs and a
  3840x2160 borderless session). Uncapped (VSync off, no limit) the menus run at about
  1000 fps. The game's clock works in whole milliseconds, which caused real bugs at these
  rates (see [Gameplay fixes](gameplay-fixes.md)).
- **No anti-aliasing option**: the game draws its 3D scene into its own framebuffer for
  post-effects, so multisampling the window would not reach it.
- **Anisotropic filtering** is applied by the runtime whenever the game selects a mipmapped
  filter.

## Field of view

The game keeps a fixed horizontal angle (`CLevel::SetTargetFov`: fovy = target × H / W;
gameplay fovy 0.6024 rad at 16:9). The `[video] fov` setting converts horizontal degrees at
16:9 to a scale on that value: `v = 2·atan(tan(h/2)·9/16)`, `scale = v / 0.60238`.

The game animates the FOV incrementally: every frame `CLevel::Render` calls
`camera->setFOV(camera->getFOV() + step)`, the step (`CLevel+0x2f0`) easing towards the target
(wider while running, narrower while aiming). So both are hooked:

- `CCameraSceneNode::setFOV` remembers the game's value and gives the camera the scaled one
  (at most 2.6 rad);
- `CCameraSceneNode::getFOV` returns the game's value again when the camera still holds the
  scaled one.

The game only ever sees its own FOV, and the run and aim transitions scale with the setting.
(A first version scaled only in `setFOV`: the scaled value was read back and added to, so the
FOV ran away to 1.49 rad instead of 0.69 and jumped when a run started or ended.)

- Only the scene manager's active camera (`g_sceneManager+0xdc`) inside a level is scaled, so
  menus are untouched.
- `CNovaSceneManager::drawWeapons` draws the gun with the same camera at `getFOV()` + an offset
  and then restores `getFOV()`. The gun pass (recognised by the caller and a value different
  from the game's) is passed through unscaled, so the weapon model keeps its shape (at 90° it
  stretched before this).

Tested at 90°: the game's value reads 0.687 rad walking and eases to 0.834 rad running and
back, as with the default FOV.
