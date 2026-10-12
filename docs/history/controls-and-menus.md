# Controls and menus

References: [Input](../input.md), [Application and settings](../app-and-settings.md),
[Nintendo Switch](../switch.md).

## Added

- **Gyro aiming** (Switch): the console in handheld mode, a Pro Controller or the right
  Joy-Con. Yaw is measured around gravity, so it works held upright or flat.
  - Settings: OFF / AIM (while aiming down the sights) / ALWAYS, and a sensitivity.
  - Added later: INVERT X-AXIS and INVERT Y-AXIS of its own, separate from the stick's
    Invert Y-axis.
- **Rumble** (all platforms): the game had none.
  - It fires on the player's shots, on hits taken, and on camera and HUD shakes.
  - Switch: HD rumble. PC: SDL gamepad rumble.
  - Settings: an on/off switch and a strength slider.
- **Toggle run** for every device: a press runs, the next press (or stopping, or aiming) walks.
- **Swap A/B, X/Y** (Switch):
  - Off (default, Nintendo's layout): A confirms and jumps.
  - On (the Xbox layout): the bottom button, B, confirms and jumps.
- **Switch button names** in prompts and on the binding page: A / B / X / Y as swapped, L, R,
  ZL, ZR, +, -, Left / Right Stick. The game's own menu icons, such as Back's "(B)", stay.
- **Graphics page** in the game's Options (PC and Switch), in place of Advanced:
  - display mode, resolution, VSync, frame cap, field of view, anisotropic filtering, HD
    textures;
  - it applies at once and saves to `nova3.ini`;
  - the Switch has no launcher, so this is where its video settings live.
- **Player IP** (Switch): match setup > Advanced has a fifth row.
  - It is the game's online-only Private Match switch, relabelled. EDIT opens the system
    keyboard, and the addresses apply at once (`[multiplayer] peers`).
  - Before this the Switch never applied that setting at all. On the PC the launcher has the
    field.
  - Entering an address on the console is untested: Citron's keyboard can't be typed into by
    a script.

## Fixed

- **Gyro rows overlapping on the Controls page**: Gyro Aiming was a wide button that ran into
  the sensitivity label below it. It is now a normal row with a value button.
- **Multiplayer "Advanced" labelled "Graphics"**: the match setup's Advanced button shares its
  text with the title Options button that became Graphics. The rename now applies only on the
  title Options screen (150498).

## Checked

On Citron, with screenshots:
- the Controls page layout, the gyro value cycling and the swap toggle, saved to the file;
- the Player IP row and its navigation, and that EDIT opens the keyboard.

On the PC: both Options screens and the multiplayer screens.
