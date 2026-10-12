// Offline game: no store, social networks, logins, ads, leaderboards or online play.
//
// The menus are a level of their own (levels.gla/menus.lvc): menu objects with Lua handlers
// (Obj<id>_OnClick, ...) calling the game's Menu_* C functions. The buttons that lead to dead
// online services or real-money purchases are listed here by object id, with what their
// handler does; they are hidden and disabled once the menus are loaded, and later attempts to
// show or enable them (screen scripts call Menu_ShowMenuObject / Menu_EnableMenuObject) are
// refused. As a second line, the store and friends screens cannot be pushed, and the C
// functions behind the remaining online features do nothing (OFFLINE_STUBS in
// recomp/arm2c.py). Local Wi-Fi multiplayer, the campaign, and the armory and killchain
// purchases paid with credits earned in play stay as they are.
#include "controls_menu.h"
#include "hooks.h"
#include "offline.h"
#include "input.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <unordered_set>

namespace {

const std::unordered_set<uint32_t> kBlockedObjects = {
    // "+" / buy buttons: Menu_PushState("CFA_Online") or ("CFA"), the store for coins and
    // credits bought with real money (main menu, multiplayer menus, armory, popups)
    13, 61, 73, 3015, 3063, 3064, 3065, 3079, 8017, 8031, 8618, 150069, 150101, 150148, 150215,
    150238, 150249, 150260, 150272, 150283, 150294, 150305, 150316, 150353, 150368, 150370, 150387,
    150395, 150402, 150420, 150640, 150661, 150746, 150754, 150762, 150787, 150803, 240020, 240093,
    245050, 245051, 245067, 245100, 245156,
    // social networks, accounts and friends: Facebook / Google+ / Game Center / Gameloft Live
    // logins and invites, the friends list and its actions, login reward
    1, 4, 11, 21, 22, 24, 26, 58, 65, 66, 99, 106, 107, 115, 116, 119, 124, 133, 3060, 8727, 8728,
    9021, 9727, 31084, 31087, 150028, 150166, 150167, 150168, 150171, 150760, 190400, 245114, 245138,
    245139, 245140, 245153, 245179,
    // online multiplayer: the ONLINE button, online create / join / quick play / playlists,
    // tournaments, leaderboards (main-menu podium, top / my rank), online statistics and match
    // history, player report, voice chat, cheat upload, push notifications
    150057, 150064, 31111, 31112, 31113, 150074, 150075, 139002, 139003, 139004, 139052, 8611, 150099,
    150422, 150491, 150400, 150504, 150744, 3045, 3046, 3050, 3051, 63, 64, 199028, 6052, 27000,
    245012, 150553,
    // Bluetooth multiplayer (already hidden by the game when Bluetooth is unavailable)
    150059, 1145, 150488,
    // web pages and store links: customer care, privacy policy, terms of use, EULA, the
    // "update the game" store button
    72, 245190, 150583, 150569, 157, 158, 159, 33, 38,
    // the cheats page's "add credits" (the earned credits come from play only)
    245010,
    // (Options > Advanced, 150501, stays: it is the Graphics button now, host/controls_menu.cpp)
};

// Currency counters found in the loaded menus (see hide_currency_counters)
std::unordered_set<uint32_t> g_counters;

bool blocked(uint32_t id) { return kBlockedObjects.count(id) != 0 || g_counters.count(id) != 0; }

void hide_object(Cpu *cpu, uint32_t object) {
    const uint32_t vtable = LD32(object);
    rt::call_nested(cpu, LD32(vtable + 0x44), {object, 0});  // Show(false)
    rt::call_nested(cpu, LD32(vtable + 0x40), {object, 0});  // Enable(false)
}

// The coin and upgrade-point counters at the top left of most screens are CMenuText objects
// whose mode (+0x58) makes CMenuText::Update show Application::GetCoinsCount() (2, the gold
// coins: the online multiplayer's currency, bought in the store) or
// CUpgradesManager::GetUpgradePoints() (1, the blue credits earned in the campaign, which buy the
// pause menu's Military Shop). Walk the menu tree (screens and containers keep their children at
// +0x38..+0x3c; ids at +8) and hide the coins; the credits stay.
void hide_currency_counters(Cpu *cpu, uint32_t object, int depth) {
    static const uint32_t container_find = rt::export_address("_ZN14CMenuContainer10FindObjectEi");
    static const uint32_t text_vtable = rt::export_address("_ZTV9CMenuText") + 8;
    if (!object || depth > 16) return;
    const uint32_t vtable = LD32(object);
    if (vtable == text_vtable) {
        if (LD32(object + 0x58) == 2) {
            g_counters.insert(LD32(object + 8));
            hide_object(cpu, object);
        }
        return;
    }
    if (LD32(vtable + 0x48) == container_find)  // a CMenuContainer (screens too)
        for (uint32_t it = LD32(object + 0x38); it < LD32(object + 0x3c); it += 4)
            hide_currency_counters(cpu, LD32(it), depth + 1);
}

}  // namespace

