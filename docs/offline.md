# Offline game

Every online service the game used is gone: Gameloft Live, Facebook, Google+, Game Center,
the store servers, ads and analytics. This port removes what led to them, so nothing on
screen invites a login or a purchase. **Local Wi-Fi multiplayer and the campaign stay; the
multiplayer armory has everything owned from the start** (below). No game file is modified:
everything is done by hooks at run time (`host/offline.cpp`, `OFFLINE_STUBS` and
`SOCIAL_DISABLED` in `recomp/arm2c.py`).

## What was removed

| Where | Removed |
|---|---|
| Main menu | both "+" store buttons, the leaderboard podium, the friends / invite icon, the gold coin counter (the blue credits counter stays) |
| Multiplayer | the ONLINE button (only LOCAL WI-FI remains: Create, Join, Armory), the online profile button, the "+" button and the coin counter |
| Online multiplayer (no longer reachable) | create / join / quick play / playlists, tournaments, leaderboards, online statistics and match history, report player, voice chat |
| Login and social | Gameloft Live / Facebook / Google+ / Game Center login popups, invites, friends list and actions, login rewards |
| Info | Customer Care, Terms of Use, Privacy Policy (About stays) |
| Options | Advanced (title and pause menus) and Customer Care (pause menu); see [Input](input.md#the-controls-page) |
| After a level or a match | the "rate this game" popup: on its first update its third button (no thanks) reads as pressed (`CMenuButtons::Pressed(2)` at `GS_IGMRateGame::Update + 0x124`) and it is never drawn, so the game goes on to the menu, the next level or the lobby as after "no thanks" |
| Armory and other screens | "+" / buy-coins buttons, coin counters |
| Pause menu in multiplayer | the "Military Shop" panel (consumables for online gold): not drawn and takes no input; the buttons stay |
| Pause menu in the campaign | the "+" next to the credits counter and the store it opened (buy credits, watch videos for credits), the "earn extra credits" popup and its "no connection" page. The Military Shop stays: its credits come from play (below) |
| Credits from outside play | video rewards (`CFAMgr::ReceiveCoin` / `ReceiveBonus`), login and welcome-back rewards, the cheats page's "add credits" |
| Online services behind it all | Gaia (login, profile sync, ban checks, "update the game"), the federation's online prices and invites, the server configuration, the ads manager (banners, interstitials, video ads) |
| Everywhere | the age prompt, ad banners and full-screen ads, interstitials, analytics tracking, Bluetooth multiplayer |

Checked with screenshots: main menu, multiplayer, local Wi-Fi, armory, local match setup and
play, info, options (controls, advanced), campaign select, campaign and multiplayer pause
pages.

## How the menus work

The menus are a level of their own, `levels.gla/menus.lvc`: menu objects (screens,
containers, buttons, texts, sprites), each with an id. Their behaviour is embedded
Lua: handlers named `Obj<id>_OnClick`, `_OnEnter`, `_OnStateIdle`, … that call the game's
`Menu_*` C functions. For example, `Obj3063_OnClick` is just `Menu_PushState("CFA_Online")`,
the store. `CMenuManager::Load` loads the level in about 95 chunks.

The analysis (in `build/analysis/`: `menu_objects.txt`, `menu_handlers.txt`):

1. Find every handler that calls a store, social, login, ads, tracking or online function, or
   pushes the store screens (`CFA`, `CFA_Online`, `FriendsList`), or sends an online event
   (`StartLeaderBoard`, `AddFriends`, `StartLoginOnline`, `GLLive: …`, `ChangeState:
   Tournaments`, `TermOfUse`, `Policy`, …).
2. Read each handler and keep only those whose button exists **for** that purpose. Several
   handlers mention the store only in an online branch and must stay:
   - `150482` is the local Create (`SetLocalGame(true)`);
   - the armory Equip / Buy buttons only go to the online store when the game is online;
   - the killchain buy buttons pay with coins earned in play.
3. Map the visible icons to ids by tapping them in a scripted run: the event log names the
   handler each tap runs (`script: start Obj3063_OnClick`).

## The removal

- **Blocked objects**: 123 ids in `kBlockedObjects`, grouped with what their handler does:

  | Group | Ids |
  |---|---|
  | Store buttons (`Menu_PushState("CFA_Online")` / `("CFA")`) | 44 |
  | Social networks, accounts, friends, login rewards | 38 |
  | Online multiplayer, tournaments, leaderboards, statistics, report, voice chat, cheat upload, push notifications | 29 |
  | Bluetooth | 3 |
  | Web pages and store links (customer care, privacy, terms, EULA, "update the game") | 9 |

  After each `CMenuManager::Load` chunk they are hidden and disabled (the object's virtual
  `Show(false)` at vtable `+0x44` and `Enable(false)` at `+0x40`).
- **No way back**: `CMenuManager::ShowMenuObjectById` / `EnableMenuObjectById` (what
  `Menu_ShowMenuObject` and friends call) refuse to turn a blocked object back on.
- **Second line of defence**: `Menu_PushState` refuses `CFA`, `CFA_Online` and `FriendsList`.
  These functions do nothing (`OFFLINE_STUBS`):
  - `Menu_ShowInterstitial`, `Menu_SendTrackingEvent`, `Menu_GoToAppStore`
  - `Menu_EnterCustomerCare`, `Menu_EnterPrivacyPolicy`
  - `Menu_ClickButtonFacebook`, `Menu_ClickButtonGoogle`, `Menu_loginFB` and the Facebook /
    Google text updates
  - `Menu_EnterCFAOnlineNEC`, `Menu_Go_To_Online_Armory`,
    `Menu_RequestSeshatPutPurchasesIfNeeded`
  - `Menu_UploadCheats`, `Menu_SocialReward`, `Menu_ToggleVoiceChat`, `Menu_AddFriendByName`,
    `Menu_SetPN`, `Menu_BTAvailable`
  - the Gameloft ads manager: `WGLAdsManager::InitGLads`, `ShowBanner`, `ShowFullScreenAd`,
    `CheckAdAvailable`
- **SocialMgr** (`SOCIAL_DISABLED`): login, logout, friends, invites, Game Center
  leaderboards and achievements, the "+1" button and the per-frame social request pump return
  0.
- **Age prompt**: answered with 18 (see [Gameplay fixes](gameplay-fixes.md#age-gate)).

## Currency counters

The top-left bars show the gold coins (yellow: the online multiplayer's currency, bought in the
store) and the campaign's credits (blue: earned in play, they buy the pause menu's Military
Shop). The coins' bar goes; the credits' stays. Each bar is three things:

- the **number**: a `CMenuText` whose mode (`+0x58`) makes `CMenuText::Update` show
  `Application::GetCoinsCount()` (mode 2) or `CUpgradesManager::GetUpgradePoints()` (mode 1).
  At load the menu tree is walked (screens and containers keep their children at
  `+0x38…+0x3c`; ids at `+8`) and every coins text is hidden and blocked, one per screen;
- the **frame and icon**: not objects but modules of each screen's layout sprite, drawn by
  `CMenuScreen::Render` through `CMenuSprite::Render`: 1891 (bar ends) and 1892 (bar tiles)
  for the frame, 531 (gold coin) and 1872 (blue credit) for the icons; the coins' bar is the
  top row (layout y 14), the credits' the next (y 66). `CSprite::PaintModule` skips the coin
  icon and the top row's frame modules of the menu sprite. The thin decorative line along the
  top edge (modules 1093 / 1094 / 1096) is part of the same layout and stays.

The menus are loaded again after a level, with a new layout sprite, so the sprite is taken at
every layout draw: taken once, both bars came back after the first level (frames and icons,
without their numbers). Checked: the title menu at the first start and after a level shows the
credits bar alone.

## Armory

The multiplayer armory was a shop: weapons, add-ons, perks, items, kill signatures,
killchains and loadout presets bought with coins (bought with money through the store, or
earned in play), most of them also gated by multiplayer level, as were the loadout slots.
Everything is owned and open from the start. The armory's tables (`GameMpManager` +0xafa4 ..
+0xafc0, one `CComponentMPUnlockTable` per kind) hold, per entry, the level needed and an
owned flag; after `GameMpManager::LoadUnlockTableData` and `LoadPurchaseData` (both at start,
`Application::GoInit`) every entry gets level 0 and owned, and every table's slot level (+8,
`IsSlotUnlock`) 0. The layouts come from `GameMpManager::Is<kind>Unlock / Is<kind>Purchased`.
The game's purchases file then saves the owned flags like purchases. Checked with
screenshots: all six presets and every slot open, every weapon and perk "Owned", and a weapon
equips. The campaign's suit upgrades (pause menu, coins earned in levels) are unchanged.

