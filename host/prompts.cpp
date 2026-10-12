// Tutorial prompts that name the PC controls, in every language.
//
// The first level teaches the controls with lines such as "Tap ^29^ to shoot!" (^29^: an
// icon of the touch button). With its joystick flag set, Application::GetString swaps in a
// controller line instead (MapJoystickTutorialString: "Press the Right Bumper to shoot!"),
// written for the Android pads of the time, so half of them name the wrong button here.
// Application::GetString is wrapped: such a line is replaced by one that names the player's
// own binding for the action, on the keyboard and mouse or on the gamepad, whichever was used
// last ("Press LMB to shoot!", "Drücke RT, um zu schießen!").
//
// The lines are found by string ID, which is the same in every language: an index into the
// current string table (CStrings; Application +0x18754: current, +0x18758: the menus' MAIN
// table, +0x1875c: the level's). The level table is the first level's when its line 0x27
// holds the fire icon (^29^) and line 0x2a the jump icon (^28^), as in every language. The sentences follow
// the game's own controller lines in each language, with the binding in place of the button.
#include "controls_menu.h"
#include "input.h"
#include "runtime.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

using settings::Action;

// Application +0x188c0: the language (Menu_ChangeLanguage; ADSGetLanguage gives the codes)
enum Lang { EN, FR, DE, SP, IT, JP, KR, CH, BR, RU, kLangs };

enum Kind { Key, Move, Look, SwitchWeapon };

struct Prompt {
    int ids[3];  // level-1 string IDs: touch line, controller line(s); 0 = none
    Action action;
    Kind kind;
    const char *text[kLangs];  // %s: the binding (Look: the mouse sentence)
    const char *pad[kLangs];   // Move, Look: the gamepad sentence
};