// CMenuManager::Load(CMemoryStream *): loads the menus in chunks (about 95 calls). After each,
// hide and disable the blocked objects loaded so far (virtual Show(bool) at +0x44, Enable(bool) at +0x40, as
// CMenuManager::ShowMenuObjectById / EnableMenuObjectById do).
extern "C" void pc_offline_menus_loaded(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t manager = cpu->r[0];
    original(cpu, entry);
    static const uint32_t find = rt::export_address("_ZN12CMenuManager10FindObjectEi");
    if (!find) return;
    for (uint32_t id : kBlockedObjects)
        if (uint32_t object = rt::call_nested(cpu, find, {manager, id})) hide_object(cpu, object);
    // the manager's screen lists (as CMenuManager::FindObject walks them)
    for (uint32_t list : {0x3cu, 0x9cu})
        for (uint32_t it = LD32(manager + list); it < LD32(manager + list + 4); it += 4)
            hide_currency_counters(cpu, LD32(it), 0);
}

// CMenuManager::ShowMenuObjectById / EnableMenuObjectById(int id, bool): a blocked object
// stays hidden and disabled.
extern "C" void pc_offline_show_object(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (blocked(cpu->r[1])) cpu->r[2] = 0;
    original(cpu, entry);
}

// Menu_PushState(name): the store (CFA, CFA_Online) and the online friends list are not
// entered.
extern "C" void pc_offline_push_state(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t to_string = rt::export_address("lua_tolstring");
    const uint32_t L = cpu->r[0];
    uint32_t name = to_string ? rt::call_nested(cpu, to_string, {L, 1, 0}) : 0;
    const char *s = name ? rt::str(name) : nullptr;
    if (s && (!std::strcmp(s, "CFA") || !std::strcmp(s, "CFA_Online") || !std::strcmp(s, "FriendsList"))) {
        rt::log("offline: menu state %s not available", s);
        cpu->r[0] = 0;  // no Lua results
        cpu->ret_pc = cpu->r[14];
        return;
    }
    original(cpu, entry);
}

// The counters' frames and icons are part of each screen's layout sprite (the CMenuSprite a
// CMenuScreen renders first, CMenuScreen::Render), drawn as modules of the menu sprite: 1891
// (bar ends) and 1892 (bar tiles) for the frames, 531 (gold coin) and 1872 (blue credit) for
// the icons; the coins' bar is the top row (layout y 14), the credits' the next (y 66). The
// same layout draws the thin line along the top edge (modules 1093/1094/1096), which stays.
// CSprite::PaintModule skips the coins' bar and icon (see pc_offline_paint_module).
//
// The menus are loaded again after a level, with a new layout sprite: the sprite is taken at
// every layout draw (taken once, the counters came back after the first level, frames and
// icons without their amounts).
namespace {
uint32_t g_menu_sprite;    // the CSprite the screens' layouts are drawn from
bool g_drawing_layout;     // inside a screen's layout CMenuSprite::Render
bool is_coin_counter_module(uint32_t module, float y) {
    return module == 531 || ((module == 1891 || module == 1892) && y < 40);
}
}  // namespace

// CMenuSprite::Render(): CMenuScreen::Render calls it at +0xd0 / +0x204 for the screen's own
// layout sprite (CMenuScreen+0x14); the CSprite it paints from is noted on the first module.
extern "C" void pc_offline_screen_sprite(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t render = rt::export_address("_ZN11CMenuScreen6RenderEv");
    g_drawing_layout = cpu->r[14] == render + 0xd0 || cpu->r[14] == render + 0x204;
    original(cpu, entry);
    g_drawing_layout = false;
}