## Multiplayer pause page

`GS_InGameMenu::RenderPausePage` draws the pause page. In multiplayer its left half is the
**Military Shop** (consumables for online gold; in a local game it says "These items are
available only in Online Multiplayer"). The page ignores `CSprite::SetClip`, and moving its
panel offset only moved the frame. So while the page draws in multiplayer, sprite modules
(`CSprite::PaintModule`) and strings (`CTTFFontSpr` / `CSpriteFont` `DrawString`) that start in
the left 52% of the screen are skipped; the page's own buttons are on the right. The campaign
pause page (suit upgrades) is not touched.

`MpManager::IsMultiplayerGame()` is a method: it reads `+4` of the `GameMpManager` instance
(`GameMpManager::Singleton`). It was once called without its object, so it read whatever `r0`
held: in the campaign the page took itself for the multiplayer one and its left panel (the
Military Shop with the Item / Upgrade tabs) was an empty blue frame. `hooks::multiplayer()`
reads the instance, for this page and every other check.

## Credits

The campaign's credits (the blue triangle) are earned in play: the upgrade collectibles hidden
in each level and the end-of-level bonus (`GS_EndLevel`). They buy the pause menu's Military
Shop: armor cores, grenade and mine packs, an ammo refill, three weapons (Plasma, Lightning,
Rail Gun) and two ability upgrades. That stays. What the game sold on top is gone:

- The pause page's "+" next to the counter opened the store page (`InGameMenuPage` 10, which
  starts `AdsManager`). The counter is a button (`GS_InGameMenu +0x3c`) whose presses are
  cleared before `UpdatePausePage` reads them (`pc_offline_pause_update`); the "+" box (modules
  407-414 of the page's sprite in its top-left corner) is not painted.
- A purchase short of credits went to that store when an ad was available, otherwise to page
  13, "You don't have enough coins to buy this item!". Pages 10, 11 (no connection) and 12
  ("earn extra credits", `RenderCFAPopup`) now become page 13 when a purchase is short (its
  shortfall at `+0x390`), and are not opened otherwise (`offline::in_game_page`, from
  `pc_hook_igm_push_page`). (`GS_InGameMenu::Update` and `Render` switch on page - 1.)
- In multiplayer the hidden Military Shop's item selector and Buy button (`+0x24`, `+0x28`)
  get their presses cleared too: the panel took taps and opened the store.
- The video rewards, login and welcome-back rewards and the "add credits" cheat do nothing, and
  Gaia, which showed the "earn extra credits" popup from its update, is off (`OFFLINE_STUBS`).

The shop is behind a **MILITARY SHOP** button, the first of the pause page's right column (the
game's own string 0x70c, so it is translated). `pc_offline_create_pause_page` makes it as the
game makes the column's buttons (`CButtonSpr`, frames 0xbb / 0xbc), first in the column's panel,
and puts the column's buttons (taken from the panel, sorted by height: Objectives to Back to
Menu, plus Debug and Cheats with the cheats on) in the rows below it, over the column's own
height (a row above Objectives fell outside the panel, which clipped it and showed a scroll
bar). Closed, the page's left half is not drawn (the same skip as in multiplayer), its controls'
presses are cleared, and the ITEM / UPGRADE tabs stand off the screen (the selected tab's glow
is drawn past the skipped modules). The button opens and closes the shop; Back closes it first:
the pause key comes as Android Back (`post_back` in `host/input.cpp` hands it to
`offline::take_back_key`), a controller's B in menus as input slot 1 (`pc_offline_igm_update`).