const Prompt kPrompts[] = {
    {{0x27, 0x140}, settings::Fire, Key,
     {"Press %s to shoot!", "Appuyez sur %s pour tirer !", "Drücke %s, um zu schießen!", "¡Pulsa %s para disparar!",
      "Premi %s per sparare!", "%sを押して撃て！", "%s을(를) 눌러 사격하세요!", "按%s射击！", "Aperte %s para atirar!",
      "Нажмите %s, чтобы выстрелить!"}},
    {{0x28, 0x141}, settings::Power, Key,
     {"Press %s to use the selected ability!", "Appuyez sur %s pour utiliser la compétence sélectionnée !",
      "Drücke %s, um die ausgewählte Fähigkeit zu benutzen!", "¡Pulsa %s para usar la habilidad seleccionada!",
      "Premi %s per usare l'abilità selezionata!", "%sを押して|アビリティを使え！", "%s을(를) 눌러 선택한 능력을 사용하세요!",
      "按%s使用所选能力！", "Aperte %s para usar a habilidade selecionada!",
      "Нажмите %s, чтобы использовать выбранную способность!"}},
    {{0x29, 0x142}, settings::MoveForward, Look,
     {"Move the mouse to look around!", "Déplacez la souris pour faire pivoter la caméra !",
      "Bewege die Maus, um die Kamera zu drehen!", "¡Mueve el ratón para girar la cámara!",
      "Muovi il mouse per spostare la visuale!", "マウスを動かして視点を動かせ！", "마우스를 움직여 시점을 움직이세요!",
      "移动鼠标旋转视角！", "Mova o mouse para girar a câmera!", "Двигайте мышь, чтобы вращать камеру!"},
     {"Use the right stick to look around!", "Utilisez le stick droit pour faire pivoter la caméra !",
      "Benutze den rechten Stick, um die Kamera zu drehen!", "¡Usa el joystick derecho para girar la cámara!",
      "Usa la levetta destra per spostare la visuale!", "右スティックで視点を動かせ！", "오른쪽 스틱으로 시점을 움직이세요!",
      "使用右摇杆旋转视角！", "Use a alavanca direita para girar a câmera!",
      "Для управления камерой используйте правый стик!"}},
    {{0x2a, 0x143}, settings::Jump, Key,
     {"Press %s to jump!", "Appuyez sur %s pour sauter !", "Drücke %s, um zu springen!", "¡Pulsa %s para saltar!",
      "Premi %s per saltare!", "%sを押して飛べ！", "%s을(를) 눌러 점프하세요!", "按%s跳跃！", "Aperte %s para pular!",
      "Нажмите %s, чтобы прыгнуть!"}},
    // "Sprint to cover!" (0x136) is the sprint line on a controller too
    {{0x2c, 0x144, 0x136}, settings::Sprint, Key,
     {"Press %s to sprint!", "Appuyez sur %s pour sprinter !", "Drücke %s, um zu sprinten!", "¡Pulsa %s para esprintar!",
      "Premi %s per scattare!", "%sを押して|スプリント！", "%s을(를) 눌러 전력 질주하세요!", "按%s冲刺！",
      "Aperte %s para correr!", "Нажмите %s, чтобы побежать!"}},
    {{0x2d, 0x145}, settings::Grenade, Key,
     {"Press %s to throw a grenade!", "Appuyez sur %s pour lancer une grenade !", "Drücke %s, um eine Granate zu werfen!",
      "¡Pulsa %s para lanzar una granada!", "Premi %s per lanciare una granata!", "%sを押して|グレネードを投げろ！",
      "%s을(를) 눌러 수류탄을 던지세요!", "按%s投掷手雷！", "Aperte %s para lançar uma granada!",
      "Нажмите %s, чтобы бросить гранату!"}},
    {{0x2e, 0x146}, settings::NextWeapon, SwitchWeapon,
     {"Press %s to switch weapons!", "Appuyez sur %s pour changer d'arme !", "Drücke %s, um die Waffe zu wechseln!",
      "¡Pulsa %s para cambiar de arma!", "Premi %s per cambiare arma!", "%sで武器変更！", "%s(으)로 무기를 교체하세요!",
      "按%s切换武器！", "Aperte %s para trocar de arma!", "Нажмите %s, чтобы переключить оружие!"}},
    {{0x2f, 0x147}, settings::Reload, Key,
     {"Press %s to reload!", "Appuyez sur %s pour recharger !", "Drücke %s, um nachzuladen!", "¡Pulsa %s para recargar!",
      "Premi %s per ricaricare!", "%sを押して|リロード！", "%s을(를) 눌러 재장전하세요!", "按%s装弹！",
      "Aperte %s para recarregar!", "Нажмите %s, чтобы перезарядить!"}},
    {{0x12e, 0x138}, settings::MoveForward, Move,
     {"Use %s to move!", "Utilisez %s pour vous déplacer !", "Benutze %s, um dich zu bewegen!", "¡Usa %s para moverte!",
      "Usa %s per muoverti!", "%sで移動！", "%s(으)로 이동하세요!", "使用%s移动！", "Use %s para se mover!",
      "Для перемещения используйте %s!"},
     {"Use the left stick to move!", "Utilisez le stick gauche pour vous déplacer !",
      "Benutze den linken Stick, um dich zu bewegen!", "¡Usa el joystick izquierdo para moverte!",
      "Usa la levetta sinistra per muoverti!", "左スティックで|移動！", "왼쪽 스틱으로 이동하세요!", "使用左摇杆移动！",
      "Use a alavanca esquerda para se mover!", "Для перемещения используйте левый стик!"}},
    {{0x12f, 0x139}, settings::Aim, Key,
     {"Press %s to switch to aim mode!", "Appuyez sur %s pour passer en mode visée !",
      "Drücke %s, um zur Zielansicht zu wechseln!", "¡Pulsa %s para cambiar al modo de precisión!",
      "Premi %s per passare alla modalità mirino!", "%sを押して|エイムモード！", "%s을(를) 눌러 조준 모드로 전환하세요!",
      "按%s切换至瞄准模式！", "Aperte %s para alterar para o modo de mira!", "Нажмите %s, чтобы прицелиться!"}},
    {{0x130, 0x13a}, settings::NextPower, Key,
     {"Press %s to switch powers!", "Appuyez sur %s pour changer de pouvoir !", "Drücke %s, um Extras zu wechseln!",
      "¡Pulsa %s para cambiar de poder!", "Premi %s per cambiare potere!", "%sを押して|能力を切り替えろ！",
      "%s을(를) 눌러 특수 능력을 바꾸세요!", "按%s切换能力！", "Aperte %s para alternar entre os poderes!",
      "Нажмите %s, чтобы выбрать способность!"}},
    {{0x131, 0x13b}, settings::Power, Key,
     {"Press %s to use the paralyze ability!", "Appuyez sur %s pour utiliser la compétence Paralysie !",
      "Drücke %s, um den Lähmer zu benutzen!", "¡Pulsa %s para usar la habilidad de parálisis!",
      "Premi %s per usare l'abilità Paralisi!", "%sを押して|パラライズアビリティ！", "%s을(를) 눌러 마비 능력을 사용하세요!",
      "按%s使用麻痹能力！", "Aperte %s para usar a habilidade 'paralisia'!", "Нажмите %s, чтобы парализовать врага!"}},
    {{0x132, 0x13c}, settings::Power, Key,
     {"Press %s to use the slow-time ability!", "Appuyez sur %s pour utiliser la compétence Ralenti !",
      "Drücke %s, um die Zeitverlangsamung zu benutzen!", "¡Pulsa %s para usar la habilidad de ralentizar el tiempo!",
      "Premi %s per usare l'abilità Rallentatore!", "%sを押して|スロータイムアビリティ！",
      "%s을(를) 눌러 시간 감속 능력을 사용하세요!", "按%s使用时间减缓能力！",
      "Aperte %s para usar a habilidade 'retardar o tempo'!", "Нажмите %s, чтобы замедлить время!"}},
    {{0x133, 0x13d}, settings::NextGrenade, Key,
     {"Press %s to switch grenades!", "Appuyez sur %s pour changer de grenades !", "Drücke %s, um die Granate zu wechseln!",
      "¡Pulsa %s para cambiar de granadas!", "Premi %s per cambiare granata!", "%sを押して|グレネード変更！",
      "%s을(를) 눌러 수류탄을 변경하세요!", "按%s切换手雷！", "Aperte %s para alternar entre as granadas!",
      "Нажмите %s, чтобы выбрать гранаты!"}},
    {{0x134, 0x13e}, settings::Grenade, Key,
     {"Press %s to place a sticky grenade!", "Appuyez sur %s pour poser une mine adhésive !",
      "Drücke %s, um die Haftgranate zu platzieren!", "¡Pulsa %s para colocar una mina pegajosa!",
      "Premi %s per piazzare una granata adesiva!", "%sを押して|スティッキーマイン設置！",
      "%s을(를) 눌러 접착 지뢰를 설치하세요!", "按%s放置粘性炸弹！", "Aperte %s para usar a mina adesiva!",
      "Нажмите %s, чтобы разместить мину-липучку!"}},
    {{0x135, 0x13f}, settings::Detonate, Key,
     {"Press %s to detonate mines!", "Appuyez sur %s pour faire sauter les mines !", "Drücke %s, um Minen zu zünden!",
      "¡Pulsa %s para detonar minas!", "Premi %s per detonare le granate!", "%sを押して|マインを起爆",
      "%s을(를) 눌러 지뢰를 폭파하세요!", "按%s引爆地雷！", "Aperte %s para detonar as minas!",
      "Нажмите %s, чтобы подорвать мину!"}},
    {{0x148}, settings::Interact, Key,
     {"Press %s to interact!", "Appuyez sur %s pour interagir !", "Drücke %s, um zu interagieren!",
      "¡Pulsa %s para interaccionar!", "Premi %s per interagire!", "%sを押して|アクション！", "%s을(를) 눌러 상호 작용하세요!",
      "按%s互动！", "Aperte %s para interagir!", "Нажмите %s для взаимодействия!"}},
};