// The pause menu's credits store. The campaign's shop (Military Shop: armor cores, grenade and
// mine packs, ammo refill, three weapons, two ability upgrades) is paid with credits earned in
// play (the levels' upgrade collectibles, the end-of-level bonus) and stays. What the game
// sold on top goes: the "+" next to the credits counter, which opened the store page (the
// counter is a button, GS_InGameMenu +0x3c, whose presses are cleared; the "+" box is modules
// 407-414 of the page's sprite in its top-left corner, not painted), and the pages a purchase
// short of credits led to:
//   10  the store: buy credits, watch videos for credits (PushPage starts AdsManager for it)
//   11  "no connection" (from the store page)
//   12  "EARN EXTRA CREDITS" (RenderCFAPopup)
// instead of page 13, "You don't have enough coins to buy this item!" (RenderNotEnoughCredits).
// (InGameMenuPage numbers: GS_InGameMenu::Update and Render switch on page - 1.)
namespace {
constexpr uint32_t kPageStore = 10, kPageNoConnection = 11, kPageEarnCredits = 12, kPageNotEnough = 13;
constexpr uint32_t kPlusButton = 0x3c, kItemSweep = 0x24, kBuyButton = 0x28;  // GS_InGameMenu: CButton*
constexpr uint32_t kShortfall = 0x390;  // the credits a purchase lacks (UpdatePausePage)

// A press not taken: CButton::Pressed is +0x28 (enabled) and +0x27 (released on it, from +0x26
// pending, CButton::Update); touch, mouse and the gamepad's confirm (CPanel) all end there.
void clear_press(uint32_t button) {
    if (!button) return;
    ST8(button + 0x26, 0);
    ST8(button + 0x27, 0);
}

// The campaign pause page keeps its Military Shop behind a MILITARY SHOP button above Objectives:
// closed, the page's left half (the shop, its credits counter) is not drawn and takes no input;
// the button, or Back, opens and closes it.
uint32_t g_shop_button;  // the button on the current pause page (0: none, multiplayer)
std::atomic<bool> g_shop_open{false};
std::atomic<bool> g_close_shop{false};  // the pause key asked (input thread)
int16_t g_tab_x[2];      // the ITEM / UPGRADE tabs' x (while hidden they are off the screen)
constexpr uint32_t kColumn = 0x70, kObjectives = 0x98;  // GS_InGameMenu: CPanel*, CButtonSpr*
constexpr uint32_t kPageSprite = 0x20, kLabelColor = 0x368;
constexpr uint32_t kTabItem = 0x2c, kTabUpgrade = 0x30;  // the shop's ITEM / UPGRADE tabs
constexpr uint32_t kPagePause = 1;
constexpr uint32_t kShopLabel = 0x70c;  // "MILITARY SHOP" (MAIN strings)

bool pressed(uint32_t button) { return button && LD8(button + 0x28) && LD8(button + 0x27); }  // CButton::Pressed

// the left half of the page is hidden: multiplayer (its shop is for online gold), or the
// campaign's shop closed
bool shop_hidden() { return hooks::multiplayer() || (g_shop_button && !g_shop_open); }

// The ITEM / UPGRADE tabs: the selected one's glow is drawn past the sprite modules skipped
// below, so while the shop is hidden the tabs stand off the screen (CButton::SetPosX; the page
// moves its buttons by differences, so they stay there).
void place_tabs(Cpu *cpu, uint32_t menu) {
    static const uint32_t set_pos_x = rt::export_address("_ZN7CButton7SetPosXEs");
    for (int i = 0; i < 2; i++)
        if (const uint32_t tab = LD32(menu + (i ? kTabUpgrade : kTabItem)); tab && set_pos_x)
            rt::call_nested(cpu, set_pos_x, {tab, uint32_t(uint16_t(shop_hidden() ? int16_t(-20000) : g_tab_x[i]))});
}
}  // namespace

uint32_t offline::in_game_page(uint32_t menu, uint32_t page) {
    if (page != kPageStore && page != kPageNoConnection && page != kPageEarnCredits) return page;
    return int32_t(LD32(menu + kShortfall)) > 0 ? kPageNotEnough : 0;
}

// GS_InGameMenu::UpdatePausePage(): no press of the "+" button reaches it, nor, in multiplayer,
// of the hidden Military Shop's item selector and Buy button (consumables for online gold; the
// panel is not drawn, pc_offline_pause_page, but it took taps: the store and its video offer).
extern "C" void pc_offline_pause_update(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t menu = cpu->r[0];
    clear_press(LD32(menu + kPlusButton));
    if (pressed(g_shop_button)) {
        static const uint32_t feedback = rt::export_address("_ZN13GS_InGameMenu17PlayFeedbackSoundEv");
        g_shop_open = !g_shop_open;
        clear_press(g_shop_button);
        place_tabs(cpu, menu);
        if (feedback) rt::call_nested(cpu, feedback, {menu});
    }
    if (shop_hidden())
        for (const uint32_t control : {kItemSweep, kBuyButton, kTabItem, kTabUpgrade}) clear_press(LD32(menu + control));
    original(cpu, entry);
}

