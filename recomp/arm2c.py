"""Static recompiler: ARMv7-A (A32) code of libGloftNAHM.so -> C.

Each guest function becomes `void f_XXXXXXXX(Cpu *cpu, uint32_t entry)`. Guest registers
live in C locals inside a function so the host compiler can register-allocate them; at
calls and returns only the registers the register-flow analysis (class Flow) finds
necessary pass through the Cpu struct. Guest memory is a flat 4 GiB window (see runtime/recomp.h);
the library is mapped at LOAD_BASE, so a link-time address A is guest address
LOAD_BASE + A.

Usage:
  arm2c.py analyze                       discover functions, write build/recomp/funcs.json
  arm2c.py gen <outdir> [addr|name ...]  emit C (all functions, or only the listed ones)
"""
import bisect, collections, io, json, os, re, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
from elf import load
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

LOAD_BASE = 0x10000000
FUNCS_JSON = 'E:/nova3/build/recomp/funcs.json'
FILE_WINDOW_BITS = 19  # one generated .c file per 512 KiB of guest code


def write_if_changed(path, text):
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w') as f:
        f.write(text)


class open_if_changed(io.StringIO):
    """`with open_if_changed(path) as f:` buffers writes, then calls write_if_changed."""
    def __init__(self, path):
        super().__init__()
        self.path = path

    def __exit__(self, *exc):
        if not exc[0]:
            write_if_changed(self.path, self.getvalue())
        return super().__exit__(*exc)

# Functions that never return: don't treat the next word as a fall-through.
NORETURN = {'abort', 'exit', '_exit', '__assert2', '__stack_chk_fail', 'longjmp', 'siglongjmp',
            'pthread_exit', '__cxa_throw', '__cxa_rethrow', '_Unwind_Resume', '__cxa_bad_cast',
            '__cxa_bad_typeid', '__cxa_pure_virtual', '_ZSt9terminatev', '__gnu_cxx::__verbose_terminate_handler',
            '_ZSt20__throw_length_errorPKc', '_ZSt17__throw_bad_allocv', '_ZSt19__throw_logic_errorPKc',
            '_ZSt20__throw_out_of_rangePKc', '_ZSt24__throw_out_of_range_fmtPKcz', '_ZSt24__throw_invalid_argumentPKc',
            '_ZSt21__throw_runtime_errorPKc', '_ZSt16__throw_bad_castv', '_ZSt25__throw_bad_function_callv',
            '_ZSt20__throw_system_errori', '_ZSt19__throw_range_errorPKc', '_ZSt21__throw_bad_exceptionv',
            '_ZSt17__throw_bad_typeidv', '_ZSt23__throw_underflow_errorPKc', '_ZSt22__throw_overflow_errorPKc',
            '_ZSt20__throw_domain_errorPKc', '_ZSt21__throw_out_of_rangePKc', '__cxa_call_unexpected',
            'restore_core_regs',  # the runtime's restore_core_regs longjmps to a landing pad
            '__cxa_end_cleanup'}  # ends a cleanup landing pad: resumes unwinding

# Run-time helpers inlined at their call sites: the ARM EABI division routines (ARMv7 phones
# had no divide instruction). Callers may only rely on the documented results afterwards
# (r0, r1, and r2/r3 for the 64-bit remainders), so the call becomes plain C.
INLINE_HELPERS = {
    '__aeabi_uidiv': 'r0 = UDIV(r0, r1);',
    '__udivsi3': 'r0 = UDIV(r0, r1);',
    '__aeabi_idiv': 'r0 = SDIV(r0, r1);',
    '__divsi3': 'r0 = SDIV(r0, r1);',
    '__aeabi_uidivmod': '{ uint32_t q_ = UDIV(r0, r1); r1 = r0 - q_ * r1; r0 = q_; }',
    '__aeabi_idivmod': '{ uint32_t q_ = SDIV(r0, r1); r1 = r0 - q_ * r1; r0 = q_; }',
    '__aeabi_uldivmod': '{ uint64_t a_ = ((uint64_t)r1 << 32) | r0, b_ = ((uint64_t)r3 << 32) | r2, q_ = b_ ? a_ / b_ : 0, m_ = a_ - q_ * b_; '
                        'r0 = (uint32_t)q_; r1 = (uint32_t)(q_ >> 32); r2 = (uint32_t)m_; r3 = (uint32_t)(m_ >> 32); }',
    '__aeabi_ldivmod': '{ int64_t a_ = (int64_t)(((uint64_t)r1 << 32) | r0), b_ = (int64_t)(((uint64_t)r3 << 32) | r2); '
                       'int64_t q_ = !b_ ? 0 : (b_ == -1 && a_ == INT64_MIN) ? a_ : a_ / b_, m_ = (int64_t)((uint64_t)a_ - (uint64_t)q_ * (uint64_t)b_); '
                       'r0 = (uint32_t)q_; r1 = (uint32_t)((uint64_t)q_ >> 32); r2 = (uint32_t)m_; r3 = (uint32_t)((uint64_t)m_ >> 32); }',
    '__udivdi3': '{ uint64_t a_ = ((uint64_t)r1 << 32) | r0, b_ = ((uint64_t)r3 << 32) | r2, q_ = b_ ? a_ / b_ : 0; '
                 'r0 = (uint32_t)q_; r1 = (uint32_t)(q_ >> 32); }',
    '__divdi3': '{ int64_t a_ = (int64_t)(((uint64_t)r1 << 32) | r0), b_ = (int64_t)(((uint64_t)r3 << 32) | r2); '
                'int64_t q_ = !b_ ? 0 : (b_ == -1 && a_ == INT64_MIN) ? a_ : a_ / b_; '
                'r0 = (uint32_t)q_; r1 = (uint32_t)((uint64_t)q_ >> 32); }',
}

# Imports compiled in place (runtime/recomp.h's IM_ / GR_ / GW_ helpers): the same host calls
# as the runtime's implementations, without the import table and with only the registers the
# C code writes (r0, or r0:r1 for doubles) changed; AAPCS leaves r1-r3, r12 and the flags
# undefined after a call, so keeping them is one valid outcome. Only side-effect-free string
# and math functions: errno, locks and allocator state stay with the runtime.
INLINE_IMPORTS = {
    'memcpy': 'GW_MEMCPY(r0, r1, r2);',
    'memmove': 'GW_MEMMOVE(r0, r1, r2);',
    'memset': 'GW_MEMSET(r0, r1, r2);',
    'wmemcpy': 'GW_WMEMCPY(r0, r1, r2);',
    'strcpy': 'GW_STRCPY(r0, r1);',
    'strncpy': 'GW_STRNCPY(r0, r1, r2);',
    'memcmp': 'r0 = GR_MEMCMP(r0, r1, r2);',
    'strlen': 'r0 = GR_STRLEN(r0);',
    'strcmp': 'r0 = GR_STRCMP(r0, r1);',
    'strncmp': 'r0 = GR_STRNCMP(r0, r1, r2);',
    'strcasecmp': 'r0 = GR_STRCASECMP(r0, r1);',
    'strchr': 'r0 = GR_STRCHR(r0, r1);',
    'strrchr': 'r0 = GR_STRRCHR(r0, r1);',
    'memchr': 'r0 = GR_MEMCHR(r0, r1, r2);',
    'wcslen': 'r0 = GR_WCSLEN(r0);',
    'tolower': 'r0 = GR_TOLOWER(r0);',
    'strstr': 'r0 = GR_STRSTR(r0, r1);',
    'isalnum': 'r0 = GR_ISALNUM(r0);',
    # the thread calls: host functions of the runtime (runtime/thread.cpp) with no register
    # state of their own; a blocked lock does not need the guest's registers either
    'pthread_getspecific': 'r0 = rt_im_getspecific(r0);',
    'pthread_self': 'r0 = rt_im_self();',
    'pthread_mutex_lock': 'r0 = rt_im_mutex_lock(r0);',
    'pthread_mutex_unlock': 'r0 = rt_im_mutex_unlock(r0);',
}
INLINE_IMPORTS.update({f: f'r0 = IM_{f}(r0);' for f in (
    'acosf', 'asinf', 'atanf', 'ceilf', 'cosf', 'expf', 'floorf', 'log10f', 'logf', 'sinf', 'tanf')})
INLINE_IMPORTS.update({f: f'r0 = IM_{f}(r0, r1);' for f in ('atan2f', 'fmodf', 'powf')})
INLINE_IMPORTS.update({f: f'{{ uint64_t d_ = IM_{f}(r0, r1); r0 = (uint32_t)d_; r1 = (uint32_t)(d_ >> 32); }}' for f in (
    'acos', 'asin', 'atan', 'ceil', 'cos', 'cosh', 'exp', 'floor', 'log', 'log10', 'sin', 'sinh', 'tan', 'tanh')})
INLINE_IMPORTS.update({f: f'{{ uint64_t d_ = IM_{f}(r0, r1, r2, r3); r0 = (uint32_t)d_; r1 = (uint32_t)(d_ >> 32); }}'
                       for f in ('atan2', 'fmod', 'pow')})

GXX_PERSONALITY = 0x16ef7f4  # __gxx_personality_v0 (not exported by name)

# Guest functions replaced or wrapped by host code. This is also the hook for hand-decompiled
# replacements and for PC-specific behaviour.
#   'sym': 'host_fn'            replace: void host_fn(Cpu *)
#   'sym': ('wrap', 'host_fn')  wrap:    void host_fn(Cpu *, uint32_t entry, GuestFn original)
#                               (the recompiled original stays available as f_<addr>_orig)
OVERRIDES = {
    'restore_core_regs': 'rt_restore_core_regs',  # C++ EH: jump to a landing pad (runtime/eh.cpp)
    '_ZN12EventManager9raiseSyncERK6IEvent': ('wrap', 'pc_hook_raise_sync'),  # host/hooks.cpp
    '_ZN14CPlayerControl15UpdateFromInputEi': ('wrap', 'pc_hook_player_update'),  # mouse look
    '_ZN14CPlayerControl19EnablePlayerControlEb': ('wrap', 'pc_hook_enable_control'),
    '_ZN20CPlayerJeepComponent17UpdateFromPhysicsEi': ('wrap', 'pc_hook_jeep_physics'),  # look on a vehicle
    '_ZN30CPlayerLimitedControlComponent6UpdateEi': ('wrap', 'pc_hook_limited_control'),  # look while hanging
    '_ZN9GS_InGame6UpdateEv': ('wrap', 'pc_hook_ingame_update'),  # gameplay mode (host/hooks.cpp)
    # hide the touch controls (each control scheme draws its own)
    '_ZN13ControlScheme4DrawEi': ('wrap', 'pc_hook_scheme_draw'),
    '_ZN14ControlScheme14DrawEi': ('wrap', 'pc_hook_scheme_draw'),
    '_ZN14ControlScheme24DrawEi': ('wrap', 'pc_hook_scheme_draw'),
    '_ZN14ControlScheme34DrawEi': ('wrap', 'pc_hook_scheme_draw'),
    '_ZN14ControlScheme44DrawEi': ('wrap', 'pc_hook_scheme_draw'),
    '_ZN14ControlScheme54DrawEi': ('wrap', 'pc_hook_scheme_draw'),
    '_ZN14ControlScheme74DrawEi': ('wrap', 'pc_hook_scheme_draw'),
    '_ZN11CQTEManager4DrawEv': ('wrap', 'pc_hook_qte_draw'),  # no touch prompts for QTEs
    '_ZN21CLevelInterfaceNormal4DrawEv': ('wrap', 'pc_hook_iface_draw'),  # no touch marker (crystals, arm console)
    '_ZN15GS_FaneMinigame6UpdateEv': ('wrap', 'pc_hook_fane_update'),  # the laser puzzle: keys / pad cursor
    '_ZN15GS_FaneMinigame10RenderBallEv': ('wrap', 'pc_hook_fane_render'),
    '_ZN4CHud14SetButtonStateEib': ('wrap', 'pc_hook_hud_button_state'),  # no pause button
    # physics on the game thread (host/hooks.cpp: the game's physics thread raced the game thread)
    '_ZN13DeviceOptions19SetUsePhysicsThreadEb': ('wrap', 'pc_hook_use_physics_thread'),
    # controller rumble (host/hooks.cpp): the player's shots, hits taken, camera / HUD shakes
    '_ZN16CWeaponComponent6DoFireEb': ('wrap', 'pc_hook_weapon_fire'),
    '_ZN15PlayerComponent13ReceiveDamageERK7CDamage': ('wrap', 'pc_hook_player_damage'),  # (3DS: less damage)
    # the campaign made easier on the 3DS (host/hooks.cpp): the enemies' accuracy; auto-aim
    '_ZN19CNPCWeaponComponent4FireERKN6glitch4core8vector3dIfEEP11CGameObjectiS5_S7_bb': ('wrap', 'pc_hook_npc_fire'),
    '_ZN14CWeaponManager15GetAutoAimCoefsEv': ('wrap', 'pc_hook_auto_aim_coefs'),
    '_ZN11CGameCamera11ShakeCameraEif': ('wrap', 'pc_hook_camera_shake'),
    '_ZN4CHud10StartShakeEif': ('wrap', 'pc_hook_camera_shake'),
    # native hot paths (host/native_hot.cpp; NOVA3_NATIVE_CHECK compares them with the originals,
    # NOVA3_NATIVE_OFF falls back): skinning (the wrapper enables the NEON branch, whose kernels
    # are these native versions), particles, the TTF glyph map
    '_ZN6glitch7collada6detail22CSoftwareSkinTechnique4skinEjRNS0_11SSkinBufferEPNS_5scene11CMeshBufferE': ('wrap', 'pc_native_skin'),
    '_ZN6glitch7collada6detail9neonSkinPEiPKfPKSt4pairIftEiS3_iS3_': 'pc_native_skin_p',
    '_ZN6glitch7collada6detail10neonSkinPNEiPKfPKSt4pairIftEiS3_iS3_iS3_iS3_': 'pc_native_skin_pn',
    '_ZN6glitch7collada6detail12neonSkinPNTBEiPKfPKSt4pairIftEiS3_iS3_iS3_iS3_iS3_iS3_iS3_iS3_': 'pc_native_skin_pntb',
    '_ZN6glitch7collada6detail25neonSkinPNTB_stride_optimEiPKfPKSt4pairIftEiS3_S3_S3_S3_iS3_S3_S3_S3_': 'pc_native_skin_pntb_so',
    '_ZN5spark9CParticle6UpdateEf': ('wrap', 'pc_native_particle_update'),
    '_ZN5spark13CPSAnim_Color7ComputeEfRN6glitch4core8vector4dIfEES5_': ('wrap', 'pc_native_anim_color'),
    '_ZN5spark12CPSAnim_Size7ComputeEfRN6glitch4core8vector2dIfEES5_': ('wrap', 'pc_native_anim_size'),
    '_ZN5spark11CPSAnim_Dir7ComputeEfRN6glitch4core8vector3dIfEES5_': ('wrap', 'pc_native_anim_dir'),
    '_ZNK8CTTFFont16CTTFGlyphWrapper8IsCachedEi': ('wrap', 'pc_native_glyph_is_cached'),
    '_ZNK8CTTFFont16CTTFGlyphWrapperixEi': ('wrap', 'pc_native_glyph_lookup'),
    '_ZN15VoxSoundManager14SetGlobalPitchEf': ('wrap', 'pc_hook_set_global_pitch'),  # slow motion at high fps
    '_ZN19CCinematicComponent17GetCinematicSpeedEv': ('wrap', 'pc_hook_cinematic_speed'),  # cinematic clock at high fps
    '_ZN11CVehicleCar25checkMoveOnPathCollisionsEi': 'pc_hook_vehicle_path_blocked',  # path cars at high fps
    '_ZNK11Application9GetStringEi': ('wrap', 'pc_hook_get_string'),  # tutorial prompts name the PC controls
    '_Z13Menu_GoToMenuP9lua_State': ('wrap', 'pc_hook_menu_goto'),  # title Controls -> the in-game menu's page
    '_ZN13GS_InGameMenu7PopPageEv': ('wrap', 'pc_hook_igm_pop_page'),
    '_ZN13GS_InGameMenu8PushPageE14InGameMenuPage': ('wrap', 'pc_hook_igm_push_page'),
    '_ZN13GS_InGameMenu15UpdatePausePageEv': ('wrap', 'pc_offline_pause_update'),
    '_ZN13GS_InGameMenu15CreatePausePageEv': ('wrap', 'pc_offline_create_pause_page'),
    '_ZN13GS_InGameMenu6UpdateEv': ('wrap', 'pc_offline_igm_update'),
    '_ZN13GS_InGameMenu18CreateControlsPageEv': ('wrap', 'pc_hook_igm_create_controls'),
    '_ZN13GS_InGameMenu18UpdateControlsPageEv': ('wrap', 'pc_hook_igm_update_controls'),
    '_ZN13GS_InGameMenu23UpdateControlSchemePageEv': ('wrap', 'pc_hook_igm_update_bindings'),
    '_ZN13GS_InGameMenu23RenderControlSchemePageEv': ('wrap', 'pc_hook_igm_render_bindings'),
    '_ZN13GS_InGameMenu17CreateOptionsPageEv': ('wrap', 'pc_hook_igm_create_options'),
    '_ZN13GS_InGameMenu7ReleaseEv': ('wrap', 'pc_hook_igm_release'),
    '_ZN12CMenuManager6RenderEv': ('wrap', 'pc_hook_title_render'),  # its text under the Controls page
    'HasGyroscope': ('wrap', 'pc_hook_has_gyroscope'),
    '_ZN14XPlayerManager11GetUsernameEv': ('wrap', 'pc_offline_username'),  # [multiplayer] nickname
    '_ZN12CMenuButtons7PressedEi': ('wrap', 'pc_offline_rate_pressed'),  # no "rate this game" popup
    '_ZN14GS_IGMRateGame6RenderEv': ('wrap', 'pc_offline_rate_render'),
    '_ZN16CUpgradesManager16GetUpgradesCountEv': ('wrap', 'pc_hook_upgrades_count'),  # (no level on the title screen)
    '_ZN13GS_InGameMenu20CreateObjectivesPageEv': ('wrap', 'pc_hook_igm_objectives_page'),
    '_ZN12CMenuManager7onEventEPK6IEventPK12EventManager': ('wrap', 'pc_hook_title_menu_event'),
    '_ZN11CMenuScreen21ProcessAccessoryInputERK20AccessoryInputStatus': ('wrap', 'pc_hook_title_menu_event'),
    # event log (host/eventlog.cpp): game console, trigger scripts, checkpoints, spawns, teleports
    '_ZN3glf7Console8PrintRawEPKc': ('wrap', 'pc_log_console'),
    '_ZN17CLuaScriptManager10GetFuncRefEPKc': ('wrap', 'pc_log_func_ref'),
    '_ZN17CLuaScriptManager13StartFunctionEiiP11ScriptParami': ('wrap', 'pc_log_start_function'),
    '_ZN17CLuaScriptManager21StartFunctionOnThreadEiiP11ScriptParami': ('wrap', 'pc_log_start_function'),
    '_ZN17CLuaScriptManager12CallFunctionEPKciP11ScriptParam': ('wrap', 'pc_log_call_function'),
    '_ZN11Application14SaveCheckPointEv': ('wrap', 'pc_log_save_checkpoint'),
    '_ZN11Application14LoadCheckPointEb': ('wrap', 'pc_log_load_checkpoint'),
    '_ZN11Application12SaveCampaignEPKcib': ('wrap', 'pc_log_save_campaign'),
    '_ZN11Application12LoadCampaignEv': ('wrap', 'pc_log_load_campaign'),
    '_ZN11Application24SaveCampaignLevelSectionEi': ('wrap', 'pc_log_save_section'),
    '_ZN6CLevel20SkipToNextCheckpointEv': ('wrap', 'pc_log_skip_checkpoint'),
    '_ZN6CLevel11RespawnInitEv': ('wrap', 'pc_log_respawn_init'),
    '_ZN15PlayerComponent11SpawnPlayerEv': ('wrap', 'pc_log_spawn_player'),
    '_ZN15PlayerComponent7RespawnEv': ('wrap', 'pc_log_respawn_player'),
    '_ZN15PlayerComponent8SaveGameEi': ('wrap', 'pc_log_player_save'),
    '_ZN15PlayerComponent8SaveGameEP17CSpawnPointObject': ('wrap', 'pc_log_player_save'),
    '_ZN15PlayerComponent13TeleportResetEv': ('wrap', 'pc_log_teleport_reset'),
    '_ZN13CZonesManager14TeleportInZoneEP5CZone': ('wrap', 'pc_log_teleport_zone'),
    '_Z20TeleportPlayerToZoneP9lua_State': ('wrap', 'pc_log_lua_teleport_zone'),
    '_Z18TeleportToWaypointP9lua_State': ('wrap', 'pc_log_lua_teleport_wp'),
    '_Z17LoadLevelSectionsP9lua_State': ('wrap', 'pc_log_lua_load_sections'),
    '_ZN6CLevel14StartCinematicERKN6glitch4core8vector3dIfEE18ECinematicSkipModeb': ('wrap', 'pc_log_cinematic_start'),
    '_ZN6CLevel12EndCinematicEv': ('wrap', 'pc_log_cinematic_end'),
    '_ZN6CLevel17UpdateMCSavedDataEv': ('wrap', 'pc_hook_section_restore'),  # fall-through after a section load
    # tutorials: the dialog stays, the highlight over the touch control goes (host/hooks.cpp)
    '_ZN20CLevelTutorialRotate4DrawEv': ('wrap', 'pc_hook_tutorial_draw'),
    '_ZN22CLevelTutorialDetonate4DrawEv': ('wrap', 'pc_hook_tutorial_draw'),
    '_ZN23CLevelTutorialIronSight4DrawEv': ('wrap', 'pc_hook_tutorial_draw'),
    '_ZN18CLevelTutorialJump4DrawEv': ('wrap', 'pc_hook_tutorial_draw'),
    '_ZN18CLevelTutorialMove4DrawEv': ('wrap', 'pc_hook_tutorial_draw'),
    '_ZN28CLevelTutorialJeepMachineGun4DrawEv': ('wrap', 'pc_hook_tutorial_draw'),
    '_ZN24CLevelTutorialJeepRocket4DrawEv': ('wrap', 'pc_hook_tutorial_draw'),
    '_ZN28CLevelTutorialMechMachineGun4DrawEv': ('wrap', 'pc_hook_tutorial_draw'),
    '_ZN24CLevelTutorialMechRocket4DrawEv': ('wrap', 'pc_hook_tutorial_draw'),
    '_ZN26CLevelTutorialSpecialPower4DrawEv': ('wrap', 'pc_hook_tutorial_draw_3c'),
    '_ZN18CLevelTutorialRush4DrawEv': ('wrap', 'pc_hook_tutorial_draw_3c'),
    '_ZN9SocialMgr12ShowInputAgeEv': ('wrap', 'pc_hook_show_input_age'),  # no age gate
    '_ZN15PlayerComponent22ComputeControlVelocityEi': ('wrap', 'pc_hook_control_velocity'),  # strafe while sprinting
    '_ZN6glitch5scene16CCameraSceneNode6setFOVEf': ('wrap', 'pc_hook_set_fov'),  # field of view setting
    '_ZNK6glitch5scene16CCameraSceneNode6getFOVEv': ('wrap', 'pc_hook_get_fov'),  # (the game reads back its own value)
    # menus on screens like the 3DS's 400x240: the 960x640 screen class (host/hooks.cpp)
    '_Z12Is480x320Resv': 'pc_hook_is_480x320_res',
    '_Z12Is960x640Resv': 'pc_hook_is_960x640_res',
    '_ZN11Application11GetFontSizeEiib': ('wrap', 'pc_hook_font_size'),
    '_ZN11CTTFFontSpr25GetOffsetForOutlinedGlyphEii': ('wrap', 'pc_hook_font_size'),
    '_ZN11CTTFFontSpr29GetBorderSizeForOutlinedGlyphEv': ('wrap', 'pc_hook_font_size'),
}