// Chapter 3 (Lost Ark), line 0x12: "Touch your arm panel to activate the jetpack." (an objective,
// with the HUD's touch marker on the arm's console; the keys and the pad start it with Interact
// or Jump, see hooks.cpp). Its table is the one of 108 strings.
constexpr int kShipStrings = 108, kShipJetpack = 0x12;
const Prompt kJetpack = {{kShipJetpack}, settings::Interact, Key,
                         {"Press %s to activate the jetpack.", "Appuyez sur %s pour activer le jetpack.",
                          "Aktiviere das Jetpack mit %s.", "Pulsa %s para activar el propulsor.",
                          "Premi %s per attivare il jet pack.", "%sを押して|ジェットパックを|使え",
                          "%s을(를) 눌러 제트팩을 가동하세요.", "按%s开启喷气背包。",
                          "Aperte %s para ativar os propulsores.", "Нажмите %s, чтобы включить реактивный ранец."}};

// MAIN 0x787, "Press X to skip" (cinematics)
const Prompt kSkip = {{0x787}, settings::Jump, Key,
                      {"Press %s to skip", "Appuyez sur %s pour passer", "%s: Überspringen", "Pulsa %s para omitir",
                       "Premi %s per saltare", "%sを押して|スキップ", "%s: 건너뛰기", "按%s跳过", "Aperte %s para pular",
                       "Нажмите %s для пропуска"}};

const char *const kMouseWheel[kLangs] = {"Mouse Wheel", "molette", "Mausrad", "rueda del ratón", "rotellina del mouse",
                                         "マウスホイール", "마우스 휠", "鼠标滚轮", "roda do mouse", "колёсико мыши"};

// Mouse buttons 1-3 (left, middle, right) as the sentences use them; key and pad button
// names stay as settings::input_label gives them.
const char *const kMouseButtons[kLangs][3] = {
    {"Left Mouse Button", "Middle Mouse Button", "Right Mouse Button"},
    {"le bouton gauche de la souris", "le bouton central de la souris", "le bouton droit de la souris"},
    {"die linke Maustaste", "die mittlere Maustaste", "die rechte Maustaste"},
    {"el botón izquierdo del ratón", "el botón central del ratón", "el botón derecho del ratón"},
    {"il tasto sinistro del mouse", "il tasto centrale del mouse", "il tasto destro del mouse"},
    {"マウスの左ボタン", "マウスの中ボタン", "マウスの右ボタン"},
    {"마우스 왼쪽 버튼", "마우스 가운데 버튼", "마우스 오른쪽 버튼"},
    {"鼠标左键", "鼠标中键", "鼠标右键"},
    {"o botão esquerdo do mouse", "o botão do meio do mouse", "o botão direito do mouse"},
    {"левую кнопку мыши", "среднюю кнопку мыши", "правую кнопку мыши"},
};