// GS_InGameMenu::CreatePausePage(): the campaign's MILITARY SHOP button at the top of the right
// column, as the game makes the column's buttons; the six share the five's height (a row
// above Objectives is outside the column's panel, which clipped it and showed a scroll bar).
extern "C" void pc_offline_create_pause_page(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t menu = cpu->r[0];
    original(cpu, entry);
    g_shop_button = 0;
    g_shop_open = false;
    for (int i = 0; i < 2; i++)
        if (const uint32_t tab = LD32(menu + (i ? kTabUpgrade : kTabItem))) g_tab_x[i] = int16_t(LD16(tab + 0x50));
    place_tabs(cpu, menu);
    const uint32_t column = LD32(menu + kColumn), objectives = LD32(menu + kObjectives);
    if (hooks::multiplayer() || !column || !objectives) return;
    // the column's buttons (CPanel controls: CButton* at +0x8c .. +0x90), top to bottom:
    // Objectives, Restart Checkpoint, Restart Level, Options, Back to Menu (and Debug and
    // Cheats with the cheats on)
    std::vector<uint32_t> rows;
    for (uint32_t it = LD32(column + 0x8c); it < LD32(column + 0x90); it += 4) rows.push_back(LD32(it));
    std::sort(rows.begin(), rows.end(), [](uint32_t a, uint32_t b) { return int16_t(LD16(a + 0x52)) < int16_t(LD16(b + 0x52)); });
    if (rows.size() < 2 || rows[0] != objectives) return;
    static const uint32_t alloc = rt::export_address("_Znwj");
    static const uint32_t ctor = rt::export_address("_ZN10CButtonSprC1EiiP7CSpriteiiiiiii");
    static const uint32_t add = rt::export_address("_ZN6CPanel10AddControlEP7CButton");
    static const uint32_t navig_up = rt::export_address("_ZN7CButton11SetNavig_UpEPS_");
    static const uint32_t navig_down = rt::export_address("_ZN7CButton13SetNavig_DownEPS_");
    if (!alloc || !ctor || !add || !navig_up || !navig_down) return;
    static const uint32_t set_pos_y = rt::export_address("_ZN7CButton7SetPosYEs");
    if (!set_pos_y) return;
    const int x = int16_t(LD16(objectives + 0x50)), y = int16_t(LD16(objectives + 0x52));
    const int last = int16_t(LD16(rows.back() + 0x52)), n = int(rows.size());
    const uint32_t button = rt::call_nested(cpu, alloc, {0xbc});
    rt::call_nested(cpu, ctor, {button, uint32_t(x), uint32_t(y), LD32(menu + kPageSprite), 0xbb, 0xbc, kShopLabel, 5, 0x22, 0, 0});
    ST8(button + 0x65, 1);
    ST32(button + 0x74, LD32(menu + kLabelColor));
    ST32(button + 4, 1);
    rt::call_nested(cpu, add, {column, button});
    // first in the panel's list too, as at the top of the column (appended, it went missing
    // from the column while the shop was open)
    const uint32_t first = LD32(column + 0x8c), end = LD32(column + 0x90);
    for (uint32_t it = end - 4; it > first; it -= 4) ST32(it, LD32(it - 4));
    ST32(first, button);
    rt::call_nested(cpu, navig_down, {button, objectives});
    rt::call_nested(cpu, navig_up, {objectives, button});
    for (int i = 0; i < n; i++)  // the n buttons in rows 1..n of n + 1 over the same span
        rt::call_nested(cpu, set_pos_y, {rows[size_t(i)], uint32_t(uint16_t(y + (last - y) * (i + 1) / n))});
    g_shop_button = button;
    place_tabs(cpu, menu);
}

