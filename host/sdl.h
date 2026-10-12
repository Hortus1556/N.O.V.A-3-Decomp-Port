// SDL for the host: SDL3 on the PC, devkitPro's SDL2 on the Switch (whose SDL3 port is only
// for licensed developers). On the Switch, only the scancode table, GL loading and audio come
// from SDL; input is read from libnx (host/input.cpp).
#pragma once
#ifdef __SWITCH__
#include <SDL2/SDL.h>
#define SDL_SCANCODE_COUNT SDL_NUM_SCANCODES
#elif defined(__3DS__) || defined(__GAMECUBE__)
// The 3DS and the GameCube have no SDL: only the scancode numbers, and SDL's names for the usual
// keys, so the keyboard bindings in nova3.ini read and write as on the PC (neither has a keyboard).
#include <SDL3/SDL_scancode.h>
#include <strings.h>
inline const char *SDL_GetScancodeName(SDL_Scancode sc) {
    static const char *const letters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                          "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static const char *const digits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    static const char *const fkeys[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return letters[sc - SDL_SCANCODE_A];
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return digits[sc - SDL_SCANCODE_1];
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return fkeys[sc - SDL_SCANCODE_F1];
    switch (sc) {
    case SDL_SCANCODE_RETURN: return "Return";
    case SDL_SCANCODE_ESCAPE: return "Escape";
    case SDL_SCANCODE_BACKSPACE: return "Backspace";
    case SDL_SCANCODE_TAB: return "Tab";
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_UP: return "Up";
    case SDL_SCANCODE_DOWN: return "Down";
    case SDL_SCANCODE_LEFT: return "Left";
    case SDL_SCANCODE_RIGHT: return "Right";
    case SDL_SCANCODE_LSHIFT: return "Left Shift";
    case SDL_SCANCODE_RSHIFT: return "Right Shift";
    case SDL_SCANCODE_LCTRL: return "Left Ctrl";
    case SDL_SCANCODE_RCTRL: return "Right Ctrl";
    case SDL_SCANCODE_LALT: return "Left Alt";
    case SDL_SCANCODE_RALT: return "Right Alt";
    default: return "";
    }
}
inline SDL_Scancode SDL_GetScancodeFromName(const char *name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    for (int sc = SDL_SCANCODE_A; sc < SDL_SCANCODE_COUNT; sc++)
        if (const char *n = SDL_GetScancodeName(SDL_Scancode(sc)); *n && !strcasecmp(n, name)) return SDL_Scancode(sc);
    return SDL_SCANCODE_UNKNOWN;
}
#else
#include <SDL3/SDL.h>
#endif
