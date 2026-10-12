// What kind of machine the host runs on. The consoles (Switch, New 3DS, GameCube) have no SDL window or
// mouse: their controllers and touch screens are read directly in input::update(), text comes
// from the system keyboard, and the video settings live on the game's Graphics page.
#pragma once
#if defined(__SWITCH__) || defined(__3DS__) || defined(__GAMECUBE__)
#define NOVA3_CONSOLE 1
#endif