# Dead online services: SocialMgr's Facebook / Google+ / Game Center layer (login, friends,
# invites, online leaderboards and achievements, the "+1" button, and the per-frame pump of
# the sociallib request queue). The servers are gone; each method does nothing and returns
# 0 (false / not logged in). Kept: construction, uids and the age (saved with the settings).
SOCIAL_DISABLED = [
    '_ZN9SocialMgr6UpdateEv',
    '_ZN9SocialMgr5LoginEN9sociallib13ClientSNSEnumE',
    '_ZN9SocialMgr6LogoutEN9sociallib13ClientSNSEnumE',
    '_ZN9SocialMgr10IsLoggedInEN9sociallib13ClientSNSEnumE',
    '_ZN9SocialMgr10GetFriendsEN9sociallib13ClientSNSEnumE',
    '_ZN9SocialMgr13InviteFriendsEv',
    '_ZN9SocialMgr19InviteFriendsGoogleEv',
    '_ZN9SocialMgr17GCShowLeaderboardEi',
    '_ZN9SocialMgr11GCPostScoreEii',
    '_ZN9SocialMgr17GCShowAchievementEv',
    '_ZN9SocialMgr17GCPostAchievementESsb',
    '_ZN9SocialMgr21GCIncreaseAchievementESsi',
    '_ZN9SocialMgr17ShowPlusOneButtonERKSsiiii',
    '_ZN9SocialMgr17ShowPlusOneInRateERKSsii',
    '_ZN9SocialMgr17HidePlusOneButtonEv',
    '_ZN9SocialMgr24IsNeedGooglePlusLocationEv',
]
OVERRIDES.update({name: 'pc_disabled_service' for name in SOCIAL_DISABLED})

# Offline game (host/offline.cpp): store, social, login, ads, tracking and online-only menu
# functions do nothing; the menu buttons that lead to them are hidden.
OFFLINE_STUBS = [
    # an analytics event with values read from the APK (oc::ocJ: sizes / checksums of its
    # entries); the only use of the APK, which a packaged copy leaves out
    '_ZN11Application21Tracking_AddEventZoroEv',
    # menu script functions (Lua C functions: returning 0 = no results)
    '_Z21Menu_ShowInterstitialP9lua_State',
    '_Z22Menu_SendTrackingEventP9lua_State',
    '_Z17Menu_GoToAppStoreP9lua_State',
    '_Z22Menu_EnterCustomerCareP9lua_State',
    '_Z23Menu_EnterPrivacyPolicyP9lua_State',
    '_Z24Menu_ClickButtonFacebookP9lua_State',
    '_Z22Menu_ClickButtonGoogleP9lua_State',
    '_Z23Menu_UpdateFacebookTextP9lua_State',
    '_Z21Menu_UpdateGoogleTextP9lua_State',
    '_Z12Menu_loginFBP9lua_State',
    '_Z22Menu_EnterCFAOnlineNECP9lua_State',
    '_Z24Menu_Go_To_Online_ArmoryP9lua_State',
    '_Z38Menu_RequestSeshatPutPurchasesIfNeededP9lua_State',
    '_Z17Menu_UploadCheatsP9lua_State',
    '_Z17Menu_SocialRewardP9lua_State',
    '_Z20Menu_ToggleVoiceChatP9lua_State',
    '_Z20Menu_AddFriendByNameP9lua_State',
    '_Z10Menu_SetPNP9lua_State',
    '_Z16Menu_BTAvailableP9lua_State',  # nil: no Bluetooth multiplayer
    # credits and rewards from outside play: the store, video and login / welcome-back rewards,
    # the cheat that adds credits, the login text field (the earned credits stay)
    '_Z12Menu_ShowCFAP9lua_State',
    '_Z16Menu_AddCurrencyP9lua_State',
    '_Z15Menu_GetRewardsP9lua_State',
    '_Z20Menu_IsOfflineRewardP9lua_State',
    '_Z24Menu_GetFirstLoginRewardP9lua_State',
    '_Z25Menu_GetStringLoginRewardP9lua_State',
    '_Z27Menu_ConfirmReceiveGGRewardP9lua_State',
    '_Z27Menu_ConfirmReceiveWBRewardP9lua_State',
    '_Z22Menu_GetCoinPacksCountP9lua_State',
    '_Z16Menu_IsAutoLoginP9lua_State',
    '_Z26Menu_AndroidLoginTextFieldP9lua_State',
    # Gameloft ads (banners, full-screen ads)
    '_ZN13WGLAdsManager9InitGLadsEv',
    '_ZN13WGLAdsManager10ShowBannerEPKc',
    '_ZN13WGLAdsManager10ShowBannerEPKcii8AD_ALIGN',
    '_ZN13WGLAdsManager16ShowFullScreenAdEPKcb',
    '_ZN13WGLAdsManager16CheckAdAvailableEPKc',
    # AdsManager: banners, interstitials, video ads
    '_ZN10AdsManager8Show_AdsEi',
    '_ZN10AdsManager8Show_AdsEiPKc',
    '_ZN10AdsManager10ShowBannerEv',
    '_ZN10AdsManager16ShowInterstitialEv',
    '_ZN10AdsManager19ShowInterstitialCFAEv',
    '_ZN10AdsManager21CheckShowInterstitialEv',
    '_ZN10AdsManager25BeginCountDownInterstitalEv',
    '_ZN10AdsManager9InitGLAdsEv',
    '_ZN10AdsManager6UpdateEv',
    '_ZN10AdsManager13UpdateDisplayEv',
    '_ZN10AdsManager18UpdateInterstitialEj',
    # CFAMgr: credits for watching videos, the "earn extra credits" popup
    '_ZN6CFAMgr11ReceiveCoinEv',
    '_ZN6CFAMgr12ReceiveBonusEv',
    '_ZN6CFAMgr12ShowCFAPopupEv',
    '_ZN6CFAMgr11ShowLoadingEb',
    '_ZN6CFAMgr13RequestFailedEv',
    '_ZN6CFAMgr18NoContentAvailableEv',
    '_ZN13GS_InGameMenu12ShowCFAPopupEv',
    # Gaia (Gameloft's online services: login, profile sync, ban checks, "update the game",
    # and the trigger of the popup above), the online prices and invites, the server config
    '_ZN7GaiaMgr4InitEv',
    '_ZN7GaiaMgr5LoginEN4gaia18BaseServiceManager11CredentialsESsSs',
    '_ZN7GaiaMgr9CheckInitEv',
    '_ZN7GaiaMgr17InitOnlineFeatureEv',
    '_ZN7GaiaMgr14CheckBanStatusEv',
    '_ZN7GaiaMgr14ShowBanMessageEv',
    '_ZN7GaiaMgr16UpdateDeviceInfoEv',
    '_ZN7GaiaMgr6UpdateEv',
    '_ZN7GaiaMgr14NeedShowUpdateEv',
    '_ZN7GaiaMgr17CheckUpdateStatusEv',
    '_ZN7GaiaMgr14RefreshProfileEv',
    '_ZN17FederationManager6UpdateEv',
    '_ZN17FederationManager24RetrieveOfflineItemPriceEv',
    '_ZN17FederationManager14HandleInvitePNEv',
    '_ZN17FederationManager12SetAuthParamEv',
    '_ZN14XPlayerManager18UpdateServerConfigEv',
]
OVERRIDES.update({name: 'pc_disabled_service' for name in OFFLINE_STUBS})
OVERRIDES.update({
    '_ZN12CMenuManager4LoadEP13CMemoryStream': ('wrap', 'pc_offline_menus_loaded'),
    '_ZN12CMenuManager18ShowMenuObjectByIdEib': ('wrap', 'pc_offline_show_object'),
    '_ZN12CMenuManager20EnableMenuObjectByIdEib': ('wrap', 'pc_offline_show_object'),
    '_Z14Menu_PushStateP9lua_State': ('wrap', 'pc_offline_push_state'),
    '_ZN13GS_InGameMenu15RenderPausePageEv': ('wrap', 'pc_offline_pause_page'),
    '_ZN11CTTFFontSpr10DrawStringEPKciiihiiS1_b': ('wrap', 'pc_offline_draw_string'),
    '_ZN11CTTFFontSpr20DrawStringLimitWidthEPKciiiihiiS1_': ('wrap', 'pc_offline_draw_string'),
    '_ZN11CSpriteFont10DrawStringEPKciiihiiS1_b': ('wrap', 'pc_offline_draw_string'),
    '_ZN11CSpriteFont20DrawStringLimitWidthEPKciiiihiiS1_': ('wrap', 'pc_offline_draw_string'),
    '_ZNK7CSprite11PaintModuleEiffjha': ('wrap', 'pc_offline_paint_module'),
    '_ZN11CMenuSprite6RenderEv': ('wrap', 'pc_offline_screen_sprite'),
    '_ZN13GameMpManager19LoadUnlockTableDataEPvi': ('wrap', 'pc_offline_unlock_tables'),  # armory: all owned
    '_ZN13GameMpManager16LoadPurchaseDataEv': ('wrap', 'pc_offline_unlock_purchases'),
})

COND = ['z', '!z', 'c', '!c', 'n', '!n', 'v', '!v', '(c && !z)', '(!c || z)', '(n == v)', '(n != v)',
        '(!z && n == v)', '(z || n != v)', None, None]