// GS_InGameMenu::Update(): on the pause page, Back (CAccessory input slot 1: a controller's B in
// menus) and slot 8 resume the game, read here before UpdatePausePage; with the campaign's shop
// open they close it instead (their release, byte 2 of the slot, is taken). The pause key comes
// as Android Back (offline::take_back_key).
extern "C" void pc_offline_igm_update(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t status = rt::export_address("_ZN10CAccessory13m_inputStatusE");
    const uint32_t back = status + 1 * 3 + 2, pause = status + 8 * 3 + 2;
    if (g_close_shop.exchange(false)) offline::close_shop(cpu, cpu->r[0]);
    if (status && (LD8(back) || LD8(pause)) && offline::close_shop(cpu, cpu->r[0])) {
        ST8(back, 0);
        ST8(pause, 0);
    }
    original(cpu, entry);
}

bool offline::take_back_key() {
    if (!g_shop_open) return false;
    g_close_shop = true;
    return true;
}

bool offline::close_shop(Cpu *cpu, uint32_t menu) {
    if (!g_shop_open || LD32(menu + 0x1e0) != kPagePause) return false;
    g_shop_open = false;
    place_tabs(cpu, menu);
    return true;
}

// GS_InGameMenu::RenderPausePage(): the pause page. In multiplayer its left panel is the
// "Military shop" for consumables bought with online gold ("These items are available only in
// Online Multiplayer" in a local game). While the page draws in multiplayer, sprite modules
// (CSprite::PaintModule, which frames and buttons end in) and strings (the fonts' DrawString)
// that start in the left half of the screen are not painted: the page's own buttons are on
// the right. (The page ignores CSprite::SetClip.) In the campaign the panel shows the suit
// upgrades and is left alone.
namespace {
bool g_pause_panel;     // drawing the multiplayer pause page
bool g_in_pause_page;   // drawing the pause page
float g_screen_w, g_screen_h;  // (meanwhile)
float g_pause_panel_x;  // what starts left of this is skipped
}

extern "C" void pc_offline_pause_page(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t clip_rect = rt::export_address("_ZN7CSprite10s_clipRectE");
    g_in_pause_page = true;
    // CSprite::s_clipRect holds the whole screen outside clipped drawing (x1 = width, y1 = height)
    g_screen_w = clip_rect ? float(int32_t(LD32(clip_rect + 8))) : 0;
    g_screen_h = clip_rect ? float(int32_t(LD32(clip_rect + 12))) : 0;
    if (clip_rect && shop_hidden()) {
        g_pause_panel_x = g_screen_w * 0.52f;
        g_pause_panel = true;
    }
    original(cpu, entry);
    g_pause_panel = false;
    g_in_pause_page = false;
}

// CSprite::PaintModule(int module, float x, float y, ...)
extern "C" void pc_offline_paint_module(Cpu *cpu, uint32_t entry, GuestFn original) {
    if (g_in_pause_page && cpu->r[1] >= 407 && cpu->r[1] <= 414) {  // the "+" box (above)
        float x, y;
        std::memcpy(&x, &cpu->r[2], 4);
        std::memcpy(&y, &cpu->r[3], 4);
        if (x < g_screen_w * 0.06f && y < g_screen_h * 0.125f) {  // (at 1280x720: x 8..52, y 8..44)
            cpu->ret_pc = cpu->r[14];
            return;
        }
    }
    if (g_drawing_layout) g_menu_sprite = cpu->r[0];
    if (g_menu_sprite && cpu->r[0] == g_menu_sprite) {
        float y;
        std::memcpy(&y, &cpu->r[3], 4);
        if (y < 120 && is_coin_counter_module(cpu->r[1], y)) {  // the counters at the top (layout coordinates)
            cpu->ret_pc = cpu->r[14];
            return;
        }
    }
    if (g_pause_panel) {
        float x;
        std::memcpy(&x, &cpu->r[2], 4);
        if (x < g_pause_panel_x) {
            cpu->ret_pc = cpu->r[14];
            return;
        }
    }
    original(cpu, entry);
}

// <font>::DrawString / DrawStringLimitWidth(const char *text, int x, ...): the pause page's
// hidden column (above), and text controls_menu::hide_text() leaves out
extern "C" void pc_offline_draw_string(Cpu *cpu, uint32_t entry, GuestFn original) {
    if ((g_pause_panel && float(int32_t(cpu->r[2])) < g_pause_panel_x) || controls_menu::hide_text()) {
        cpu->ret_pc = cpu->r[14];
        return;
    }
    original(cpu, entry);
}



