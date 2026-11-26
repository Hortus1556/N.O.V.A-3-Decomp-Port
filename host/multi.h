// Several game windows on one PC: each nova3.exe takes a player slot (1, 2, ...) and claims its
// own controller, so every window is played with a different gamepad.
//
// Slots and controllers are named mutexes (Windows): the system frees them when the process
// ends, even after a crash, so nothing stale is ever left behind. Elsewhere there is one
// player and every claim succeeds.
#pragma once
#include <string>
#include <vector>

namespace multi {

// Once, early (before the log and the save folder): takes slot `preferred` when it is free,
// else the first free slot, 1-8.
int claim_slot(int preferred = 0);
// This window's player number: the one player 1 gave it (set_player), else its slot (1
// until claim_slot). It decides the view, controller, name and save folder.
int slot();
// The slot this process actually holds (another copy of the game may hold the player's own).
int held_slot();
// Split screen: the player number player 1 gave this window (--player).
void set_player(int player);
// Another nova3.exe holds a slot (checked at most once a second; any thread).
bool others_running();
// The nova3.exe with slot n is still open.
bool slot_alive(int n);

// [launcher] windows = N: the tiles the monitor is split into, all the same size. 2 and 3
// side by side, 4 as 2 x 2, 5-6 as 3 x 2, 7-8 as 4 x 2. Player k takes tile k (left to
// right, top to bottom).
void grid(int windows, int &cols, int &rows);

// [launcher] pads: each player's controller, "|"-separated device paths ("" = the first free).
std::vector<std::string> split_list(const std::string &s);
std::string join_list(const std::vector<std::string> &v);

// One window split into a view per player (Windows): player 1 opens the container window,
// and every player's game window (each its own process) becomes a child window of it. HWNDs
// travel between the processes as numbers (--split N:monitor:hwnd).
//
// The view of player `slot` inside the container's client area (container coordinates).
void tile_rect(void *container, int windows, int slot, int &x, int &y, int &w, int &h);
// Make `window` (this process's top-level HWND) a child of `container` at that rectangle.
bool embed(void *window, void *container, int x, int y, int w, int h);
// The keyboard focus is in `window` (another player's view may have it, in another process).
bool has_focus(void *window);
// Give `window` the keyboard focus (a click in this player's view).
void take_focus(void *window);
// The container window still exists.
bool window_alive(void *container);
// The container window is the active window (the player is in the game, not alt-tabbed away).
bool is_foreground(void *container);

// Mouse look in a view (SDL's relative mouse mode needs a focused top-level window): the
// cursor is kept inside the view and put back at its center after every move; the distance
// it travelled is the look movement.
void lock_cursor(void *view);
void unlock_cursor();
void recenter_cursor(void *view, int &dx, int &dy);

// A controller, by a key that is the same in every process (the device path). Returns false
// when another window already has it. release_pad() frees the one this window holds.
bool claim_pad(const std::string &key);
void release_pad();

}  // namespace multi