PC_VAR = re.compile(r'(?<![\w>.])pc(?!\w)')
REG = ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12', 'sp', 'lr', 'pc']


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Image:
    """ELF + symbol/relocation facts the generator needs."""

    def __init__(self):
        self.e = e = load()
        self.text_addr, self.text = e.sec('.text')
        self.text_end = self.text_addr + len(self.text)
        self.plt_addr, plt = e.sec('.plt')
        self.syms = {}
        self.sym_by_name = {}
        dynsym = list(e.symbols())
        for n, v, s, t, b, shn in dynsym:
            if shn and n and t == 2:
                self.syms.setdefault(v, n)
                self.sym_by_name[n] = v
        # PLT entry i (20-byte header, 12-byte entries) <-> .rel.plt entry i
        _, relplt = e.sec('.rel.plt')
        self.plt = {}
        for i in range(len(relplt) // 8):
            _, info = struct.unpack_from('<II', relplt, i * 8)
            self.plt[self.plt_addr + 20 + 12 * i] = dynsym[info >> 8][0]
        self.imports = sorted(set(self.plt.values()))
        # function pointers stored in data (vtables, callbacks)
        _, reldyn = e.sec('.rel.dyn')
        self.ptr_targets = set()
        for i in range(len(reldyn) // 8):
            off, info = struct.unpack_from('<II', reldyn, i * 8)
            if info & 0xff == 23:
                v = e.u32(off)
                if self.text_addr <= v < self.text_end:
                    self.ptr_targets.add(v)
            elif info & 0xff == 2:
                n, v, *_ = dynsym[info >> 8]
                if self.text_addr <= v < self.text_end:
                    self.ptr_targets.add(v)

    def word(self, a):
        return struct.unpack_from('<I', self.text, a - self.text_addr)[0]

    def in_text(self, a):
        return self.text_addr <= a < self.text_end

    def landing_pads(self):
        """{function start: set(landing pad addresses)} from the C++ LSDAs in .ARM.extab."""
        a, ex = self.e.sec('.ARM.exidx')
        out = collections.defaultdict(set)

        def prel31(addr):
            return (addr + sext(self.e.u32(addr) & 0x7fffffff, 31)) & 0xffffffff

        def uleb(d, o):
            r = sh = 0
            while True:
                b = d[o]
                o += 1
                r |= (b & 0x7f) << sh
                sh += 7
                if not b & 0x80:
                    return r, o

        pers = self.sym_by_name.get('__gxx_personality_v0', GXX_PERSONALITY)
        for i in range(len(ex) // 8):
            ent = a + i * 8
            w1 = self.e.u32(ent + 4)
            if w1 == 1 or w1 & 0x80000000:
                continue
            fn, eht = prel31(ent), prel31(ent + 4)
            if self.e.u32(eht) & 0x80000000:
                continue
            if pers is not None and prel31(eht) != pers:
                continue
            lsda = eht + 8 + 4 * ((self.e.u32(eht + 4) >> 24) & 0xff)
            d = self.e.read(lsda, 4096) or b''
            try:
                o = 0
                if d[o] != 0xff:
                    continue  # explicit LPStart: not used by this binary
                o += 1
                tt = d[o]
                o += 1
                if tt != 0xff:
                    _, o = uleb(d, o)
                if d[o] != 0x01:  # call-site table encoding: uleb128
                    continue
                cs_len, o = uleb(d, o + 1)
                end = o + cs_len
                while o < end:
                    _, o = uleb(d, o)
                    _, o = uleb(d, o)
                    lp, o = uleb(d, o)
                    _, o = uleb(d, o)
                    if lp:
                        out[fn & ~1].add((fn & ~1) + lp)
            except IndexError:
                continue
        return out

    def exidx_starts(self):
        a, ex = self.e.sec('.ARM.exidx')
        out = set()
        for i in range(len(ex) // 8):
            w0, = struct.unpack_from('<I', ex, i * 8)
            off = sext(w0 & 0x7fffffff, 31)
            out.add((a + i * 8 + off) & 0xffffffff)
        return out


# ----------------------------------------------------------------------------
# Control-flow facts for one instruction (used by discovery and generation).

def branch_info(w, addr):
    """Return (kind, target) for direct branches: kind in {'b','bl','blx'} or None."""
    cond = w >> 28
    if (w >> 25) & 7 == 5:
        tgt = (addr + 8 + (sext(w & 0xffffff, 24) << 2)) & 0xffffffff
        if cond == 0xf:
            return 'blx', tgt | 1 | ((w >> 23) & 2)
        return ('bl' if w & (1 << 24) else 'b'), tgt
    return None, None


def writes_pc_terminal(w):
    """True if the instruction unconditionally transfers control (no fall-through)."""
    cond = w >> 28
    if cond != 0xe:
        return False
    op = (w >> 25) & 7
    if op == 5 and not w & (1 << 24):  # b
        return True
    if w & 0x0ffffff0 == 0x012fff10:  # bx
        return True
    if op == 4 and w & (1 << 20) and w & (1 << 15):  # ldm with pc
        return True
    if op in (2, 3) and w & (1 << 20) and (w >> 12) & 15 == 15 and not (op == 3 and w & 16):  # ldr pc
        return True
    if op in (0, 1) and (w >> 12) & 15 == 15 and (w >> 21) & 0xf not in (8, 9, 10, 11) and not (op == 0 and w & 0x90 == 0x90):
        return True  # data-processing writing pc
    return False


# ----------------------------------------------------------------------------

class Analysis:
    def __init__(self, img):
        self.img = img

    def discover(self):
        img = self.img
        starts = set()
        thumb = set()
        for a, n in img.syms.items():
            if img.in_text(a & ~1):
                (thumb if a & 1 else starts).add(a & ~1)
        for a in img.exidx_starts():
            if not img.in_text(a) or a in starts:
                continue
            # Unwind entries don't always flag Thumb code. Compiled ARM functions begin with an
            # unconditional instruction; a Thumb `push {...}` there means Thumb.
            h = struct.unpack_from('<H', img.text, a - img.text_addr)[0]
            if img.word(a) >> 28 not in (0xe, 0xf) and h & 0xfe00 == 0xb400:
                thumb.add(a)
            else:
                starts.add(a)
        for p in img.ptr_targets:
            if not p & 1:
                starts.add(p)
                continue
            # An odd pointer is a Thumb entry only if it starts like a function (push {...});
            # other odd values pointing into .text are data.
            h = struct.unpack_from('<H', img.text, (p & ~1) - img.text_addr)[0]
            if h & 0xfe00 == 0xb400:
                thumb.add(p & ~1)
        starts -= thumb
        # BL targets are function starts; also gather B targets for later entry analysis
        md_words = img.text
        for off in range(0, len(md_words), 4):
            w, = struct.unpack_from('<I', md_words, off)
            k, t = branch_info(w, img.text_addr + off)
            if k == 'bl' and img.in_text(t):
                starts.add(t)
        starts = sorted(s for s in starts if s % 4 == 0)
        self.starts = starts
        self.thumb = sorted(thumb)
        return starts


# ----------------------------------------------------------------------------
# Code generation

class Unsupported(Exception):
    pass


class FuncGen:
    def __init__(self, g, start, end):
        self.g, self.img = g, g.img
        self.start, self.end = start, end
        self.out = []
        self.labels = set()
        self.reach = {}
        self.unsupported = collections.Counter()
        self.extra_entries = set()

    # --- helpers -------------------------------------------------------------
    def r(self, n, addr, pc_off=8):
        """Read register n as an expression (pc reads are constants)."""
        if n == 15:
            return f'0x{(LOAD_BASE + addr + pc_off) & 0xffffffff:08x}u'
        return REG[n]

    def emit(self, s):
        self.out.append(s)

    def call_direct(self, target, ret_addr, tail=False):
        """Code for calling guest address `target` (link-time) and returning to ret_addr."""
        img = self.img
        name = img.plt.get(target)
        ret = f'0x{LOAD_BASE + ret_addr:08x}u'
        pre = '' if tail else f'lr = {ret}; '
        if name == 'setjmp':  # returns twice: must be expanded in the caller's own frame
            call = 'GUEST_SETJMP()'
        elif name in INLINE_IMPORTS and getattr(self.g, 'inline_imports', True):
            call = INLINE_IMPORTS[name] + (' RET' if tail else '')
        elif name is not None:
            # host imports always return to lr; a tail-called import returns for us
            call = f'IMPORT({self.g.import_index[name]}) /* {name} */' + ('; RET' if tail else '')
        elif target in self.g.inline_helpers and not tail:
            call = self.g.inline_helpers[target]
        else:
            if target in self.g.emit_set:
                fn = f'f_{target:08x}'
            elif img.in_text(target) and self.g.func_of(target) in self.g.emit_set:
                self.g.mid_entries.add(target)
                fn = f'e_{target:08x}'
            else:
                fn = None
            if fn:
                call = f'TAIL_CALL({fn})' if tail else f'CALL_RET({fn}, {ret})'
            else:
                t = f'0x{LOAD_BASE + target:08x}u'
                call = f'TAIL_CALLI({t})' if tail else f'CALLI_RET({t}, {ret})'
        return pre + call

    def dispatch_tail(self, target):
        """Branch to an address outside this function: tail call."""
        return f'{{ {self.call_direct(target, 0, tail=True)}; }}'

    def goto(self, target):
        if self.start <= target < self.end and target in self.reach:
            self.labels.add(target)
            return f'goto L_{target:08x};'
        return self.dispatch_tail(target)

    # --- discovery inside the function ----------------------------------------
    def explore(self):
        img = self.img
        work = [self.start] + sorted(self.g.entries_by_func.get(self.start, ()))
        while work:
            a = work.pop()
            while self.start <= a < self.end and a not in self.reach:
                w = img.word(a)
                self.reach[a] = w
                k, t = branch_info(w, a)
                cond = w >> 28
                if k == 'b':
                    if self.start <= t < self.end:
                        work.append(t)
                    if cond == 0xe:
                        break
                elif k in ('bl', 'blx'):
                    if k == 'bl' and img.plt.get(t) in NORETURN or img.syms.get(t) in NORETURN:
                        if cond == 0xe:
                            break
                tbl = self.jump_table(a, w)
                if tbl:
                    work.extend(x for x in tbl[1] if self.start <= x < self.end)
                    for i in range(len(tbl[1]) if tbl[0] == 'add' else 0):
                        pass
                    if tbl[0] == 'add':
                        # entries are themselves instructions at a+8+4i
                        work.extend(a + 8 + 4 * i for i in range(len(tbl[1])))
                if writes_pc_terminal(w):
                    break
                a += 4

    def jump_table(self, a, w):
        """Recognize `addCC pc, pc, rX, lsl #2` / `ldrCC pc, [pc, rX, lsl #2]` switch idioms.

        Returns (kind, [targets], index_reg) or None. Table size comes from the closest
        preceding `cmp rX, #imm`.
        """
        img = self.img
        is_add = w & 0x0ffffff0 == 0x008ff100  # add pc, pc, rm, lsl #2
        is_ldr = w & 0x0ffffff0 == 0x079ff100  # ldr pc, [pc, rm, lsl #2]
        if not (is_add or is_ldr):
            return None
        rm = w & 15
        n = None
        for back in range(1, 8):
            p = a - 4 * back
            if p < self.start:
                break
            pw = img.word(p)
            if pw & 0x0ff0f000 == 0x03500000 and (pw >> 16) & 15 == rm:  # cmp rm, #imm
                rot = (pw >> 8) & 15
                imm = pw & 0xff
                n = ((imm >> (2 * rot)) | (imm << (32 - 2 * rot))) & 0xffffffff if rot else imm
                break
        if n is None or n > 4096:
            return None
        if is_add:
            return 'add', [a + 8 + 4 * i for i in range(n + 1)], rm
        return 'ldr', [img.word(a + 8 + 4 * i) for i in range(n + 1)], rm

    # --- generation ------------------------------------------------------------
    def lower(self):
        """The function as C statements (FuncIR), plumbing still in the all-register macros;
        Flow rewrites them with the registers each site needs."""
        override = getattr(self.g, 'override_addrs', {}).get(self.start)
        wrap = None
        if isinstance(override, tuple):
            wrap, override = override[1], None
        if override:
            fx = FuncIR(self, [])
            fx.stub = (f'/* {fx.name or ""}: replaced by host {override} */\nvoid {override}(Cpu *cpu);\n'
                       f'void f_{self.start:08x}(Cpu *cpu, uint32_t entry) {{ (void)entry; {override}(cpu); cpu->ret_pc = cpu->r[14]; }}')
            return fx
        self.explore()
        stmts = []
        for a in sorted(self.reach):
            w = self.reach[a]
            self.out = []
            ok = False
            try:
                self.insn(a, w)
                if any(PC_VAR.search(x) for x in self.out):
                    raise Unsupported('pc as operand')
                ok = True
            except Unsupported as ex:
                self.out = []
                self.unsupported[str(ex)] += 1
                self.emit(f'UNIMPL(0x{LOAD_BASE + a:08x}u, 0x{w:08x}u); /* {ex} */')
            except (IndexError, KeyError):  # invalid/UNPREDICTABLE encoding (often data decoded as code)
                self.out = []
                self.unsupported['invalid encoding'] += 1
                self.emit(f'UNIMPL(0x{LOAD_BASE + a:08x}u, 0x{w:08x}u); /* invalid */')
            vop = arm_vop(w) if ok and len(self.out) == 1 else None
            pcv = LOAD_BASE + a + 8
            stmts += [(a, x, vop if k == 0 else None, pcv) for k, x in enumerate(self.out)]
            nxt = a + 4
            if nxt not in self.reach and not writes_pc_terminal(w):
                # fall-through leaves the function (into next function or data)
                if self.img.in_text(nxt) and nxt == self.end:
                    stmts.append((a, self.dispatch_tail(nxt), None, pcv))
                else:
                    stmts.append((a, 'RET;', None, pcv))
        stmts.append((None, 'RET;', None, 0))
        fx = FuncIR(self, stmts)
        fx.wrap = wrap
        return fx

    def generate(self):
        return render_old(self.lower())

    def cond_wrap(self, w, code):
        c = w >> 28
        if c == 0xe:
            return code
        return f'if ({COND[c]}) {{ {code} }}'

    def insn(self, a, w):
        cond = w >> 28
        if cond == 0xf:
            return self.uncond(a, w)
        op = (w >> 25) & 7
        if op == 5:
            k, t = branch_info(w, a)
            if k == 'bl':
                code = self.call_direct(t, a + 4) + ';'
            else:
                code = self.goto(t)
            self.emit(self.cond_wrap(w, code))
            return
        if op == 4:
            return self.ldm_stm(a, w)
        if op in (2, 3) and not (op == 3 and w & 16):
            return self.ldr_str(a, w)
        if op == 3:
            return self.media(a, w)
        if op in (0, 1):
            return self.dp_misc(a, w)
        if op in (6, 7):
            return self.coproc(a, w)
        raise Unsupported(f'op{op}')

    # unconditional space ---------------------------------------------------------
    def uncond(self, a, w):
        k, t = branch_info(w, a)
        if k == 'blx':
            self.emit(f'lr = 0x{LOAD_BASE + a + 4:08x}u; CALLI_RET(0x{LOAD_BASE + t:08x}u, 0x{LOAD_BASE + a + 4:08x}u);')
            return
        if w & 0xff70f000 == 0xf550f000 or w & 0xff70f010 == 0xf750f000 or w & 0xfd70f000 == 0xf510f000:
            self.emit('/* pld */')
            return
        if w & 0xfffffff0 == 0xf57ff050 or w & 0xfffffff0 == 0xf57ff040:
            self.emit('BARRIER();')
            return
        if w & 0xfffffff0 == 0xf57ff060:
            self.emit('/* isb */')
            return
        if w == 0xf57ff01f:
            self.emit('cpu->ex_addr = ~0u; /* clrex */')
            return
        raise Unsupported('uncond')

    # data-processing / misc ---------------------------------------------------------
    def shifter(self, a, w, need_carry):
        """Operand2 -> (value expr, carry expr or None). Emits temps as needed."""
        if w & (1 << 25):
            imm = w & 0xff
            rot = ((w >> 8) & 15) * 2
            v = ((imm >> rot) | (imm << (32 - rot))) & 0xffffffff if rot else imm
            carry = ('c' if rot == 0 else str(v >> 31))
            return f'0x{v:08x}u', carry
        rm = w & 15
        typ = (w >> 5) & 3
        if w & 16:  # register-shifted register
            rs = (w >> 8) & 15
            fn = ['SH_LSL_R', 'SH_LSR_R', 'SH_ASR_R', 'SH_ROR_R'][typ]
            return f'{fn}({self.r(rm, a, 12)}, {self.r(rs, a, 12)}, &sc)', 'sc'
        n = (w >> 7) & 31
        m = self.r(rm, a)
        if typ == 0:
            if n == 0:
                return m, 'c'
            return f'({m} << {n})', f'(({m} >> {32 - n}) & 1)'
        if typ == 1:
            if n == 0:
                return '0u', f'({m} >> 31)'
            return f'({m} >> {n})', f'(({m} >> {n - 1}) & 1)'
        if typ == 2:
            if n == 0:
                return f'(uint32_t)((int32_t){m} >> 31)', f'({m} >> 31)'
            return f'(uint32_t)((int32_t){m} >> {n})', f'(({m} >> {n - 1}) & 1)'
        if n == 0:
            return f'((c << 31) | ({m} >> 1))', f'({m} & 1)'
        return f'ROR({m}, {n})', f'(({m} >> {n - 1}) & 1)'

    def dp_misc(self, a, w):
        op1 = (w >> 20) & 0x1f
        if not w & (1 << 25):
            if w & 0x90 == 0x90 and (w >> 5) & 3 == 0:
                if op1 >> 4 == 0:
                    return self.multiply(a, w)
                if op1 >> 3 == 2 and not op1 & 0x4 and w & 0xf00 == 0xf00 and op1 & 0x18 == 0x10 and (w >> 20) & 0x1b == 0x10:
                    raise Unsupported('swp')
                if op1 >> 3 == 3 or (op1 >> 3 == 2 and False):
                    return self.sync(a, w)
                if op1 & 0x18 == 0x18:
                    return self.sync(a, w)
                raise Unsupported('mul/sync?')
            if w & 0x90 == 0x90:
                return self.extra_ldst(a, w)
            if op1 & 0x19 == 0x10:
                return self.misc(a, w)
        else:
            if op1 == 0x10:  # movw
                rd = (w >> 12) & 15
                v = ((w >> 4) & 0xf000) | (w & 0xfff)
                self.emit(self.cond_wrap(w, f'{REG[rd]} = 0x{v:x}u;'))
                return
            if op1 == 0x14:  # movt
                rd = (w >> 12) & 15
                v = ((w >> 4) & 0xf000) | (w & 0xfff)
                self.emit(self.cond_wrap(w, f'{REG[rd]} = ({REG[rd]} & 0xffffu) | 0x{v << 16:08x}u;'))
                return
            if op1 & 0x1b == 0x12:  # msr imm / hints
                if w & 0x0fffff00 == 0x0320f000:
                    self.emit('/* hint */')
                    return
                raise Unsupported('msr imm')
        return self.dataproc(a, w)

    def dataproc(self, a, w):
        opc = (w >> 21) & 15
        s = (w >> 20) & 1
        rn = (w >> 16) & 15
        rd = (w >> 12) & 15
        logical = opc in (0, 1, 8, 9, 12, 13, 14, 15)
        op2, carry = self.shifter(a, w, s and logical)
        n = self.r(rn, a, 12 if (w & 0x02000010) == 0x10 else 8)
        code = []
        if logical:
            expr = {0: f'{n} & {op2}', 1: f'{n} ^ {op2}', 8: f'{n} & {op2}', 9: f'{n} ^ {op2}',
                    12: f'{n} | {op2}', 13: f'{op2}', 14: f'{n} & ~{op2}', 15: f'~{op2}'}[opc]
            if s or opc in (8, 9):
                # an operand without shifter carry-out leaves C alone: not touching c keeps the
                # flags from looking live (register-flow analysis)
                pre = '{ uint32_t sc = c; (void)sc; ' if 'sc' in op2 else '{ '
                if carry == 'c':
                    code.append(f'{pre}uint32_t res = {expr};')
                    code.append('n = res >> 31; z = res == 0;')
                else:
                    code.append(f'{pre}uint32_t res = {expr}; uint32_t co = {carry};')
                    code.append('n = res >> 31; z = res == 0; c = co;')
                if opc not in (8, 9):
                    code.append(self.set_rd(rd, 'res', a, s))
                code.append('}')
            else:
                if 'sc' in op2:
                    code.append(f'{{ uint32_t sc; {self.set_rd(rd, expr, a, s)} }}')
                else:
                    code.append(self.set_rd(rd, expr, a, s))
        else:
            # arithmetic: compute as x + y + cin
            x, y, cin = {2: (n, f'~({op2})', '1'), 3: (op2, f'~{n}', '1'), 4: (n, op2, '0'),
                         5: (n, op2, 'c'), 6: (n, f'~({op2})', 'c'), 7: (op2, f'~{n}', 'c'),
                         10: (n, f'~({op2})', '1'), 11: (n, op2, '0')}[opc]
            # the shifter's carry-out is unused here (arithmetic sets C itself): sc need not
            # start as c, and reading c would make the flags look live
            pre = '{ uint32_t sc; (void)sc; ' if 'sc' in op2 else '{ '
            if s or opc in (10, 11):
                code.append(f'{pre}uint32_t xa = {x}, xb = {y}; uint64_t wide = (uint64_t)xa + xb + {cin}; uint32_t res = (uint32_t)wide;')
                code.append('n = res >> 31; z = res == 0; c = (uint32_t)(wide >> 32); v = ((xa ^ res) & (xb ^ res)) >> 31;')
                if opc not in (10, 11):
                    code.append(self.set_rd(rd, 'res', a, s))
                code.append('}')
            else:
                expr = {2: f'{n} - {op2}', 3: f'{op2} - {n}', 4: f'{n} + {op2}', 5: f'{n} + {op2} + c',
                        6: f'{n} - {op2} - !c', 7: f'{op2} - {n} - !c'}[opc]
                code.append(f'{pre}{self.set_rd(rd, expr, a, s)} }}')
        self.emit(self.cond_wrap(w, ' '.join(code)))

    def set_rd(self, rd, expr, a, s):
        if rd != 15:
            return f'{REG[rd]} = {expr};'
        if s:
            raise Unsupported('dp pc S (exception return)')
        w = self.img.word(a)
        tbl = self.jump_table(a, w)
        if tbl and tbl[0] == 'add':
            cases = ' '.join(f'case {i}: goto L_{t:08x};' for i, t in enumerate(tbl[1]))
            for t in tbl[1]:
                self.labels.add(t)
            return f'switch ({REG[tbl[2]]}) {{ {cases} }} UNREACHABLE(0x{LOAD_BASE + a:08x}u);'
        if w & 0x0fffffff == 0x01a0f00e:  # mov pc, lr
            return 'RET;'
        return f'{{ uint32_t tgt = {expr}; TAIL_CALLI(tgt); }}'

    def misc(self, a, w):
        if (w >> 7) & 1 and not (w >> 4) & 1:  # SMLA/SMLAW/SMULW/SMLAL/SMUL<x><y>
            return self.halfmul(a, w)
        op2 = (w >> 4) & 7
        op = (w >> 21) & 3
        rm = w & 15
        if op2 == 1 and op == 1:  # bx
            if rm == 14:
                self.emit(self.cond_wrap(w, 'RET;'))
            else:
                self.emit(self.cond_wrap(w, f'{{ TAIL_CALLI({REG[rm]}); }}'))
            return
        if op2 == 3 and op == 1:  # blx reg
            self.emit(self.cond_wrap(w, f'{{ uint32_t tgt = {REG[rm]}; lr = 0x{LOAD_BASE + a + 4:08x}u; CALLI_RET(tgt, 0x{LOAD_BASE + a + 4:08x}u); }}'))
            return
        if op2 == 1 and op == 3:  # clz
            rd = (w >> 12) & 15
            self.emit(self.cond_wrap(w, f'{REG[rd]} = CLZ({REG[rm]});'))
            return
        if op2 == 0 and op in (0, 2) and not w & (1 << 22):  # mrs rd, apsr
            if op == 0:
                rd = (w >> 12) & 15
                self.emit(self.cond_wrap(w, f'{REG[rd]} = (n << 31) | (z << 30) | (c << 29) | (v << 28);'))
                return
            if (w >> 16) & 15 in (8, 12):  # msr apsr_nzcvq, rm
                self.emit(self.cond_wrap(w, f'n = {REG[rm]} >> 31; z = ({REG[rm]} >> 30) & 1; c = ({REG[rm]} >> 29) & 1; v = ({REG[rm]} >> 28) & 1;'))
                return
        if op2 == 5:  # qadd/qsub/qdadd/qdsub
            rd, rn = (w >> 12) & 15, (w >> 16) & 15
            fn = ['QADD', 'QSUB', 'QDADD', 'QDSUB'][op]
            self.emit(self.cond_wrap(w, f'{REG[rd]} = {fn}({REG[rm]}, {REG[rn]});'))
            return
        if op2 == 7 and op == 1:
            self.emit('TRAP_BKPT();')
            return
        raise Unsupported('misc')

    def halfmul(self, a, w):
        op = (w >> 21) & 3
        rd, ra, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
        x, y = (w >> 5) & 1, (w >> 6) & 1
        hm = f'(int32_t)(int16_t)({REG[rm]} >> {16 * x})'
        hs = f'(int32_t)(int16_t)({REG[rs]} >> {16 * y})'
        if op == 3:
            self.emit(self.cond_wrap(w, f'{REG[rd]} = (uint32_t)({hm} * {hs});'))
        elif op == 0:
            self.emit(self.cond_wrap(w, f'{REG[rd]} = (uint32_t)({hm} * {hs}) + {REG[ra]};'))
        elif op == 1:
            prod = f'(uint32_t)(((int64_t)(int32_t){REG[rm]} * (int16_t)({REG[rs]} >> {16 * y})) >> 16)'
            self.emit(self.cond_wrap(w, f'{REG[rd]} = {prod}' + ('' if x else f' + {REG[ra]}') + ';'))
        else:
            self.emit(self.cond_wrap(w, f'{{ int64_t acc = (int64_t)(((uint64_t){REG[rd]} << 32) | {REG[ra]}) + (int64_t)({hm} * {hs}); {REG[ra]} = (uint32_t)acc; {REG[rd]} = (uint32_t)((uint64_t)acc >> 32); }}'))

    def multiply(self, a, w):
        op = (w >> 21) & 7
        s = (w >> 20) & 1
        rd, ra, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
        D, A, S, M = REG[rd], REG[ra], REG[rs], REG[rm]
        flags = ''
        if op == 0:
            code = f'{D} = {M} * {S};'
            flags = f' n = {D} >> 31; z = {D} == 0;'
        elif op == 1:
            code = f'{D} = {M} * {S} + {A};'
            flags = f' n = {D} >> 31; z = {D} == 0;'
        elif op == 3:
            code = f'{D} = {A} - {M} * {S};'
        elif op == 2:
            code = f'{{ uint64_t p = (uint64_t){M} * {S} + {A} + {D}; {A} = (uint32_t)p; {D} = (uint32_t)(p >> 32); }}'
        else:
            signed = op & 2
            acc = op & 1
            mul = f'(uint64_t)((int64_t)(int32_t){M} * (int32_t){S})' if signed else f'(uint64_t){M} * {S}'
            add = f' + (((uint64_t){D} << 32) | {A})' if acc else ''
            code = f'{{ uint64_t p = {mul}{add}; {A} = (uint32_t)p; {D} = (uint32_t)(p >> 32); }}'
            flags = f' n = {D} >> 31; z = ({D} | {A}) == 0;'
        self.emit(self.cond_wrap(w, code + (flags if s else '')))

    def sync(self, a, w):
        op = (w >> 21) & 3
        load = (w >> 20) & 1
        rn, rt, rm = (w >> 16) & 15, (w >> 12) & 15, w & 15
        if (w >> 23) & 1 == 0:
            raise Unsupported('swp')
        size = {0: 32, 1: 64, 2: 8, 3: 16}[op]
        N = REG[rn]
        if load:
            if size == 64:
                code = f'{{ uint64_t t = LD64({N}); {REG[rt]} = (uint32_t)t; {REG[rt + 1]} = (uint32_t)(t >> 32); cpu->ex_addr = {N}; cpu->ex_val = t; }}'
            else:
                code = f'{{ {REG[rt]} = LD{size}({N}); cpu->ex_addr = {N}; cpu->ex_val = {REG[rt]}; }}'
        else:
            rd, rt = rt, rm
            if size == 64:
                val = f'((uint64_t){REG[rt + 1]} << 32) | {REG[rt]}'
            else:
                val = REG[rt]
            code = f'{REG[rd]} = STREX{size}(cpu, {N}, {val});'
        self.emit(self.cond_wrap(w, code))

    def extra_ldst(self, a, w):
        p, u, i, wb, l = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, rt = (w >> 16) & 15, (w >> 12) & 15
        op2 = (w >> 5) & 3
        off = f'0x{((w >> 4) & 0xf0) | (w & 15):x}u' if i else REG[w & 15]
        base = self.r(rn, a)
        addr = f'{base} {"+" if u else "-"} {off}'
        ea = addr if p else base
        if op2 == 1:
            kind = 'ldrh' if l else 'strh'
        elif op2 == 2:
            kind = 'ldrsb' if l else 'ldrd'
        else:
            kind = 'ldrsh' if l else 'strd'
        T = REG[rt]
        if kind == 'ldrh':
            acc = f'{T} = LD16(ea);'
        elif kind == 'strh':
            acc = f'ST16(ea, {T});'
        elif kind == 'ldrsb':
            acc = f'{T} = (uint32_t)(int32_t)(int8_t)LD8(ea);'
        elif kind == 'ldrsh':
            acc = f'{T} = (uint32_t)(int32_t)(int16_t)LD16(ea);'
        elif kind == 'ldrd':
            acc = f'{T} = LD32(ea); {REG[rt + 1]} = LD32(ea + 4);'
        else:
            acc = f'ST32(ea, {T}); ST32(ea + 4, {REG[rt + 1]});'
        if rt == 15:
            raise Unsupported('extra ldst pc')
        wbcode = ''
        if not p or wb:
            if rn == 15:
                raise Unsupported('wb pc')
            wbcode = f' {REG[rn]} = {addr};'
        # Load into base register with writeback: loaded value wins (UNPREDICTABLE anyway).
        self.emit(self.cond_wrap(w, f'{{ uint32_t ea = {ea};{wbcode} {acc} }}' if l else f'{{ uint32_t ea = {ea}; {acc}{wbcode} }}'))

    def ldr_str(self, a, w):
        reg_off = (w >> 25) & 1
        p, u, b, wb, l = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, rt = (w >> 16) & 15, (w >> 12) & 15
        if reg_off:
            if (w >> 4) & 1:
                raise Unsupported('media in ldst')
            off, _ = self.shifter(a, w & ~(1 << 25), False)
        else:
            off = f'0x{w & 0xfff:x}u'
        # literal load: fold to a constant (no relocations touch .text)
        if rn == 15 and l and not reg_off and p and not wb and rt != 15:
            lit = a + 8 + ((w & 0xfff) if u else -(w & 0xfff))
            if self.img.in_text(lit):
                v = self.img.word(lit) if not b else self.img.text[lit - self.img.text_addr]
                self.emit(self.cond_wrap(w, f'{REG[rt]} = 0x{v:08x}u;'))
                return
        base = self.r(rn, a)
        addr = f'{base} {"+" if u else "-"} {off}'
        ea = addr if p else base
        wbcode = ''
        if not p or wb:
            wbcode = f' {REG[rn]} = {addr};'
        if l:
            ld = f'LD8(ea)' if b else 'LD32(ea)'
            if rt == 15:
                if b:
                    raise Unsupported('ldrb pc')
                tbl = self.jump_table(a, w)
                if tbl and tbl[0] == 'ldr':
                    cases = []
                    for i, t in enumerate(tbl[1]):
                        if self.start <= t < self.end and t in self.reach:
                            self.labels.add(t)
                            cases.append(f'case {i}: goto L_{t:08x};')
                        else:
                            cases.append(f'case {i}: {self.dispatch_tail(t)}')
                    self.emit(self.cond_wrap(w, f'switch ({REG[tbl[2]]}) {{ {" ".join(cases)} }} UNREACHABLE(0x{LOAD_BASE + a:08x}u);'))
                    return
                if rn == 13 and not p and u and not reg_off:  # ldr pc, [sp], #n: pop {pc} (+ discard)
                    self.emit(self.cond_wrap(w, f'{{ uint32_t rp = LD32(sp); sp += {w & 0xfff}u; RET_TO(rp); }}'))
                    return
                self.emit(self.cond_wrap(w, f'{{ uint32_t ea = {ea};{wbcode} uint32_t tgt = LD32(ea); TAIL_CALLI(tgt); }}'))
                return
            self.emit(self.cond_wrap(w, f'{{ uint32_t ea = {ea};{wbcode} {REG[rt]} = {ld}; }}'))
        else:
            val = self.r(rt, a, 12) if rt == 15 else REG[rt]
            st = f'ST8(ea, (uint8_t){val});' if b else f'ST32(ea, {val});'
            self.emit(self.cond_wrap(w, f'{{ uint32_t ea = {ea}; {st}{wbcode} }}'))

    def ldm_stm(self, a, w):
        p, u, s, wb, l = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn = (w >> 16) & 15
        regs = [i for i in range(16) if w & (1 << i)]
        if s:
            raise Unsupported('ldm/stm ^')
        cnt = len(regs)
        if u:
            start = '4' if p else '0'
        else:
            start = f'-{4 * cnt}' if p else f'-{4 * cnt - 4}'
        N = REG[rn]
        code = [f'uint32_t ea = {N} + (uint32_t)({start});']
        newbase = f'{N} + {4 * cnt}u' if u else f'{N} - {4 * cnt}u'
        ret = False
        if l:
            for k, r in enumerate(regs):
                if r == 15:
                    ret = True
                    code.append(f'uint32_t rp = LD32(ea + {4 * k});')
                    continue
                if r == rn and wb:
                    continue
                code.append(f'{REG[r]} = LD32(ea + {4 * k});')
            if wb and rn not in regs:
                code.append(f'{N} = {newbase};')
            elif wb:
                pass
            if ret:
                code.append('RET_TO(rp);')
        else:
            for k, r in enumerate(regs):
                val = self.r(r, a, 12) if r == 15 else REG[r]
                code.append(f'ST32(ea + {4 * k}, {val});')
            if wb:
                code.append(f'{N} = {newbase};')
        self.emit(self.cond_wrap(w, '{ ' + ' '.join(code) + ' }'))

    def media(self, a, w):
        op1 = (w >> 20) & 0x1f
        op2 = (w >> 5) & 7
        rd, rn, rm = (w >> 12) & 15, (w >> 16) & 15, w & 15
        D, N, M = REG[rd], REG[rn], REG[rm]
        if op1 & 0x18 == 0x08:  # pack/unpack/saturate/reverse
            rot = ((w >> 10) & 3) * 8
            src = f'ROR({M}, {rot})' if rot else M
            ext = {0x0a: ('(uint32_t)(int32_t)(int8_t)', 3), 0x0b: ('(uint32_t)(int32_t)(int16_t)', 3),
                   0x0e: ('(uint32_t)(uint8_t)', 3), 0x0f: ('(uint32_t)(uint16_t)', 3)}
            if op1 in ext and op2 == 3:
                cast = ext[op1][0]
                if rn == 15:
                    self.emit(self.cond_wrap(w, f'{D} = {cast}({src});'))
                else:
                    self.emit(self.cond_wrap(w, f'{D} = {N} + {cast}({src});'))
                return
            if op1 == 0x0c and op2 == 3:  # uxtb16
                body = f'({src}) & 0x00ff00ffu'
                self.emit(self.cond_wrap(w, f'{D} = {body};' if rn == 15 else f'{D} = (({N} & 0xffff0000u) + ({body} & 0xffff0000u)) | (({N} + {body}) & 0xffffu);'))
                return
            if op1 == 0x0b and op2 == 1:
                self.emit(self.cond_wrap(w, f'{D} = BSWAP32({M});'))
                return
            if op1 == 0x0b and op2 == 5:
                self.emit(self.cond_wrap(w, f'{D} = ((({M} >> 8) & 0x00ff00ffu) | (({M} << 8) & 0xff00ff00u));'))
                return
            if op1 == 0x0f and op2 == 5:
                self.emit(self.cond_wrap(w, f'{D} = (uint32_t)(int32_t)(int16_t)((({M} >> 8) & 0xff) | (({M} & 0xff) << 8));'))
                return
            if op1 == 0x0f and op2 == 1:
                self.emit(self.cond_wrap(w, f'{D} = RBIT({M});'))
                return
            if op1 == 0x08 and op2 & 1 == 0:  # pkhbt/pkhtb
                sh = (w >> 7) & 31
                if op2 & 2:
                    t = f'(uint32_t)((int32_t){M} >> {sh if sh else 31})'
                    self.emit(self.cond_wrap(w, f'{D} = ({N} & 0xffff0000u) | ({t} & 0xffffu);'))
                else:
                    self.emit(self.cond_wrap(w, f'{D} = ({N} & 0xffffu) | (({M} << {sh}) & 0xffff0000u);'))
                return
            if op1 & 0x1a == 0x0a and op2 & 1 == 0:  # ssat/usat
                sat = (rn) + (0 if op1 & 4 else 1)
                sh = (w >> 7) & 31
                src = f'(int32_t)({M} << {sh})' if not op2 & 2 else f'((int32_t){M} >> {sh if sh else 31})'
                fn = 'USAT' if op1 & 4 else 'SSAT'
                self.emit(self.cond_wrap(w, f'{D} = {fn}({src}, {sat});'))
                return
            if op1 == 0x08 and op2 == 5:  # sel
                raise Unsupported('sel')
        if op1 & 0x1e == 0x1a and op2 & 3 == 2:  # sbfx
            lsb, wid = (w >> 7) & 31, ((w >> 16) & 31) + 1
            self.emit(self.cond_wrap(w, f'{D} = (uint32_t)(((int32_t)({M} << {32 - lsb - wid})) >> {32 - wid});'))
            return
        if op1 & 0x1e == 0x1e and op2 & 3 == 2:  # ubfx
            lsb, wid = (w >> 7) & 31, ((w >> 16) & 31) + 1
            mask = (1 << wid) - 1
            self.emit(self.cond_wrap(w, f'{D} = ({M} >> {lsb}) & 0x{mask:x}u;'))
            return
        if op1 & 0x1e == 0x1c and op2 & 3 == 0:  # bfi/bfc
            lsb, msb = (w >> 7) & 31, (w >> 16) & 31
            if msb < lsb:
                raise Unsupported('bfi range')
            mask = ((1 << (msb - lsb + 1)) - 1) << lsb
            if rm == 15:
                self.emit(self.cond_wrap(w, f'{D} &= 0x{~mask & 0xffffffff:08x}u;'))
            else:
                self.emit(self.cond_wrap(w, f'{D} = ({D} & 0x{~mask & 0xffffffff:08x}u) | (({M} << {lsb}) & 0x{mask:08x}u);'))
            return
        if op1 == 0x18 and op2 == 0:  # usad8 / usada8
            raise Unsupported('usad8')
        if op1 & 0x18 == 0x10:  # signed multiplies, sdiv/udiv
            rd2, ra, rs = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15
            D2, A, S = REG[rd2], REG[ra], REG[rs]
            if op1 == 0x11 and op2 == 0:
                self.emit(self.cond_wrap(w, f'{D2} = SDIV({M}, {S});'))
                return
            if op1 == 0x13 and op2 == 0:
                self.emit(self.cond_wrap(w, f'{D2} = UDIV({M}, {S});'))
                return
            if op1 == 0x15 and op2 & 6 == 0:  # smmul/smmla (+r)
                rnd = ' + 0x80000000ll' if op2 & 1 else ''
                acc = f'((int64_t)(int32_t){A} << 32) + ' if ra != 15 else ''
                self.emit(self.cond_wrap(w, f'{D2} = (uint32_t)((uint64_t)({acc}(int64_t)(int32_t){M} * (int32_t){S}{rnd}) >> 32);'))
                return
            raise Unsupported('smul-media')
        if op1 == 0x1f and op2 == 7:
            self.emit(f'TRAP_UDF(0x{LOAD_BASE + a:08x}u);')
            return
        raise Unsupported(f'media {op1:#x}/{op2}')

    # coprocessor / VFP ------------------------------------------------------------
    @staticmethod
    def vreg(w, dbl, which):
        """which: 'd' (Vd,D bit22, 12), 'n' (Vn,N bit7, 16), 'm' (Vm,M bit5, 0)."""
        pos, bit = {'d': (12, 22), 'n': (16, 7), 'm': (0, 5)}[which]
        v = (w >> pos) & 15
        x = (w >> bit) & 1
        return (x << 4 | v) if dbl else (v << 1 | x)

    def coproc(self, a, w):
        cp = (w >> 8) & 15
        if cp not in (10, 11):
            if (w >> 24) & 15 == 15:
                raise Unsupported('svc')
            raise Unsupported(f'cp{cp}')
        dbl = cp == 11
        if (w >> 25) & 7 == 6:
            return self.vfp_ldst(a, w, dbl)
        if (w >> 24) & 15 == 0xe and (w >> 4) & 1 == 0:
            return self.vfp_dp(a, w, dbl)
        if (w >> 24) & 15 == 0xe and (w >> 4) & 1:
            return self.vfp_xfer(a, w, dbl)
        raise Unsupported('vfp?')

    def vfp_ldst(self, a, w, dbl):
        p, u, d, wb, l = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn = (w >> 16) & 15
        imm = (w & 0xff) * 4
        if (w >> 21) & 0xf == 0x2:  # 64-bit transfers between core & ext regs (vmov rt, rt2, dm)
            rt, rt2 = (w >> 12) & 15, (w >> 16) & 15
            m = self.vreg(w, dbl, 'm')
            if dbl:
                if l:
                    self.emit(self.cond_wrap(w, f'{REG[rt]} = cpu->vr.w[VS({2 * m})]; {REG[rt2]} = cpu->vr.w[VS({2 * m + 1})];'))
                else:
                    self.emit(self.cond_wrap(w, f'cpu->vr.w[VS({2 * m})] = {REG[rt]}; cpu->vr.w[VS({2 * m + 1})] = {REG[rt2]};'))
            else:
                if l:
                    self.emit(self.cond_wrap(w, f'{REG[rt]} = cpu->vr.w[VS({m})]; {REG[rt2]} = cpu->vr.w[VS({m + 1})];'))
                else:
                    self.emit(self.cond_wrap(w, f'cpu->vr.w[VS({m})] = {REG[rt]}; cpu->vr.w[VS({m + 1})] = {REG[rt2]};'))
            return
        vd = self.vreg(w, dbl, 'd')
        base = self.r(rn, a)
        if p and not wb:  # vldr/vstr
            ea = f'{base} {"+" if u else "-"} {imm}u'
            if rn == 15 and l:
                lit = a + 8 + (imm if u else -imm)
                if dbl:
                    v = self.img.word(lit) | self.img.word(lit + 4) << 32
                    self.emit(self.cond_wrap(w, f'cpu->vr.x[{vd}] = 0x{v:016x}ull;'))
                else:
                    self.emit(self.cond_wrap(w, f'cpu->vr.w[VS({vd})] = 0x{self.img.word(lit):08x}u;'))
                return
            if dbl:
                code = f'cpu->vr.x[{vd}] = LD64({ea});' if l else f'ST64({ea}, cpu->vr.x[{vd}]);'
            else:
                code = f'cpu->vr.w[VS({vd})] = LD32({ea});' if l else f'ST32({ea}, cpu->vr.w[VS({vd})]);'
            self.emit(self.cond_wrap(w, code))
            return
        # vldm/vstm/vpush/vpop
        count = (w & 0xff) // (2 if dbl else 1)
        if dbl and w & 1:
            raise Unsupported('fldmx')
        size = imm
        N = REG[rn]
        start = f'{N}' if u else f'{N} - {size}u'
        code = [f'uint32_t ea = {start};']
        for k in range(count):
            if dbl:
                code.append(f'cpu->vr.x[{vd + k}] = LD64(ea + {8 * k});' if l else f'ST64(ea + {8 * k}, cpu->vr.x[{vd + k}]);')
            else:
                code.append(f'cpu->vr.w[VS({vd + k})] = LD32(ea + {4 * k});' if l else f'ST32(ea + {4 * k}, cpu->vr.w[VS({vd + k})]);')
        if wb:
            code.append(f'{N} = {N} {"+" if u else "-"} {size}u;')
        self.emit(self.cond_wrap(w, '{ ' + ' '.join(code) + ' }'))

    def vfp_xfer(self, a, w, dbl):
        rt = (w >> 12) & 15
        opc = (w >> 21) & 7
        l = (w >> 20) & 1
        if opc == 7 and (w >> 16) & 15 == 1:  # vmrs/vmsr fpscr
            if l:
                if rt == 15:
                    self.emit(self.cond_wrap(w, 'n = cpu->fpscr >> 31; z = (cpu->fpscr >> 30) & 1; c = (cpu->fpscr >> 29) & 1; v = (cpu->fpscr >> 28) & 1;'))
                else:
                    self.emit(self.cond_wrap(w, f'{REG[rt]} = cpu->fpscr;'))
            else:
                self.emit(self.cond_wrap(w, f'cpu->fpscr = {REG[rt]};'))
            return
        if not dbl and opc == 0:  # vmov rt <-> sn
            sn = self.vreg(w, False, 'n')
            self.emit(self.cond_wrap(w, f'{REG[rt]} = cpu->vr.w[VS({sn})];' if l else f'cpu->vr.w[VS({sn})] = {REG[rt]};'))
            return
        if dbl and not l and (w >> 22) & 1 == 0 and (w >> 5) & 3 == 0 and opc & 4 == 0:  # vmov.32 dd[x], rt
            dn = self.vreg(w, True, 'n')
            x = (w >> 21) & 1
            self.emit(self.cond_wrap(w, f'cpu->vr.w[VS({2 * dn + x})] = {REG[rt]};'))
            return
        if dbl and l and (w >> 22) & 1 == 0 and (w >> 5) & 3 == 0 and opc & 6 == 0:  # vmov.32 rt, dn[x]
            dn = self.vreg(w, True, 'n')
            x = (w >> 21) & 1
            self.emit(self.cond_wrap(w, f'{REG[rt]} = cpu->vr.w[VS({2 * dn + x})];'))
            return
        raise Unsupported('vfp xfer')

    def vfp_dp(self, a, w, dbl):
        T = 'double' if dbl else 'float'
        R = 'cpu->vr.d' if dbl else 'cpu->vr.s'
        d, n, m = self.vreg(w, dbl, 'd'), self.vreg(w, dbl, 'n'), self.vreg(w, dbl, 'm')
        D, N, M = (f'{R}[{i}]' if dbl else f'{R}[VS({i})]' for i in (d, n, m))
        opc1 = ((w >> 20) & 0xb)
        op6 = (w >> 6) & 1
        code = None
        if opc1 == 0x0:
            code = f'{D} = {D} + {N} * {M};' if not op6 else f'{D} = {D} - {N} * {M};'
        elif opc1 == 0x1:
            code = f'{D} = -{D} + {N} * {M};' if not op6 else f'{D} = -{D} - {N} * {M};'  # vnmls / vnmla
        elif opc1 == 0x2:
            code = f'{D} = {N} * {M};' if not op6 else f'{D} = -({N} * {M});'
        elif opc1 == 0x3:
            code = f'{D} = {N} + {M};' if not op6 else f'{D} = {N} - {M};'
        elif opc1 == 0x8 and not op6:
            code = f'{D} = {N} / {M};'
        elif opc1 == 0xa:  # vfma / vfms (fused)
            fma = 'fma' if dbl else 'fmaf'
            code = f'{D} = {fma}({N}, {M}, {D});' if not op6 else f'{D} = {fma}(-{N}, {M}, {D});'
        elif opc1 == 0x9:  # vfnms / vfnma (fused)
            fma = 'fma' if dbl else 'fmaf'
            code = f'{D} = {fma}({N}, {M}, -{D});' if not op6 else f'{D} = {fma}(-{N}, {M}, -{D});'
        elif opc1 == 0xb:
            opc2 = (w >> 16) & 15
            if not op6:  # vmov imm
                imm8 = ((w >> 12) & 0xf0) | (w & 15)
                code = f'{D} = VFP_IMM_{"D" if dbl else "S"}(0x{imm8:x});'
            elif opc2 == 0:
                code = f'{D} = {M};' if not (w >> 7) & 1 else f'{D} = FABS({M});'
            elif opc2 == 1:
                code = f'{D} = -{M};' if not (w >> 7) & 1 else f'{D} = SQRT({M});'
            elif opc2 in (4, 5):
                rhs = '0' if opc2 == 5 else M
                code = f'cpu->fpscr = VFP_CMP(cpu->fpscr, (double){D}, (double){rhs});'
            elif opc2 == 7 and (w >> 7) & 1:  # vcvt between precisions
                if dbl:
                    sd = self.vreg(w, False, 'd')
                    code = f'cpu->vr.s[VS({sd})] = (float)cpu->vr.d[{m}];'
                else:
                    dd = self.vreg(w, True, 'd')
                    code = f'cpu->vr.d[{dd}] = (double)cpu->vr.s[VS({m})];'
            elif opc2 == 8:  # int -> fp; source is a single reg
                sm = self.vreg(w, False, 'm')
                src = f'(int32_t)cpu->vr.w[VS({sm})]' if (w >> 7) & 1 else f'cpu->vr.w[VS({sm})]'
                code = f'{D} = ({T}){src};'
            elif opc2 in (10, 11, 14, 15):  # vcvt fixed-point <-> fp, in place
                to_fixed = opc2 & 4
                unsigned = opc2 & 1
                size = 32 if (w >> 7) & 1 else 16
                frac = size - (((w & 15) << 1) | ((w >> 5) & 1))
                scale = f'{float(1 << frac)!r}'
                if to_fixed:
                    fn = ('CVT_U' if unsigned else 'CVT_S') + str(size) + '_RZ'
                    tgt = f'cpu->vr.w[VS({2 * d})]' if dbl else f'cpu->vr.w[VS({d})]'
                    code = f'{tgt} = {fn}((double){D} * {scale});'
                    if dbl:
                        code += f' cpu->vr.w[VS({2 * d + 1})] = 0;'
                else:
                    src = f'cpu->vr.w[VS({2 * d})]' if dbl else f'cpu->vr.w[VS({d})]'
                    cast = {(16, 0): '(int16_t)', (16, 1): '(uint16_t)', (32, 0): '(int32_t)', (32, 1): '(uint32_t)'}[(size, unsigned)]
                    code = f'{D} = ({T})((double){cast}{src} / {scale});'
            elif opc2 in (12, 13):  # fp -> int; dest is a single reg
                sd = self.vreg(w, False, 'd')
                fn = ('CVT_S32' if opc2 == 13 else 'CVT_U32') + ('_RZ' if (w >> 7) & 1 else '_RN')
                code = f'cpu->vr.w[VS({sd})] = {fn}((double){M});'
        if code is None:
            raise Unsupported('vfp dp')
        self.emit(self.cond_wrap(w, code))



# ----------------------------------------------------------------------------
# Thumb (16-bit Thumb-1 + BL/BLX). Only boost.thread and two libgcc switch helpers are
# Thumb code. Each 16-bit instruction is rewritten into the equivalent A32 encoding and
# fed to the (differentially tested) ARM generator; pc-relative forms and control flow
# are handled directly. Thumb-1 ALU instructions always set flags (there are no IT blocks).

THUMB_CASE_HELPERS = {'__gnu_thumb1_case_uqi': ('B', 1), '__gnu_thumb1_case_sqi': ('b', 1),
                      '__gnu_thumb1_case_uhi': ('H', 2), '__gnu_thumb1_case_shi': ('h', 2)}


def thumb_bl(h1, h2, a):
    """32-bit BL/BLX immediate -> (kind, target) or (None, None)."""
    if h1 >> 11 != 0x1e or h2 >> 14 != 3:
        return None, None
    s = (h1 >> 10) & 1
    j1, j2 = (h2 >> 13) & 1, (h2 >> 11) & 1
    i1, i2 = 1 - (j1 ^ s), 1 - (j2 ^ s)
    imm = sext((s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3ff) << 12) | ((h2 & 0x7ff) << 1), 25)
    if h2 & 0x1000:
        return 'bl', (a + 4 + imm) & 0xffffffff
    return 'blx', (((a + 4) & ~3) + imm) & 0xffffffff


class ThumbFuncGen(FuncGen):
    def hw(self, a):
        return struct.unpack_from('<H', self.img.text, a - self.img.text_addr)[0]

    def size(self, a):
        return 4 if self.hw(a) >> 11 in (0x1d, 0x1e, 0x1f) else 2

    def case_helper(self, a):
        h = self.hw(a)
        if h >> 11 != 0x1e:
            return None
        k, t = thumb_bl(h, self.hw(a + 2), a)
        if k != 'bl':
            return None
        return THUMB_CASE_HELPERS.get(self.img.syms.get(t | 1))

    def case_table(self, a, helper):
        """Targets of `bl __gnu_thumb1_case_*` at a; index in r0, count from `cmp r0, #N`."""
        n = None
        for back in range(2, 14, 2):
            h = self.hw(a - back)
            if h >> 8 == 0x28:  # cmp r0, #imm8
                n = h & 0xff
                break
        if n is None:
            raise Unsupported('case table size')
        fmt, width = helper
        base = a + 4
        vals = [struct.unpack_from('<' + fmt, self.img.text, base - self.img.text_addr + width * i)[0]
                for i in range(n + 1)]
        return [base + 2 * v for v in vals]

    def branch(self, a):
        """(kind, target, cond) for Thumb control flow, kind None otherwise."""
        h = self.hw(a)
        if h >> 12 == 0xd and (h >> 8) & 15 < 14:
            return 'bcc', (a + 4 + (sext(h & 0xff, 8) << 1)) & 0xffffffff, (h >> 8) & 15
        if h >> 11 == 0x1c:
            return 'b', (a + 4 + (sext(h & 0x7ff, 11) << 1)) & 0xffffffff, 14
        if h & 0xf500 == 0xb100:  # cbz/cbnz
            return 'cbz', a + 4 + ((((h >> 9) & 1) << 6) | (((h >> 3) & 31) << 1)), 14
        if h >> 11 == 0x1e:
            k, t = thumb_bl(h, self.hw(a + 2), a)
            return k, t, 14
        return None, None, None

    def terminal(self, a):
        h = self.hw(a)
        k, t, _ = self.branch(a)
        if k == 'b':
            return True
        if k == 'bl' and (self.case_helper(a) or self.img.syms.get(t | 1) in NORETURN):
            return True
        if k == 'blx' and self.img.syms.get(t) in NORETURN:
            return True
        if h & 0xff87 == 0x4700:  # bx rm
            return True
        if h & 0xff00 == 0xbd00:  # pop {.., pc}
            return True
        if h & 0xff87 == 0x4687:  # mov pc, rm
            return True
        return False

    def explore(self, extra=()):  # extra: more entries (landing pads, for tools/v6patch.py)
        work = [self.start, *extra]
        while work:
            a = work.pop()
            while self.start <= a < self.end and a not in self.reach:
                self.reach[a] = self.hw(a)
                k, t, _ = self.branch(a)
                if k in ('b', 'bcc', 'cbz') and self.start <= t < self.end:
                    work.append(t)
                helper = self.case_helper(a)
                if helper:
                    try:
                        work.extend(x for x in self.case_table(a, helper) if self.start <= x < self.end)
                    except Unsupported:
                        pass
                if self.terminal(a):
                    break
                a += self.size(a)

    def lower(self):
        self.explore()
        stmts = []
        for a in sorted(self.reach):
            self.out = []
            ok = False
            try:
                self.tinsn(a)
                if any(PC_VAR.search(x) for x in self.out):
                    raise Unsupported('pc as operand')
                ok = True
            except Unsupported as ex:
                self.out = []
                self.unsupported['thumb ' + str(ex)] += 1
                self.emit(f'UNIMPL(0x{LOAD_BASE + a:08x}u, 0x{self.hw(a):04x}u); /* thumb {ex} */')
            except (IndexError, KeyError, struct.error):
                self.out = []
                self.unsupported['thumb invalid encoding'] += 1
                self.emit(f'UNIMPL(0x{LOAD_BASE + a:08x}u, 0x{self.hw(a):04x}u); /* thumb invalid */')
            vop = thumb_vop(self.hw(a)) if ok and len(self.out) == 1 and self.size(a) == 2 else None
            pcv = LOAD_BASE + a + 4
            stmts += [(a, x, vop if k == 0 else None, pcv) for k, x in enumerate(self.out)]
            nxt = a + self.size(a)
            if nxt not in self.reach and not self.terminal(a):
                stmts.append((a, 'RET;', None, pcv))
        stmts.append((None, 'RET;', None, 0))
        return FuncIR(self, stmts, 'thumb')

    def generate(self):
        return render_old(self.lower())

    def call_thumb(self, target, ret):
        if target in self.g.thumb_set:
            return f'lr = 0x{(LOAD_BASE + ret) | 1:08x}u; CALL_RET(t_{target:08x}, 0x{(LOAD_BASE + ret) | 1:08x}u);'
        return (f'lr = 0x{(LOAD_BASE + ret) | 1:08x}u; '
                f'CALLI_RET(0x{(LOAD_BASE + target) | 1:08x}u, 0x{(LOAD_BASE + ret) | 1:08x}u);')

    def tinsn(self, a):
        h = self.hw(a)
        k, t, cond = self.branch(a)
        pcv = LOAD_BASE + a + 4
        if k == 'bcc':
            self.emit(f'if ({COND[cond]}) {{ {self.goto(t)} }}')
            return
        if k == 'b':
            self.emit(self.goto(t))
            return
        if k == 'cbz':
            rn = REG[h & 7]
            test = f'{rn} != 0' if h & 0x800 else f'{rn} == 0'
            self.emit(f'if ({test}) {{ {self.goto(t)} }}')
            return
        if k in ('bl', 'blx'):
            helper = self.case_helper(a)
            if helper:
                cases = []
                for i, tgt in enumerate(self.case_table(a, helper)):
                    self.labels.add(tgt)
                    cases.append(f'case {i}: goto L_{tgt:08x};')
                self.emit(f'switch (r0) {{ {" ".join(cases)} }} UNREACHABLE(0x{LOAD_BASE + a:08x}u);')
                return
            if k == 'bl':
                self.emit(self.call_thumb(t, a + 4))
            else:
                self.emit(self.call_direct(t, (a + 4) | 1) + ';')
            return
        if h >> 11 == 0x09:  # ldr rt, [pc, #imm8*4]
            lit = ((a + 4) & ~3) + (h & 0xff) * 4
            self.emit(f'{REG[(h >> 8) & 7]} = 0x{self.img.word(lit):08x}u;')
            return
        if h >> 11 == 0x14:  # adr
            self.emit(f'{REG[(h >> 8) & 7]} = 0x{LOAD_BASE + ((a + 4) & ~3) + (h & 0xff) * 4:08x}u;')
            return
        if h >> 11 == 0x15:  # add rd, sp, #imm8*4
            self.emit(f'{REG[(h >> 8) & 7]} = sp + {(h & 0xff) * 4}u;')
            return
        if h >> 8 == 0xb0:  # add/sub sp, #imm7*4
            self.emit(f'sp {"-" if h & 0x80 else "+"}= {(h & 0x7f) * 4}u;')
            return
        if h >> 10 == 0x11:  # special data / branch-exchange with high registers
            op = (h >> 8) & 3
            rm = (h >> 3) & 15
            rd = ((h >> 4) & 8) | (h & 7)
            src = f'0x{pcv:08x}u' if rm == 15 else REG[rm]
            if op == 3:
                if h & 0x80:  # blx rm
                    self.emit(f'{{ uint32_t tgt = {src}; lr = 0x{(LOAD_BASE + a + 2) | 1:08x}u; '
                              f'CALLI_RET(tgt, 0x{(LOAD_BASE + a + 2) | 1:08x}u); }}')
                elif rm == 14:
                    self.emit('RET;')
                else:
                    self.emit(f'{{ TAIL_CALLI({src}); }}')
                return
            if rd == 15:
                if op == 2:
                    self.emit('RET;' if rm == 14 else f'{{ TAIL_CALLI({src}); }}')
                    return
                raise Unsupported('add/cmp pc')
            if op == 0:
                self.emit(f'{REG[rd]} = {REG[rd]} + {src};')
            elif op == 2:
                self.emit(f'{REG[rd]} = {src};')
            else:
                if rm == 15:
                    raise Unsupported('cmp pc')
                self.insn(a, 0xe1500000 | rd << 16 | rm)
            return
        if h & 0xff00 == 0xbe00:
            self.emit('TRAP_BKPT();')
            return
        if h & 0xff00 == 0xbf00 and h & 0xf == 0:  # nop-compatible hints
            self.emit('/* hint */')
            return
        if h >> 8 == 0xde:
            self.emit(f'TRAP_UDF(0x{LOAD_BASE + a:08x}u);')
            return
        self.insn(a, thumb16_to_arm(h))


def thumb16_to_arm(h):
    """Rewrite a 16-bit Thumb data/memory instruction as the equivalent A32 encoding."""
    AL = 0xe0000000
    r0_, r3_, r6_, r8_ = h & 7, (h >> 3) & 7, (h >> 6) & 7, (h >> 8) & 7

    def dp(opc, s, rn, rd, op2, imm=False):
        return AL | (1 << 25 if imm else 0) | opc << 21 | s << 20 | rn << 16 | rd << 12 | op2

    top5 = h >> 11
    if top5 in (0, 1, 2):  # lsl/lsr/asr #imm5 -> movs rd, rm, <shift> #imm
        return dp(13, 1, 0, r0_, ((h >> 6) & 31) << 7 | top5 << 5 | r3_)
    if top5 == 3:  # adds/subs rd, rn, rm|#imm3
        sub, imm = (h >> 9) & 1, (h >> 10) & 1
        return dp(2 if sub else 4, 1, r3_, r0_, r6_, imm)
    if top5 >> 2 == 1:  # movs/cmp/adds/subs rdn, #imm8
        opc = {0: 13, 1: 10, 2: 4, 3: 2}[top5 & 3]
        return dp(opc, 1, r8_ if opc != 13 else 0, r8_ if opc != 10 else 0, h & 0xff, True)
    if h >> 10 == 0x10:  # data-processing register
        op, rm, rd = (h >> 6) & 15, r3_, r0_
        if op in (2, 3, 4, 7):  # shifts by register
            typ = {2: 0, 3: 1, 4: 2, 7: 3}[op]
            return dp(13, 1, 0, rd, rm << 8 | typ << 5 | 1 << 4 | rd)
        if op == 9:  # rsbs rd, rm, #0
            return dp(3, 1, rm, rd, 0, True)
        if op == 13:  # muls rd, rm, rd
            return AL | 1 << 20 | rd << 16 | rd << 8 | 0x90 | rm
        if op == 15:
            return dp(15, 1, 0, rd, rm)
        opc = {0: 0, 1: 1, 5: 5, 6: 6, 8: 8, 10: 10, 11: 11, 12: 12, 14: 14}[op]
        return dp(opc, 1, rd, rd if opc not in (8, 10, 11) else 0, rm)
    if h >> 12 == 5:  # load/store register offset
        op, rm, rn, rt = (h >> 9) & 7, r6_, r3_, r0_
        if op in (0, 2, 4, 6):  # str, strb, ldr, ldrb
            return AL | 0x07800000 | ((op >> 1) & 1) << 22 | (op >> 2) << 20 | rn << 16 | rt << 12 | rm
        sh = {1: (0, 1), 3: (1, 2), 5: (1, 1), 7: (1, 3)}[op]
        return AL | 0x01800090 | sh[0] << 20 | rn << 16 | rt << 12 | sh[1] << 5 | rm
    if h >> 13 == 3:  # str/ldr(b) imm5
        b, l, imm = (h >> 12) & 1, (h >> 11) & 1, (h >> 6) & 31
        return AL | 0x05800000 | b << 22 | l << 20 | r3_ << 16 | r0_ << 12 | (imm if b else imm * 4)
    if h >> 12 == 8:  # strh/ldrh imm5*2
        l, imm = (h >> 11) & 1, ((h >> 6) & 31) * 2
        return AL | 0x01c000b0 | l << 20 | r3_ << 16 | r0_ << 12 | (imm >> 4) << 8 | (imm & 15)
    if h >> 12 == 9:  # str/ldr rt, [sp, #imm8*4]
        return AL | 0x058d0000 | ((h >> 11) & 1) << 20 | r8_ << 12 | (h & 0xff) * 4
    if h >> 12 == 0xb:
        if h & 0xff00 == 0xb200:  # sxth/sxtb/uxth/uxtb
            base = {0: 0x06bf0070, 1: 0x06af0070, 2: 0x06ff0070, 3: 0x06ef0070}[(h >> 6) & 3]
            return AL | base | r0_ << 12 | r3_
        if h & 0xff00 == 0xba00 and (h >> 6) & 3 != 2:  # rev/rev16/revsh
            base = {0: 0x06bf0f30, 1: 0x06bf0fb0, 3: 0x06ff0fb0}[(h >> 6) & 3]
            return AL | base | r0_ << 12 | r3_
        if h & 0xfe00 == 0xb400:  # push {list[, lr]}
            return AL | 0x092d0000 | (h & 0xff) | ((h >> 8) & 1) << 14
        if h & 0xfe00 == 0xbc00:  # pop {list[, pc]}
            return AL | 0x08bd0000 | (h & 0xff) | ((h >> 8) & 1) << 15
    if h >> 12 == 0xc:  # stmia/ldmia rn!, {list}
        l, rn, lst = (h >> 11) & 1, r8_, h & 0xff
        wb = 0 if l and lst & (1 << rn) else 1
        return AL | 0x08800000 | wb << 21 | l << 20 | rn << 16 | lst
    raise Unsupported(f'{h:04x}')


# ----------------------------------------------------------------------------
# Register flow: which registers cross the Cpu struct at calls, returns and prologues.
#
# Guest registers live in C locals; the Cpu struct is how they reach other functions. Moving
# all 15 registers and 4 flags at every call and return cost about a quarter of the run
# time, and storing the flags kept their computations alive. So every function gets, per
# entry point (computed over the call graph to a fixed point):
#   sem_in     registers whose entry value it, or anything it calls, may use
#   may_write  registers that may differ from their entry value when it returns
#   mre        whether it may return somewhere other than its lr (see RET_CHECK)
# and per function demand: the flags a caller reads after calling it (libgcc's compare
# helpers return results in flags; ordinary code never relies on flags across a call).
# A call stores what the callee reads and the caller changed since the struct last held it,
# loads what the callee may write; a return stores what the function changed. Invariant at
# every return: each struct register is as it was at entry unless the function changed the
# register, then it holds the new value; struct flags are only meaningful where demanded.
# Host code (rt_dispatch, imports, hooks) reads and writes the struct directly, so those
# sites store and load every register.
#
# may_write and mre come from a value analysis that knows push/pop: registers saved on the
# stack and restored are unchanged. Each entry's exits are summarised in terms of its entry
# registers and of what the stack held above its sp, so code that pops its caller's frame
# (libgcc helpers, GCC's shared epilogues, cold blocks split into separate functions) is
# resolved where it is called from. Pushes count as reads (sem_in): the C++ unwinder rebuilds
# a landing pad's r4-r11 from saved registers, so those must hold the real values.
#
# Trusted, not checked: indirect call targets, imports and host hooks keep r4-r11 and sp (the
# code still reloads everything after them; the callers' own summaries assume it), and do not
# pass flags in or out (only kernel helpers return flags, through the struct); a frame's save
# area is written only through sp at known offsets, never through pointers into the frame;
# compiled code returns only to its lr or to a return address saved on its stack, so a path
# that would return to a computed or loaded value is infeasible (the report counts those).
#
# Masks: bit i = r<i> (13 sp, 14 lr), bits 16-19 = flags n z c v.

ALLREGS = 0x7fff
FLAGS = 0xf0000
ALLV = ALLREGS | FLAGS
CLOBBER_REGS = 0x500f            # r0-r3, r12, lr: what an AAPCS callee may change
CLOBBER = CLOBBER_REGS | FLAGS
IMPORT_CLOBBER = 0x100f          # host imports return in r0-r3 and keep lr (rt::call_nested)
IMPORT_READS = 0x600f            # what a plain import reads: r0-r3, sp (stack arguments), lr


def plain_import(name):
    """Imports that are host code only: they read their arguments (r0-r3, the stack) and lr
    (rt_call_import), change r0-r3 / r12 at most, and never run guest code (no callbacks, no
    exceptions, no setjmp), so the caller's r4-r11 need not be in the struct around them.
    GL (the wrappers of runtime/gles.cpp), the allocator, waits on condition variables, clock."""
    return (bool(re.fullmatch(r'gl[A-Z]\w*', name)) or
            name in ('malloc', 'free', 'calloc', 'realloc', 'memalign', '_Znwj', '_Znaj', '_ZdlPv', '_ZdaPv',
                     'clock', 'pthread_cond_wait', 'pthread_cond_timedwait', 'pthread_cond_signal',
                     'pthread_cond_broadcast', 'pthread_mutex_trylock'))
CALLEE_SAVED = 0x2ff0            # r4-r11, sp: restored by the C++ unwinder at a landing pad
VARS = REG[:15] + [None, 'n', 'z', 'c', 'v']
VARBIT = {v: i for i, v in enumerate(VARS) if v}
K = 32                           # value base of constants
ORIG = [(i, 0) for i in range(20)]
SCRATCH = ('X', 0)               # computed by arithmetic: never a code address callers expect
LOADED = ('L', 0)                # loaded from memory other than this frame
EQ, ORIGS, UNK = 0, 1, 2         # struct value: = local, = entry value, unknown

IDENT_RE = re.compile(r'(?<!->)(?<![\w.])(r1[0-2]|r[0-9]|sp|lr|[nzcv])\b(\s*(?:[-+&|^]|<<|>>)?=(?!=))?')
COMMENT_RE = re.compile(r'/\*.*?\*/')
CONST_RE = re.compile(r'(r1[0-2]|r[0-9]|sp|lr) = 0x([0-9a-f]+)u;')
EVENT_RE = re.compile(
    r'(?P<call>CALL_RET)\((?P<cf>[fte]_[0-9a-f]{8}), (?P<cr>0x[0-9a-f]+u)\)'
    r'|(?P<calli>CALLI_RET)\((?P<ct>[^,()]+), (?P<cir>0x[0-9a-f]+u)\)'
    r'|(?P<tail>TAIL_CALL)\((?P<tf>[fte]_[0-9a-f]{8})\)'
    r'|(?P<taili>TAIL_CALLI)\((?P<tt>[^()]+)\)'
    r'|(?P<imp>IMPORT)\((?P<ii>\d+)\)'
    r'|(?P<sj>GUEST_SETJMP)\(\)'
    r'|(?P<retto>RET_TO)\((?P<rp>\w+)\)'
    r'|(?P<ret>\bRET\b)'
    r'|(?P<unimpl>UNIMPL)\((?P<ua>[^()]*)\)'
    r'|(?P<unr>UNREACHABLE)\((?P<ura>[^()]*)\)'
    r'|(?P<trap>TRAP_(?:BKPT|UDF))\([^()]*\)'
    r'|(?P<goto>goto) L_(?P<gl>[0-9a-f]{8});')
ENDS = {'ret', 'retto', 'tail', 'taili', 'unimpl', 'unr', 'trap', 'goto'}


def bits(m):
    while m:
        b = m & -m
        yield b.bit_length() - 1
        m ^= b


def var_list(mask, mac):
    return ' '.join(f'{mac}({VARS[b]})' for b in bits(mask))


def var_names(mask):
    return ' '.join(VARS[b] for b in bits(mask))


def s32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x & 0x80000000 else x


def vadd(v, k):
    """Value v plus constant k. Values: (r, k) = entry value of register r plus k, (K, k) =
    constant k, (('S', o), k) = what the stack held at entry sp + o (o >= 0: the caller's
    frame) plus k, ('X', k) = a scratch value (never a code address of interest),
    ('M', v) = v or a scratch value, None = unknown."""
    if v is None or k is None:
        return None
    if v[0] == K:
        return (K, (v[1] + k) & 0xffffffff)
    if v[0] == 'M':
        return SCRATCH if k else v
    return (v[0], s32(v[1] + k))


def exact(v):
    """Whether two equal values are certainly the same runtime value: not for scratch / loaded /
    maybe values, nor ('T', o) (a caller-frame slot that may have been overwritten)."""
    b = v[0]
    return not (b == 'X' or b == 'L' or b == 'M' or (b.__class__ is tuple and b[0] == 'T'))


def scratchy(v):
    return v[0] == K or v[0] == 'X' or v[0] == 'L'


def retlike(v):
    """Values a return can go to: lr at entry or a return address saved on the stack."""
    return v == ORIG[14] or (v[1] == 0 and v[0].__class__ is tuple)


def vjoin(a, b):
    """Join of two different values. A return address joined with scratch values remains
    ('M', it): following such a path back through a scratch value is not a real return."""
    if a is None or b is None:
        return None
    if a[0] == 'M':
        a, b = b, a
    if b[0] == 'M':
        return b if a == b[1] or scratchy(a) else None
    if scratchy(a):
        return SCRATCH if scratchy(b) else ('M', b) if retlike(b) else None
    if scratchy(b) and retlike(a):
        return ('M', a)
    return None


def arm_vop(w):
    """The part of an ARM instruction the value analysis models (moves, address arithmetic,
    loads/stores that may hit the stack); None: its written registers become unknown."""
    if w >> 28 == 0xf:
        return None
    op = (w >> 25) & 7
    rn, rd = (w >> 16) & 15, (w >> 12) & 15
    if op == 4:
        if w & (1 << 22):
            return None
        regs = tuple(i for i in range(16) if w >> i & 1)
        return ('ldm' if w & (1 << 20) else 'stm', rn, regs, (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1)
    if op in (2, 3) and not (op == 3 and w & 16):
        if op == 3:
            off = None if w & 0xff0 else ('r', w & 15)
        else:
            off = ('i', w & 0xfff)
        return ('mem', (w >> 20) & 1, 1 if w & (1 << 22) else 4, rn, rd, off, (w >> 24) & 1, (w >> 23) & 1,
                (w >> 21) & 1)
    if op in (0, 1):
        imm = w & (1 << 25)
        op1 = (w >> 20) & 0x1f
        if not imm:
            if w & 0x90 == 0x90:
                if (w >> 5) & 3 == 0:
                    if op1 >> 4 == 0:
                        return None  # multiply
                    return None if w & (1 << 20) else ('stg', rn)  # ldrex / strex
                l, op2 = (w >> 20) & 1, (w >> 5) & 3
                size = 2 if op2 == 1 or (op2 == 3 and l) else 1 if op2 == 2 and l else 8
                off = ('i', ((w >> 4) & 0xf0) | (w & 15)) if w & (1 << 22) else ('r', w & 15)
                return ('mem', l, size, rn, rd, off, (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1)
            if op1 & 0x19 == 0x10:
                return None
        elif op1 in (0x10, 0x14) or op1 & 0x1b == 0x12:
            return None
        opc = (w >> 21) & 15
        if rd == 15:
            return None
        if imm:
            rot = ((w >> 8) & 15) * 2
            v = w & 0xff
            v = ((v >> rot) | (v << (32 - rot))) & 0xffffffff if rot else v
            if opc == 4:
                return ('addi', rd, rn, v)
            if opc == 2:
                return ('addi', rd, rn, -v)
            return None
        if w & 0xff0:
            return None
        if opc == 13:
            return ('mov', rd, w & 15)
        if opc in (2, 4):
            return ('addr', rd, rn, w & 15, 1 if opc == 4 else -1)
        return None
    if op == 6 and (w >> 9) & 7 == 5:  # VFP load/store (cp10/cp11)
        if (w >> 21) & 0xf == 2:
            return None
        p, u, wb, l = (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1, (w >> 20) & 1
        imm = (w & 0xff) * 4
        if p and not wb:
            return ('vmem', l, rn, imm if u else -imm, 8 if (w >> 8) & 1 else 4)
        return ('vmemm', l, rn, u, wb, imm)
    return None


def thumb_vop(h):
    if h >> 8 == 0xb0:  # add/sub sp, #imm
        return ('addi', 13, 13, -(h & 0x7f) * 4 if h & 0x80 else (h & 0x7f) * 4)
    if h >> 11 == 0x15:  # add rd, sp, #imm
        return ('addi', (h >> 8) & 7, 13, (h & 0xff) * 4)
    if h >> 10 == 0x11:
        rm, rd = (h >> 3) & 15, ((h >> 4) & 8) | (h & 7)
        return ('mov', rd, rm) if (h >> 8) & 3 == 2 and rd != 15 and rm != 15 else None
    if h >> 11 in (0x09, 0x14, 0x1c, 0x1d, 0x1e, 0x1f) or h >> 12 == 0xd:
        return None
    try:
        return arm_vop(thumb16_to_arm(h))
    except (Unsupported, KeyError):
        return None


# Stack slots: {offset from entry sp: value}. A word not listed still holds its entry-time
# contents. Values written by a push (stm / str with sp writeback: the register save area) are
# kept as ('P', value) and trusted until overwritten through sp; every other slot is a local,
# which a call or a store through a pointer the analysis cannot place may change (code passes
# the addresses of its locals around: a std::list keeps its head node on the stack).

def slot_default(slots, o):
    # Above the entry sp is the caller's frame: ('S', o) is its contents at entry. Once
    # something may have written there through a pointer (a call, a store the analysis cannot
    # place), ('T', o) is the same but only valid if the caller saved a register in that slot
    # (save areas are never written through pointers); the caller decides (Flow.forward subst).
    if o < 0:
        return None
    return (('T' if None in slots else 'S', o), 0)


def slot_value(slots, o):
    v = slots[o] if o in slots else slot_default(slots, o)
    return v[1] if v is not None and v[0] == 'P' else v


def sload(slots, a):
    if a is None:
        return None
    if a[0] == 13:
        return slot_value(slots, a[1])
    return LOADED


def sstore(slots, a, v, size, push=False):
    """Store to address value a (stack slots only; see pointer_store for other bases)."""
    if a is None or a[0] != 13:
        return
    o = a[1]
    if size == 4 and not o & 3:
        slots[o] = ('P', v) if push and v is not None else v
    else:
        slots[o & ~3] = slots[(o + size - 1) & ~3] = None


def locals_clobbered(slots):
    """Locals may have changed (a call, a store through an unknown pointer); saves stay."""
    for q, v in slots.items():
        if q is not None and v is not None and v[0] != 'P':
            slots[q] = None
    slots[None] = True


def pointer_store(slots, base):
    """A store through an address with value base, not a known stack slot. Constants are
    globals. An entry register (an argument) cannot point to this frame's locals, which did not
    exist yet, but may point into the caller's frame; anything else may point anywhere."""
    if base is not None and base[0] == K:
        return
    if base is not None and base[0].__class__ is int and base[0] < 20 and base[0] != 13:
        slots[None] = True
        return
    locals_clobbered(slots)


def apply_vop(vop, lv, slots, pcv):
    """Apply vop to the value state; returns (value of a loaded return address, written mask)."""
    k = vop[0]

    def rv(r):
        return (K, pcv) if r == 15 else lv[r]

    if k == 'const':
        lv[vop[1]] = (K, vop[2])
        return None, 1 << vop[1]
    if k == 'mov':
        lv[vop[1]] = rv(vop[2])
        return None, 1 << vop[1]
    if k == 'addi':
        lv[vop[1]] = vadd(rv(vop[2]), vop[3])
        return None, 1 << vop[1]
    if k == 'addr':
        _, rd, rn, rm, sign = vop
        x, y = rv(rn), rv(rm)
        if y is not None and y[0] == K:
            lv[rd] = vadd(x, sign * s32(y[1]))
        elif x is not None and x[0] == K and sign == 1:
            lv[rd] = vadd(y, s32(x[1]))
        else:
            lv[rd] = None
        return None, 1 << rd
    if k == 'mem':
        _, l, size, rn, rt, off, p, u, wb = vop
        base = rv(rn)
        if off is None:
            o = None
        elif off[0] == 'i':
            o = off[1]
        else:
            y = rv(off[1])
            o = s32(y[1]) if y is not None and y[0] == K else None
        if o is not None and not u:
            o = -o
        addr = vadd(base, o) if p else base
        stackish = rn == 13 or (base is not None and base[0] == 13)
        newb = vadd(base, o) if (not p or wb) and rn != 15 else None
        written, rp = 0, None
        if l:
            if (not p or wb) and rn != 15:
                lv[rn] = newb
                written |= 1 << rn
            if rt == 15:
                rp = sload(slots, addr)
            elif size == 8:
                lv[rt] = sload(slots, addr)
                lv[rt + 1] = sload(slots, vadd(addr, 4))
                written |= 3 << rt
            else:
                lv[rt] = sload(slots, addr) if size == 4 else SCRATCH
                written |= 1 << rt
        else:
            push = rn == 13 and p and wb
            if addr is None and stackish:
                locals_clobbered(slots)
            elif not stackish:
                pointer_store(slots, base)
            elif size == 8:
                sstore(slots, addr, rv(rt), 4, push)
                sstore(slots, vadd(addr, 4), rv(rt + 1), 4, push)
            else:
                sstore(slots, addr, rv(rt) if size == 4 and rt != 15 else None, size, push)
            if (not p or wb) and rn != 15:
                lv[rn] = newb
                written |= 1 << rn
        return rp, written
    if k in ('ldm', 'stm'):
        _, rn, regs, p, u, wb = vop
        base = rv(rn)
        cnt = len(regs)
        start = (4 if p else 0) if u else (-4 * cnt if p else -4 * cnt + 4)
        newb = vadd(base, 4 * cnt if u else -4 * cnt)
        written, rp = 0, None
        if k == 'ldm':
            for j, r in enumerate(regs):
                val = sload(slots, vadd(base, start + 4 * j))
                if r == 15:
                    rp = val
                elif r == rn and wb:
                    continue
                else:
                    lv[r] = val
                    written |= 1 << r
            if wb and rn not in regs:
                lv[rn] = newb
                written |= 1 << rn
        else:
            if base is None and rn == 13:
                locals_clobbered(slots)
            elif base is None or base[0] != 13:
                pointer_store(slots, base)
            else:
                push = rn == 13 and wb
                for j, r in enumerate(regs):
                    sstore(slots, vadd(base, start + 4 * j), None if r == 15 else lv[r], 4, push)
            if wb:
                lv[rn] = newb
                written |= 1 << rn
        return rp, written
    if k == 'vmem':
        _, l, rn, off, size = vop
        base = rv(rn)
        if not l:
            a = vadd(base, off)
            if a is None or a[0] != 13:
                if rn == 13 or base is None or base[0] == 13:
                    locals_clobbered(slots)
                else:
                    pointer_store(slots, base)
            else:
                for q in range(0, size, 4):
                    sstore(slots, vadd(a, q), None, 4)
        return None, 0
    if k == 'vmemm':
        _, l, rn, u, wb, size = vop
        base = rv(rn)
        if not l:
            a = base if u else vadd(base, -size)
            if a is None or a[0] != 13:
                if rn == 13 or base is None or base[0] == 13:
                    locals_clobbered(slots)
                else:
                    pointer_store(slots, base)
            else:
                for q in range(0, size, 4):
                    sstore(slots, vadd(a, q), None, 4)
        if wb:
            lv[rn] = vadd(base, size if u else -size)
            return None, 1 << rn
        return None, 0
    if k == 'stg':
        base = lv[vop[1]]
        if vop[1] == 13 or base is None or base[0] == 13:
            locals_clobbered(slots)
        else:
            pointer_store(slots, base)
        return None, 0
    return None, 0


class Node:
    """One generated C statement with what the analysis needs to know about it."""
    __slots__ = ('addr', 'text', 'cond', 'uses', 'defs', 'vop', 'evs', 'spans', 'switch', 'store', 'alu',
                 'pcv', 'tgts', 'masks')

    def __init__(self, addr, text, vop, pcv):
        self.addr, self.text, self.vop, self.pcv = addr, text, vop, pcv
        self.masks = None


class FlowError(Exception):
    pass


def parse_node(addr, text, vop, pcv):
    n = Node(addr, text, vop, pcv)
    blank = COMMENT_RE.sub(lambda m: ' ' * len(m.group()), text)
    n.cond = blank.startswith('if (')
    n.switch = 'switch (' in blank
    evs, spans = [], []
    pre = list(blank)
    first = None
    for m in EVENT_RE.finditer(blank):
        g = m.group
        if g('call'):
            name = g('cf')
            ev = ('call', name[0], int(name[2:], 16), int(g('cr')[2:-1], 16))
        elif g('calli'):
            ev = ('calli', g('ct').strip())
        elif g('tail'):
            name = g('tf')
            ev = ('tail', name[0], int(name[2:], 16))
        elif g('taili'):
            ev = ('taili', g('tt').strip())
        elif g('imp'):
            ev = ('import', int(g('ii')))
        elif g('sj'):
            ev = ('setjmp',)
        elif g('retto'):
            ev = ('retto', g('rp'))
        elif g('ret'):
            ev = ('ret',)
        elif g('unimpl'):
            ev = ('unimpl',)
        elif g('unr'):
            ev = ('unr',)
        elif g('trap'):
            ev = ('trap',)
        else:
            ev = ('goto', int(g('gl'), 16))
        evs.append(ev)
        spans.append(m.span())
        if first is None:
            first = m.start()
        for i in range(m.start(), m.end()):
            pre[i] = ' '
    pre = ''.join(pre)
    uses = defs = 0
    for m in IDENT_RE.finditer(pre):
        if first is not None and m.start() > first and not n.switch:
            raise FlowError('register code after a control transfer')
        b = 1 << VARBIT[m.group(1)]
        op = m.group(2)
        if op is None:
            uses |= b
        else:
            defs |= b
            if op.strip() != '=':
                uses |= b
    # register targets of indirect transfers are read by the event itself
    for ev in evs:
        if ev[0] in ('calli', 'taili') and ev[1] in VARBIT:
            uses |= 1 << VARBIT[ev[1]]
    n.uses, n.defs, n.evs, n.spans = uses, defs, evs, spans
    n.store = bool(re.search(r'\bST(?:8|16|32|64|REX\d+)\(|\bGW_', pre))
    n.alu = not re.search(r'\bLD(?:8|16|32|64)\(|cpu->', pre)
    body = blank[blank.index(') { ') + 4:-2] if n.cond else blank
    m = CONST_RE.fullmatch(body.strip())
    if m:
        n.vop = ('const', VARBIT[m.group(1)], int(m.group(2), 16))
    if n.switch and any(e[0] not in ('goto', 'tail', 'taili', 'unr') for e in evs):
        raise FlowError('unexpected transfer in a switch')
    return n


EXIT_RECORDS = 8  # per entry; more are merged


def merge_exits(recs, level):
    """Bound the records of an entry. Level 0 keeps them all (up to EXIT_RECORDS), level 1
    joins those with the same return target, level 2 joins everything; an entry only moves
    up, so the fixed point cannot oscillate between a merged and an unmerged set."""
    if level == 0 and len(recs) <= EXIT_RECORDS:
        return frozenset(recs), 0
    by_target = {}
    for lv, t in recs:
        cur = by_target.get(t)
        by_target[t] = lv if cur is None else tuple(a if a == b else vjoin(a, b) for a, b in zip(cur, lv))
    if level <= 1 and len(by_target) <= EXIT_RECORDS:
        return frozenset((lv, t) for t, lv in by_target.items()), 1
    lvs = list(by_target.values())
    lv = lvs[0]
    for other in lvs[1:]:
        lv = tuple(a if a == b else vjoin(a, b) for a, b in zip(lv, other))
    return frozenset([(lv, None)]), 2


class FuncIR:
    """A lowered function: C statements in address order plus what rendering needs."""

    def __init__(self, fg, stmts, kind='arm'):
        self.start, self.end, self.kind = fg.start, fg.end, kind
        g = fg.g
        self.name = fg.img.syms.get(self.start | (1 if kind == 'thumb' else 0))
        self.eh = kind == 'arm' and self.start in getattr(g, 'eh_funcs', ())
        self.entries = sorted(getattr(g, 'entries_by_func', {}).get(self.start, ())) if kind == 'arm' else []
        self.pads = getattr(g, 'eh_pads', {}).get(self.start, set()) if self.eh else set()
        self.labels = set(fg.labels) | set(self.entries)
        self.wrap = None
        self.stmts = stmts  # [(addr, text, vop, pcv)]; the final fall-off RET has addr None
        self.nodes = None
        self.style = 'new' if kind == 'arm' else 'old'
        self.stub = None


# ----------------------------------------------------------------------------

class Flow:
    """Interprocedural register-flow analysis and rendering of the _X macro forms."""

    def __init__(self, g, irs):
        self.g, self.irs = g, irs
        self.mw, self.mre, self.sem = {}, {}, {}
        self.exits = {}       # entry -> exit records (see forward)
        self.exit_level = {}  # entry -> how far its records were merged (merge_exits)
        self.unknown = set()  # entries summarised as "anything" (see run)
        self.pruned = set()   # entries with an exit dropped as infeasible (see forward exit_)
        self.ind_mw = ALLV     # registers an indirect call reloads (set in run)
        self.verbose = bool(os.environ.get('ARM2C_VERBOSE'))  # progress of the fixed points
        self.loaded_returns = set()  # functions that would return through a value loaded from memory
        self.dem = collections.defaultdict(int)
        # Flags and indirect calls: compiled code never relies on flags across a call (AAPCS), so
        # indirect calls store no flags and their targets need not return any. The kernel
        # helpers that do return flags (__kuser_cmpxchg: C) write them into the struct, which is
        # reloaded after every indirect call.
        self.overrides = getattr(g, 'override_addrs', {})
        # what an indirect call can reach: addresses that exist as pointers (data relocations,
        # symbols, PC-relative code addresses) and Thumb code; not the targets of b / bl
        self.dispatchable = (set(g.img.ptr_targets) | {a & ~1 for a in g.img.syms} | g.pointer_entries |
                             getattr(g, 'code_ptr_targets', set()) | set(g.thumb))
        img = g.img
        self.noret = {f for f in irs if img.syms.get(f | (1 if irs[f].kind == 'thumb' else 0)) in NORETURN}
        self.noret_imports = {i for n, i in g.import_index.items() if n in NORETURN}
        self.plain_imports = {i for n, i in g.import_index.items() if plain_import(n)}
        self.owner = {}
        self.callers = collections.defaultdict(set)
        self.fallbacks = []
        for f, fx in irs.items():
            if fx.stub is not None:
                continue
            try:
                self.prepare(fx)
            except FlowError as ex:
                fx.style = 'old'
                fx.nodes = None
                self.fallbacks.append((f, str(ex)))
        self.root_keys = {k for fx in irs.values() if fx.nodes is not None for k in fx.root_keys}
        for f, fx in irs.items():
            for n in fx.nodes or ():
                for ev in n.evs:
                    if ev[0] in ('call', 'tail'):
                        key = ev[2]
                        o = self.owner.get(key)
                        if o is None:
                            o = self.owner[key] = g.func_of(key) if ev[1] == 'e' else key
                        self.callers[o].add(f)

    # --- preparation ----------------------------------------------------------
    def prepare(self, fx):
        nodes = [parse_node(*s) for s in fx.stmts]
        fx.stmts = None
        at = {}
        for i, n in enumerate(nodes):
            if n.addr is not None and n.addr not in at:
                at[n.addr] = i
        starts = {0}
        roots = [0]
        for e in fx.entries:
            if e not in at:
                raise FlowError(f'entry {e:#x} not generated')
            starts.add(at[e])
            if e not in fx.pads:
                roots.append(at[e])
        pads = [at[p] for p in sorted(fx.pads)]
        for n in nodes:
            n.tgts = []
            for ev in n.evs:
                if ev[0] == 'goto':
                    if ev[1] not in at:
                        raise FlowError(f'goto {ev[1]:#x} outside the function')
                    n.tgts.append(at[ev[1]])
                    starts.add(at[ev[1]])
        fx.nodes, fx.starts, fx.roots, fx.pad_idx = nodes, starts, roots, pads
        fx.root_keys = [nodes[i].addr for i in roots]
        fx.root_keys[0] = fx.start

    def callee(self, kind, key):
        """(sem_in, may_write, mre, reload-everything) of the entry `key` called as f_/e_/t_."""
        owner = self.owner.get(key, key)
        ov = self.overrides.get(owner)
        if ov is not None:
            if kind == 'e':
                return ALLV, ALLV, True, True
            if isinstance(ov, tuple):  # the hook gets the full state and calls the original
                return (ALLREGS | (self.sem.get(key, 0) & FLAGS), CLOBBER_REGS | self.mw.get(key, 0),
                        self.mre.get(key, False), True)
            return ALLREGS, CLOBBER_REGS, False, True
        if key not in self.root_keys:  # not analysed (or not an entry the analysis knows)
            return ALLV, ALLV, True, True
        if key in self.unknown:
            return self.sem.get(key, 0) | ALLREGS, ALLV, False, True
        return self.sem.get(key, 0), self.mw.get(key, 0), self.mre.get(key, False), False

    def func_of_key(self, key):
        return key if key in self.irs else self.g.func_of(key)

    def demand(self, f):
        return self.dem[f] & FLAGS

    def noreturn_ev(self, ev):
        if ev[0] == 'call':
            return ev[2] in self.noret and ev[1] != 'e'
        if ev[0] == 'import':
            return ev[1] in self.noret_imports
        return False

    # --- forward: values (may_write / mre) and, when emitting, struct sync ---------
    def exits_of(self, kind, key):
        """Exit records of a callee entry, or None where only its summary is known."""
        owner = self.owner.get(key, key)
        if owner in self.overrides or key not in self.root_keys or key in self.unknown:
            return None
        return self.exits.get(key, ())

    def forward(self, fx, roots, sync):
        """Values (and, when emitting, the struct relation) through the function from roots.
        Value mode collects exit records: the registers at a return and where it returns to,
        in terms of the entry state; emitting mode records what each site stores and loads."""
        nodes, starts, pads = fx.nodes, fx.starts, fx.pad_idx
        instate = {}
        work, inwork = [], set()
        reached = set()
        exits = set()
        res = {'exits': exits}
        f = fx.start
        retmask = ALLREGS | self.demand(f)
        callee, noreturn_ev, exits_of = self.callee, self.noreturn_ev, self.exits_of

        def copy(st):
            return [st[0][:], dict(st[1]), st[2][:] if sync else None]

        def join(cur, st):
            """cur := cur joined with st (in place); True if cur changed."""
            lv, sl, sv = cur
            nlv, nsl, nsv = st
            changed = False
            if sync:
                for b in range(20):
                    a, c = sv[b], nsv[b]
                    if a != c:
                        # keep whichever relation to the struct holds on both paths
                        eqa = a == EQ or (a == ORIGS and lv[b] == ORIG[b])
                        eqb = c == EQ or (c == ORIGS and nlv[b] == ORIG[b])
                        oa = a == ORIGS or (a == EQ and lv[b] == ORIG[b])
                        ob = c == ORIGS or (c == EQ and nlv[b] == ORIG[b])
                        r = EQ if eqa and eqb else ORIGS if oa and ob else UNK
                        if r != a:
                            sv[b] = r
                            changed = True
            for b in range(20):
                if lv[b] is not None and lv[b] != nlv[b]:
                    v = vjoin(lv[b], nlv[b])
                    if v != lv[b]:
                        lv[b] = v
                        changed = True
            if sl != nsl:
                new = {}
                if None in sl or None in nsl:
                    new[None] = True
                for q in set(sl) | set(nsl):
                    if q is None:
                        continue
                    va = sl[q] if q in sl else slot_default(sl, q)
                    vb = nsl[q] if q in nsl else slot_default(nsl, q)
                    v = va if va == vb else None
                    if v != slot_default(new, q):
                        new[q] = v
                if new != sl:
                    sl.clear()
                    sl.update(new)
                    changed = True
            return changed

        def join_into(i, st):
            cur = instate.get(i)
            if cur is None:
                instate[i] = copy(st)
            elif not join(cur, st):
                return
            if i not in inwork:
                inwork.add(i)
                work.append(i)

        def dirty(st):
            lv, _, sv = st
            m = 0
            for b in range(20):
                s = sv[b]
                if s != EQ and not (s == ORIGS and lv[b] == ORIG[b]):
                    m |= 1 << b
            return m & ~0x8000

        def spill(st, mask):
            sv = st[2]
            for b in bits(mask):
                sv[b] = EQ

        def written(st, mask):
            # what other code leaves in registers is never a return address of ours
            lv = st[0]
            for b in bits(mask):
                lv[b] = SCRATCH if b != 13 else None

        def exit_(lv, target):
            if sync or target is None:
                pass
            elif target[0] == 'M':
                target = target[1]  # the other possibility is a scratch value: see below
            elif target[0] == 'L':
                self.loaded_returns.add(f)
            if target is not None and target != ORIG[14] and (target[0].__class__ is not tuple or target[1]):
                # Compiled code returns to its lr or to a return address saved on the stack,
                # never to a computed, constant or loaded value: such a path is infeasible.
                # It typically is code shared between functions, entered from a caller that
                # already saved its return address and uses lr as a temporary.
                res['pruned'] = True
                return
            if not sync:
                # callers need to know which registers come back unchanged, which come from
                # their stack (a frame popped for them) and where sp ends up; other values
                # are reduced so that exits differing only in, say, a constant in r0 merge
                exits.add((tuple(v if v is None or v == ORIG[b] or v[0] == 13 or v[0] == 'M' or v[0].__class__ is tuple
                                 else SCRATCH for b, v in enumerate(lv)), target))

        def subst(v, lv, slots):
            """A callee's value v in terms of this function's state at the call."""
            if v is None:
                return None
            b, k = v
            if b == K or b == 'X' or b == 'L':
                return v
            if b == 'M':
                return vjoin(subst(k, lv, slots), SCRATCH)
            if b.__class__ is int:
                return vadd(lv[b], k)
            # what the callee found above its sp: trusted only where this function saved
            # registers (or in turn found it in its own caller's frame)
            a = vadd(lv[13], b[1])
            if a is None or a[0] != 13:
                return None
            o = a[1]
            if o in slots:
                raw = slots[o]
                if raw is None or raw[0] != 'P':
                    return None  # a local: callers' frames are only trusted where registers were saved
                return vadd(raw[1], k)
            d = slot_default(slots, o)
            if d is None:
                return None
            if b[0] == 'T':
                d = (('T', d[0][1]), 0)
            return vadd(d, k)

        def apply_exits(recs, st, cont_target):
            """Callee exits applied to st: returns the joined values of the exits that come
            back to cont_target (None if none do); the others leave this function. A tail
            transfer passes cont_target None: every exit is then one of this function's."""
            lv, slots = st[0], st[1]
            cont = None
            for clv, ct in recs:
                nlv = [subst(v, lv, slots) for v in clv]
                t = subst(ct, lv, slots)
                if cont_target is None:
                    exit_(nlv, t)
                    continue
                if t == cont_target or t is None:
                    if cont is None:
                        cont = nlv
                    else:
                        for b in range(20):
                            if cont[b] != nlv[b]:
                                cont[b] = vjoin(cont[b], nlv[b])
                if t != cont_target:  # an unknown target may be either
                    exit_(nlv, t)
            return cont

        def eh_edge(st):
            if not pads:
                return
            lv = st[0][:]
            for b in bits(CLOBBER):
                lv[b] = None
            est = [lv, st[1], [EQ] * 20 if sync else None]
            for p in pads:
                join_into(p, est)

        def record(n, ei, *masks):
            if n.masks is None:
                n.masks = [None] * len(n.evs)
            old = n.masks[ei]
            n.masks[ei] = masks if old is None else tuple(a | b for a, b in zip(old, masks))

        def taken(i, n, st):
            lv, slots, sv = st
            old = lv[:] if sync and (n.defs or n.vop) else None
            rp, vw = apply_vop(n.vop, lv, slots, n.pcv) if n.vop else (None, 0)
            rest = n.defs & ~vw
            v = SCRATCH if n.alu else LOADED
            for b in bits(rest):
                lv[b] = v if b != 13 else None
            if n.store and not n.vop:
                locals_clobbered(slots)
            if old is not None:
                for b in bits(n.defs | vw):
                    if sv[b] == EQ and (lv[b] is None or not exact(lv[b]) or lv[b] != old[b]):
                        sv[b] = ORIGS if old[b] == ORIG[b] else UNK
            if n.switch:
                gi = 0
                for ei, ev in enumerate(n.evs):
                    k = ev[0]
                    if k == 'goto':
                        join_into(n.tgts[gi], st)
                        gi += 1
                    elif k in ('tail', 'taili'):
                        tail(n, ei, ev, copy(st))
                return None
            gi = 0
            for ei, ev in enumerate(n.evs):
                k = ev[0]
                if k in ('call', 'calli', 'import', 'setjmp'):
                    locals_clobbered(slots)
                if k == 'call':
                    sem, mw, mre, cons = callee(ev[1], ev[2])
                    eh_edge(st)
                    if sync:
                        s = sem & dirty(st)
                        spill(st, s)
                    if noreturn_ev(ev):
                        if sync:
                            record(n, ei, s, 0, 0)
                        return None
                    recs = None if sync else exits_of(ev[1], ev[2])
                    if recs is not None:
                        lv[14] = (K, ev[3])
                        cont = apply_exits(recs, st, lv[14])
                        if cont is None:
                            return None  # never comes back here
                        lv[:] = cont
                        continue
                    reload = (ALLREGS | (mw & FLAGS)) if cons else mw
                    written(st, mw)
                    if sync:
                        for b in range(16, 20):
                            sv[b] = UNK
                        spill(st, reload)
                        record(n, ei, s, reload, dirty(st) & retmask)
                    if mre:
                        exit_(lv, None)
                elif k == 'calli':
                    eh_edge(st)
                    if sync:
                        s = ALLREGS & dirty(st)
                    written(st, CLOBBER)
                    if sync:
                        # the struct now holds what was stored (r4-r11 / sp: kept by the
                        # callee) and what is reloaded
                        spill(st, s | self.ind_mw)
                        record(n, ei, s, self.ind_mw, 0)
                elif k == 'import':
                    eh_edge(st)
                    # a plain import needs only its arguments in the struct and changes only
                    # r0-r3 / r12 there: r4-r11 keep whatever relation they had
                    plain = ev[1] in self.plain_imports
                    if sync:
                        s = (IMPORT_READS if plain else ALLREGS) & dirty(st)
                        spill(st, s)
                    if noreturn_ev(ev):
                        if sync:
                            record(n, ei, s, 0)
                        return None
                    written(st, IMPORT_CLOBBER)
                    if sync:
                        reload = IMPORT_CLOBBER if plain else ALLREGS
                        spill(st, reload)
                        record(n, ei, s, reload)
                elif k == 'setjmp':
                    eh_edge(st)
                    if sync:
                        s = ALLV & dirty(st)
                    written(st, CLOBBER)
                    if sync:
                        spill(st, ALLV)
                        record(n, ei, s, ALLV)
                elif k == 'unimpl':
                    if sync:
                        record(n, ei, ALLV & dirty(st))
                    return None
                elif k in ('unr', 'trap'):
                    return None
                elif k == 'ret':
                    if sync:
                        record(n, ei, dirty(st) & retmask)
                    exit_(lv, lv[14])
                    return None
                elif k == 'retto':
                    if sync:
                        record(n, ei, dirty(st) & retmask)
                    exit_(lv, rp)
                    return None
                elif k in ('tail', 'taili'):
                    tail(n, ei, ev, st)
                    return None
                elif k == 'goto':
                    join_into(n.tgts[gi], st)
                    gi += 1
                    return None
            return st

        def tail(n, ei, ev, st):
            lv = st[0]
            if ev[0] == 'tail':
                sem, mw, mre, cons = callee(ev[1], ev[2])
                if sync:
                    record(n, ei, (ALLREGS | (sem & FLAGS) | self.demand(f)) & dirty(st))
                if noreturn_ev(('call',) + ev[1:]):
                    return
                recs = None if sync else exits_of(ev[1], ev[2])
                if recs is not None:
                    # the callee returns for this function, so its exits are this function's
                    # (which also resolves code shared between functions that pops the frame
                    # of whoever branched to it)
                    apply_exits(recs, st, None)
                    return
                target = lv[14] if not mre else None
            else:
                if sync:
                    record(n, ei, (ALLREGS | self.demand(f)) & dirty(st))
                mw, target = CLOBBER, lv[14]
            written(st, mw)
            exit_(lv, target)

        for r in roots:
            join_into(r, [list(ORIG), {}, [ORIGS] * 20 if sync else None])
        nn = len(nodes)
        while work:
            i = work.pop()
            inwork.discard(i)
            st = copy(instate[i])
            while True:
                reached.add(i)
                n = nodes[i]
                if n.cond:
                    t = taken(i, n, copy(st))
                    if t is not None:
                        join(st, t)
                else:
                    st = taken(i, n, st)
                if st is None:
                    break
                i += 1
                if i >= nn:
                    break
                if i in starts:
                    join_into(i, st)
                    break
        res['reached'] = reached
        return res

    # --- backward: semantic liveness (sem_in, demand) or C-level reads (prologue loads) ---
    def backward(self, fx, mode):
        nodes, starts = fx.nodes, fx.starts
        f = fx.start
        callee, noreturn_ev = self.callee, self.noreturn_ev
        mwu = 0
        for k in fx.root_keys:
            mwu |= self.mw.get(k, 0)
        dem = self.demand(f)
        retuse = (mwu & ALLREGS) | (mwu & dem)
        live_in = {}
        contrib = collections.defaultdict(int)
        out = {}
        pads = fx.pad_idx
        reached = fx.reached if mode == 'P' else None

        def eh_live():
            m = 0
            for p in pads:
                m |= live_in.get(p, 0)
            return m & CALLEE_SAVED

        def ev_bwd(i, n, ei, ev, t):
            k = ev[0]
            if mode == 'P':
                masks = n.masks[ei] if n.masks else None
                if masks is None:
                    return 0
                if k == 'call':
                    s, l, p = masks
                    return s | ((t | p) & ~l)
                if k == 'calli':
                    s, l, p = masks
                    return s | ((t | p) & ~l)
                if k in ('import', 'setjmp'):
                    return masks[0] | (t & ~masks[1])
                if k == 'unimpl':
                    return masks[0]
                if k == 'ret':
                    return masks[0] | (1 << 14)
                if k in ('retto', 'tail', 'taili'):
                    return masks[0]
                return 0
            if k == 'call':
                sem, mw, mre, cons = callee(ev[1], ev[2])
                la = 0 if noreturn_ev(ev) else t
                if mre:
                    la |= retuse
                if la & FLAGS & mw:
                    contrib[self.owner[ev[2]]] |= la & FLAGS & mw
                return sem | (la & ~(mw & ~sem)) | eh_live()
            if k == 'calli':
                return ALLREGS | eh_live()
            if k == 'import':
                if ev[1] in self.plain_imports and not noreturn_ev(ev):
                    return IMPORT_READS | (t & ~IMPORT_CLOBBER) | eh_live()
                return ALLREGS | (0 if noreturn_ev(ev) else t & FLAGS) | eh_live()
            if k in ('setjmp', 'unimpl'):
                # both store every register, but neither reads the flags (setjmp saves r4-r13;
                # rt_unimplemented stops the game)
                return ALLREGS | eh_live()
            if k == 'ret':
                return (1 << 14) | retuse
            if k == 'retto':
                return retuse
            if k == 'tail':
                sem, mw, mre, cons = callee(ev[1], ev[2])
                if dem:
                    contrib[self.owner[ev[2]]] |= dem
                return sem | (retuse & ~mw)
            if k == 'taili':
                return ALLREGS | dem
            return 0

        def node_bwd(i, n, after):
            if reached is not None and i not in reached:
                return 0  # not emitted
            if n.switch:
                t = 0
                gi = 0
                for ei, ev in enumerate(n.evs):
                    if ev[0] == 'goto':
                        t |= live_in.get(n.tgts[gi], 0)
                        gi += 1
                    elif ev[0] in ('tail', 'taili'):
                        t |= ev_bwd(i, n, ei, ev, 0)
            else:
                ends = any(e[0] in ENDS or self.noreturn_ev(e) for e in n.evs)
                t = 0 if ends else after
                gi = len(n.tgts)
                for ei in range(len(n.evs) - 1, -1, -1):
                    ev = n.evs[ei]
                    if ev[0] == 'goto':
                        gi -= 1
                        t = live_in.get(n.tgts[gi], 0)
                    else:
                        t = ev_bwd(i, n, ei, ev, t)
            t = n.uses | (t & ~n.defs)
            if n.cond:
                t |= after | n.uses
            return t

        # blocks: a start runs until the next start or a node with no fall-through
        order = sorted(starts)
        spans = []
        for bi, s in enumerate(order):
            e = order[bi + 1] if bi + 1 < len(order) else len(nodes)
            spans.append((s, e))
        changed = True
        while changed:
            changed = False
            for s, e in reversed(spans):
                live = live_in.get(e, 0) if e < len(nodes) else 0
                for i in range(e - 1, s - 1, -1):
                    live = node_bwd(i, nodes[i], live)
                if live != live_in.get(s, 0) or s not in live_in:
                    if live_in.get(s) != live:
                        live_in[s] = live
                        changed = True
        out['live_in'] = live_in
        out['contrib'] = contrib
        return out

    # --- driver ---------------------------------------------------------------
    def postorder(self):
        """Functions with callees before callers (cycles broken arbitrarily)."""
        callees = collections.defaultdict(set)
        for o, cs in self.callers.items():
            for c in cs:
                callees[c].add(o)
        seen, order = set(), []
        for root in self.irs:
            if root in seen:
                continue
            stack = [(root, iter(callees.get(root, ())))]
            seen.add(root)
            while stack:
                node, it = stack[-1]
                nxt = next(it, None)
                if nxt is None:
                    order.append(node)
                    stack.pop()
                elif nxt not in seen and nxt in self.irs:
                    seen.add(nxt)
                    stack.append((nxt, iter(callees.get(nxt, ()))))
        return order

    def run(self):
        order = [f for f in self.postorder() if self.irs[f].nodes is not None]
        # exits (hence may_write / mre): least fixed point from "never returns". An entry
        # that ends up with no exit at all although it is not a known noreturn function is
        # taken as unknown instead (it may well return: the analysis gives up on a path only
        # when it proves it infeasible, but a wrong "never returns" would drop the reloads
        # after every call to it), and its callers are redone.
        work = collections.deque(order)
        inq = set(order)
        passes = 0
        while True:
            self.v_drain(work, inq, order, passes)
            passes = self.v_passes
            # (an entry whose paths all end in noreturn calls or traps really never returns)
            empty = [k for k in self.root_keys if not self.exits.get(k) and k in self.pruned
                     and k not in self.unknown]
            if not empty:
                break
            # one wave at a time from the bottom: an entry that only lacks exits because it
            # calls another one of these may get its exits once that one is unknown
            es = set(empty)
            leaves = [k for k in empty if not any(
                ev[0] in ('call', 'tail') and ev[2] in es and ev[2] != k
                for n in self.irs[self.func_of_key(k)].nodes for ev in n.evs)]
            for k in leaves or empty:
                self.unknown.add(k)
                self.mw[k], self.mre[k] = ALLV, False
                o = self.func_of_key(k)
                for c in self.callers.get(o, ()):
                    if c not in inq and self.irs[c].nodes is not None:
                        inq.add(c)
                        work.append(c)
        # What an indirect call may change: anything a function reachable through a pointer
        # may change (kernel helpers and imports: AAPCS's r0-r3, r12, lr; the flags always,
        # __kuser_cmpxchg returns C and Z in the struct). An indirect call reloads just that.
        # Compiled code keeps r4-r11 and sp (AAPCS; the values above already rely on it at
        # indirect calls). A target whose summary does not show that (Thumb code is not
        # analysed; push/pop through computed addresses defeats the analysis) is called
        # through a wrapper that checks it at run time (rt_aapcs_checked, table.c) instead of
        # making every indirect call reload everything.
        ind, self.aapcs_checked = CLOBBER, set()
        thumb, mids = set(self.g.thumb), self.g.mid_entries
        for k in self.dispatchable:
            if k in thumb or k in self.irs or k in mids:
                mw = self.callee('e' if k in mids and k not in self.irs else 'f', k)[1]
                if mw & ALLREGS & ~CLOBBER_REGS:
                    self.aapcs_checked.add(k)
                else:
                    ind |= mw
        self.ind_mw = ind & ~0x8000
        self.l_phase(order)

    def v_drain(self, work, inq, order, passes):
        while work:
            f = work.popleft()
            inq.discard(f)
            fx = self.irs[f]
            passes += 1
            if self.verbose and passes % 10000 == 0:
                print(f'  value pass {passes}, {len(work)} queued', flush=True)
            if passes == 10 * len(order):
                # not settling: merge every entry's records into one, which must settle
                print(f'  register flow: value analysis not settling after {passes} passes, merging')
                for key in self.exits:
                    self.exits[key], self.exit_level[key] = merge_exits(self.exits[key], 2)
                    self.exit_level[key] = 2
            changed = False
            for r, key in zip(fx.roots, fx.root_keys):
                if key in self.unknown:
                    continue
                res = self.forward(fx, [r], False)
                if res.get('pruned'):
                    self.pruned.add(key)
                old = self.exits.get(key, frozenset())
                new, self.exit_level[key] = merge_exits(old | res['exits'], self.exit_level.get(key, 0))
                if new != old:
                    self.exits[key] = frozenset(new)
                    mw, mre = 0, False
                    for lv, target in new:
                        for b in range(20):
                            if lv[b] != ORIG[b]:
                                mw |= 1 << b
                        mre = mre or target != ORIG[14]
                    self.mw[key], self.mre[key] = mw & ~0x8000, mre
                    changed = True
            if changed:
                for c in self.callers.get(f, ()):
                    if c not in inq and self.irs[c].nodes is not None:
                        inq.add(c)
                        work.append(c)
        self.v_passes = passes

    def l_phase(self, order):
        # sem_in / demand: least fixed point from nothing
        work = collections.deque(order)
        inq = set(order)
        passes = 0
        while work:
            f = work.popleft()
            inq.discard(f)
            fx = self.irs[f]
            passes += 1
            if self.verbose and passes % 10000 == 0:
                print(f'  liveness pass {passes}, {len(work)} queued', flush=True)
            out = self.backward(fx, 'L')
            push = set()
            for r, key in zip(fx.roots, fx.root_keys):
                s = self.sem.get(key, 0) | out['live_in'].get(r, 0)
                if s != self.sem.get(key, 0):
                    self.sem[key] = s
                    push |= self.callers.get(f, set())
            for o, m in out['contrib'].items():
                if self.dem[o] | m != self.dem[o]:
                    self.dem[o] |= m
                    push.add(o)
            for c in push:
                if c not in inq and c in self.irs and self.irs[c].nodes is not None:
                    inq.add(c)
                    work.append(c)
        self.l_passes = passes
        # what each site moves, then what each prologue loads
        for f in order:
            fx = self.irs[f]
            if fx.style != 'new':
                continue
            res = self.forward(fx, fx.roots, True)
            fx.reached = res['reached']
            out = self.backward(fx, 'P')
            m = 0
            for r in fx.roots:
                m |= out['live_in'].get(r, 0)
            fx.loads = m & ~0x8000

    def report(self):
        """Statistics: what the generated plumbing moves compared with the all-register macros
        (19 stores before and 19 loads after every call, 19 stores at every return)."""
        pc = lambda m: bin(m).count('1')
        calls = collections.Counter()
        st = collections.Counter()
        fewer = 0
        new = [fx for fx in self.irs.values() if fx.nodes is not None and fx.style == 'new']
        for fx in new:
            for n in fx.nodes:
                for ev, masks in zip(n.evs, n.masks or [None] * len(n.evs)):
                    if masks is None:
                        continue
                    k = ev[0]
                    if k in ('call', 'calli', 'import', 'setjmp'):
                        kind = 'direct' if k == 'call' else k
                        if k == 'call' and self.owner.get(ev[2], ev[2]) in self.overrides:
                            kind = 'host'  # hooks and replacements: all registers
                        calls[kind] += 1
                        st[kind + ' stores'] += pc(masks[0])
                        st[kind + ' loads'] += pc(masks[1])
                        fewer += pc(masks[0]) < 19
                    elif k in ('ret', 'retto', 'tail', 'taili'):
                        calls['return'] += 1
                        st['return stores'] += pc(masks[0])
            st['prologue loads'] += pc(fx.loads)
        old = [fx for fx in self.irs.values() if fx.stub is None and fx.style != 'new']
        nsite = sum(calls[k] for k in ('direct', 'host', 'calli', 'import', 'setjmp'))
        print(f'register flow: {len(new)} functions analysed and emitted with exact lists, '
              f'{len(old)} with the all-register macros ({sum(fx.kind == "thumb" for fx in old)} Thumb, '
              f'{len(self.fallbacks)} not analysable), '
              f'{sum(fx.stub is not None for fx in self.irs.values())} host replacements')
        print(f'  fixed point: {self.v_passes} value passes, {self.l_passes} liveness passes')
        for k in ('direct', 'host', 'calli', 'import', 'setjmp'):
            if calls[k]:
                print(f'  {k:7} calls {calls[k]:7}: {st[k + " stores"] / calls[k]:5.2f} stores, '
                      f'{st[k + " loads"] / calls[k]:5.2f} loads per call (was 19 / 19)')
        if nsite:
            print(f'  all calls     {nsite:7}: {sum(st[k + " stores"] for k in calls if k != "return") / nsite:5.2f} '
                  f'stores per call; {fewer} of them store fewer than 19')
        if calls['return']:
            print(f'  returns/tails {calls["return"]:7}: {st["return stores"] / calls["return"]:5.2f} stores (was 19)')
        print(f'  prologues: {st["prologue loads"] / max(1, len(new)):5.2f} loads (was 19)')
        keys = sorted(self.root_keys)
        mre = [k for k in keys if self.mre.get(k)]
        lost = [k for k in keys if k in self.unknown or any(t is None for _, t in self.exits.get(k, ()))]
        odd = [k for k in keys if self.mw.get(k, 0) & ALLREGS & ~CLOBBER_REGS]
        odd_ind = [k for k in odd if k in self.dispatchable]
        flags_in = [k for k in keys if self.sem.get(k, 0) & FLAGS]
        print(f'  entries: {len(keys)}; may return elsewhere: {len(mre)} (to an unknown address: {len(lost)}); '
              f'may change r4-r11/sp: {len(odd)} '
              f'({len(odd_ind)} of them reachable through pointers or symbols); without any exit, '
              f'so summarised as unknown: {len(self.unknown)}')
        print(f'  paths returning through a value loaded from non-stack memory (taken as infeasible): '
              f'{len(self.loaded_returns)} functions ' +
              ' '.join(f'{f:08x}' for f in sorted(self.loaded_returns)[:12]))
        print(f'  indirect calls reload: {var_names(self.ind_mw)}; targets checked at run time to keep '
              f'r4-r11/sp: {len(self.aapcs_checked)} ({len(self.aapcs_checked & set(self.g.thumb))} Thumb)')
        print(f'  read flags at entry: {len(flags_in)}; return flags: {sum(1 for f in self.dem if self.dem[f])}; '
              f"(flags are never stored at indirect calls)")
        flag_ind = [k for k in flags_in if k in self.dispatchable]
        print(f"  pointer-reachable entries the analysis sees reading flags at entry (not passed: AAPCS): {len(flag_ind)} " +
              ' '.join(f'{k:08x}:{var_names(self.sem[k] & FLAGS).replace(" ", "")}' for k in flag_ind[:12]))
        for k in odd_ind[:20]:
            print(f'    {k:08x} {self.g.img.syms.get(k, "?")}: may change {var_names(self.mw[k] & ALLREGS & ~CLOBBER_REGS)}')
        for f, why in self.fallbacks[:20]:
            print(f'    not analysed {f:08x}: {why}')

    # --- rendering ----------------------------------------------------------------
    def render(self, fx):
        if fx.stub is not None:
            return fx.stub
        if fx.style != 'new' or not x_wanted(fx.start):
            return render_old(fx)
        lines = [f'/* {fx.name} */' if fx.name else '']
        if fx.eh:
            lines += ['#undef FRAME_EXIT', '#define FRAME_EXIT rt_eh_pop(&eh_)']
        fname = f'f_{fx.start:08x}' + ('_orig' if fx.wrap else '')
        lines += [f'void {fname}(Cpu *cpu, uint32_t entry) {{', f'    PROLOGUE_X({var_list(fx.loads, "LV")});']
        if fx.eh:
            lines.append(f'    EH_BEGIN(0x{fx.start:08x}u, 0x{fx.end:08x}u);')
        if fx.entries:
            lines.append('    switch (entry) {')
            for e in fx.entries:
                lines.append(f'    case 0x{e:08x}u: goto L_{e:08x};')
            lines.append('    }')
        reached = fx.reached
        prev = None
        for i, n in enumerate(fx.nodes):
            if n.addr is not None and n.addr != prev and n.addr in fx.labels:
                lines.append(f'L_{n.addr:08x}:;')
            if n.addr is not None:
                prev = n.addr
            here = n.addr if n.addr is not None else prev
            if i not in reached:
                continue  # dead: every C path into it ends in a trap or a return first
            text = self.render_node(n, here)
            lines.append(f'    {text}' + (f' /*@{n.addr:x}*/' if n.addr is not None else ''))
        lines.append('}')
        if fx.eh:
            lines += ['#undef FRAME_EXIT', '#define FRAME_EXIT ((void)0)']
        if fx.wrap:
            lines += [f'void {fx.wrap}(Cpu *cpu, uint32_t entry, GuestFn original);',
                      f'void f_{fx.start:08x}(Cpu *cpu, uint32_t entry) {{ {fx.wrap}(cpu, entry, {fname}); }}']
        return '\n'.join(lines)

    def render_node(self, n, here):
        if not n.evs:
            return n.text
        out, pos = [], 0
        for ei, (ev, (a, b)) in enumerate(zip(n.evs, n.spans)):
            out.append(n.text[pos:a])
            pos = b
            src = n.text[a:b]
            k = ev[0]
            masks = n.masks[ei] if n.masks else None
            if k == 'goto':
                out.append(src)
                continue
            if k == 'trap':
                out.append(f'TRAP_X({src})')
                continue
            if k == 'unr':
                out.append(src.replace('UNREACHABLE(', 'UNREACHABLE_X('))
                continue
            if masks is None:
                out.append(f'NORETURN_X(0x{LOAD_BASE + here:08x}u)')
                continue
            m = EVENT_RE.fullmatch(src)
            sv = [var_list(x, 'SV' if j != 1 or k in ('ret', 'retto', 'tail', 'taili', 'unimpl') else 'LV')
                  for j, x in enumerate(masks)]
            if k == 'call':
                # an analysed callee that always returns to its lr: no return check (CALLN_X)
                _, _, mre, cons = self.callee(ev[1], ev[2])
                if not mre and not cons:
                    out.append(f'CALLN_X({m.group("cf")}, {sv[0]}, {sv[1]})')
                else:
                    out.append(f'CALL_X({m.group("cf")}, {m.group("cr")}, {sv[0]}, {sv[1]}, {sv[2]})')
            elif k == 'calli':
                out.append(f'CALLI_X({m.group("ct")}, {m.group("cir")}, {sv[0]}, {sv[1]}, {sv[2]})')
            elif k == 'tail':
                out.append(f'TAIL_X({m.group("tf")}, {sv[0]})')
            elif k == 'taili':
                out.append(f'TAILI_X({m.group("tt")}, {sv[0]})')
            elif k == 'import':
                out.append(f'IMPORT_X({m.group("ii")}, {sv[0]}, {sv[1] if len(sv) > 1 else ""})')
            elif k == 'setjmp':
                out.append(f'SETJMP_X({sv[0]}, {sv[1]})')
            elif k == 'ret':
                out.append(f'RET_X({sv[0]})')
            elif k == 'retto':
                out.append(f'RET_TO_X({m.group("rp")}, {sv[0]})')
            elif k == 'unimpl':
                out.append(f'UNIMPL_X({m.group("ua")}, {sv[0]})')
            if self.noreturn_ev(ev):
                out.append(f'; NORETURN_X(0x{LOAD_BASE + here:08x}u)')
        out.append(n.text[pos:])
        return ''.join(out)


def x_ranges():
    """NOVA3_X_ONLY=lo-hi[,lo-hi...] (hex link addresses): only functions starting there use
    the _X forms, the others the all-register macros (summaries unchanged; both keep the Cpu
    struct right at boundaries, so they mix). For bisecting a translation problem."""
    spec = os.environ.get('NOVA3_X_ONLY')
    if not spec:
        return None
    return [tuple(int(x, 16) for x in part.split('-')) for part in spec.split(',') if part]


X_RANGES = x_ranges()


def x_wanted(start):
    return X_RANGES is None or any(lo <= start < hi for lo, hi in X_RANGES)


def render_old(fx):
    """Every register through the Cpu struct at every boundary (Thumb code, fallbacks, and
    standalone use by tests/difftest.py)."""
    if fx.stub is not None:
        return fx.stub
    pfx = 't' if fx.kind == 'thumb' else 'f'
    lines = [(f'/* {fx.name} (Thumb) */' if fx.kind == 'thumb' else f'/* {fx.name} */') if fx.name else '']
    if fx.eh:
        lines += ['#undef FRAME_EXIT', '#define FRAME_EXIT rt_eh_pop(&eh_)']
    fname = f'{pfx}_{fx.start:08x}' + ('_orig' if fx.wrap else '')
    lines += [f'void {fname}(Cpu *cpu, uint32_t entry) {{', '    PROLOGUE;']
    if fx.eh:
        lines.append(f'    EH_BEGIN(0x{fx.start:08x}u, 0x{fx.end:08x}u);')
    if fx.entries:
        lines.append('    switch (entry) {')
        for e in fx.entries:
            lines.append(f'    case 0x{e:08x}u: goto L_{e:08x};')
        lines.append('    }')
    stmts = fx.stmts if fx.stmts is not None else [(n.addr, n.text) for n in fx.nodes]
    prev = None
    for s in stmts:
        addr, text = s[0], s[1]
        if addr is not None and addr != prev and addr in fx.labels:
            lines.append(f'L_{addr:08x}:;')
        if addr is not None:
            prev = addr
        lines.append(f'    {text}' + (f' /*@{addr:x}*/' if addr is not None else ''))
    lines.append('}')
    if fx.eh:
        lines += ['#undef FRAME_EXIT', '#define FRAME_EXIT ((void)0)']
    if fx.wrap:
        lines += [f'void {fx.wrap}(Cpu *cpu, uint32_t entry, GuestFn original);',
                  f'void f_{fx.start:08x}(Cpu *cpu, uint32_t entry) {{ {fx.wrap}(cpu, entry, {fname}); }}']
    return '\n'.join(lines)


class Generator:
    def __init__(self):
        self.img = Image()
        self.mid_entries = set()
        self.inline_helpers = {self.img.sym_by_name[n]: code for n, code in INLINE_HELPERS.items()
                               if n in self.img.sym_by_name}

    def plan(self, subset=None):
        an = Analysis(self.img)
        starts = an.discover()
        self.thumb = an.thumb
        self.starts = starts
        self.func_set = set(starts)
        self.import_index = {n: i for i, n in enumerate(self.img.imports)}
        self.entries_by_func = collections.defaultdict(set)
        self.pointer_entries = set()  # code pointers that land inside another function's code
        self.code_ptr_targets = set()  # every PC-relative code address (indirect call targets)
        # Fixed point over reachable code: ARM->Thumb `blx` targets and PC-relative code
        # pointers (`ldr rY,[pc]; add rX,pc,rY` / `add|sub rX,pc,#imm`, i.e. callbacks such as
        # FreeType's allocator) reveal functions that have no symbol, unwind entry or BL caller.
        known = set(self.thumb)
        while True:
            ends = starts[1:] + [self.img.text_end]
            self.starts = starts
            self.ranges = dict(zip(starts, ends))
            start_set = set(starts)
            candidates = set()
            for f in starts:
                fg = FuncGen(self, f, self.ranges[f])
                fg.explore()
                for a, t in self.code_pointers(fg):
                    self.code_ptr_targets.add(t & ~1)
                    if t & 1:
                        h = struct.unpack_from('<H', self.img.text, (t & ~1) - self.img.text_addr)[0]
                        if (t & ~1) not in known and h & 0xfe00 == 0xb400:
                            known.add(t & ~1)
                    elif t % 4 == 0 and t not in start_set and t not in self.pointer_entries:
                        candidates.add(t)
            fresh = set()
            for t in candidates:
                owner = self.func_of(t) if t >= starts[0] else None
                if owner is not None:
                    fg = FuncGen(self, owner, self.ranges[owner])
                    fg.explore()
                    if t in fg.reach:
                        self.pointer_entries.add(t)  # mid-function target: callable entry, no split
                        continue
                fresh.add(t)
            if not fresh:
                break
            starts = sorted(start_set | fresh)
        for t in self.pointer_entries:
            self.entries_by_func[self.func_of(t)].add(t)
        self.thumb = sorted(known)
        # Thumb functions end at the next function of either kind.
        bounds = sorted(set(starts) | known | {v & ~1 for v in self.img.syms})
        self.thumb_set = set(self.thumb)
        self.thumb_ranges = {}
        for t in self.thumb:
            i = bisect.bisect_right(bounds, t)
            self.thumb_ranges[t] = bounds[i] if i < len(bounds) else self.img.text_end
        self.thumb_bodies = []
        # C++ landing pads are entered from the runtime (runtime/eh.cpp): they become
        # function entries, and their functions register an EH catch frame.
        self.eh_funcs = set()
        self.eh_pads = collections.defaultdict(set)
        for fn, pads in self.img.landing_pads().items():
            if fn in self.thumb_set:
                continue  # Thumb (boost.thread) landing pads are not supported
            for pad in pads:
                owner = self.func_of(pad) if starts and pad >= starts[0] else None
                if owner != fn:
                    continue  # pad outside its function's recompiled range: not a real ARM pad
                self.entries_by_func[owner].add(pad)
                self.eh_pads[owner].add(pad)
                self.eh_funcs.add(owner)
        self.override_addrs = {self.img.sym_by_name[n] & ~1: h for n, h in OVERRIDES.items() if n in self.img.sym_by_name}
        if subset:
            self.selected = subset
        else:
            self.selected = starts

    def code_pointers(self, fg):
        """(insn addr, target) for blx immediates and PC-relative addresses computed into .text."""
        lit = {}
        for a in sorted(fg.reach):
            w = fg.reach[a]
            k, t = branch_info(w, a)
            target = None
            if k == 'blx':
                target = t
            elif w & 0x0f7f0000 == 0x051f0000 and w >> 28 != 0xf:  # ldr rt, [pc, #+/-imm]
                lit_addr = a + 8 + ((w & 0xfff) if w & (1 << 23) else -(w & 0xfff))
                if self.img.in_text(lit_addr):
                    lit[(w >> 12) & 15] = self.img.word(lit_addr)
                continue
            elif w & 0x0fff0ff0 == 0x008f0000:  # add rd, pc, rm
                rm = w & 15
                if rm in lit:
                    target = (a + 8 + lit[rm]) & 0xffffffff
            elif w & 0x0fbf0000 == 0x028f0000 or w & 0x0fbf0000 == 0x024f0000:  # add/sub rd, pc, #imm (adr)
                rot = ((w >> 8) & 15) * 2
                imm = w & 0xff
                imm = ((imm >> rot) | (imm << (32 - rot))) & 0xffffffff if rot else imm
                target = (a + 8 + (imm if (w >> 21) & 0xf == 4 else -imm)) & 0xffffffff
            if k in ('bl', 'blx'):
                lit.clear()  # calls clobber argument registers
            if target is not None and self.img.in_text(target & ~1):
                yield a, target

    def resolve(self, s):
        if s.startswith('0x'):
            return int(s, 16)
        if s in self.img.sym_by_name:
            return self.img.sym_by_name[s] & ~1
        hits = [v for n, v in self.img.sym_by_name.items() if s in n]
        if len(hits) != 1:
            raise SystemExit(f'{s}: {len(hits)} matches')
        return hits[0] & ~1

    def callee_closure(self, roots):
        """Roots plus every function they reach through direct calls/tail branches."""
        seen, work = set(), list(roots)
        while work:
            f = work.pop()
            if f in seen:
                continue
            seen.add(f)
            fg = FuncGen(self, f, self.ranges[f])
            fg.explore()
            for a, w in fg.reach.items():
                k, t = branch_info(w, a)
                if k in ('b', 'bl') and self.img.in_text(t) and not (f <= t < self.ranges[f]):
                    work.append(self.func_of(t))
        return sorted(seen)

    def func_of(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        return self.starts[i]

    def run(self, outdir, subset_names):
        self.plan()
        subset = [self.resolve(s) for s in subset_names] if subset_names else None
        if subset:
            subset = self.callee_closure(subset)
        selected = subset or self.starts
        self.emit_set = set(selected)
        self.mid_entries |= {t for t in self.pointer_entries if self.func_of(t) in self.emit_set}
        os.makedirs(outdir, exist_ok=True)
        unsupported = collections.Counter()
        irs = {}
        for f in selected:
            fg = FuncGen(self, f, self.ranges[f])
            try:
                irs[f] = fg.lower()
            except Exception as ex:
                raise RuntimeError(f'function {f:#x}: {ex}')
            unsupported.update(fg.unsupported)
        # Thumb code before the fixed point below: its blx calls can reach ARM code after a
        # function's last instruction (the linker's Thumb->ARM long-branch veneers), entries
        # that function must handle like any other mid-function entry.
        thumb = self.thumb if subset is None else []
        for t in thumb:
            fg = ThumbFuncGen(self, t, self.thumb_ranges[t])
            irs[t] = fg.lower()
            unsupported.update(fg.unsupported)
        # Lowering discovers calls into the middle of functions; lower those functions again
        # with the new entry points until no new entries appear (code reached from a new
        # entry can branch to further mid-function targets).
        done = set()
        while True:
            fresh = self.mid_entries - done
            if not fresh:
                break
            done |= fresh
            for e in fresh:
                self.entries_by_func[self.func_of(e)].add(e)
            for f in {self.func_of(e) for e in fresh}:
                if f in irs:
                    irs[f] = FuncGen(self, f, self.ranges[f]).lower()
        flow = Flow(self, irs)
        flow.run()
        self.aapcs_checked = flow.aapcs_checked  # table.c wraps these
        bodies = [(f, flow.render(irs[f])) for f in selected]
        self.thumb_bodies = [flow.render(irs[t]) for t in thumb]
        self.write(outdir, bodies, selected, subset is not None)
        total = sum(unsupported.values())
        print(f'{len(bodies)} functions, {len(self.mid_entries)} mid-entries, {total} unsupported insns')
        for k, v in unsupported.most_common(25):
            print(f'  {v:7}  {k}')
        flow.report()

    def code_file(self, bodies):
        """One translation unit. It declares only what it references (no shared header),
        so a change elsewhere in the binary doesn't force recompiling this file."""
        text = '\n\n'.join(bodies)
        protos = sorted(set(re.findall(r'\b([ft]_[0-9a-f]{8})\b', text)))
        entries = sorted(set(re.findall(r'\be_([0-9a-f]{8})\b', text)))
        protos = sorted(set(protos) | {f'f_{self.func_of(int(e, 16)):08x}' for e in entries})  # e_ macros call their owner
        head = ['#define RECOMP_GENERATED 1', '#include "recomp.h"', '']
        head += [f'void {p}(Cpu *cpu, uint32_t entry);' for p in protos]
        head += [f'#define e_{e}(cpu, entry) f_{self.func_of(int(e, 16)):08x}(cpu, 0x{e}u)' for e in entries]
        return '\n'.join(head) + '\n\n' + text + '\n'

    def write(self, outdir, bodies, selected, partial):
        # Files cover fixed guest address windows and are only rewritten when their content
        # changes, so regenerating after a small fix recompiles only the affected files.
        groups = collections.defaultdict(list)
        for start, b in bodies:
            groups[start >> FILE_WINDOW_BITS].append(b)
        wanted = {f'code_{k:04x}.c' for k in groups} | {'code_thumb.c', 'funcs.h', 'table.c'}
        for name in os.listdir(outdir):
            if name.endswith(('.c', '.h')) and name not in wanted:
                os.remove(os.path.join(outdir, name))
        for k, chunk in groups.items():
            write_if_changed(os.path.join(outdir, f'code_{k:04x}.c'), self.code_file(chunk))
        write_if_changed(os.path.join(outdir, 'code_thumb.c'), self.code_file(self.thumb_bodies))
        self._write_header_and_table(sorted(selected), partial, outdir)

    def _write_header_and_table(self, sel, partial, outdir):
        with open_if_changed(os.path.join(outdir, 'funcs.h')) as f:
            f.write('#pragma once\n')
            for s in sel:
                f.write(f'void f_{s:08x}(Cpu *cpu, uint32_t entry);\n')
            for t in (self.thumb if self.thumb_bodies else ()):
                f.write(f'void t_{t:08x}(Cpu *cpu, uint32_t entry);\n')
            for e in sorted(self.mid_entries):
                f.write(f'#define e_{e:08x}(cpu, entry) f_{self.func_of(e):08x}(cpu, 0x{e:08x}u)\n')
            if partial:  # calls to functions outside the subset go through the dispatcher
                f.write('#define RECOMP_PARTIAL 1\n')
        with open_if_changed(os.path.join(outdir, 'table.c')) as f:
            f.write('#include "recomp.h"\n#include "funcs.h"\n\n')
            f.write(f'const uint32_t g_load_base = 0x{LOAD_BASE:08x}u;\n')
            rows = [(s, f's') for s in sel] + [(e, 'e') for e in self.mid_entries if self.func_of(e) in set(sel)]
            rows += [(t | 1, 't') for t in self.thumb] if self.thumb_bodies else []
            table = []
            for a, kind in sorted(rows):
                fn = {'s': f'f_{a:08x}', 't': f't_{a & ~1:08x}'}.get(kind) or f'f_{self.func_of(a):08x}'
                if a & ~1 in self.aapcs_checked:  # see Flow.run: indirect calls rely on AAPCS
                    f.write(f'static void ck_{a & ~1:08x}(Cpu *cpu, uint32_t link) '
                            f'{{ rt_aapcs_checked(cpu, {fn}, link); }}\n')
                    fn = f'ck_{a & ~1:08x}'
                table.append(f'    {{0x{LOAD_BASE + a:08x}u, {fn}, 0x{a & ~1:08x}u}},\n')
            f.write('\nconst FuncEntry g_funcs[] = {\n' + ''.join(table))
            f.write('};\nconst uint32_t g_func_count = sizeof(g_funcs) / sizeof(g_funcs[0]);\n')
            f.write('const char *const g_import_names[] = {\n')
            for n in self.img.imports:
                f.write(f'    "{n}",\n')
            f.write('};\nconst uint32_t g_import_count = sizeof(g_import_names) / sizeof(g_import_names[0]);\n')


def main(argv):
    if argv[0] == 'gen':
        Generator().run(argv[1], argv[2:])
    elif argv[0] == 'analyze':
        g = Generator()
        g.plan()
        os.makedirs(os.path.dirname(FUNCS_JSON), exist_ok=True)
        json.dump({'starts': g.starts, 'thumb': g.thumb}, open(FUNCS_JSON, 'w'))
        print(f'{len(g.starts)} ARM functions, {len(g.thumb)} Thumb functions')
    else:
        print(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