// The action's first control on the device, or "".
std::string binding(Action a, bool pad, Lang lang) {
    for (int code : input::config().bindings[a]) {
        if ((code >= settings::kPadBase) != pad) continue;
        const int mouse = code - settings::kMouseBase;
        if (code >= settings::kMouseBase && code < settings::kPadBase && mouse >= 1 && mouse <= 3)
            return kMouseButtons[lang][mouse - 1];
        return settings::input_label(code, input::config().swap_ab_xy);
    }
    return "";
}

std::string format(const char *pattern, const std::string &binding) {
    std::string out = pattern;
    if (const size_t at = out.find("%s"); at != std::string::npos) out.replace(at, 2, binding);
    return out;
}

// The sentence for the device used last, or "" to keep the game's line (action unbound).
std::string sentence(const Prompt &p, Lang lang, bool pad) {
    switch (p.kind) {
    case Look:
        return pad ? p.pad[lang] : p.text[lang];
    case Move: {
        if (pad) return p.pad[lang];
        std::string keys;
        for (Action a : {settings::MoveForward, settings::MoveLeft, settings::MoveBack, settings::MoveRight}) {
            const std::string k = binding(a, false, lang);
            if (k.empty()) return "";
            keys += (keys.empty() ? "" : " ") + k;
        }
        return format(p.text[lang], keys);
    }
    case SwitchWeapon:
        if (!pad) {  // the wheel switches weapons too
            const std::string key = binding(p.action, false, lang);
            return format(p.text[lang], key.empty() ? kMouseWheel[lang] : key + " / " + kMouseWheel[lang]);
        }
        break;
    case Key:
        break;
    }
    std::string control = binding(p.action, pad, lang);
    if (control.empty()) control = binding(p.action, !pad, lang);
    return control.empty() ? "" : format(p.text[lang], control);
}

// CStrings: text at data (+0) + offsets[id] (+8); the string names (+0x14 .. +0x18), one per
// string, give the count.
int string_count(uint32_t strings) { return strings ? int((LD32(strings + 0x18) - LD32(strings + 0x14)) / 4) : 0; }
bool has_string(uint32_t strings, int id) { return id >= 0 && id < string_count(strings); }
const char *string_at(uint32_t strings, int id) {
    return has_string(strings, id) ? rt::str(LD32(strings) + LD32(LD32(strings + 8) + 4 * uint32_t(id))) : nullptr;
}
bool holds(uint32_t strings, int id, const char *part) {
    const char *s = string_at(strings, id);
    return s && std::strstr(s, part);
}

}  // namespace

// Application::GetString(int id) -> const char *
extern "C" void pc_hook_get_string(Cpu *cpu, uint32_t entry, GuestFn original) {
    static const auto by_id = [] {
        std::unordered_map<int, const Prompt *> m;
        for (const Prompt &p : kPrompts)
            for (int id : p.ids)
                if (id) m[id] = &p;
        return m;
    }();
    static std::unordered_map<std::string, uint32_t> guest_text;  // sentence -> guest copy (kept)
    const uint32_t app = cpu->r[0];
    const int id = int32_t(cpu->r[1]);
    if (const uint32_t text = controls_menu::string(id)) {  // the Controls pages
        cpu->r[0] = text;
        cpu->ret_pc = cpu->r[14];
        return;
    }
    const uint32_t current = LD32(app + 0x18754);
    const Prompt *prompt = nullptr;
    if (current == LD32(app + 0x18758)) {
        if (id == kSkip.ids[0]) prompt = &kSkip;
    } else if (current == LD32(app + 0x1875c)) {
        auto it = by_id.find(id);
        if (it != by_id.end() && holds(current, 0x27, "^29^") && holds(current, 0x2a, "^28^")) prompt = it->second;
        else if (id == kShipJetpack && string_count(current) == kShipStrings) prompt = &kJetpack;
    }
    original(cpu, entry);
    if (!prompt) return;
    const uint32_t language = LD32(app + 0x188c0);
    const std::string line = sentence(*prompt, language < kLangs ? Lang(language) : EN, input::using_gamepad());
    if (line.empty()) return;
    uint32_t &copy = guest_text[line];
    if (!copy) copy = rt::gstrdup(line.c_str());
    cpu->r[0] = copy;
}