A press is `CButton::Pressed`: `+0x28` (enabled: `SetEnabled(false)` clears it) and `+0x27`
(released on the button, from `+0x26`). Clearing a press clears `+0x26` / `+0x27` only: clearing
`+0x28` disabled the button, which then was not drawn.

Tested (PC, 1280x720 and 1920x1080): the column with MILITARY SHOP on top and nothing on the
left; the button opens the shop (counter without "+", the credits icon), again closes it, Esc
closes it and a second Esc resumes; Buy short of credits shows the "not enough coins" page with
OK. Multiplayer: the pause page without shop or button, taps on its left half do nothing; a
local match is hosted.

## Nickname

Local Wi-Fi matches name the player with `XPlayerManager::GetUsername()` (given to the network
layer by `MpManager::StartServer` / `StartClient`), which was the Gameloft Live user name: empty
here, the lobby showed garbage. It returns `[multiplayer] nickname` (launcher: Multiplayer →
Nickname) when set. Tested: a hosted match lists "Adding player to lobby <nickname>".

## Not covered

- Screens that scripted runs could not reach (end-of-match and end-of-mission results,
  rarely used popups) were not checked by eye. Anything online there leads only to stubbed
  functions.
- The game files keep the removed screens' textures and strings: the menus level
  (`menus.lvc`) and the sprites load them by name at start, so they stay in the archives,
  never shown.