// XPlayerManager::GetUsername(): the player's name in local Wi-Fi matches (MpManager::
// StartServer / StartClient give it to the network layer) is [multiplayer] nickname from the
// launcher, when set.
extern "C" void pc_offline_username(Cpu *cpu, uint32_t entry, GuestFn original) {
    static std::string current;
    static uint32_t text = 0;  // guest copy, kept while the name is the same
    const std::string &nickname = input::config().nickname;
    if (nickname.empty()) {
        original(cpu, entry);
        return;
    }
    if (!text || nickname != current) {
        current = nickname;
        text = rt::gstrdup(current.c_str());  // (a new name leaks the old copy: a few bytes per rename)
    }
    cpu->r[0] = text;
    cpu->ret_pc = cpu->r[14];
}

// GS_IGMRateGame: after a level or a match, a "rate this game" popup (Rate now / Later / No
// thanks) linking to the Play Store. Its buttons then continue to the menu, the next level or
// the lobby. On its first update the third button (no thanks) reads as pressed
// (CMenuButtons::Pressed(2), called at GS_IGMRateGame::Update + 0x120, returning to + 0x124), and it is never drawn.
extern "C" void pc_offline_rate_pressed(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const uint32_t update = rt::export_address("_ZN14GS_IGMRateGame6UpdateEv");
    if (update && cpu->r[14] == update + 0x124 && cpu->r[1] == 2) {
        cpu->r[0] = 1;
        cpu->ret_pc = cpu->r[14];
        return;
    }
    original(cpu, entry);
}
extern "C" void pc_offline_rate_render(Cpu *cpu, uint32_t entry, GuestFn original) {
    (void)entry, (void)original;
    cpu->ret_pc = cpu->r[14];
}

// GameMpManager::LoadUnlockTableData / LoadPurchaseData (Application::GoInit, once each): the
// multiplayer armory's tables (one call per table), then which entries the player bought.
// Every entry is owned and needs no multiplayer level, and every loadout slot is open: the
// armory was a shop (coins, or money through the store) gated by multiplayer level. Set in
// memory at every start; the game's purchases file then keeps the owned flags as purchases.
namespace {
struct UnlockTable {
    uint32_t field;     // GameMpManager +: the table (CComponentMPUnlockTable*)
    uint32_t entries;   // table +: std::vector of entries
    uint32_t size;      // entry size
    int32_t level;      // entry +: multiplayer level needed, or -1
    uint32_t purchased; // entry +: bought (byte)
    bool slot;          // table +8: level needed for the slot (IsSlotUnlock)
};
// from GameMpManager::Is<kind>Unlock / Is<kind>Purchased / IsSlotUnlock
const UnlockTable kUnlockTables[] = {
    {0xafa4, 0xc, 0x44, 0xc, 0x38, true},   // primary weapons
    {0xafa8, 0xc, 0x44, 0xc, 0x38, true},   // secondary weapons
    {0xafac, 0xc, 0x40, 0xc, 0x3c, true},   // weapon add-ons
    {0xafb0, 0xc, 0x30, 0xc, 0x2c, true},   // perks (+0x10: a second requirement, see below)
    {0xafb4, 0xc, 0x2c, 0xc, 0x28, true},   // items
    {0xafb8, 0xc, 0x20, 0x8, 0x18, false},  // kill signatures
    {0xafbc, 0x4, 0x24, 0xc, 0x18, false},  // killchains
    {0xafc0, 0x4, 0x1c, -1, 0x14, false},   // presets
};

int unlock_armory(uint32_t manager) {
    int count = 0;
    for (const UnlockTable &t : kUnlockTables) {
        const uint32_t table = LD32(manager + t.field);
        if (!table) continue;
        if (t.slot) ST32(table + 8, 0);
        for (uint32_t e = LD32(table + t.entries); e + t.size <= LD32(table + t.entries + 4); e += t.size, count++) {
            if (t.level >= 0) ST32(e + uint32_t(t.level), 0);
            ST8(e + t.purchased, 1);
            if (t.field == 0xafb0) ST32(e + 0x10, 0);  // IsPerkUnlock: also GameMpManager +0xb060 >= this
        }
    }
    return count;
}
}  // namespace

extern "C" void pc_offline_unlock_tables(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t manager = cpu->r[0];
    original(cpu, entry);
    unlock_armory(manager);
}
extern "C" void pc_offline_unlock_purchases(Cpu *cpu, uint32_t entry, GuestFn original) {
    const uint32_t manager = cpu->r[0];
    original(cpu, entry);
    rt::log("offline: armory unlocked (%d entries)", unlock_armory(manager));
}
