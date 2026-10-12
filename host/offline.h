// Offline game: what the port removes of the online, store, social and ad services
// (offline.cpp, docs/offline.md).
#pragma once

#include <cstdint>

struct Cpu;

namespace offline {

// GS_InGameMenu::PushPage: the pause menu page shown instead of `page`, 0 for none. The
// credits store and its video ads are gone; a purchase short of earned credits says so.
uint32_t in_game_page(uint32_t menu, uint32_t page);

// Back on the campaign pause page (GS_InGameMenu::Update, PopPage): closes its Military Shop
// when open (true: the page stays).
bool close_shop(Cpu *cpu, uint32_t menu);

// The pause key (posted as Android Back, input.cpp) while the campaign's shop is open: true, and
// the shop closes on the game thread's next pause menu update instead of the game resuming.
bool take_back_key();

}  // namespace offline
