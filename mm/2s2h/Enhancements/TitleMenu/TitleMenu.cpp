#include "TitleMenu.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <imgui.h>
#include <imgui_internal.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/GuiWindow.h>
#include <ship/window/gui/resource/GuiTexture.h>
#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>
#include <fast/backends/gfx_rendering_api.h>
#include <spdlog/spdlog.h>
#ifdef __ANDROID__
#include <jni.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#endif

#include "2s2h/BenPort.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"
#include "2s2h/BenGui/Translation.h"
#include "2s2h/Language/GameLanguage.h"
#include "2s2h/Mods/ModInstaller.h"
#include "2s2h/Enhancements/Controls/ButtonPrompts.h"
#include "2s2h/Enhancements/QuestMenu/QuestRenderer.h"
#include "IntroVideo.h"
#include "2s2h/BenGui/Menu.h"
#include <ship/controller/controldeck/ControlDeck.h>

extern "C" {
#include <z64.h>
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "build.h"
#include "overlays/gamestates/ovl_file_choose/z_file_select.h"
#include "overlays/gamestates/ovl_opening/z_opening.h"
extern FileSelectState* gFileSelectState;
void Sram_EraseSave(FileSelectState* fileSelect, SramContext* sramCtx, s32 fileNum);
void Sram_SetFlashPagesDefault(SramContext* sramCtx, u32 curPage, u32 numPages);
void Sram_StartWriteToFlashDefault(SramContext* sramCtx);
void Sram_UpdateWriteToFlashDefault(SramContext* sramCtx);
bool Intro_GameMuted(); // BenPort.cpp: the game's sound is off for the opening
}

// The 2 Ship 2 Harkinian contributors listed in the settings menu's About page (BenMenu.cpp).
namespace BenGui {
extern std::vector<std::string> contributors;
}

#define CVAR_ENABLED "gEnhancements.TitleMenu.Enabled"
// Final Hours unless the player picks another (a new name, so a choice saved before 4.0.2-mm3d.8 starts over).
#define CVAR_MUSIC "gEnhancements.TitleMenu.MenuMusic"
#define CVAR_LAST_SLOT "gEnhancements.TitleMenu.LastSlot"
#define CVAR_EFFECTS "gEnhancements.TitleMenu.Effects"

using QuestUi::Col;
using QuestUi::Page;

namespace TitleMenu {
namespace {

// ----- Layout of the 1920x1080 page (the picture's own size after make_assets.py).

constexpr float kW = 1920.0f, kH = 1080.0f;
// The main list: nine rows over the panel painted in the picture.
constexpr float kRow0 = 452, kRowStep = 60;
constexpr float kPlateX0 = 111, kPlateW = 664, kPlateH = 60, kGlow = 26;
constexpr float kIconX = 199, kIconSize = 40, kLabelX = 258, kLabelScale = 0.82f;
// The moon layer (moon.png): its left edge on the page and the moon's circle.
constexpr float kMoonX0 = 1022, kMoonCX = 1492, kMoonCY = -43;
const ImVec2 kMoonEyes[] = { ImVec2(1296, 163), ImVec2(1662, 18) };
const ImVec2 kFairy(1170, 445);
// The frame of the other screens.
constexpr float kPanelX0 = 100, kPanelY0 = 70, kPanelX1 = 1820, kPanelY1 = 1010;
constexpr float kContentX0 = 160, kContentX1 = 1760, kContentY0 = 248, kContentY1 = 900;

float RowY(int i) {
    return kRow0 + kRowStep * i;
}

// ----- Colours: purple, gold and black.

const ImU32 cText = Col(245, 238, 250);
const ImU32 cDim = Col(196, 182, 212);
const ImU32 cGold = Col(236, 204, 120);
const ImU32 cGoldBright = Col(255, 228, 150);
const ImU32 cOn = Col(150, 235, 140);

ImU32 Alpha(ImU32 col, float a) {
    int alpha = (int)(((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
    return (col & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
}

// ----- Music, from the game's own soundtrack.

struct Track {
    const char* name;
    u16 seqId;
};
const Track kTracks[] = {
    { "Últimas Horas", NA_BGM_FINAL_HOURS },
    { "Tema de Majora", NA_BGM_MAJORAS_THEME },
    { "Torre do Relógio", NA_BGM_CLOCK_TOWER },
    { "Castelo de Ikana", NA_BGM_IKANA_CASTLE },
    { "Seleção de arquivo original", NA_BGM_FILE_SELECT },
};
constexpr int kTrackCount = (int)(sizeof(kTracks) / sizeof(kTracks[0]));

// ----- Screens. Each opens over the one before it; B goes back along the way the player came.

enum class Screen { Main, Files, Settings, Controls, Extras, Gallery, Edition, Content, Credits };

enum MainItem {
    ITEM_START,
    ITEM_CONTINUE,
    ITEM_LOAD,
    ITEM_FILES,
    ITEM_SETTINGS,
    ITEM_CONTROLS,
    ITEM_EXTRAS,
    ITEM_CREDITS,
    ITEM_QUIT,
    ITEM_COUNT
};

struct MainEntry {
    const char* icon;
    const char* label;
    const char* help;
};
const MainEntry kMain[ITEM_COUNT] = {
    { "icon_triforce.png", "label_start.png", "Começa um jogo novo no primeiro arquivo vazio. Os outros saves ficam." },
    { "icon_hourglass.png", "label_continue.png", "Volta ao último arquivo jogado." },
    { "icon_folder.png", "label_load.png", "Escolhe um arquivo salvo para jogar." },
    { "icon_book.png", "label_files.png", "Vê os arquivos, cria um jogo novo ou exclui um arquivo." },
    { "icon_gear.png", "label_settings.png", "Vídeo, resolução, áudio, idioma, controles e acessibilidade." },
    { "icon_gamepad.png", "label_controls.png", "Os botões do jogo, as combinações e a configuração." },
    { "icon_mask.png", "label_extras.png", "Galeria, artes conceituais, a edição e os conteúdos adicionais." },
    { "icon_scroll.png", "label_credits.png", "Quem fez o jogo, o port e a Definitive Edition." },
    { "icon_door.png", "label_quit.png", "Fecha o jogo." },
};

// Where a screen stands: what is selected, which page, and whether the focus is on its buttons.
struct Frame {
    Screen screen = Screen::Main;
    int sel = 0;
    int page = 0;
    int mode = 0;      // Files: 0 load, 1 manage. Gallery: 0 gallery, 1 concept art.
    int focus = 0;     // Files: 0 the list, 1 the file's buttons
    int action = 0;    // the button chosen among the file's buttons
    bool zoom = false; // Gallery: the picture fills the screen
    int scroll = 0;    // Settings: the first row shown, when there are more than fit
};
std::vector<Frame> sStack;

Frame& Cur() {
    if (sStack.empty()) {
        sStack.push_back(Frame());
    }
    return sStack.back();
}

// A yes / no question over the current screen.
struct Confirm {
    bool open = false;
    std::string title, text, note;
    std::string yes = "Sim", no = "Não";
    bool yesSelected = false;
    std::function<void()> onYes;
};
Confirm sConfirm;

// ----- State. The game thread (the input hook) and the GUI draw run one after the other on the same thread.

enum class Pending { None, Load, NewGame, GameOptions, OriginalTitle, Quit };

FileSelectState* sFileSelect = nullptr; // the file select the menu belongs to
bool sActive = false;                   // drawn and driving input
bool sLeaving = false;                  // a save is loading: fading out with the file select
bool sAllowOriginalTitle = false;       // Extras > original title screen: let TitleSetup run once
Pending sPending = Pending::None;
int sPendingSlot = 0;
int sInjectA = 0;       // frames left in which to press A for the file select
int sHandedOver = 0;    // frames the game's own screens (name entry, options) have been up
int sAwaitNewFile = -1; // the file being named in the game's name entry, started as soon as it exists
int sEraseSlot = -1;    // a file being erased
bool sEraseWriting = false;
float sCursorY = -1.0f;
float sFade = 0.0f;    // 0 hidden .. 1 shown
float sScreenT = 1.0f; // 0 .. 1 after a screen change
float sScreenDir = 1.0f;
// 0 .. 1 while the menu is built up after the opening's moon (IntroVideo): the veil, then each row in turn.
float sBuild = 1.0f;
float sModalT = 0.0f;
float sTime = 0.0f;
int sRepeatY = 0, sRepeatYTimer = 0, sRepeatX = 0, sRepeatXTimer = 0;
float sKeyboardNavTime = 0.0f; // seconds left in which the keyboard, not the controller, drives the menu
std::string sToast;
float sToastTime = 0.0f;
// The footer shows touch hints after a tap, and the controller's (or keyboard's) buttons after a press. On Android
// the menu starts in touch mode; the first press on a controller switches it.
#ifdef __ANDROID__
bool sTouchMode = true;
#else
bool sTouchMode = false;
#endif
bool sPointerInputs = false; // taps and clicks reach the menu this frame
bool sTapTaken = false;      // one tap triggers one button

bool Enabled() {
    return CVarGetInteger(CVAR_ENABLED, 1) != 0;
}

void Toast(const std::string& text) {
    sToast = text;
    sToastTime = 3.5f + std::min(4.0f, text.size() / 40.0f);
}

void SaveCVars() {
    Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

// Sets an option the way the settings menu does: the value, the hooks that depend on it, and the config file.
void SetCVarInt(const char* cvar, int value) {
    CVarSetInteger(cvar, value);
    ShipInit::Init(cvar);
    SaveCVars();
}

void SetCVarFloat(const char* cvar, float value) {
    CVarSetFloat(cvar, value);
    ShipInit::Init(cvar);
    SaveCVars();
}

// ----- Saves, as the file select read them from the save files.

bool Slot3Enabled() {
    return CVarGetInteger("gEnhancements.Saving.FileSlot3", true) != 0;
}

int SlotCount() {
    return Slot3Enabled() ? 3 : 2;
}

bool SlotOccupied(FileSelectState* fs, int slot) {
    if (fs == nullptr) {
        return false;
    }
    if (!gSaveContext.flashSaveAvailable) {
        SramContext* sramCtx = &fs->sramCtx;
        return NO_FLASH_SLOT_OCCUPIED(sramCtx, slot);
    }
    return SLOT_OCCUPIED(fs, slot);
}

// The file played last, or the first one that exists; -1 when there is none.
int ContinueSlot(FileSelectState* fs) {
    if (fs == nullptr) {
        return -1;
    }
    int last = CVarGetInteger(CVAR_LAST_SLOT, -1);
    if (last >= 0 && last < SlotCount() && SlotOccupied(fs, last)) {
        return last;
    }
    for (int i = 0; i < SlotCount(); i++) {
        if (SlotOccupied(fs, i)) {
            return i;
        }
    }
    return -1;
}

int EmptySlot(FileSelectState* fs) {
    for (int i = 0; fs != nullptr && i < SlotCount(); i++) {
        if (!SlotOccupied(fs, i)) {
            return i;
        }
    }
    return -1;
}

// The file select is on its main screen and waiting for the player.
bool FileSelectReady(FileSelectState* fs) {
    return fs != nullptr && fs->menuMode == FS_MENU_MODE_CONFIG && fs->configMode == CM_MAIN_MENU;
}

// The game's file name characters (digits, letters, space, '-' and '.').
char NameChar(u8 c) {
    if (c <= 9) {
        return (char)('0' + c);
    }
    if (c >= 0x0A && c <= 0x23) {
        return (char)('A' + c - 0x0A);
    }
    if (c >= 0x24 && c <= 0x3D) {
        return (char)('a' + c - 0x24);
    }
    if (c == 0x3F) {
        return '-';
    }
    if (c == 0x40) {
        return '.';
    }
    return ' ';
}

struct FileInfo {
    bool occupied = false;
    bool owl = false; // an owl save: the file continues from the owl statue
    std::string name;
    int hearts = 0, heartPieces = 0;
    bool doubleDefense = false;
    bool remains[4] = {};
    int remainsCount = 0, songs = 0, masks = 0, rupees = 0, cycles = 0, day = 0, hour = 0, minute = 0;
};

// What the file select shows for a file (from the owl save when there is one), nothing else.
FileInfo ReadFile(FileSelectState* fs, int slot) {
    FileInfo info;
    if (!SlotOccupied(fs, slot)) {
        return info;
    }
    info.occupied = true;
    int i = slot;
    if (fs->isOwlSave[slot + FILE_NUM_OWL_SAVE_OFFSET]) {
        info.owl = true;
        i = slot + FILE_NUM_OWL_SAVE_OFFSET;
    }
    for (int c = 0; c < 8; c++) {
        info.name += NameChar((u8)fs->fileNames[slot][c]);
    }
    while (!info.name.empty() && info.name.back() == ' ') {
        info.name.pop_back();
    }
    info.hearts = fs->healthCapacity[i] / 0x10;
    info.heartPieces = fs->heartPieceCount[i];
    info.doubleDefense = fs->defenseHearts[i] > 0;
    for (int r = 0; r < 4; r++) {
        info.remains[r] = (fs->questItems[i] & gBitFlags[QUEST_REMAINS_ODOLWA + r]) != 0;
        info.remainsCount += info.remains[r] ? 1 : 0;
    }
    const int songs[] = { QUEST_SONG_TIME,   QUEST_SONG_HEALING, QUEST_SONG_EPONA,   QUEST_SONG_SOARING,
                          QUEST_SONG_STORMS, QUEST_SONG_SONATA,  QUEST_SONG_LULLABY, QUEST_SONG_BOSSA_NOVA,
                          QUEST_SONG_ELEGY,  QUEST_SONG_OATH };
    for (int song : songs) {
        info.songs += (fs->questItems[i] & gBitFlags[song]) ? 1 : 0;
    }
    info.masks = fs->maskCount[i];
    info.rupees = fs->rupees[i];
    info.cycles = fs->threeDayResetCount[i];
    info.day = fs->day[i];
    int minutes = (int)TIME_TO_MINUTES_F(fs->time[i]);
    info.hour = (minutes / 60) % 24;
    info.minute = minutes % 60;
    return info;
}

std::string DayText(int day) {
    if (day <= 0) {
        return "Antes do primeiro dia";
    }
    if (day <= 3) {
        char text[32];
        snprintf(text, sizeof(text), "Dia %d", day);
        return text;
    }
    return "Últimas horas";
}

// ----- Settings: real options of the game and the port, read and written through their CVars.

// Switch: on / off like a Toggle, read and changed through functions instead of a CVar (a mod file).
enum class Kind { Toggle, Choice, Percent, Action, Switch, Number };

struct Setting {
    std::string label;
    std::string help;
    Kind kind;
    const char* cvar = nullptr;
    int def = 0;                                                // Toggle / Choice default
    std::vector<const char*> choices;                           // Choice labels
    std::vector<int> values;                                    // Choice values (index when empty)
    float fdef = 1.0f, fmin = 0.0f, fmax = 1.0f, fstep = 0.05f; // Percent
    std::function<void()> apply;                                // after a change
    std::function<void()> action;                               // Action
    std::function<std::string()> value;                         // Action: text on the right
    std::function<bool()> get;                                  // Switch: on or off
    bool integer = false, percent = false;                      // Number: whole numbers, shown as a percentage
    std::function<bool()> disabled;                             // greyed out and not changed while true
};

Setting Toggle(const char* label, const char* help, const char* cvar, int def, std::function<void()> apply = nullptr) {
    Setting s{ label, help, Kind::Toggle };
    s.cvar = cvar;
    s.def = def;
    s.apply = apply;
    return s;
}

Setting Choice(const char* label, const char* help, const char* cvar, int def, std::vector<const char*> choices,
               std::vector<int> values = {}, std::function<void()> apply = nullptr) {
    Setting s{ label, help, Kind::Choice };
    s.cvar = cvar;
    s.def = def;
    s.choices = choices;
    s.values = values;
    s.apply = apply;
    return s;
}

Setting Percent(const char* label, const char* help, const char* cvar, float def, float min, float max, float step,
                std::function<void()> apply = nullptr) {
    Setting s{ label, help, Kind::Percent };
    s.cvar = cvar;
    s.fdef = def;
    s.fmin = min;
    s.fmax = max;
    s.fstep = step;
    s.apply = apply;
    return s;
}

Setting Action(const std::string& label, const std::string& help, std::function<void()> action,
               std::function<std::string()> value = nullptr) {
    Setting s{ label, help, Kind::Action };
    s.action = action;
    s.value = value;
    return s;
}

Setting Switch(const std::string& label, const std::string& help, std::function<bool()> get,
               std::function<void()> toggle) {
    Setting s{ label, help, Kind::Switch };
    s.get = get;
    s.action = toggle;
    return s;
}

bool SettingOn(const Setting& s) {
    return s.kind == Kind::Switch ? s.get() : CVarGetInteger(s.cvar, s.def) != 0;
}

// The choice the option is on; -1 when it holds a value set elsewhere (the full settings menu) that is not listed.
int ChoiceIndex(const Setting& s) {
    int v = CVarGetInteger(s.cvar, s.def);
    if (s.values.empty()) {
        return std::clamp(v, 0, (int)s.choices.size() - 1);
    }
    for (size_t i = 0; i < s.values.size(); i++) {
        if (s.values[i] == v) {
            return (int)i;
        }
    }
    return -1;
}

std::string SettingValue(const Setting& s) {
    switch (s.kind) {
        case Kind::Toggle:
            return CVarGetInteger(s.cvar, s.def) ? "Ligado" : "Desligado";
        case Kind::Choice: {
            int i = ChoiceIndex(s);
            return i >= 0 ? s.choices[i] : std::to_string(CVarGetInteger(s.cvar, s.def));
        }
        case Kind::Percent: {
            char text[16];
            snprintf(text, sizeof(text), "%d%%", (int)std::lround(CVarGetFloat(s.cvar, s.fdef) * 100.0f));
            return text;
        }
        case Kind::Action:
            return s.value ? s.value() : "";
        case Kind::Switch:
            return s.get() ? "Ligado" : "Desligado";
        case Kind::Number: {
            char text[24];
            if (s.integer) {
                snprintf(text, sizeof(text), "%d", CVarGetInteger(s.cvar, (int)s.fdef));
            } else if (s.percent) {
                snprintf(text, sizeof(text), "%d%%", (int)std::lround(CVarGetFloat(s.cvar, s.fdef) * 100.0f));
            } else {
                snprintf(text, sizeof(text), "%.2f", CVarGetFloat(s.cvar, s.fdef));
            }
            return text;
        }
    }
    return "";
}

// Left / right (dir -1 / 1), or A (dir 0: toggles, next choice, runs an action).
void ChangeSetting(const Setting& s, int dir) {
    if (s.disabled && s.disabled()) {
        Audio_PlaySfx(NA_SE_SY_ERROR);
        return;
    }
    switch (s.kind) {
        case Kind::Toggle:
            SetCVarInt(s.cvar, CVarGetInteger(s.cvar, s.def) ? 0 : 1);
            break;
        case Kind::Choice: {
            int n = (int)s.choices.size();
            int i = std::max(ChoiceIndex(s), 0);
            i = (i + (dir == 0 ? 1 : dir) + n) % n;
            SetCVarInt(s.cvar, s.values.empty() ? i : s.values[i]);
            break;
        }
        case Kind::Percent: {
            if (dir == 0) {
                return;
            }
            float v = CVarGetFloat(s.cvar, s.fdef) + dir * s.fstep;
            v = std::clamp(std::round(v / s.fstep) * s.fstep, s.fmin, s.fmax);
            SetCVarFloat(s.cvar, v);
            break;
        }
        case Kind::Action:
            if (dir == 0 && s.action) {
                s.action();
            }
            return;
        case Kind::Switch:
            if (s.action) {
                s.action();
            }
            break;
        case Kind::Number: {
            if (dir == 0) {
                return;
            }
            if (s.integer) {
                int step = std::max(1, (int)s.fstep);
                int v = std::clamp(CVarGetInteger(s.cvar, (int)s.fdef) + dir * step, (int)s.fmin, (int)s.fmax);
                SetCVarInt(s.cvar, v);
            } else {
                float v = CVarGetFloat(s.cvar, s.fdef) + dir * s.fstep;
                v = std::clamp(std::round(v / s.fstep) * s.fstep, s.fmin, s.fmax);
                SetCVarFloat(s.cvar, v);
            }
            break;
        }
    }
    Audio_PlaySfx(NA_SE_SY_CURSOR);
    if (s.apply) {
        s.apply();
    }
}

// Other windows of the port (the settings menu, the button editor) take over input while they are open.
std::shared_ptr<Ship::GuiWindow> InputEditor() {
    return Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGuiWindow("2S2H Input Editor");
}

bool ShipWindowOpen();

bool OtherWindowOpen() {
    if (ShipWindowOpen()) {
        return true;
    }
    auto gui = Ship::Context::GetInstance()->GetWindow()->GetGui();
    auto menu = gui->GetMenu();
    if (menu != nullptr && menu->IsVisible()) {
        return true;
    }
    auto editor = InputEditor();
    return editor != nullptr && editor->IsVisible();
}

void OpenSettingsMenu() {
    auto menu = Ship::Context::GetInstance()->GetWindow()->GetGui()->GetMenu();
    if (menu != nullptr && !menu->IsVisible()) {
        Audio_PlaySfx(NA_SE_SY_DECIDE);
        menu->ToggleVisibility();
    }
}

void OpenInputEditor() {
    auto editor = InputEditor();
    if (editor != nullptr) {
        Audio_PlaySfx(NA_SE_SY_DECIDE);
        editor->Show();
    } else {
        OpenSettingsMenu();
    }
}

void PlayMenuMusic() {
    int index = std::clamp(CVarGetInteger(CVAR_MUSIC, 0), 0, kTrackCount - 1);
    SEQCMD_PLAY_SEQUENCE(SEQ_PLAYER_BGM_MAIN, 0, kTracks[index].seqId);
}

void SetVolume(u8 player, const char* cvar) {
    AudioSeq_SetPortVolumeScale(player, CVarGetFloat(cvar, 1.0f));
}

struct Tab {
    const char* name;
    std::vector<Setting> rows;
};

std::vector<Tab>& SettingsTabs() {
    static std::vector<Tab> tabs;
    if (!tabs.empty()) {
        return tabs;
    }
    auto window = []() { return Ship::Context::GetInstance()->GetWindow(); };
    tabs.push_back(
        { "Vídeo",
          {
              Choice("Quadros por segundo",
                     "Quadros extras desenhados entre os do jogo. Só muda a imagem, não a velocidade do jogo.",
                     "gInterpolationFPS", 20, { "Original (20)", "30", "60", "120" }, { 20, 30, 60, 120 }),
              Toggle("Acompanhar a taxa da tela", "Usa a taxa de atualização da tela no lugar da escolhida acima.",
                     "gMatchRefreshRate", 0),
              Toggle("Sincronia vertical (VSync)", "Tira o corte na imagem, limitando os quadros à taxa da tela.",
                     CVAR_VSYNC_ENABLED, 1),
              Choice("Filtro de textura", "Como as texturas são suavizadas. Vale quando o jogo abrir de novo.",
                     CVAR_TEXTURE_FILTER, Fast::FILTER_THREE_POINT,
                     { "Três pontos (como no N64)", "Linear", "Nenhum (pixels)" }),
#ifndef __ANDROID__
              Action(
                  "Tela cheia", "Alterna entre janela e tela cheia.", [window]() { window()->ToggleFullscreen(); },
                  [window]() { return std::string(window()->IsFullscreen() ? "Ligada" : "Desligada"); }),
#endif
          } });
    tabs.push_back(
        { "Resolução",
          {
              Percent("Resolução interna", "Desenha o jogo numa resolução maior ou menor que a da tela.",
                      CVAR_INTERNAL_RESOLUTION, 1.0f, 0.5f, 2.0f, 0.1f,
                      [window]() { window()->SetResolutionMultiplier(CVarGetFloat(CVAR_INTERNAL_RESOLUTION, 1)); }),
              Choice("Antisserrilhado (MSAA)", "Suaviza as bordas dos modelos. Valores altos pesam mais.",
                     CVAR_MSAA_VALUE, 1, { "Desligado", "2x", "4x", "8x" }, { 1, 2, 4, 8 },
                     [window]() { window()->SetMsaaLevel(CVarGetInteger(CVAR_MSAA_VALUE, 1)); }),
              Toggle("Modo N64 (4:3, 240p)", "A proporção e a resolução do Nintendo 64.", CVAR_LOW_RES_MODE, 0),
#ifdef __ANDROID__
              Choice("Tamanho da interface", "Tamanho dos menus e janelas do port. Vale quando o jogo abrir de novo.",
                     "gSettings.Android.UISize", 0, { "Automático", "Pequeno", "Médio", "Grande", "Muito grande" }),
#endif
          } });
    tabs.push_back(
        { "Áudio",
          {
              Percent("Volume geral", "O volume de todo o som do jogo.", "gSettings.Audio.MasterVolume", 1.0f, 0.0f,
                      1.0f, 0.05f),
              Percent("Música", "O volume da música.", "gSettings.Audio.MainMusicVolume", 1.0f, 0.0f, 1.0f, 0.05f,
                      []() { SetVolume(SEQ_PLAYER_BGM_MAIN, "gSettings.Audio.MainMusicVolume"); }),
              Percent("Efeitos sonoros", "O volume dos efeitos.", "gSettings.Audio.SoundEffectsVolume", 1.0f, 0.0f,
                      1.0f, 0.05f, []() { SetVolume(SEQ_PLAYER_SFX, "gSettings.Audio.SoundEffectsVolume"); }),
              Percent("Fanfarras", "O volume das fanfarras (itens, segredos).", "gSettings.Audio.FanfareVolume", 1.0f,
                      0.0f, 1.0f, 0.05f, []() { SetVolume(SEQ_PLAYER_FANFARE, "gSettings.Audio.FanfareVolume"); }),
              Percent("Ambiente", "O volume dos sons do ambiente.", "gSettings.Audio.AmbienceVolume", 1.0f, 0.0f, 1.0f,
                      0.05f, []() { SetVolume(SEQ_PLAYER_AMBIENCE, "gSettings.Audio.AmbienceVolume"); }),
              Choice("Música da tela inicial", "A música que toca aqui, da trilha original do jogo.", CVAR_MUSIC, 0,
                     { kTracks[0].name, kTracks[1].name, kTracks[2].name, kTracks[3].name, kTracks[4].name }, {},
                     []() { PlayMenuMusic(); }),
              Action("Som e mira do jogo original", "Abre as opções do próprio jogo: som (estéreo, mono...) e mira Z.",
                     []() { sPending = Pending::GameOptions; }),
          } });
    tabs.push_back(
        { "Idioma",
          {
              Choice("Idioma dos menus", "Idioma da tela inicial e dos menus do port.", CVAR_MENU_LANGUAGE,
                     BenGui::MENU_LANGUAGE_PORTUGUESE_BR, { "Português (Brasil)", "English" }),
              Action(
                  "Idioma do texto do jogo",
                  "O texto do jogo vem em inglês; o português usa um pacote de tradução. Vale quando o jogo abrir "
                  "de novo.",
                  []() {
                      auto next = GameLanguage::GetSaved(appShortName) == GameLanguage::LANGUAGE_ENGLISH
                                      ? GameLanguage::LANGUAGE_PORTUGUESE_BR
                                      : GameLanguage::LANGUAGE_ENGLISH;
                      GameLanguage::Save(next, appShortName);
                      Audio_PlaySfx(NA_SE_SY_CURSOR);
                      Toast("Vale quando o jogo abrir de novo.");
                  },
                  []() {
                      return std::string(GameLanguage::GetSaved(appShortName) == GameLanguage::LANGUAGE_ENGLISH
                                             ? "Inglês (original)"
                                             : "Português (pacote)");
                  }),
#ifdef __ANDROID__
              Action("Escolher o pacote de tradução",
                     "Abre os arquivos do celular para escolher o pacote em português.",
                     []() {
                         Audio_PlaySfx(NA_SE_SY_DECIDE);
                         GameLanguage::OpenPackPicker();
                     }),
#endif
          } });
    tabs.push_back(
        { "Controles",
          {
              Action("Configurar botões", "Abre o editor de botões do port, para trocar o que cada botão faz.",
                     []() { OpenInputEditor(); }),
              Choice("Ícones dos botões", "Como o jogo mostra os botões. Automático segue o controle usado por último.",
                     "gEnhancements.ButtonPrompts.Style", 0,
                     { "Automático", "N64", "Xbox", "PlayStation", "Nintendo Switch", "Teclado" }),
              Toggle("Mira pelo giroscópio", "Mira em primeira pessoa movendo o aparelho ou o controle.",
                     "gEnhancements.Camera.FirstPerson.GyroEnabled", 0),
              Toggle("Mira pelo analógico direito", "Mira em primeira pessoa com o analógico direito.",
                     "gEnhancements.Camera.FirstPerson.RightStickEnabled", 0),
              Toggle("Inverter o eixo Y na primeira pessoa", "Para cima olha para baixo, como no original.",
                     "gEnhancements.Camera.FirstPerson.InvertY", 1),
              Toggle("Câmera livre", "Gira a câmera com o analógico direito.", "gEnhancements.Camera.FreeLook.Enable",
                     0),
              Toggle("Navegar nos menus do port com o controle", "Usa o controle nos menus de configuração do port.",
                     CVAR_IMGUI_CONTROLLER_NAV, 0),
          } });
    tabs.push_back(
        { "Acessibilidade",
          {
              Toggle("Sem clarão ao derrotar inimigos", "Tira o flash branco da tela ao matar um inimigo.",
                     "gEnhancements.A11y.NoScreenFlashForEnemyKill", 0),
              Toggle("Mira do arco", "Mostra um ponto de mira ao usar o arco.", "gEnhancements.Graphics.BowReticle", 0),
              Toggle("Marcar os Octoroks da galeria de tiro", "Destaca os alvos do minigame.",
                     "gEnhancements.Minigames.MarkShootingGalleryOctoroks", 0),
              Toggle("Segurar B para acelerar cenas", "Segure B durante uma cena para ela andar mais rápido.",
                     "gEnhancements.Cutscenes.HoldBToFastForward", 0),
              Toggle("Animações da tela inicial", "A lua, os brilhos e as partículas desta tela.", CVAR_EFFECTS, 1),
          } });
    tabs.push_back(
        { "Definitive",
          {
              Toggle("Iluminação estilo 3DS", "Luz e cores no estilo do Majora's Mask 3D.",
                     "gEnhancements.MM3D.Lighting.Enabled", 0),
              Toggle("Cel shading", "Visual de desenho animado.", "gEnhancements.Graphics.CelShading", 0),
              Toggle("Contorno preto", "Contorno nos modelos, junto com o cel shading.",
                     "gEnhancements.Graphics.CelOutline", 1),
              Toggle("Caderno dos Bombers desde o início", "O caderno já vem com o jogo novo.",
                     "gEnhancements.MM3D.EarlyBombersNotebook", 0),
              Toggle("Estátuas da coruja salvam e continuam", "Como no 3DS: salvar numa estátua não fecha o jogo.",
                     "gEnhancements.MM3D.OwlStatueSaveAndContinue", 0),
              Toggle("Goron rola sem segurar A", "Como no 3DS.", "gEnhancements.MM3D.GoronRollWithoutHolding", 0),
              Toggle("Variações da Canção do Tempo no pause", "Mostra as canções invertida e dupla no pause.",
                     "gEnhancements.MM3D.TimeSongVariantsInPause", 1),
              Toggle("Dicas dentro do Caderno dos Bombers", "Notas sobre o que fazer em cada evento do caderno.",
                     "gEnhancements.MM3D.BombersNotebookGuide", 1),
              Action("Todas as opções", "Abre o menu completo do port, com todas as melhorias.",
                     []() { OpenSettingsMenu(); }),
          } });
    tabs.push_back({ "2Ship", {} }); // every option of the port's own menu, filled by RefreshShipTab
    tabs.push_back({ "Mods", {} });  // filled from the mods folder by RefreshModsTab
    return tabs;
}

// ----- Settings > Mods: the mods folder, live loading and Google Drive. The rows are rebuilt from the folder each
// second (and after a change), so a mod copied in by the Android picker or a download shows up by itself. Rows are
// only rebuilt before they are drawn, never from inside a row's own action.

constexpr const char* kModsTabName = "Mods";
std::vector<ModInstaller::ModFile> sMods;
std::vector<ModInstaller::DriveEntry> sDriveFolder;
float sModsScan = -1.0f;
bool sModsDirty = true;
int sModsNotice = 0;
bool sDriveListed = false; // the remembered folder was listed this session

Tab& ModsTab() {
    return SettingsTabs().back();
}

bool IsModsTab(const Frame& f) {
    return f.screen == Screen::Settings && f.page == (int)SettingsTabs().size() - 1;
}

std::string ShortName(const std::string& name, size_t max = 44) {
    if (name.size() <= max) {
        return name;
    }
    // Cut on a UTF-8 character boundary.
    size_t cut = max - 3;
    while (cut > 0 && (name[cut] & 0xC0) == 0x80) {
        cut--;
    }
    return name.substr(0, cut) + "...";
}

std::string MegaBytes(uint64_t bytes) {
    char text[32];
    snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
    for (char& c : text) {
        if (c == '.') {
            c = ',';
        }
    }
    return text;
}

std::string DownloadValue() {
    ModInstaller::DownloadStatus st = ModInstaller::GetDownloadStatus();
    if (!ModInstaller::DownloadBusy()) {
        return "";
    }
    if (st.state == ModInstaller::DownloadState::Listing) {
        return "Abrindo a pasta...";
    }
    if (st.total > 0) {
        return "Baixando " + std::to_string((int)(st.done * 100 / st.total)) + "%";
    }
    return st.done > 0 ? "Baixando " + MegaBytes(st.done) : "Baixando...";
}

void RebuildModsRows() {
    std::vector<Setting>& rows = ModsTab().rows;
    rows.clear();
    const bool live = ModInstaller::LiveApplyEnabled();
    rows.push_back(Toggle("Aplicar na hora",
                          "Liga e desliga os mods sem fechar o jogo. Texturas mudam na hora; modelos e cenários, ao "
                          "entrar numa nova área; músicas, quando o jogo abrir de novo.",
                          CVAR_MODS_LIVE_APPLY, 1, []() {
                              sModsDirty = true;
                              if (ModInstaller::LiveApplyEnabled()) {
                                  ModInstaller::RequestApply();
                              }
                          }));
    int pending = ModInstaller::PendingCount(appShortName);
    if (pending > 0) {
        rows.push_back(Action(
            "Aplicar as mudanças agora", "Carrega no jogo os mods ligados e tira os desligados, sem fechar o jogo.",
            []() {
                Audio_PlaySfx(NA_SE_SY_DECIDE);
                ModInstaller::RequestApply();
                sModsDirty = true;
            },
            [pending]() { return std::to_string(pending) + (pending == 1 ? " mudança" : " mudanças"); }));
    }
    rows.push_back(Action(
        "Baixar do Google Drive",
#ifdef __ANDROID__
        "Cole o link de um mod (.o2r, .otr, .zip) ou de uma pasta com mods. No Drive, compartilhe como \"Qualquer "
        "pessoa com o link\".",
#else
        "Copie o link de um mod (.o2r, .otr, .zip) ou de uma pasta com mods e escolha esta opção. No Drive, "
        "compartilhe como \"Qualquer pessoa com o link\".",
#endif
        []() {
            if (ModInstaller::DownloadBusy()) {
                Toast("Espere o download terminar.");
                return;
            }
            Audio_PlaySfx(NA_SE_SY_DECIDE);
            if (!ModInstaller::AskDriveLink()) {
                Toast("Copie um link do Google Drive primeiro.");
            }
            sModsDirty = true;
        },
        []() { return DownloadValue(); }));
    rows.push_back(Action("Adicionar do aparelho",
#ifdef __ANDROID__
                          "Abre os arquivos do celular para escolher um mod (.o2r, .otr ou .zip).",
#else
                          "Abre uma janela para escolher um mod (.o2r, .otr ou .zip) e o copia para a pasta mods.",
#endif
                          []() {
                              Audio_PlaySfx(NA_SE_SY_DECIDE);
                              std::string message = ModInstaller::OpenPicker(appShortName);
                              if (!message.empty()) {
                                  for (char& c : message) {
                                      if (c == '\n') {
                                          c = ' ';
                                      }
                                  }
                                  Toast(message);
                              }
                              sModsDirty = true;
                          }));
    rows.push_back(Toggle("Texturas alternativas", "Pacotes de texturas HD costumam precisar disto ligado.",
                          "gEnhancements.Mods.AlternateAssets", 0));

    if (sMods.empty()) {
        rows.push_back(Action("Nenhum mod instalado",
                              "Os mods ficam na pasta: " + ModInstaller::GetModsFolder(appShortName), nullptr));
    }
    for (const ModInstaller::ModFile& mod : sMods) {
        bool active = ModInstaller::IsActive(mod);
        std::string help = mod.name + ". ";
        if (active == mod.enabled) {
            help += active ? "Está no jogo agora." : "Desligado.";
        } else {
            help += live ? "Aplicando..." : "Vale quando o jogo abrir de novo ou em \"Aplicar as mudanças agora\".";
        }
        ModInstaller::ModFile copy = mod;
        rows.push_back(Switch(
            ShortName(std::filesystem::path(mod.name).stem().string()), help, [copy]() { return copy.enabled; },
            [copy]() {
                if (!ModInstaller::SetEnabled(copy, !copy.enabled)) {
                    Toast("Não foi possível mudar o arquivo do mod.");
                } else if (!ModInstaller::LiveApplyEnabled()) {
                    Toast("Vale quando o jogo abrir de novo.");
                }
                sModsDirty = true;
            }));
    }

    if (ModInstaller::HasDriveFolder()) {
        rows.push_back(Action(
            "Pasta do Google Drive", "Os mods da pasta cujo link foi colado. Escolha para ver a lista de novo.",
            []() {
                Audio_PlaySfx(NA_SE_SY_DECIDE);
                ModInstaller::RefreshDriveFolder();
                sModsDirty = true;
            },
            []() {
                if (ModInstaller::DownloadBusy() &&
                    ModInstaller::GetDownloadStatus().state == ModInstaller::DownloadState::Listing) {
                    return std::string("Abrindo...");
                }
                return std::string("Atualizar");
            }));
        for (const ModInstaller::DriveEntry& entry : sDriveFolder) {
            bool installed = ModInstaller::IsInstalled(appShortName, entry.name);
            ModInstaller::DriveEntry copy = entry;
            rows.push_back(Action(
                "Baixar: " + ShortName(entry.name, 38),
                installed ? entry.name + " já está na pasta de mods. Escolha para baixar de novo (atualizar)."
                          : "Baixa " + entry.name + " do Google Drive para a pasta de mods.",
                [copy]() {
                    if (ModInstaller::DownloadBusy()) {
                        Toast("Espere o download terminar.");
                        return;
                    }
                    Audio_PlaySfx(NA_SE_SY_DECIDE);
                    ModInstaller::DownloadDriveFile(copy);
                    sModsDirty = true;
                },
                [installed]() { return std::string(installed ? "Instalado" : "Baixar"); }));
        }
        rows.push_back(
            Action("Esquecer a pasta do Google Drive", "Tira a pasta desta lista. Os mods baixados ficam.", []() {
                Audio_PlaySfx(NA_SE_SY_CANCEL);
                ModInstaller::ForgetDriveFolder();
                sModsDirty = true;
            }));
    }
}

// Each frame: news from the mods (applied, downloaded) as a toast, and the rows when something changed.
void RefreshModsTab(bool visible) {
    ModInstaller::Notice notice = ModInstaller::LastNotice();
    if (notice.serial != sModsNotice) {
        sModsNotice = notice.serial;
        if (!notice.text.empty() && sActive) {
            Toast(notice.text);
        }
        sModsDirty = true;
    }
    if (!visible) {
        return;
    }
    if (!sDriveListed && ModInstaller::HasDriveFolder() && !ModInstaller::DownloadBusy()) {
        sDriveListed = true;
        ModInstaller::RefreshDriveFolder();
    }
    if (sModsDirty || sTime >= sModsScan) {
        sMods = ModInstaller::List(appShortName);
        sDriveFolder = ModInstaller::GetDriveFolder();
        sModsScan = sTime + 1.0f;
        sModsDirty = false;
        RebuildModsRows();
    }
}

// ----- Configurações > 2Ship: every option of the port's own settings menu (BenMenu), so none of them needs that
// menu. The list shows each header and sidebar of the menu; A on one shows its options, read from the menu's own
// widgets each frame: their CVars, defaults, limits, callbacks and the rules that hide or lock them.

constexpr const char* kShipTabName = "2Ship";
int sShipSection = -1;                        // the sidebar shown, or -1 for the list of sidebars
int sShipListSel = 0;                         // the list's row to come back to
int sShipPending = -2;                        // a choice made by a row's action, applied before the next rebuild
std::shared_ptr<Ship::GuiWindow> sShipWindow; // a window opened from the tab: it takes the input while it is open

bool IsShipTab(const Frame& f) {
    auto& tabs = SettingsTabs();
    return f.screen == Screen::Settings && f.page >= 0 && f.page < (int)tabs.size() &&
           std::string(tabs[f.page].name) == kShipTabName;
}

Tab& ShipTab() {
    for (Tab& t : SettingsTabs()) {
        if (std::string(t.name) == kShipTabName) {
            return t;
        }
    }
    return SettingsTabs().back();
}

bool ShipWindowOpen() {
    if (sShipWindow != nullptr && !sShipWindow->IsVisible()) {
        sShipWindow = nullptr;
    }
    return sShipWindow != nullptr;
}

// The widget's label without the value format ("Volume: %.0f%%") or the ImGui id ("##...").
std::string ShipLabel(const std::string& name) {
    std::string s = BenGui::Tr(name);
    size_t cut = std::min(s.find("##"), s.find('%'));
    if (cut != std::string::npos) {
        s.erase(cut);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == ':')) {
        s.pop_back();
    }
    return s.empty() ? BenGui::Tr(name) : s;
}

struct ShipSidebar {
    std::string header, name;
    SidebarEntry* entry;
};

std::vector<ShipSidebar> ShipSidebars(Ship::Menu& menu) {
    std::vector<ShipSidebar> out;
    auto& entries = menu.GetMenuEntries();
    for (const std::string& header : menu.GetMenuOrder()) {
        auto it = entries.find(header);
        if (it == entries.end()) {
            continue;
        }
        for (const std::string& name : it->second.sidebarOrder) {
            auto side = it->second.sidebars.find(name);
            if (name.empty() || name == "Search" || side == it->second.sidebars.end()) {
                continue;
            }
            out.push_back({ header, name, &side->second });
        }
    }
    return out;
}

bool ShipConvertible(const WidgetInfo& w) {
    switch (w.type) {
        case WIDGET_CVAR_CHECKBOX:
        case WIDGET_CVAR_COMBOBOX:
        case WIDGET_CVAR_SLIDER_INT:
        case WIDGET_CVAR_SLIDER_FLOAT:
            return w.cVar != nullptr && w.cVar[0] != '\0' && w.options != nullptr;
        case WIDGET_BUTTON:
            return w.callback != nullptr;
        case WIDGET_WINDOW_BUTTON:
            return w.windowName != nullptr && w.windowName[0] != '\0';
        default:
            return false;
    }
}

// One row for a widget, or false when it is hidden now or has no row form (colors, text, custom drawing).
bool ShipRow(WidgetInfo& widget, std::vector<Setting>& rows) {
    if (!ShipConvertible(widget)) {
        return false;
    }
    WidgetInfo* w = &widget;
    if (w->preFunc != nullptr) {
        w->ResetDisables();
        w->preFunc(*w);
        if (w->isHidden) {
            return false;
        }
    }
    bool locked = (w->options != nullptr && w->options->disabled) || !w->activeDisables.empty();
    std::string help = (w->options != nullptr && w->options->tooltip != nullptr) ? BenGui::Tr(w->options->tooltip) : "";
    auto apply = [w]() {
        if (w->callback != nullptr) {
            w->callback(*w);
        }
    };
    Setting s{ ShipLabel(w->name), help, Kind::Toggle };
    s.cvar = w->cVar;
    switch (w->type) {
        case WIDGET_CVAR_CHECKBOX: {
            auto o = std::static_pointer_cast<UIWidgets::CheckboxOptions>(w->options);
            s.def = o->defaultValue ? 1 : 0;
            s.apply = apply;
            break;
        }
        case WIDGET_CVAR_COMBOBOX: {
            auto o = std::static_pointer_cast<UIWidgets::ComboboxOptions>(w->options);
            s.kind = Kind::Choice;
            s.def = (int)o->defaultIndex;
            if (std::holds_alternative<UIWidgets::ComboVec_t>(o->comboVariant)) {
                auto vec = std::get<UIWidgets::ComboVec_t>(o->comboVariant);
                if (vec == nullptr || vec->empty()) {
                    return false;
                }
                for (const char* label : *vec) {
                    s.choices.push_back(BenGui::Tr(label));
                }
            } else {
                auto map = std::get<UIWidgets::ComboMap_t>(o->comboVariant);
                if (map == nullptr || map->empty()) {
                    return false;
                }
                std::vector<int> keys;
                for (auto& [k, v] : *map) {
                    keys.push_back(k);
                }
                std::sort(keys.begin(), keys.end());
                for (int k : keys) {
                    s.values.push_back(k);
                    s.choices.push_back(BenGui::Tr(map->at(k)));
                }
            }
            s.apply = apply;
            break;
        }
        case WIDGET_CVAR_SLIDER_INT: {
            auto o = std::static_pointer_cast<UIWidgets::IntSliderOptions>(w->options);
            s.kind = Kind::Number;
            s.integer = true;
            s.fmin = (float)o->min;
            s.fmax = (float)o->max;
            s.fstep = (float)std::max(1, o->step);
            s.fdef = (float)o->defaultValue;
            s.apply = apply;
            break;
        }
        case WIDGET_CVAR_SLIDER_FLOAT: {
            auto o = std::static_pointer_cast<UIWidgets::FloatSliderOptions>(w->options);
            s.kind = Kind::Number;
            s.percent = o->isPercentage;
            s.fmin = o->min;
            s.fmax = o->max;
            s.fstep = o->step > 0.0f ? o->step : 0.01f;
            // A fine slider would take hundreds of presses on a pad: at most about 50 steps end to end.
            if ((s.fmax - s.fmin) / s.fstep > 50.0f) {
                s.fstep = (s.fmax - s.fmin) / 50.0f;
                s.fstep = std::max(s.fstep, o->step);
            }
            s.fdef = o->defaultValue;
            s.apply = apply;
            break;
        }
        case WIDGET_BUTTON:
            s.kind = Kind::Action;
            s.cvar = nullptr;
            s.action = apply;
            break;
        case WIDGET_WINDOW_BUTTON: {
            s.kind = Kind::Action;
            s.cvar = nullptr;
            std::string windowName = w->windowName;
            if (s.help.empty()) {
                s.help = "Abre a janela " + BenGui::Tr(windowName) + ". Feche a janela para voltar a este menu.";
            }
            s.action = [windowName]() {
                auto window = Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGuiWindow(windowName);
                if (window != nullptr) {
                    Audio_PlaySfx(NA_SE_SY_DECIDE);
                    window->Show();
                    sShipWindow = window;
                }
            };
            s.value = []() { return std::string("Abrir"); };
            break;
        }
        default:
            return false;
    }
    if (locked) {
        std::string why = w->options->disabledTooltip != nullptr ? BenGui::Tr(w->options->disabledTooltip) : "";
        s.help = "Bloqueada agora. " + (why.empty() ? s.help : why);
        s.disabled = []() { return true; };
    }
    rows.push_back(std::move(s));
    return true;
}

void RefreshShipTab(Frame& f) {
    auto gui = Ship::Context::GetInstance()->GetWindow()->GetGui();
    auto menu = std::static_pointer_cast<Ship::Menu>(gui->GetMenu());
    Tab& tab = ShipTab();
    tab.rows.clear();
    if (menu == nullptr) {
        return;
    }
    // The rules behind hidden and locked options, which the menu itself only works out while it is open.
    for (auto& [reason, info] : menu->GetDisabledMap()) {
        if (info.evaluation != nullptr) {
            info.active = info.evaluation(info);
        }
    }
    std::vector<ShipSidebar> sidebars = ShipSidebars(*menu);
    if (sShipPending != -2) {
        if (sShipPending >= 0) {
            sShipListSel = f.sel;
            f.sel = 0;
            f.scroll = 0;
        } else {
            f.sel = sShipListSel;
        }
        sShipSection = sShipPending;
        sShipPending = -2;
    }
    if (sShipSection >= (int)sidebars.size()) {
        sShipSection = -1;
    }
    if (sShipSection < 0) {
        for (int i = 0; i < (int)sidebars.size(); i++) {
            std::vector<Setting> probe;
            int count = 0;
            for (auto& column : sidebars[i].entry->columnWidgets) {
                for (WidgetInfo& w : column) {
                    count += ShipRow(w, probe) ? 1 : 0;
                }
            }
            if (count == 0) {
                continue;
            }
            std::string label = BenGui::Tr(sidebars[i].header) + " › " + BenGui::Tr(sidebars[i].name);
            std::string value = std::to_string(count) + (count == 1 ? " opção" : " opções");
            tab.rows.push_back(Action(
                label, "Todas as opções do menu do 2Ship, sem precisar abrir esse menu. A abre esta seção.",
                [i]() {
                    Audio_PlaySfx(NA_SE_SY_DECIDE);
                    sShipPending = i;
                },
                [value]() { return value; }));
        }
        return;
    }
    const ShipSidebar& side = sidebars[sShipSection];
    tab.rows.push_back(Action(("‹ " + BenGui::Tr(side.header) + " › " + BenGui::Tr(side.name)),
                              "Volta à lista de seções. B também volta.", []() {
                                  Audio_PlaySfx(NA_SE_SY_CANCEL);
                                  sShipPending = -1;
                              }));
    for (auto& column : side.entry->columnWidgets) {
        for (WidgetInfo& w : column) {
            ShipRow(w, tab.rows);
        }
    }
}

// B inside a section goes back to the list instead of leaving Configurações.
bool ShipTabBack(Frame& f) {
    if (!IsShipTab(f) || sShipSection < 0) {
        return false;
    }
    sShipPending = -1;
    return true;
}

// ----- Controls: the game's buttons and what they do, drawn with the icons of the controller in use.

struct ButtonRow {
    std::vector<ButtonPrompt> prompts; // empty: the letter in a ring
    const char* letter;
    const char* name;
    const char* does;
};

const std::vector<ButtonRow>& ButtonRows() {
    static const std::vector<ButtonRow> rows = {
        { { BUTTON_PROMPT_STICK },
          "O",
          "Analógico",
          "Move o Link. Inclinado um pouco ele anda, inclinado todo ele corre." },
        { { BUTTON_PROMPT_A }, "A", "A", "Ação: falar, abrir, pegar, empurrar e confirmar. Correndo, rola." },
        { { BUTTON_PROMPT_B }, "B", "B", "Espada. Nas formas Deku, Goron e Zora, o golpe de cada uma." },
        { { BUTTON_PROMPT_CLEFT, BUTTON_PROMPT_CDOWN, BUTTON_PROMPT_CRIGHT },
          "C",
          "C esquerda, baixo, direita",
          "Usam os itens e as máscaras que você equipou nesses botões no pause." },
        { { BUTTON_PROMPT_CUP }, "C", "C cima", "Olha em primeira pessoa. Quando a Tatl brilha, chama a Tatl." },
        { { BUTTON_PROMPT_Z }, "Z", "Z", "Mira (Z-targeting): trava a câmera e o Link no alvo." },
        { { BUTTON_PROMPT_R }, "R", "R", "Defende com o escudo. As formas Deku, Goron e Zora têm a defesa delas." },
        { { BUTTON_PROMPT_START }, "S", "Start", "Pausa: itens, mapa, missões e máscaras." },
        { {}, "L", "L", "Sem função no jogo original. Algumas opções do port usam este botão." },
    };
    return rows;
}

struct Combo {
    const char* keys;
    const char* does;
};

const std::vector<Combo>& Combos() {
    static const std::vector<Combo> combos = {
        { "Z + A, para trás", "Mortal para trás, para escapar de um ataque." },
        { "Z + A, para o lado", "Salto lateral, com o alvo travado." },
        { "Z + A, para a frente", "Ataque com salto no alvo travado." },
        { "Segurar B e soltar", "Ataque giratório carregado." },
        { "Girar o analógico + B", "Ataque giratório rápido." },
        { "Correr + A", "Rolamento. Na forma Goron, segure A para rolar como bola." },
        { "Forma Deku: A numa flor", "Entra na flor; soltar A lança o Link para planar." },
        { "Forma Zora: segurar A na água", "Nada rápido. B lança as barbatanas." },
        { "Segurar B numa cena", "Acelera a cena (opção Segurar B para acelerar cenas)." },
        { "Back / Select no pause", "Abre o menu de missões da Definitive Edition." },
    };
    return combos;
}

// ----- Extras.

enum ExtrasItem {
    EXTRA_GALLERY,
    EXTRA_CONCEPTS,
    EXTRA_EDITION,
    EXTRA_CONTENT,
    EXTRA_MUSIC,
    EXTRA_OPENING,
    EXTRA_COUNT
};

struct ExtrasEntry {
    const char* name;
    const char* help;
};
const ExtrasEntry kExtras[EXTRA_COUNT] = {
    { "Galeria", "A arte da tela inicial e as máscaras e os restos dos chefes, como aparecem no jogo." },
    { "Artes conceituais", "As artes conceituais da Definitive Edition, que deram origem a estas telas." },
    { "Informações da edição", "Versão, base, commit e data desta compilação." },
    { "Conteúdos adicionais", "O que a Definitive Edition traz além do jogo original, e onde encontrar." },
    { "Música da tela inicial", "Troca a música desta tela entre faixas da trilha original." },
    { "Abertura original", "Mostra a tela de título original do jogo (\"Press Start\")." },
};

// The gallery's pictures: art from this menu's folder, or a page of the game's own icons.
struct Picture {
    const char* file; // nullptr: a page of game icons
    const char* title;
    const char* caption;
};

const std::vector<Picture>& GalleryPictures(int mode) {
    static const std::vector<Picture> gallery = {
        { "background.jpg", "A tela inicial",
          "A lua de Majora sobre a Cidade do Relógio, com a Torre do Relógio e o Link." },
        { nullptr, "As 24 máscaras", "Todas as máscaras de Termina, como aparecem no menu de pausa." },
        { nullptr, "Os restos dos chefes", "Os restos de Odolwa, Goht, Gyorg e Twinmold, que libertam os gigantes." },
    };
    static const std::vector<Picture> concepts = {
        { "concept_1.jpg", "Tela inicial", "A arte conceitual desta tela inicial." },
        { "concept_2.jpg", "Ícone da Definitive Edition", "A arte do ícone e da identidade visual da edição." },
        { "concept_3.jpg", "Caderno dos Bombers", "A arte conceitual do menu de missões e do caderno." },
        { "concept_4.jpg", "A lua sobre a Cidade do Relógio", "O Link num telhado, diante da lua que cai." },
        { "concept_5.jpg", "Link e a Máscara de Majora", "O Link de perfil, com a máscara e a Tatl ao lado." },
        { "concept_6.jpg", "A Máscara de Majora", "A máscara sozinha, no meio do céu roxo." },
        { "concept_7.jpg", "Skull Kid", "O Skull Kid com a máscara, diante da lua, e a Tatl." },
        { "concept_8.jpg", "O Carnaval do Tempo", "A praça da Cidade do Relógio enfeitada para o festival." },
        { "concept_9.jpg", "As transformações", "As formas Deku, Goron, Zora e a Fierce Deity." },
        { "concept_10.jpg", "Anju e Kafei", "Os dois juntos, com um ovo da máscara nas mãos." },
        { "concept_11.jpg", "O gigante", "Um gigante atrás das ruínas, ao pôr do sol." },
        { "concept_12.jpg", "Pântano do Sul", "As casas de madeira sobre a água do pântano." },
        { "concept_13.jpg", "Montanhas Nevadas", "O pico de Snowhead coberto de neve." },
        { "concept_14.jpg", "Great Bay", "O laboratório e o píer sobre o mar." },
        { "concept_15.jpg", "Ikana", "As ruínas e a caveira de pedra do vale de Ikana." },
        { "concept_16.jpg", "A ocarina e o livro", "A Ocarina do Tempo e um livro antigo, com a Tatl." },
        { "concept_17.jpg", "As máscaras", "Uma prateleira com máscaras de Termina." },
        { "concept_18.jpg", "Os quatro heróis", "As silhuetas do Link e das três formas sob a lua." },
        { "concept_19.jpg", "Esboços", "Os desenhos a lápis do Link, da máscara e da Torre do Relógio." },
    };
    return mode == 0 ? gallery : concepts;
}

struct MaskName {
    int item;
    const char* name;
};
const MaskName kMasks[24] = {
    { ITEM_MASK_DEKU, "Máscara Deku" },
    { ITEM_MASK_GORON, "Máscara Goron" },
    { ITEM_MASK_ZORA, "Máscara Zora" },
    { ITEM_MASK_FIERCE_DEITY, "Divindade Feroz" },
    { ITEM_MASK_TRUTH, "Máscara da Verdade" },
    { ITEM_MASK_KAFEIS_MASK, "Máscara do Kafei" },
    { ITEM_MASK_ALL_NIGHT, "Máscara da Noite Toda" },
    { ITEM_MASK_BUNNY, "Capuz de Coelho" },
    { ITEM_MASK_KEATON, "Máscara Keaton" },
    { ITEM_MASK_GARO, "Máscara do Garo" },
    { ITEM_MASK_ROMANI, "Máscara da Romani" },
    { ITEM_MASK_CIRCUS_LEADER, "Líder do Circo" },
    { ITEM_MASK_POSTMAN, "Chapéu do Carteiro" },
    { ITEM_MASK_COUPLE, "Máscara do Casal" },
    { ITEM_MASK_GREAT_FAIRY, "Máscara da Grande Fada" },
    { ITEM_MASK_GIBDO, "Máscara Gibdo" },
    { ITEM_MASK_DON_GERO, "Máscara do Don Gero" },
    { ITEM_MASK_KAMARO, "Máscara do Kamaro" },
    { ITEM_MASK_CAPTAIN, "Chapéu do Capitão" },
    { ITEM_MASK_STONE, "Máscara de Pedra" },
    { ITEM_MASK_BREMEN, "Máscara Bremen" },
    { ITEM_MASK_BLAST, "Máscara Explosiva" },
    { ITEM_MASK_SCENTS, "Máscara dos Aromas" },
    { ITEM_MASK_GIANT, "Máscara do Gigante" },
};

struct ContentEntry {
    const char* name;
    const char* text;
    const char* where;
};

const std::vector<ContentEntry>& ContentEntries() {
    static const std::vector<ContentEntry> entries = {
        { "Tela inicial da Definitive Edition", "Esta tela, com arquivos, configurações, controles, extras e créditos.",
          "Ao abrir o jogo" },
        { "Menu de missões", "Missões principais e secundárias, máscaras, pessoas, lugares e registros, lidos do save.",
          "Pause, pergaminho QUESTS (ou L, Back / Select)" },
        { "Recursos do Majora's Mask 3D",
          "Caderno dos Bombers desde o início, estátuas que salvam e continuam, "
          "Goron que rola sem segurar A, prêmios trocados como no 3DS e mais.",
          "Configurações > Definitive" },
        { "Iluminação estilo 3DS e cel shading", "Luz e cores do 3DS, visual de desenho com contorno preto.",
          "Configurações > Definitive" },
        { "Segurar B para acelerar cenas", "Uma cena anda mais rápido enquanto B está apertado.",
          "Configurações > Acessibilidade" },
        { "Ícones de botões do seu controle",
          "Os botões aparecem como os do controle em uso: Xbox, PlayStation, "
          "Nintendo ou teclado.",
          "Configurações > Controles" },
        { "Pacotes de tradução", "O texto do jogo em português com um pacote de tradução.", "Configurações > Idioma" },
        { "Mods e músicas próprias", "Pacotes de texturas, mods e uma pasta de músicas suas.",
          "Configurações > Definitive > Todas as opções" },
        { "Controles na tela, giroscópio e controle Bluetooth",
          "No Android, controles de toque e mira pelo giroscópio.", "Configurações > Controles" },
    };
    return entries;
}

// ----- Credits: real people and projects only.

struct CreditPage {
    const char* title;
    std::vector<std::pair<std::string, std::string>> lines; // name, role
};

std::vector<CreditPage>& CreditPages() {
    static std::vector<CreditPage> pages;
    if (!pages.empty()) {
        return pages;
    }
    pages.push_back({ "Majora's Mask Definitive Edition",
                      {
                          { "Rafael (lokodoshape-png)", "Direção, artes conceituais e testes" },
                          { "Claude (Anthropic)", "Programação das telas e dos recursos novos" },
                      } });
    pages.push_back({ "Desenvolvedores originais",
                      {
                          { "Nintendo EAD", "The Legend of Zelda: Majora's Mask (2000)" },
                          { "Grezzo e Nintendo", "Majora's Mask 3D (2015), inspiração dos recursos 3DS" },
                          { "zeldaret", "Decompilação do Majora's Mask, base de todo o port" },
                          { "Harbour Masters", "2 Ship 2 Harkinian, o port para PC" },
                          { "libultraship", "A biblioteca que roda o jogo em PC e Android" },
                      } });
    // The 2 Ship 2 Harkinian contributors, in pages of 14.
    const size_t perPage = 14;
    for (size_t i = 0; i < BenGui::contributors.size(); i += perPage) {
        CreditPage page{ "Colaboradores do 2 Ship 2 Harkinian", {} };
        for (size_t j = i; j < std::min(BenGui::contributors.size(), i + perPage); j++) {
            page.lines.push_back({ BenGui::contributors[j], "" });
        }
        pages.push_back(page);
    }
    return pages;
}

// ----- Navigation.

enum class Nav { Up, Down, Left, Right, Accept, Back, Prev, Next };

const char* ScreenTitle(const Frame& f) {
    switch (f.screen) {
        case Screen::Main:
            return "Tela inicial";
        case Screen::Files:
            return f.mode == 0 ? "Carregar Jogo" : "Arquivos de Jogo";
        case Screen::Settings:
            return "Configurações";
        case Screen::Controls:
            return "Controles";
        case Screen::Extras:
            return "Extras";
        case Screen::Gallery:
            return f.mode == 0 ? "Galeria" : "Artes conceituais";
        case Screen::Edition:
            return "Informações da edição";
        case Screen::Content:
            return "Conteúdos adicionais";
        case Screen::Credits:
            return "Créditos";
    }
    return "";
}

int PageCount(const Frame& f) {
    switch (f.screen) {
        case Screen::Settings:
            return (int)SettingsTabs().size();
        case Screen::Controls:
            return 3;
        case Screen::Gallery:
            return (int)GalleryPictures(f.mode).size();
        case Screen::Credits:
            return (int)CreditPages().size();
        default:
            return 1;
    }
}

const char* ControlsPageName(int page) {
    static const char* names[] = { "Botões", "Combinações", "Configurar" };
    return names[std::clamp(page, 0, 2)];
}

// The rows a screen's selection moves through.
int RowCount(const Frame& f) {
    switch (f.screen) {
        case Screen::Main:
            return ITEM_COUNT;
        case Screen::Files:
            return SlotCount();
        case Screen::Settings:
            return (int)SettingsTabs()[std::clamp(f.page, 0, (int)SettingsTabs().size() - 1)].rows.size();
        case Screen::Controls:
            return f.page == 2 ? (int)SettingsTabs()[4].rows.size() : 0;
        case Screen::Extras:
            return EXTRA_COUNT;
        case Screen::Content:
            return (int)ContentEntries().size();
        default:
            return 0;
    }
}

const std::vector<Setting>* RowsOf(const Frame& f) {
    if (f.screen == Screen::Settings) {
        return &SettingsTabs()[std::clamp(f.page, 0, (int)SettingsTabs().size() - 1)].rows;
    }
    if (f.screen == Screen::Controls && f.page == 2) {
        return &SettingsTabs()[4].rows;
    }
    return nullptr;
}

// The Extras' music row works like a setting.
const Setting& MusicSetting() {
    return SettingsTabs()[2].rows[5];
}

void Push(Screen screen, int mode = 0) {
    Frame f;
    f.screen = screen;
    f.mode = mode;
    if (screen == Screen::Files) {
        int slot = ContinueSlot(sFileSelect);
        f.sel = mode == 0 && slot >= 0 ? slot : 0;
    }
    sStack.push_back(f);
    sScreenT = 0.0f;
    sScreenDir = 1.0f;
    Audio_PlaySfx(NA_SE_SY_DECIDE);
}

void Pop() {
    if (sStack.size() <= 1) {
        return;
    }
    sStack.pop_back();
    sScreenT = 0.0f;
    sScreenDir = -1.0f;
    sCursorY = -1.0f;
    Audio_PlaySfx(NA_SE_SY_CANCEL);
}

void Show() {
    sActive = true;
    sLeaving = false;
    sConfirm.open = false;
    sPending = Pending::None;
    sHandedOver = 0;
    sStack.clear();
    sStack.reserve(16); // a tap may push a screen while the one under is still being drawn
    Frame f;
    f.sel = ContinueSlot(sFileSelect) >= 0 ? ITEM_CONTINUE : ITEM_START;
    sStack.push_back(f);
    sScreenT = 0.0f;
    sScreenDir = -1.0f;
    sCursorY = -1.0f;
}

void Ask(const std::string& title, const std::string& text, const std::string& note, bool yesFirst,
         std::function<void()> onYes, const std::string& yes = "Sim", const std::string& no = "Não") {
    sConfirm.open = true;
    sConfirm.title = title;
    sConfirm.text = text;
    sConfirm.note = note;
    sConfirm.yes = yes;
    sConfirm.no = no;
    sConfirm.yesSelected = yesFirst;
    sConfirm.onYes = onYes;
    sModalT = 0.0f;
    Audio_PlaySfx(NA_SE_SY_DECIDE);
}

void CloseConfirm(bool yes) {
    sConfirm.open = false;
    if (yes && sConfirm.onYes) {
        auto action = sConfirm.onYes;
        sConfirm.onYes = nullptr;
        action();
    } else {
        Audio_PlaySfx(NA_SE_SY_CANCEL);
    }
}

bool Busy() {
    return sEraseSlot >= 0 || sPending != Pending::None || sLeaving;
}

std::string FileLabel(int slot) {
    FileInfo info = ReadFile(sFileSelect, slot);
    char text[64];
    if (info.occupied) {
        snprintf(text, sizeof(text), "Arquivo %d (%s)", slot + 1, info.name.c_str());
    } else {
        snprintf(text, sizeof(text), "Arquivo %d", slot + 1);
    }
    return text;
}

void LoadSlot(int slot) {
    Audio_PlaySfx(NA_SE_SY_FSEL_DECIDE_L);
    sPending = Pending::Load;
    sPendingSlot = slot;
}

void NewGame(int slot) {
    Audio_PlaySfx(NA_SE_SY_FSEL_DECIDE_L);
    sPending = Pending::NewGame;
    sPendingSlot = slot;
}

void AskNewGame(int slot) {
    Ask("Iniciar Jogo", "O jogo novo começa no " + FileLabel(slot) + ".",
        "Você escolhe o nome do Link em seguida. Os outros arquivos continuam guardados.", true,
        [slot]() { NewGame(slot); });
}

void AskErase(int slot) {
    Ask(
        "Excluir arquivo", "Excluir o " + FileLabel(slot) + "?",
        "O progresso deste arquivo, e o save da coruja dele, serão apagados de vez.", false,
        [slot]() {
            sEraseSlot = slot;
            sEraseWriting = false;
            Audio_PlaySfx(NA_SE_EV_DIAMOND_SWITCH);
        },
        "Excluir", "Cancelar");
}

void ActivateMain(int item) {
    FileSelectState* fs = sFileSelect;
    switch (item) {
        case ITEM_START: {
            int slot = EmptySlot(fs);
            if (slot < 0) {
                Audio_PlaySfx(NA_SE_SY_ERROR);
                Toast("Todos os arquivos estão em uso. Exclua um em Arquivos de Jogo.");
                return;
            }
            AskNewGame(slot);
            break;
        }
        case ITEM_CONTINUE: {
            int slot = ContinueSlot(fs);
            if (slot < 0) {
                Audio_PlaySfx(NA_SE_SY_ERROR);
                Toast("Nenhum jogo salvo ainda. Escolha Iniciar Jogo.");
                return;
            }
            LoadSlot(slot);
            break;
        }
        case ITEM_LOAD:
            Push(Screen::Files, 0);
            break;
        case ITEM_FILES:
            Push(Screen::Files, 1);
            break;
        case ITEM_SETTINGS:
            Push(Screen::Settings);
            break;
        case ITEM_CONTROLS:
            Push(Screen::Controls);
            break;
        case ITEM_EXTRAS:
            Push(Screen::Extras);
            break;
        case ITEM_CREDITS:
            Push(Screen::Credits);
            break;
        case ITEM_QUIT:
            Ask(
                "Sair", "Fechar o jogo?", "O que não foi salvo no jogo não fica guardado.", false,
                []() { sPending = Pending::Quit; }, "Sair", "Ficar");
            break;
    }
}

// The buttons under a file in Arquivos de Jogo.
std::vector<const char*> FileActions(int slot) {
    if (SlotOccupied(sFileSelect, slot)) {
        return { "Jogar", "Excluir" };
    }
    return { "Criar jogo" };
}

void RunFileAction(int slot, int action) {
    if (SlotOccupied(sFileSelect, slot)) {
        if (action == 0) {
            LoadSlot(slot);
        } else {
            AskErase(slot);
        }
    } else {
        AskNewGame(slot);
    }
}

void AcceptFile(Frame& f) {
    int slot = f.sel;
    if (f.mode == 0) {
        if (SlotOccupied(sFileSelect, slot)) {
            LoadSlot(slot);
        } else {
            AskNewGame(slot);
        }
        return;
    }
    if (f.focus == 0) {
        f.focus = 1;
        f.action = 0;
        Audio_PlaySfx(NA_SE_SY_DECIDE);
    } else {
        RunFileAction(slot, f.action);
    }
}

void ActivateExtra(Frame& f, int item, int dir) {
    switch (item) {
        case EXTRA_GALLERY:
            Push(Screen::Gallery, 0);
            break;
        case EXTRA_CONCEPTS:
            Push(Screen::Gallery, 1);
            break;
        case EXTRA_EDITION:
            Push(Screen::Edition);
            break;
        case EXTRA_CONTENT:
            Push(Screen::Content);
            break;
        case EXTRA_MUSIC:
            ChangeSetting(MusicSetting(), dir);
            break;
        case EXTRA_OPENING:
            sPending = Pending::OriginalTitle;
            break;
    }
}

void ChangePage(Frame& f, int dir) {
    int n = PageCount(f);
    if (n <= 1) {
        return;
    }
    f.page = (f.page + dir + n) % n;
    f.sel = 0;
    Audio_PlaySfx(NA_SE_SY_WIN_SCROLL_LEFT);
}

void Navigate(Nav nav) {
    if (Busy()) {
        return;
    }
    if (sConfirm.open) {
        switch (nav) {
            case Nav::Left:
            case Nav::Right:
            case Nav::Up:
            case Nav::Down:
                sConfirm.yesSelected = !sConfirm.yesSelected;
                Audio_PlaySfx(NA_SE_SY_CURSOR);
                break;
            case Nav::Accept:
                CloseConfirm(sConfirm.yesSelected);
                break;
            case Nav::Back:
                CloseConfirm(false);
                break;
            default:
                break;
        }
        return;
    }
    Frame& f = Cur();
    int rows = RowCount(f);
    auto moveSel = [&](int d) {
        if (rows > 0) {
            f.sel = (f.sel + d + rows) % rows;
            Audio_PlaySfx(NA_SE_SY_CURSOR);
        }
    };

    if (nav == Nav::Back) {
        if (ShipTabBack(f)) {
            Audio_PlaySfx(NA_SE_SY_CANCEL);
        } else if (f.screen == Screen::Files && f.focus == 1) {
            f.focus = 0;
            Audio_PlaySfx(NA_SE_SY_CANCEL);
        } else if (f.screen == Screen::Gallery && f.zoom) {
            f.zoom = false;
            Audio_PlaySfx(NA_SE_SY_CANCEL);
        } else {
            Pop();
        }
        return;
    }
    if (nav == Nav::Prev || nav == Nav::Next) {
        ChangePage(f, nav == Nav::Prev ? -1 : 1);
        return;
    }

    switch (f.screen) {
        case Screen::Main:
            if (nav == Nav::Up || nav == Nav::Down) {
                moveSel(nav == Nav::Up ? -1 : 1);
            } else if (nav == Nav::Accept) {
                ActivateMain(f.sel);
            }
            break;
        case Screen::Files:
            if (f.focus == 0) {
                if (nav == Nav::Up || nav == Nav::Down) {
                    moveSel(nav == Nav::Up ? -1 : 1);
                } else if (nav == Nav::Accept || (nav == Nav::Right && f.mode == 1)) {
                    AcceptFile(f);
                }
            } else {
                int n = (int)FileActions(f.sel).size();
                if (nav == Nav::Left || nav == Nav::Right) {
                    f.action = (f.action + (nav == Nav::Left ? -1 : 1) + n) % n;
                    Audio_PlaySfx(NA_SE_SY_CURSOR);
                } else if (nav == Nav::Up || nav == Nav::Down) {
                    f.focus = 0;
                    moveSel(nav == Nav::Up ? -1 : 1);
                } else if (nav == Nav::Accept) {
                    RunFileAction(f.sel, std::clamp(f.action, 0, n - 1));
                }
            }
            break;
        case Screen::Settings:
        case Screen::Controls: {
            const std::vector<Setting>* list = RowsOf(f);
            if (list == nullptr || list->empty()) {
                if (nav == Nav::Left || nav == Nav::Right) {
                    ChangePage(f, nav == Nav::Left ? -1 : 1);
                }
                break;
            }
            if (nav == Nav::Up || nav == Nav::Down) {
                moveSel(nav == Nav::Up ? -1 : 1);
            } else if (nav == Nav::Left || nav == Nav::Right) {
                const Setting& s = (*list)[std::clamp(f.sel, 0, rows - 1)];
                if (s.kind == Kind::Action) {
                    // Left / right on a button changes the tab, like L / R.
                    ChangePage(f, nav == Nav::Left ? -1 : 1);
                } else {
                    ChangeSetting(s, nav == Nav::Left ? -1 : 1);
                }
            } else if (nav == Nav::Accept) {
                ChangeSetting((*list)[std::clamp(f.sel, 0, rows - 1)], 0);
            }
            break;
        }
        case Screen::Extras:
            if (nav == Nav::Up || nav == Nav::Down) {
                moveSel(nav == Nav::Up ? -1 : 1);
            } else if (nav == Nav::Accept) {
                ActivateExtra(f, f.sel, 0);
            } else if ((nav == Nav::Left || nav == Nav::Right) && f.sel == EXTRA_MUSIC) {
                ActivateExtra(f, f.sel, nav == Nav::Left ? -1 : 1);
            }
            break;
        case Screen::Gallery:
            if (nav == Nav::Left || nav == Nav::Right) {
                ChangePage(f, nav == Nav::Left ? -1 : 1);
            } else if (nav == Nav::Accept && GalleryPictures(f.mode)[f.page].file != nullptr) {
                f.zoom = !f.zoom;
                Audio_PlaySfx(f.zoom ? NA_SE_SY_DECIDE : NA_SE_SY_CANCEL);
            }
            break;
        case Screen::Content:
            if (nav == Nav::Up || nav == Nav::Down) {
                moveSel(nav == Nav::Up ? -1 : 1);
            }
            break;
        case Screen::Credits:
            if (nav == Nav::Left || nav == Nav::Right || nav == Nav::Accept) {
                ChangePage(f, nav == Nav::Left ? -1 : 1);
            }
            break;
        case Screen::Edition:
            break;
    }
}

// ----- Input: the game's controller (stick / d-pad, A, B, Start, L / Z and R), the keyboard and taps.

void ClearInput(Input* input) {
    input->press.button = 0;
    input->rel.button = 0;
    input->rel.stick_x = input->rel.stick_y = 0;
    input->press.stick_x = input->press.stick_y = 0;
    input->cur.button = 0;
    input->cur.stick_x = input->cur.stick_y = 0;
}

// A held direction moves once, then repeats after a pause.
void Repeat(int dir, int& last, int& timer, Nav neg, Nav pos) {
    if (dir == 0) {
        last = 0;
        return;
    }
    if (dir != last) {
        last = dir;
        timer = 10;
    } else if (--timer > 0) {
        return;
    } else {
        timer = 4;
    }
    Navigate(dir < 0 ? neg : pos);
}

// The game's controller (the keyboard's game keys and the touch buttons arrive here too). While the arrows, Enter
// or Backspace are in use the keyboard is read on the GUI side instead (KeyboardNav), so a key that is also mapped
// to the controller does not count twice.
void HandlePad(Input* input) {
    if (sKeyboardNavTime > 0.0f) {
        return;
    }
    u16 press = input->press.button, held = input->cur.button;
    s8 stickX = input->cur.stick_x, stickY = input->cur.stick_y;
    if (press != 0 || std::abs(stickX) > 30 || std::abs(stickY) > 30) {
        sTouchMode = false;
    }
    if (CHECK_BTN_ANY(press, BTN_A | BTN_START)) {
        Navigate(Nav::Accept);
        return;
    }
    if (CHECK_BTN_ANY(press, BTN_B)) {
        Navigate(Nav::Back);
        return;
    }
    if (CHECK_BTN_ANY(press, BTN_L | BTN_Z)) {
        Navigate(Nav::Prev);
        return;
    }
    if (CHECK_BTN_ANY(press, BTN_R)) {
        Navigate(Nav::Next);
        return;
    }
    int dy = 0, dx = 0;
    if (stickY > 30 || CHECK_BTN_ANY(held, BTN_DUP)) {
        dy = -1;
    } else if (stickY < -30 || CHECK_BTN_ANY(held, BTN_DDOWN)) {
        dy = 1;
    }
    if (stickX < -30 || CHECK_BTN_ANY(held, BTN_DLEFT)) {
        dx = -1;
    } else if (stickX > 30 || CHECK_BTN_ANY(held, BTN_DRIGHT)) {
        dx = 1;
    }
    Repeat(dy, sRepeatY, sRepeatYTimer, Nav::Up, Nav::Down);
    Repeat(dx, sRepeatX, sRepeatXTimer, Nav::Left, Nav::Right);
}

// The arrows, Enter, Backspace and Page Up / Page Down, read once per GUI frame.
void KeyboardNav(float dt) {
    sKeyboardNavTime = std::max(0.0f, sKeyboardNavTime - dt);
    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    const ImGuiKey keys[] = { ImGuiKey_UpArrow,    ImGuiKey_DownArrow, ImGuiKey_LeftArrow,
                              ImGuiKey_RightArrow, ImGuiKey_Enter,     ImGuiKey_KeypadEnter,
                              ImGuiKey_Backspace,  ImGuiKey_PageUp,    ImGuiKey_PageDown };
    bool any = false;
    for (ImGuiKey key : keys) {
        any = any || ImGui::IsKeyDown(key);
    }
    if (!any) {
        return;
    }
    sKeyboardNavTime = 0.3f;
    sTouchMode = false;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        Navigate(Nav::Up);
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        Navigate(Nav::Down);
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        Navigate(Nav::Left);
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        Navigate(Nav::Right);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        Navigate(Nav::Accept);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        Navigate(Nav::Back);
    } else if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) {
        Navigate(Nav::Prev);
    } else if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) {
        Navigate(Nav::Next);
    }
}

// ----- Textures. Loaded once from the archives with smooth (linear) filtering, so the art stays clean when the
// screen is not 1080p and the moon can move by fractions of a pixel.

struct Tex {
    bool tried = false;
    ImTextureID id = nullptr;
    ImVec2 size = ImVec2(0, 0);
};
std::map<std::string, Tex> sTextures;

const Tex& Load(const char* file) {
    Tex& tex = sTextures[file];
    if (tex.tried) {
        return tex;
    }
    tex.tried = true;
    std::string path = std::string("overlays/titlemenu/") + file;
    auto context = Ship::Context::GetInstance();
    auto archives = context->GetResourceManager()->GetArchiveManager();
    auto window = std::dynamic_pointer_cast<Fast::Fast3dWindow>(context->GetWindow());
    if (archives == nullptr || !archives->HasFile(path) || window == nullptr) {
        SPDLOG_WARN("Title menu: missing {}", path);
        return tex;
    }
    auto initData = std::make_shared<Ship::ResourceInitData>();
    initData->Format = RESOURCE_FORMAT_BINARY;
    initData->Type = static_cast<uint32_t>(RESOURCE_TYPE_GUI_TEXTURE);
    initData->ResourceVersion = 0;
    initData->Path = path;
    auto image =
        std::static_pointer_cast<Ship::GuiTexture>(context->GetResourceManager()->LoadResource(path, false, initData));
    auto interpreter = window->GetInterpreterWeak().lock();
    if (image == nullptr || image->Data == nullptr || interpreter == nullptr) {
        SPDLOG_WARN("Title menu: could not read {}", path);
        return tex;
    }
    Fast::GfxRenderingAPI* api = interpreter->GetCurrentRenderingAPI();
    // The renderers only give a texture linear filtering while the game's filter is linear, so switch it for the
    // moment of the upload.
    Fast::FilteringMode filter = api->GetTextureFilter();
    if (filter != Fast::FILTER_LINEAR) {
        api->SetTextureFilter(Fast::FILTER_LINEAR);
    }
    uint32_t id = api->NewTexture();
    api->SelectTexture(0, id);
    api->SetSamplerParameters(0, true, G_TX_CLAMP, G_TX_CLAMP);
    api->UploadTexture(image->Data, image->Metadata.Width, image->Metadata.Height);
    if (filter != Fast::FILTER_LINEAR) {
        api->SetTextureFilter(filter);
    }
    tex.id = api->GetTextureById(id);
    tex.size = ImVec2((float)image->Metadata.Width, (float)image->Metadata.Height);
    context->GetResourceManager()->UnloadResource(path); // the pixels now live on the GPU
    return tex;
}

// Draws a PNG at its own size times `scale` (page units are pixels at 1080p), its left edge at x and centred on cy.
// Returns the drawn width.
float Image(const Page& pg, const char* file, float x, float cy, ImU32 tint = IM_COL32_WHITE, float scale = 1.0f,
            float scaleY = -1.0f) {
    const Tex& tex = Load(file);
    if (tex.id == nullptr) {
        return 0.0f;
    }
    float w = tex.size.x * scale, h = tex.size.y * (scaleY > 0 ? scaleY : scale);
    pg.dl->AddImage(tex.id, pg.P(x, cy - h / 2), pg.P(x + w, cy + h / 2), ImVec2(0, 0), ImVec2(1, 1), tint);
    return w;
}

ImVec2 Size(const char* file) {
    return Load(file).size;
}

// ----- Drawing.

float Ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

bool GameRect(ImVec2& pos, ImVec2& size) {
    ImGuiWindow* gameWindow = ImGui::FindWindowByName("Main Game");
    if (gameWindow == nullptr || gameWindow->Hidden) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        pos = vp->Pos;
        size = vp->Size;
    } else {
        pos = gameWindow->InnerRect.Min;
        size = gameWindow->InnerRect.GetSize();
    }
    return size.x > 0 && size.y > 0;
}

bool Effects() {
    return CVarGetInteger(CVAR_EFFECTS, 1) != 0;
}

// A soft glow: rings of fading colour.
void Glow(const Page& pg, ImVec2 c, float r, ImU32 col, float strength) {
    for (int i = 5; i >= 0; i--) {
        float rr = r * (0.35f + i * 0.25f);
        pg.dl->AddCircleFilled(pg.P(c.x, c.y), pg.S(rr), Alpha(col, strength * (0.10f + (5 - i) * 0.05f)), 32);
    }
}

// The picture fits the screen; on wider or taller screens a blurred copy fills the rest. The moon drifts and
// breathes slowly, its eyes and Tatl glow, and motes of light rise through the air.
void DrawBackground(ImDrawList* dl, ImVec2 pos, ImVec2 size, const Page& pg, float a) {
    ImU32 white = Alpha(IM_COL32_WHITE, a);
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Alpha(Col(10, 4, 20), a));
    const Tex& blur = Load("background_blur.jpg");
    if (blur.id != nullptr) {
        float cover = std::max(size.x / kW, size.y / kH);
        ImVec2 c(pos.x + size.x / 2, pos.y + size.y / 2);
        dl->AddImage(blur.id, ImVec2(c.x - kW * cover / 2, c.y - kH * cover / 2),
                     ImVec2(c.x + kW * cover / 2, c.y + kH * cover / 2), ImVec2(0, 0), ImVec2(1, 1),
                     Alpha(Col(150, 140, 160), a));
    }
    ImVec2 p0 = pg.P(0, 0), p1 = pg.P(kW, kH);
    const Tex& bg = Load("background.jpg");
    if (bg.id != nullptr) {
        dl->AddImage(bg.id, p0, p1, ImVec2(0, 0), ImVec2(1, 1), white);
    }

    bool fx = Effects();
    float t = sTime;
    // The moon: always a little larger than the painted one under it, so the painted edge never shows.
    float scale = 1.009f + 0.005f * std::sin(t * 0.45f);
    float dx = 1.2f * std::sin(t * 0.23f);
    float dy = -1.5f + 1.5f * std::sin(t * 0.31f);
    auto moonPoint = [&](ImVec2 p) {
        return ImVec2(kMoonCX + (p.x - kMoonCX) * scale + dx, kMoonCY + (p.y - kMoonCY) * scale + dy);
    };
    dl->PushClipRect(p0, p1, true);
    const Tex& moon = Load("moon.png");
    if (fx && moon.id != nullptr) {
        ImVec2 m0 = moonPoint(ImVec2(kMoonX0, 0)), m1 = moonPoint(ImVec2(kMoonX0 + moon.size.x, moon.size.y));
        dl->AddImage(moon.id, pg.P(m0.x, m0.y), pg.P(m1.x, m1.y), ImVec2(0, 0), ImVec2(1, 1), white);
    }
    if (fx) {
        // The eyes smoulder, one a little after the other.
        for (int i = 0; i < 2; i++) {
            float pulse = 0.55f + 0.45f * std::sin(t * 1.6f + i * 1.3f);
            Glow(pg, moon.id != nullptr ? moonPoint(kMoonEyes[i]) : kMoonEyes[i], 46.0f, Col(255, 110, 30), a * pulse);
        }
        float flutter = 0.6f + 0.25f * std::sin(t * 5.0f) + 0.15f * std::sin(t * 13.0f);
        Glow(pg, kFairy, 40.0f, Col(255, 244, 200), a * flutter);
        // Motes of light drifting up, each on its own loop.
        for (int i = 0; i < 42; i++) {
            float seed = std::fmod(i * 0.61803f, 1.0f);
            float speed = 18.0f + 30.0f * std::fmod(i * 0.377f, 1.0f);
            float life = std::fmod(t * speed / kH + seed, 1.0f);
            float x = std::fmod(seed * 1931.0f + i * 97.0f, kW) + 26.0f * std::sin(t * 0.7f + i);
            float y = kH + 20 - life * (kH + 40);
            float fade = std::sin(life * 3.14159f);
            ImU32 col = (i % 3 == 0) ? Col(255, 170, 90) : Col(200, 150, 255);
            float r = 1.4f + 2.2f * std::fmod(i * 0.29f, 1.0f);
            dl->AddCircleFilled(pg.P(x, y), pg.S(r * 2.4f), Alpha(col, a * fade * 0.12f), 12);
            dl->AddCircleFilled(pg.P(x, y), pg.S(r), Alpha(col, a * fade * 0.75f), 12);
        }
    }
    // DEFINITIVE EDITION under the logo, with a slow shimmer.
    float shimmer = fx ? 0.85f + 0.15f * std::sin(t * 1.1f) : 1.0f;
    float ew = Size("edition.png").x * 0.82f;
    Image(pg, "edition.png", 635 - ew / 2, 384, Alpha(IM_COL32_WHITE, a * shimmer), 0.82f);
    dl->PopClipRect();

    // Soft seams where the blurred copy shows.
    float feather = pg.S(40);
    ImU32 dark = Alpha(Col(10, 4, 20, 200), a), clear = Col(10, 4, 20, 0);
    if (p0.x > pos.x + 1) {
        dl->AddRectFilledMultiColor(ImVec2(p0.x, p0.y), ImVec2(p0.x + feather, p1.y), dark, clear, clear, dark);
        dl->AddRectFilledMultiColor(ImVec2(p1.x - feather, p0.y), ImVec2(p1.x, p1.y), clear, dark, dark, clear);
    }
    if (p0.y > pos.y + 1) {
        dl->AddRectFilledMultiColor(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y + feather), dark, dark, clear, clear);
        dl->AddRectFilledMultiColor(ImVec2(p0.x, p1.y - feather), ImVec2(p1.x, p1.y), clear, clear, dark, dark);
    }
}

// A dark veil behind the buttons, fading to the right and at the top and bottom.
void DrawVeil(const Page& pg, float a) {
    ImU32 dark = Alpha(Col(12, 4, 24, 160), a), clear = Col(12, 4, 24, 0);
    float x0 = 70, x1 = 900, y0 = 412, y1 = 975;
    pg.dl->AddRectFilledMultiColor(pg.P(x0, y0), pg.P(x1, y1), dark, clear, clear, dark);
    pg.dl->AddRectFilledMultiColor(pg.P(x0, y0 - 60), pg.P(x1, y0), clear, clear, clear, dark);
    pg.dl->AddRectFilledMultiColor(pg.P(x0, y1), pg.P(x1, y1 + 60), dark, clear, clear, clear);
}

void DrawSeparator(const Page& pg, float y, float a) {
    ImU32 line = Alpha(Col(205, 175, 225, 70), a), clear = Col(205, 175, 225, 0);
    pg.dl->AddRectFilledMultiColor(pg.P(172, y), pg.P(420, y + 1.5f), clear, line, line, clear);
    pg.dl->AddRectFilledMultiColor(pg.P(420, y), pg.P(680, y + 1.5f), line, clear, clear, line);
}

// ----- Widgets.

ImU32 RowTint(bool selected, float a) {
    return Alpha(selected ? Col(255, 255, 255) : Col(222, 212, 228), a);
}

// A tap or click on a rectangle of the page. The pointer is read directly rather than through ImGui's hovered
// window, so the game's own full-screen window can never end up in front of the menu and swallow the taps.
bool Tap(const Page& pg, float x, float y, float w, float h, bool* hovered = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 p0 = pg.P(x, y), p1 = pg.P(x + w, y + h);
    auto inside = [&](ImVec2 m) { return m.x >= p0.x && m.x < p1.x && m.y >= p0.y && m.y < p1.y; };
    bool over = sPointerInputs && ImGui::IsMousePosValid(&io.MousePos) && inside(io.MousePos);
    if (hovered != nullptr) {
        *hovered = over;
    }
    if (over && !sTapTaken && io.MouseReleased[0] && inside(io.MouseClickedPos[0])) {
        sTapTaken = true;
        return true;
    }
    return false;
}

// The pointer moved over a row: select it, as the controller would.
bool PointerMoved() {
    ImGuiIO& io = ImGui::GetIO();
    return io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
}

void Arrow(const Page& pg, float cx, float cy, float dir, ImU32 col, float s = 1.0f) {
    pg.dl->AddTriangleFilled(pg.P(cx - 7 * s * dir, cy - 11 * s), pg.P(cx + 8 * s * dir, cy),
                             pg.P(cx - 7 * s * dir, cy + 11 * s), col);
}

void Txt(const Page& pg, float x, float y, float size, ImU32 col, const std::string& text, ImFont* font = nullptr,
         float wrap = 0.0f) {
    if (wrap <= 0.0f) {
        QuestUi::Text(pg, x + 1.5f, y + 2.0f, size, Col(0, 0, 0, (int)(150 * ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255)),
                      text.c_str(), 0.0f, font);
    }
    QuestUi::Text(pg, x, y, size, col, text.c_str(), wrap, font);
}

float TxtW(const Page& pg, float size, const std::string& text, ImFont* font = nullptr) {
    return QuestUi::TextWidth(pg, size, text.c_str(), font);
}

void TxtRight(const Page& pg, float right, float y, float size, ImU32 col, const std::string& text,
              ImFont* font = nullptr) {
    Txt(pg, right - TxtW(pg, size, text, font), y, size, col, text, font);
}

void TxtCenter(const Page& pg, float cx, float y, float size, ImU32 col, const std::string& text,
               ImFont* font = nullptr) {
    Txt(pg, cx - TxtW(pg, size, text, font) / 2, y, size, col, text, font);
}

// A purple button, gold-edged and glowing while selected. Returns true when tapped.
bool Button(const Page& pg, float x, float y, float w, float h, const std::string& text, bool selected, float a,
            float size = 28, bool* hovered = nullptr) {
    ImVec2 p0(x, y), p1(x + w, y + h);
    bool over = false;
    bool tapped = Tap(pg, x, y, w, h, &over);
    if (hovered != nullptr) {
        *hovered = over;
    }
    bool down = over && ImGui::IsMouseDown(0);
    if (selected) {
        float pulse = 0.8f + 0.2f * std::sin(sTime * 3.0f);
        for (int i = 3; i >= 1; i--) {
            pg.dl->AddRect(pg.P(p0.x - i * 3, p0.y - i * 3), pg.P(p1.x + i * 3, p1.y + i * 3),
                           Alpha(Col(200, 110, 255), a * pulse * 0.18f), pg.S(12 + i * 3), 0, pg.S(3));
        }
    }
    ImU32 top = selected || down ? Col(150, 60, 200) : Col(48, 20, 72);
    ImU32 bottom = selected || down ? Col(88, 28, 136) : Col(26, 10, 42);
    QuestUi::Panel(pg, p0, p1, Alpha(top, a * 0.95f), Alpha(bottom, a * 0.95f), 12);
    pg.dl->AddRect(pg.P(p0.x, p0.y), pg.P(p1.x, p1.y), Alpha(selected ? cGold : Col(150, 110, 180), a), pg.S(12), 0,
                   pg.S(selected ? 2.5f : 1.5f));
    TxtCenter(pg, x + w / 2, y + h / 2 - size * 0.62f, size, Alpha(selected ? Col(255, 255, 255) : cText, a), text);
    return tapped;
}

// The row highlight: the glowing plate from the picture, sliding to the selected row.
void Highlight(const Page& pg, float x, float y, float w, float h, float a) {
    float pulse = 0.82f + 0.18f * std::sin(sTime * 3.0f);
    const Tex& tex = Load("highlight.png");
    if (tex.id != nullptr) {
        // highlight.png is the 664 x 86 plate with a 26 px glow around it.
        float sx = w / 664.0f, sy = h / 86.0f;
        pg.dl->AddImage(tex.id, pg.P(x - 26 * sx, y - 26 * sy), pg.P(x + w + 26 * sx, y + h + 26 * sy), ImVec2(0, 0),
                        ImVec2(1, 1), Alpha(IM_COL32_WHITE, a * pulse));
    } else {
        QuestUi::Panel(pg, ImVec2(x, y), ImVec2(x + w, y + h), Alpha(Col(140, 60, 200), a), Alpha(Col(80, 30, 130), a),
                       10);
    }
}

// A button hint like the picture's: the controller's own icon (or a ring with the letter) and a word. The whole
// hint also takes a tap or click. Returns the right edge.
float Hint(const Page& pg, ButtonPrompt prompt, const char* letter, const char* label, float x, float cy, float a,
           Nav nav) {
    float x0 = x;
    const char* tex = ButtonPrompts_Icon(prompt);
    ImU32 col = Alpha(Col(235, 228, 240), a);
    if (tex == nullptr || !QuestUi::GameIcon(pg, tex, x + 18, cy, 38)) {
        pg.dl->AddCircle(pg.P(x + 18, cy), pg.S(17), col, 0, pg.S(2.5f));
        float lw = TxtW(pg, 20, letter);
        QuestUi::Text(pg, x + 18 - lw / 2, cy - 13, 20, col, letter);
    }
    x += 44;
    Txt(pg, x, cy - 17, 26, col, label);
    x += TxtW(pg, 26, label);
    if (Tap(pg, x0 - 8, cy - 28, x - x0 + 16, 56)) {
        Navigate(nav);
    }
    return x;
}

// The hints of a screen, right-aligned at `right`. After a tap, the touch hint instead.
void DrawHints(const Page& pg, float right, float cy, float a, bool select, bool back, bool pages) {
    if (sTouchMode) {
        std::string hint = pages ? "Toque numa opção. Setas mudam a página." : "Toque numa opção para escolher.";
        TxtRight(pg, right, cy - 17, 26, Alpha(Col(235, 228, 240), a * 0.9f), hint);
        return;
    }
    struct H {
        ButtonPrompt prompt;
        const char* letter;
        const char* label;
        Nav nav;
    };
    std::vector<H> hints;
    if (pages) {
        hints.push_back({ BUTTON_PROMPT_L, "L", "", Nav::Prev });
        hints.push_back({ BUTTON_PROMPT_R, "R", "Página", Nav::Next });
    }
    if (select) {
        hints.push_back({ BUTTON_PROMPT_A, "A", "Selecionar", Nav::Accept });
    }
    if (back) {
        hints.push_back({ BUTTON_PROMPT_B, "B", "Voltar", Nav::Back });
    }
    float w = 0;
    for (const H& h : hints) {
        w += 44 + TxtW(pg, 26, h.label) + 30;
    }
    float x = right - w + 30;
    for (const H& h : hints) {
        x = Hint(pg, h.prompt, h.letter, h.label, x, cy, a, h.nav) + (h.label[0] ? 30 : 8);
    }
}

void DrawToast(const Page& pg, float dt, float a) {
    if (sToastTime <= 0) {
        return;
    }
    sToastTime -= dt;
    float t = std::clamp(sToastTime / 0.4f, 0.0f, 1.0f) * a;
    float size = 26, textW = TxtW(pg, size, sToast), maxW = 1500;
    // Long messages (the mods') wrap over a few lines, growing upwards.
    bool wrap = textW + 48 > maxW;
    float w = wrap ? maxW : textW + 48;
    float h = wrap ? QuestUi::WrappedHeight(pg, size, w - 48, sToast.c_str()) + 22 : 52;
    float x = kW / 2 - w / 2, y = 982 - h;
    QuestUi::Panel(pg, ImVec2(x, y), ImVec2(x + w, y + h), Alpha(Col(60, 24, 90, 235), t),
                   Alpha(Col(30, 10, 50, 235), t), 12);
    pg.dl->AddRect(pg.P(x, y), pg.P(x + w, y + h), Alpha(Col(200, 150, 255), t), pg.S(12), 0, pg.S(1.5f));
    Txt(pg, x + 24, y + 10, size, Alpha(Col(255, 255, 255), t), sToast, nullptr, wrap ? w - 48 : 0.0f);
}

// ----- The main screen: the nine items over the painted panel.

// How far row i has come in while the menu is built up after the opening (1 once it is in place).
float RowBuild(int i) {
    return Ease(std::clamp(sBuild * 1.8f - i * 0.08f, 0.0f, 1.0f));
}

void DrawMain(const Page& pg, float dt, float a) {
    Frame& f = Cur();
    for (int i = 1; i < ITEM_COUNT; i++) {
        DrawSeparator(pg, RowY(i) - kRowStep / 2, a * 0.8f * RowBuild(i));
    }
    float target = RowY(f.sel);
    if (sCursorY < 0) {
        sCursorY = target;
    }
    sCursorY += (target - sCursorY) * std::min(1.0f, dt * 16.0f);
    Highlight(pg, kPlateX0, sCursorY - kPlateH / 2, kPlateW, kPlateH, a * RowBuild(f.sel));

    bool hasSave = ContinueSlot(sFileSelect) >= 0;
    bool hasEmpty = EmptySlot(sFileSelect) >= 0;
    bool inputs = !sConfirm.open;
    for (int i = 0; i < ITEM_COUNT; i++) {
        float cy = RowY(i);
        bool selected = f.sel == i;
        bool dim = ((i == ITEM_CONTINUE || i == ITEM_LOAD) && !hasSave) || (i == ITEM_START && !hasEmpty);
        float built = RowBuild(i);
        ImU32 tint = RowTint(selected, a * built * (dim ? 0.45f : 1.0f));
        float shift = (selected ? 6.0f : 0.0f) - (1.0f - built) * 90.0f;
        const char* icon = kMain[i].icon;
        Image(pg, icon, kIconX - kIconSize / 2 + shift, cy, tint, kIconSize / std::max(Size(icon).x, 1.0f));
        Image(pg, kMain[i].label, kLabelX + shift, cy - 2, tint, kLabelScale);
        bool hovered = false;
        bool tapped = inputs && Tap(pg, kPlateX0, cy - kPlateH / 2, kPlateW, kPlateH, &hovered);
        if (inputs && hovered && (PointerMoved() || tapped) && f.sel != i) {
            f.sel = i;
            Audio_PlaySfx(NA_SE_SY_CURSOR);
        }
        if (tapped && !Busy()) {
            ActivateMain(i);
        }
    }
    // Help for the selected item, and the buttons.
    float rest = RowBuild(ITEM_COUNT);
    Txt(pg, 111, 1012, 24, Alpha(cDim, a * rest), kMain[f.sel].help);
    DrawHints(pg, 1860, 1032 - 4, a * rest, true, false, false);
}

// ----- The frame of the other screens: title, the way here, back button, pages, help and hints.

void DrawFrame(const Page& pg, float a, const Frame& f, const std::string& help, bool select = true) {
    // Dim the picture, keep the moon glowing through.
    pg.dl->AddRectFilled(pg.P(0, 0), pg.P(kW, kH), Alpha(Col(8, 3, 16, 150), a));
    ImVec2 p0(kPanelX0, kPanelY0), p1(kPanelX1, kPanelY1);
    float glow = 0.6f + 0.2f * std::sin(sTime * 2.0f);
    for (int i = 4; i >= 1; i--) {
        pg.dl->AddRect(pg.P(p0.x - i * 3, p0.y - i * 3), pg.P(p1.x + i * 3, p1.y + i * 3),
                       Alpha(Col(190, 90, 255), a * glow * 0.12f), pg.S(22 + i * 3), 0, pg.S(3));
    }
    QuestUi::Panel(pg, p0, p1, Alpha(Col(34, 12, 54, 238), a), Alpha(Col(12, 4, 22, 242), a), 20);
    pg.dl->AddRect(pg.P(p0.x, p0.y), pg.P(p1.x, p1.y), Alpha(Col(40, 20, 10, 230), a), pg.S(20), 0, pg.S(5));
    pg.dl->AddRect(pg.P(p0.x, p0.y), pg.P(p1.x, p1.y), Alpha(cGold, a), pg.S(20), 0, pg.S(3));
    pg.dl->AddRect(pg.P(p0.x + 10, p0.y + 10), pg.P(p1.x - 10, p1.y - 10), Alpha(Col(236, 214, 160, 90), a), pg.S(14),
                   0, pg.S(1));
    // Diamond studs at the corners.
    ImVec2 corners[4] = {
        { p0.x + 26, p0.y + 26 }, { p1.x - 26, p0.y + 26 }, { p0.x + 26, p1.y - 26 }, { p1.x - 26, p1.y - 26 }
    };
    for (ImVec2 c : corners) {
        float r = 8.0f;
        pg.dl->AddQuadFilled(pg.P(c.x, c.y - r), pg.P(c.x + r, c.y), pg.P(c.x, c.y + r), pg.P(c.x - r, c.y),
                             Alpha(cGold, a));
    }

    // Title, and the way here.
    ImFont* title = QuestUi::TitleFont();
    TxtCenter(pg, kW / 2, 92, 52, Alpha(cGoldBright, a), ScreenTitle(f), title);
    std::string crumb;
    for (size_t i = 0; i < sStack.size(); i++) {
        crumb += (i ? "  >  " : "") + std::string(ScreenTitle(sStack[i]));
    }
    TxtCenter(pg, kW / 2, 160, 22, Alpha(cDim, a * 0.9f), crumb);
    ImU32 line = Alpha(Col(236, 204, 120, 150), a), clear = Col(236, 204, 120, 0);
    pg.dl->AddRectFilledMultiColor(pg.P(kContentX0, 206), pg.P(kW / 2, 207.5f), clear, line, line, clear);
    pg.dl->AddRectFilledMultiColor(pg.P(kW / 2, 206), pg.P(kContentX1, 207.5f), line, clear, clear, line);

    // Back, always there.
    bool hovered = false;
    if (Tap(pg, 140, 100, 200, 60, &hovered)) {
        Navigate(Nav::Back);
    }
    QuestUi::Panel(pg, ImVec2(140, 100), ImVec2(340, 160), Alpha(hovered ? Col(110, 44, 160) : Col(52, 22, 78), a),
                   Alpha(Col(26, 10, 42), a), 30);
    pg.dl->AddRect(pg.P(140, 100), pg.P(340, 160), Alpha(Col(215, 170, 255), a), pg.S(30), 0, pg.S(1.5f));
    Arrow(pg, 176, 130, -1, Alpha(cText, a));
    Txt(pg, 200, 112, 28, Alpha(cText, a), "Voltar");

    // Pages: number and arrows.
    int pages = PageCount(f);
    if (pages > 1) {
        char text[24];
        snprintf(text, sizeof(text), "%d / %d", f.page + 1, pages);
        float cx = 1660;
        TxtCenter(pg, cx, 112, 28, Alpha(cText, a), text);
        bool hl = false, hr = false;
        if (Tap(pg, cx - 130, 96, 70, 68, &hl)) {
            Navigate(Nav::Prev);
        }
        if (Tap(pg, cx + 60, 96, 70, 68, &hr)) {
            Navigate(Nav::Next);
        }
        Arrow(pg, cx - 90, 130, -1, Alpha(hl ? cGoldBright : cGold, a), 1.3f);
        Arrow(pg, cx + 90, 130, 1, Alpha(hr ? cGoldBright : cGold, a), 1.3f);
    }

    // Help for what is selected, and the buttons.
    pg.dl->AddRectFilledMultiColor(pg.P(kContentX0, 912), pg.P(kW / 2, 913.5f), clear, line, line, clear);
    pg.dl->AddRectFilledMultiColor(pg.P(kW / 2, 912), pg.P(kContentX1, 913.5f), line, clear, clear, line);
    Txt(pg, kContentX0 + 10, 934, 24, Alpha(cDim, a), help, nullptr, 900);
    DrawHints(pg, kContentX1, 958, a, select, true, pages > 1);
}

// Tabs across the top of a screen with pages (Settings, Controls).
void DrawTabs(const Page& pg, float a, Frame& f, const std::vector<std::string>& names) {
    float size = 26, gap = 14, total = 0;
    for (const std::string& n : names) {
        total += TxtW(pg, size, n) + 44 + gap;
    }
    total -= gap;
    float x = kW / 2 - total / 2, y = 222;
    for (int i = 0; i < (int)names.size(); i++) {
        float w = TxtW(pg, size, names[i]) + 44;
        bool sel = f.page == i;
        bool hovered = false;
        if (Tap(pg, x, y, w, 50, &hovered) && !sel) {
            f.page = i;
            f.sel = 0;
            Audio_PlaySfx(NA_SE_SY_WIN_SCROLL_LEFT);
        }
        QuestUi::Panel(pg, ImVec2(x, y), ImVec2(x + w, y + 50),
                       Alpha(sel ? Col(120, 52, 170) : (hovered ? Col(70, 30, 104) : Col(40, 16, 62)), a),
                       Alpha(sel ? Col(70, 24, 110) : Col(22, 8, 36), a), 25);
        pg.dl->AddRect(pg.P(x, y), pg.P(x + w, y + 50), Alpha(sel ? cGold : Col(120, 90, 150), a), pg.S(25), 0,
                       pg.S(sel ? 2.0f : 1.0f));
        Txt(pg, x + 22, y + 9, size, Alpha(sel ? cGoldBright : cDim, a), names[i]);
        x += w + gap;
    }
}

// A list of settings: name on the left, value on the right with arrows. Returns the help of the selected one.
std::string DrawSettingRows(const Page& pg, float a, Frame& f, const std::vector<Setting>& rows, float y0, float rowH) {
    if (rows.empty()) {
        return "";
    }
    const int count = (int)rows.size();
    f.sel = std::clamp(f.sel, 0, count - 1);
    float x0 = kContentX0 + 40, x1 = kContentX1 - 40;
    // More rows than fit (the Mods tab): the list scrolls with the selection, the arrows or the wheel.
    const int visible = std::max(1, (int)((kContentY1 - 12 - y0) / rowH));
    const int maxScroll = std::max(0, count - visible);
    if (maxScroll > 0) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (sPointerInputs && wheel != 0.0f) {
            f.scroll = std::clamp(f.scroll + (wheel > 0 ? -1 : 1), 0, maxScroll);
            f.sel = std::clamp(f.sel, f.scroll, f.scroll + visible - 1);
        }
    }
    if (f.sel < f.scroll) {
        f.scroll = f.sel;
    } else if (f.sel >= f.scroll + visible) {
        f.scroll = f.sel - visible + 1;
    }
    f.scroll = std::clamp(f.scroll, 0, maxScroll);
    if (maxScroll > 0) {
        float cx = kW / 2, top = y0 - 14, bottom = y0 + visible * rowH + 14;
        bool hu = false, hd = false;
        bool canUp = f.scroll > 0, canDown = f.scroll < maxScroll;
        if (canUp && Tap(pg, cx - 80, top - 16, 160, 32, &hu)) {
            f.scroll = std::max(0, f.scroll - (visible - 1));
            f.sel = std::clamp(f.sel, f.scroll, f.scroll + visible - 1);
            Audio_PlaySfx(NA_SE_SY_CURSOR);
        }
        if (canDown && Tap(pg, cx - 80, bottom - 16, 160, 32, &hd)) {
            f.scroll = std::min(maxScroll, f.scroll + (visible - 1));
            f.sel = std::clamp(f.sel, f.scroll, f.scroll + visible - 1);
            Audio_PlaySfx(NA_SE_SY_CURSOR);
        }
        ImU32 up = Alpha(hu ? cGoldBright : cGold, a * (canUp ? 1.0f : 0.25f));
        ImU32 down = Alpha(hd ? cGoldBright : cGold, a * (canDown ? 1.0f : 0.25f));
        pg.dl->AddTriangleFilled(pg.P(cx, top - 9), pg.P(cx + 14, top + 7), pg.P(cx - 14, top + 7), up);
        pg.dl->AddTriangleFilled(pg.P(cx - 14, bottom - 7), pg.P(cx + 14, bottom - 7), pg.P(cx, bottom + 9), down);
    }
    Highlight(pg, x0, y0 + (f.sel - f.scroll) * rowH + 4, x1 - x0, rowH - 8, a);
    for (int i = f.scroll; i < std::min(count, f.scroll + visible); i++) {
        const Setting& s = rows[i];
        float y = y0 + (i - f.scroll) * rowH, cy = y + rowH / 2;
        bool selected = f.sel == i;
        ImU32 col = Alpha(selected ? Col(255, 255, 255) : cText, a * (s.disabled && s.disabled() ? 0.45f : 1.0f));
        Txt(pg, x0 + 30, cy - 17, 28, col, s.label);
        std::string value = SettingValue(s);
        float vRight = x1 - 30;
        bool arrows = s.kind == Kind::Choice || s.kind == Kind::Percent || s.kind == Kind::Number;
        if (s.kind == Kind::Toggle || s.kind == Kind::Switch) {
            bool on = SettingOn(s);
            float pw = 150, px = vRight - pw;
            QuestUi::Panel(pg, ImVec2(px, cy - 20), ImVec2(px + pw, cy + 20),
                           Alpha(on ? Col(60, 120, 60) : Col(60, 30, 70), a),
                           Alpha(on ? Col(30, 70, 30) : Col(34, 14, 44), a), 20);
            pg.dl->AddRect(pg.P(px, cy - 20), pg.P(px + pw, cy + 20), Alpha(on ? cOn : Col(150, 110, 180), a), pg.S(20),
                           0, pg.S(1.5f));
            TxtCenter(pg, px + pw / 2, cy - 15, 24, Alpha(on ? Col(230, 255, 225) : cDim, a), value);
        } else if (arrows) {
            float vw = 300;
            Arrow(pg, vRight - vw, cy, -1, Alpha(selected ? cGoldBright : cGold, a));
            Arrow(pg, vRight, cy, 1, Alpha(selected ? cGoldBright : cGold, a));
            float size = TxtW(pg, 26, value) > vw - 50 ? 22.0f : 26.0f;
            TxtCenter(pg, vRight - vw / 2, cy - size * 0.66f, size, Alpha(cGoldBright, a), value);
        } else if (!value.empty()) {
            TxtRight(pg, vRight - 30, cy - 15, 24, Alpha(cGold, a), value);
            Arrow(pg, vRight, cy, 1, Alpha(cGold, a), 0.8f);
        } else {
            Arrow(pg, vRight, cy, 1, Alpha(cGold, a), 0.8f);
        }
        // Taps: the arrows change the value, the rest of the row selects it and toggles or runs it.
        if (Busy() || sConfirm.open) {
            continue;
        }
        bool hovered = false;
        if (arrows) {
            if (Tap(pg, vRight - 340, y, 170, rowH)) {
                f.sel = i;
                ChangeSetting(s, -1);
                continue;
            }
            if (Tap(pg, vRight - 170, y, 200, rowH)) {
                f.sel = i;
                ChangeSetting(s, 1);
                continue;
            }
        }
        bool tapped = Tap(pg, x0, y, x1 - x0, rowH, &hovered);
        if (hovered && (PointerMoved() || tapped) && f.sel != i) {
            f.sel = i;
            Audio_PlaySfx(NA_SE_SY_CURSOR);
        }
        if (tapped) {
            ChangeSetting(s, arrows ? 1 : 0);
        }
    }
    return rows[f.sel].help;
}

// ----- Carregar Jogo and Arquivos de Jogo.

void DrawFiles(const Page& pg, float a, Frame& f) {
    FileSelectState* fs = sFileSelect;
    int count = SlotCount();
    f.sel = std::clamp(f.sel, 0, count - 1);
    float cardX = kContentX0 + 20, cardW = 640, cardH = 196, gap = 20, y0 = 236;

    for (int i = 0; i < count; i++) {
        FileInfo info = ReadFile(fs, i);
        float y = y0 + i * (cardH + gap);
        bool selected = f.sel == i;
        bool hovered = false;
        bool tapped = !sConfirm.open && !Busy() && Tap(pg, cardX, y, cardW, cardH, &hovered);
        if (hovered && (PointerMoved() || tapped) && f.sel != i) {
            f.sel = i;
            f.focus = 0;
            Audio_PlaySfx(NA_SE_SY_CURSOR);
        }
        if (selected && f.focus == 0) {
            Highlight(pg, cardX, y, cardW, cardH, a);
        }
        QuestUi::Panel(pg, ImVec2(cardX + 8, y + 8), ImVec2(cardX + cardW - 8, y + cardH - 8),
                       Alpha(Col(44, 18, 66, 220), a), Alpha(Col(20, 8, 32, 220), a), 14);
        pg.dl->AddRect(pg.P(cardX + 8, y + 8), pg.P(cardX + cardW - 8, y + cardH - 8),
                       Alpha(selected ? cGold : Col(130, 96, 160), a), pg.S(14), 0, pg.S(selected ? 2.0f : 1.0f));
        char head[32];
        snprintf(head, sizeof(head), "Arquivo %d", i + 1);
        Txt(pg, cardX + 34, y + 24, 22, Alpha(cGold, a), head);
        if (!info.occupied) {
            Txt(pg, cardX + 34, y + 62, 36, Alpha(cDim, a), "Vazio", QuestUi::TitleFont());
            Txt(pg, cardX + 34, y + 128, 22, Alpha(cDim, a * 0.8f), "Um jogo novo pode começar aqui.");
        } else {
            Txt(pg, cardX + 34, y + 56, 40, Alpha(Col(255, 255, 255), a), info.name.empty() ? "?" : info.name,
                QuestUi::TitleFont());
            // Hearts, as many as the file has (up to 20), in two rows of ten.
            for (int h = 0; h < std::min(info.hearts, 20); h++) {
                QuestUi::IconHeart(pg, cardX + 46 + (h % 10) * 30, y + 130 + (h / 10) * 28, 26,
                                   Alpha(info.doubleDefense ? Col(250, 250, 250) : Col(220, 40, 70), a));
            }
            std::string day = DayText(info.day);
            char when[48];
            snprintf(when, sizeof(when), "%02d:%02d", info.hour, info.minute);
            TxtRight(pg, cardX + cardW - 34, y + 26, 24, Alpha(cText, a), day);
            TxtRight(pg, cardX + cardW - 34, y + 58, 24, Alpha(cDim, a), when);
            if (info.owl) {
                TxtRight(pg, cardX + cardW - 34, y + 90, 20, Alpha(cGold, a), "Save da coruja");
            }
        }
        if (tapped) {
            f.sel = i;
            if (f.mode == 0) {
                AcceptFile(f);
            } else {
                f.focus = 1;
                f.action = 0;
                Audio_PlaySfx(NA_SE_SY_DECIDE);
            }
        }
    }

    // The selected file, in detail.
    float dx0 = kContentX0 + 700, dx1 = kContentX1 - 20, dy0 = y0, dy1 = 896;
    QuestUi::Panel(pg, ImVec2(dx0, dy0), ImVec2(dx1, dy1), Alpha(Col(26, 10, 40, 220), a),
                   Alpha(Col(14, 6, 24, 220), a), 16);
    pg.dl->AddRect(pg.P(dx0, dy0), pg.P(dx1, dy1), Alpha(Col(150, 110, 180), a), pg.S(16), 0, pg.S(1.5f));
    FileInfo info = ReadFile(fs, f.sel);
    float x = dx0 + 40, y = dy0 + 30;
    char line[96];
    snprintf(line, sizeof(line), "Arquivo %d", f.sel + 1);
    Txt(pg, x, y, 24, Alpha(cGold, a), line);
    y += 38;
    if (!info.occupied) {
        Txt(pg, x, y, 44, Alpha(cText, a), "Arquivo vazio", QuestUi::TitleFont());
        Txt(pg, x, y + 70, 26, Alpha(cDim, a), "Nenhum jogo salvo aqui. Comece um jogo novo neste arquivo.", nullptr,
            dx1 - x - 40);
    } else {
        Txt(pg, x, y, 48, Alpha(Col(255, 255, 255), a), info.name.empty() ? "?" : info.name, QuestUi::TitleFont());
        y += 76;
        // Each fact with the game's own icon beside its value (the heart piece, the Deku mask, the ocarina, the
        // rupee), or one drawn here (the sun for the day, the hourglass for the cycles).
        enum class Icon { Day, Heart, Mask, Song, Rupee, Cycle };
        struct Fact {
            std::string name, value;
            Icon icon;
        };
        snprintf(line, sizeof(line), "%s, %02d:%02d%s", DayText(info.day).c_str(), info.hour, info.minute,
                 info.owl ? " (save da coruja)" : "");
        std::vector<Fact> facts;
        facts.push_back({ "Dia atual", line, Icon::Day });
        snprintf(line, sizeof(line), "%d%s", info.hearts,
                 info.heartPieces ? (" e " + std::to_string(info.heartPieces) + "/4 de peça").c_str() : "");
        facts.push_back({ "Corações", line, Icon::Heart });
        snprintf(line, sizeof(line), "%d de 24", info.masks);
        facts.push_back({ "Máscaras", line, Icon::Mask });
        snprintf(line, sizeof(line), "%d de 10", info.songs);
        facts.push_back({ "Canções", line, Icon::Song });
        facts.push_back({ "Rupias", std::to_string(info.rupees), Icon::Rupee });
        facts.push_back({ "Ciclos de três dias", std::to_string(info.cycles), Icon::Cycle });
        auto tinted = [&](const char* path, float cx, float cy, float w, float h, ImU32 tint) {
            ImTextureID id = QuestUi::Texture(path);
            if (id != nullptr) {
                pg.dl->AddImage(id, pg.P(cx - w / 2, cy - h / 2), pg.P(cx + w / 2, cy + h / 2), ImVec2(0, 0),
                                ImVec2(1, 1), tint);
            }
        };
        for (const Fact& fact : facts) {
            Txt(pg, x, y, 26, Alpha(cDim, a), fact.name);
            float ix = x + 318, iy = y + 15;
            switch (fact.icon) {
                case Icon::Day:
                    QuestUi::IconSun(pg, ix, iy, 30);
                    break;
                case Icon::Heart:
                    tinted((const char*)gItemIcons[ITEM_HEART_PIECE], ix, iy, 36, 36, Alpha(IM_COL32_WHITE, a));
                    break;
                case Icon::Mask:
                    tinted((const char*)gItemIcons[ITEM_MASK_DEKU], ix, iy, 36, 36, Alpha(IM_COL32_WHITE, a));
                    break;
                case Icon::Song:
                    tinted((const char*)gItemIcons[ITEM_OCARINA_OF_TIME], ix, iy, 36, 36, Alpha(IM_COL32_WHITE, a));
                    break;
                case Icon::Rupee:
                    tinted("__OTR__parameter_static/gRupeeCounterIconTex", ix, iy, 28, 28,
                           Alpha(Col(120, 255, 110), a));
                    break;
                case Icon::Cycle:
                    Image(pg, "icon_hourglass.png", ix - 16, iy, Alpha(IM_COL32_WHITE, a),
                          32.0f / std::max(Size("icon_hourglass.png").y, 1.0f));
                    break;
            }
            Txt(pg, x + 350, y, 26, Alpha(cText, a), fact.value);
            y += 42;
        }
        // The bosses' remains, lit when the file has them.
        Txt(pg, x, y + 10, 26, Alpha(cDim, a), "Restos dos chefes");
        for (int r = 0; r < 4; r++) {
            float cx = x + 330 + r * 74, cy = y + 26;
            const char* tex = (const char*)gItemIcons[ITEM_REMAINS_ODOLWA + r];
            if (info.remains[r]) {
                QuestUi::GameIcon(pg, tex, cx, cy, 58);
            } else {
                pg.dl->AddCircle(pg.P(cx, cy), pg.S(24), Alpha(Col(120, 90, 150), a), 0, pg.S(1.5f));
            }
        }
        snprintf(line, sizeof(line), "%d de 4", info.remainsCount);
        Txt(pg, x + 330 + 4 * 74, y + 10, 26, Alpha(cText, a), line);
    }

    // The file's buttons.
    std::vector<const char*> actions;
    if (f.mode == 0) {
        actions.push_back(info.occupied ? "Jogar" : "Criar jogo");
    } else {
        actions = FileActions(f.sel);
    }
    float bw = 240, bh = 64, bx = dx1 - 40 - actions.size() * (bw + 20) + 20, by = dy1 - 40 - bh;
    for (int i = 0; i < (int)actions.size(); i++) {
        bool sel = f.mode == 0 ? f.focus == 0 : (f.focus == 1 && f.action == i);
        bool hovered = false;
        if (Button(pg, bx + i * (bw + 20), by, bw, bh, actions[i], sel, a, 28, &hovered) && !Busy() && !sConfirm.open) {
            f.focus = 1;
            f.action = i;
            if (f.mode == 0) {
                AcceptFile(f);
            } else {
                RunFileAction(f.sel, i);
            }
        }
    }
}

std::string FilesHelp(const Frame& f) {
    if (sEraseSlot >= 0) {
        return "Excluindo o arquivo...";
    }
    if (f.mode == 0) {
        return "Escolha o arquivo e aperte A para jogar. Num arquivo vazio, começa um jogo novo.";
    }
    if (f.focus == 0) {
        return "Escolha um arquivo e aperte A para ver o que fazer com ele.";
    }
    return SlotOccupied(sFileSelect, f.sel) ? "Jogar continua o arquivo. Excluir apaga o arquivo, com confirmação."
                                            : "Começa um jogo novo neste arquivo. Você escolhe o nome do Link.";
}

// ----- Configurações.

std::string DrawSettings(const Page& pg, float a, Frame& f) {
    std::vector<std::string> names;
    for (const Tab& t : SettingsTabs()) {
        names.push_back(t.name);
    }
    f.page = std::clamp(f.page, 0, (int)names.size() - 1);
    DrawTabs(pg, a, f, names);
    if (IsModsTab(f)) {
        RefreshModsTab(true);
    }
    if (IsShipTab(f)) {
        RefreshShipTab(f);
    } else {
        sShipSection = -1;
        sShipPending = -2;
    }
    return DrawSettingRows(pg, a, f, SettingsTabs()[f.page].rows, 300, 64);
}

// ----- Controles.

std::string DrawControls(const Page& pg, float a, Frame& f) {
    DrawTabs(pg, a, f, { ControlsPageName(0), ControlsPageName(1), ControlsPageName(2) });
    if (f.page == 2) {
        return DrawSettingRows(pg, a, f, SettingsTabs()[4].rows, 300, 64);
    }
    float x0 = kContentX0 + 40, y = 296;
    if (f.page == 0) {
        for (const ButtonRow& row : ButtonRows()) {
            float cy = y + 28;
            float ix = x0 + 30;
            bool drawn = false;
            for (ButtonPrompt prompt : row.prompts) {
                const char* tex = ButtonPrompts_Icon(prompt);
                if (tex != nullptr && QuestUi::GameIcon(pg, tex, ix, cy, 46)) {
                    ix += 50;
                    drawn = true;
                }
            }
            if (!drawn) {
                pg.dl->AddCircle(pg.P(ix, cy), pg.S(21), Alpha(cGold, a), 0, pg.S(2.5f));
                float lw = TxtW(pg, 24, row.letter);
                QuestUi::Text(pg, ix - lw / 2, cy - 15, 24, Alpha(cGold, a), row.letter);
            }
            Txt(pg, x0 + 190, cy - 16, 26, Alpha(cGoldBright, a), row.name);
            Txt(pg, x0 + 560, cy - 16, 26, Alpha(cText, a), row.does, nullptr, kContentX1 - x0 - 600);
            y += 66;
        }
        return "Os ícones seguem o controle em uso. Troque os botões em Configurar.";
    }
    for (const Combo& c : Combos()) {
        Txt(pg, x0 + 30, y + 10, 26, Alpha(cGoldBright, a), c.keys);
        Txt(pg, x0 + 560, y + 10, 26, Alpha(cText, a), c.does, nullptr, kContentX1 - x0 - 600);
        y += 60;
    }
    return "Combinações do jogo original e da Definitive Edition.";
}

// ----- Extras.

std::string DrawExtras(const Page& pg, float a, Frame& f) {
    f.sel = std::clamp(f.sel, 0, EXTRA_COUNT - 1);
    float x0 = kContentX0 + 40, x1 = kContentX1 - 40, y0 = 250, rowH = 104;
    Highlight(pg, x0, y0 + f.sel * rowH + 6, x1 - x0, rowH - 12, a);
    for (int i = 0; i < EXTRA_COUNT; i++) {
        float y = y0 + i * rowH;
        bool selected = f.sel == i;
        Txt(pg, x0 + 34, y + 18, 32, Alpha(selected ? Col(255, 255, 255) : cText, a), kExtras[i].name,
            QuestUi::TitleFont());
        Txt(pg, x0 + 34, y + 62, 22, Alpha(cDim, a), kExtras[i].help);
        float cy = y + rowH / 2;
        if (i == EXTRA_MUSIC) {
            std::string value = SettingValue(MusicSetting());
            Arrow(pg, x1 - 380, cy, -1, Alpha(selected ? cGoldBright : cGold, a));
            Arrow(pg, x1 - 30, cy, 1, Alpha(selected ? cGoldBright : cGold, a));
            TxtCenter(pg, x1 - 205, cy - 17, 26, Alpha(cGoldBright, a), value);
        } else {
            Arrow(pg, x1 - 30, cy, 1, Alpha(cGold, a), 0.8f);
        }
        if (Busy() || sConfirm.open) {
            continue;
        }
        if (i == EXTRA_MUSIC) {
            if (Tap(pg, x1 - 420, y, 175, rowH)) {
                f.sel = i;
                ActivateExtra(f, i, -1);
                continue;
            }
            if (Tap(pg, x1 - 245, y, 245, rowH)) {
                f.sel = i;
                ActivateExtra(f, i, 1);
                continue;
            }
        }
        bool hovered = false;
        bool tapped = Tap(pg, x0, y, x1 - x0, rowH, &hovered);
        if (hovered && (PointerMoved() || tapped) && f.sel != i) {
            f.sel = i;
            Audio_PlaySfx(NA_SE_SY_CURSOR);
        }
        if (tapped) {
            ActivateExtra(f, i, 1);
        }
    }
    return kExtras[f.sel].help;
}

// Fits a picture in a box, keeping its shape.
void FitImage(const Page& pg, const Tex& tex, float x0, float y0, float x1, float y1, float a) {
    float k = std::min((x1 - x0) / tex.size.x, (y1 - y0) / tex.size.y);
    float w = tex.size.x * k, h = tex.size.y * k;
    float x = (x0 + x1) / 2 - w / 2, y = (y0 + y1) / 2 - h / 2;
    pg.dl->AddRectFilled(pg.P(x - 6, y - 6), pg.P(x + w + 6, y + h + 6), Alpha(Col(10, 4, 18), a), pg.S(8));
    pg.dl->AddImage(tex.id, pg.P(x, y), pg.P(x + w, y + h), ImVec2(0, 0), ImVec2(1, 1), Alpha(IM_COL32_WHITE, a));
    pg.dl->AddRect(pg.P(x - 6, y - 6), pg.P(x + w + 6, y + h + 6), Alpha(cGold, a), pg.S(8), 0, pg.S(2));
}

std::string DrawGallery(const Page& pg, float a, Frame& f) {
    const std::vector<Picture>& pics = GalleryPictures(f.mode);
    f.page = std::clamp(f.page, 0, (int)pics.size() - 1);
    const Picture& pic = pics[f.page];
    float x0 = kContentX0 + 110, x1 = kContentX1 - 110, y0 = 236, y1 = 800;
    if (pic.file != nullptr) {
        const Tex& tex = Load(pic.file);
        if (tex.id != nullptr) {
            FitImage(pg, tex, x0, y0, x1, y1, a);
            if (!Busy() && !sConfirm.open && Tap(pg, x0, y0, x1 - x0, y1 - y0)) {
                f.zoom = true;
                Audio_PlaySfx(NA_SE_SY_DECIDE);
            }
        }
    } else if (f.page == 1) {
        // The masks, six by four.
        for (int i = 0; i < 24; i++) {
            float cx = x0 + 100 + (i % 6) * ((x1 - x0 - 200) / 5), cy = y0 + 60 + (i / 6) * 140;
            QuestUi::GameIcon(pg, (const char*)gItemIcons[kMasks[i].item], cx, cy, 76);
            float size = TxtW(pg, 20, kMasks[i].name) > 220 ? 17.0f : 20.0f;
            TxtCenter(pg, cx, cy + 44, size, Alpha(cText, a), kMasks[i].name);
        }
    } else {
        const char* names[4] = { "Restos de Odolwa", "Restos de Goht", "Restos de Gyorg", "Restos de Twinmold" };
        const char* where[4] = { "Templo de Woodfall", "Templo de Snowhead", "Templo de Great Bay",
                                 "Templo da Stone Tower" };
        for (int i = 0; i < 4; i++) {
            float cx = x0 + 120 + i * ((x1 - x0 - 240) / 3), cy = y0 + 220;
            Glow(pg, ImVec2(cx, cy), 90, Col(200, 150, 255), a * 0.6f);
            QuestUi::GameIcon(pg, (const char*)gItemIcons[ITEM_REMAINS_ODOLWA + i], cx, cy, 150);
            TxtCenter(pg, cx, cy + 110, 28, Alpha(cGoldBright, a), names[i]);
            TxtCenter(pg, cx, cy + 150, 22, Alpha(cDim, a), where[i]);
        }
    }
    // Arrows at the sides.
    bool hl = false, hr = false;
    if (Tap(pg, kContentX0, y0, 100, y1 - y0, &hl)) {
        Navigate(Nav::Prev);
    }
    if (Tap(pg, kContentX1 - 100, y0, 100, y1 - y0, &hr)) {
        Navigate(Nav::Next);
    }
    Arrow(pg, kContentX0 + 50, (y0 + y1) / 2, -1, Alpha(hl ? cGoldBright : cGold, a), 2.0f);
    Arrow(pg, kContentX1 - 50, (y0 + y1) / 2, 1, Alpha(hr ? cGoldBright : cGold, a), 2.0f);
    TxtCenter(pg, kW / 2, 818, 34, Alpha(cGoldBright, a), pic.title, QuestUi::TitleFont());
    TxtCenter(pg, kW / 2, 864, 24, Alpha(cDim, a), pic.caption);
    return pic.file != nullptr ? "A amplia a imagem. Esquerda e direita trocam de imagem."
                               : "Esquerda e direita trocam de página.";
}

// The picture over the whole screen; any tap or B closes it.
void DrawZoom(const Page& pg, float a, Frame& f) {
    const Picture& pic = GalleryPictures(f.mode)[f.page];
    const Tex& tex = Load(pic.file);
    pg.dl->AddRectFilled(pg.P(-2000, -2000), pg.P(kW + 2000, kH + 2000), Alpha(Col(0, 0, 0, 235), a));
    if (tex.id != nullptr) {
        FitImage(pg, tex, 20, 20, kW - 20, kH - 70, a);
    }
    TxtCenter(pg, kW / 2, kH - 58, 26, Alpha(cText, a), std::string(pic.title) + "  (toque ou B para voltar)");
    if (Tap(pg, -2000, -2000, kW + 4000, kH + 4000)) {
        f.zoom = false;
        Audio_PlaySfx(NA_SE_SY_CANCEL);
    }
}

// ----- Informações da edição.

// The edition's version, as in Android/app/build.gradle (versionName) and the CHANGELOG.
#define EDITION_VERSION "4.0.2-mm3d.13"

std::string DrawEdition(const Page& pg, float a) {
    float x = kContentX0 + 80, y = 260;
    Txt(pg, x, y, 44, Alpha(cGoldBright, a), "Majora's Mask Android Definitive Edition", QuestUi::TitleFont());
    y += 80;
    std::string hash(gGitCommitHash);
    char commit[160];
    snprintf(commit, sizeof(commit), "%s (branch %s)", hash.substr(0, 9).c_str(), gGitBranch);
    const char* platform =
#ifdef __ANDROID__
        "Android";
#elif defined(_WIN32)
        "Windows";
#elif defined(__APPLE__)
        "macOS";
#else
        "Linux";
#endif
    std::vector<std::pair<std::string, std::string>> facts = {
        { "Versão da edição", EDITION_VERSION },
        { "Base", std::string("2 Ship 2 Harkinian ") + gBuildVersion },
        { "Commit", commit },
        { "Compilado em", gBuildDate },
        { "Plataforma", platform },
    };
    for (const auto& fact : facts) {
        Txt(pg, x, y, 28, Alpha(cDim, a), fact.first);
        Txt(pg, x + 380, y, 28, Alpha(cText, a), fact.second);
        y += 52;
    }
    y += 20;
    Txt(pg, x, y, 24, Alpha(cDim, a),
        "The Legend of Zelda: Majora's Mask é da Nintendo. Este projeto não traz a ROM nem arquivos do jogo; eles "
        "são gerados a partir da sua própria cópia.",
        nullptr, kContentX1 - x - 80);
    return "Informações desta compilação.";
}

// ----- Conteúdos adicionais.

std::string DrawContent(const Page& pg, float a, Frame& f) {
    const std::vector<ContentEntry>& entries = ContentEntries();
    f.sel = std::clamp(f.sel, 0, (int)entries.size() - 1);
    float x0 = kContentX0 + 30, x1 = kContentX0 + 760, y0 = 236, rowH = 72;
    Highlight(pg, x0, y0 + f.sel * rowH + 4, x1 - x0, rowH - 8, a);
    for (int i = 0; i < (int)entries.size(); i++) {
        float y = y0 + i * rowH;
        bool selected = f.sel == i;
        float size = TxtW(pg, 26, entries[i].name) > x1 - x0 - 60 ? 22.0f : 26.0f;
        Txt(pg, x0 + 30, y + rowH / 2 - size * 0.66f, size, Alpha(selected ? Col(255, 255, 255) : cText, a),
            entries[i].name);
        bool hovered = false;
        bool tapped = !sConfirm.open && Tap(pg, x0, y, x1 - x0, rowH, &hovered);
        if (hovered && (PointerMoved() || tapped) && f.sel != i) {
            f.sel = i;
            Audio_PlaySfx(NA_SE_SY_CURSOR);
        }
    }
    const ContentEntry& e = entries[f.sel];
    float dx = x1 + 50, dy = y0 + 20, dw = kContentX1 - dx - 30;
    Txt(pg, dx, dy, 36, Alpha(cGoldBright, a), e.name, QuestUi::TitleFont(), dw);
    dy += QuestUi::WrappedHeight(pg, 36, dw, e.name, QuestUi::TitleFont()) + 24;
    Txt(pg, dx, dy, 28, Alpha(cText, a), e.text, nullptr, dw);
    dy += QuestUi::WrappedHeight(pg, 28, dw, e.text) + 30;
    Txt(pg, dx, dy, 24, Alpha(cGold, a), "Onde encontrar");
    Txt(pg, dx, dy + 36, 26, Alpha(cDim, a), e.where, nullptr, dw);
    return "O que a Definitive Edition traz além do jogo original.";
}

// ----- Créditos.

std::string DrawCredits(const Page& pg, float a, Frame& f) {
    std::vector<CreditPage>& pages = CreditPages();
    f.page = std::clamp(f.page, 0, (int)pages.size() - 1);
    const CreditPage& page = pages[f.page];
    TxtCenter(pg, kW / 2, 236, 38, Alpha(cGoldBright, a), page.title, QuestUi::TitleFont());
    bool names = !page.lines.empty() && page.lines[0].second.empty();
    if (names) {
        // Contributors: two columns.
        int half = ((int)page.lines.size() + 1) / 2;
        for (int i = 0; i < (int)page.lines.size(); i++) {
            float cx = i < half ? kW / 2 - 330 : kW / 2 + 330;
            TxtCenter(pg, cx, 320 + (i % half) * 76, 30, Alpha(cText, a), page.lines[i].first);
        }
    } else {
        float y = 330;
        for (const auto& line : page.lines) {
            TxtCenter(pg, kW / 2, y, 32, Alpha(Col(255, 255, 255), a), line.first, QuestUi::TitleFont());
            TxtCenter(pg, kW / 2, y + 46, 24, Alpha(cDim, a), line.second);
            y += 108;
        }
    }
    bool hl = false, hr = false;
    if (Tap(pg, kContentX0, 236, 120, 660, &hl)) {
        Navigate(Nav::Prev);
    }
    if (Tap(pg, kContentX1 - 120, 236, 120, 660, &hr)) {
        Navigate(Nav::Next);
    }
    Arrow(pg, kContentX0 + 50, 566, -1, Alpha(hl ? cGoldBright : cGold, a), 2.0f);
    Arrow(pg, kContentX1 - 50, 566, 1, Alpha(hr ? cGoldBright : cGold, a), 2.0f);
    return "Esquerda e direita (ou L e R) passam as páginas.";
}

// ----- The yes / no question.

void DrawConfirm(const Page& pg, float dt, float a) {
    sModalT = std::min(1.0f, sModalT + dt * 6.0f);
    float t = Ease(sModalT) * a;
    pg.dl->AddRectFilled(pg.P(-2000, -2000), pg.P(kW + 2000, kH + 2000), Alpha(Col(6, 2, 12, 160), t));
    float w = 860, h = 400;
    ImVec2 at(kW / 2 - w / 2, kH / 2 - h / 2 + (1.0f - t) * 30.0f), b(at.x + w, at.y + h);
    float glow = 0.6f + 0.2f * std::sin(sTime * 2.0f);
    for (int i = 4; i >= 1; i--) {
        pg.dl->AddRect(pg.P(at.x - i * 3, at.y - i * 3), pg.P(b.x + i * 3, b.y + i * 3),
                       Alpha(Col(190, 90, 255), t * glow * 0.14f), pg.S(18 + i * 3), 0, pg.S(3));
    }
    QuestUi::Panel(pg, at, b, Alpha(Col(46, 18, 70, 250), t), Alpha(Col(20, 8, 34, 250), t), 16);
    pg.dl->AddRect(pg.P(at.x, at.y), pg.P(b.x, b.y), Alpha(cGold, t), pg.S(16), 0, pg.S(2.5f));
    TxtCenter(pg, kW / 2, at.y + 34, 42, Alpha(cGoldBright, t), sConfirm.title, QuestUi::TitleFont());
    Txt(pg, at.x + 56, at.y + 112, 30, Alpha(cText, t), sConfirm.text, nullptr, w - 112);
    float ny = at.y + 112 + QuestUi::WrappedHeight(pg, 30, w - 112, sConfirm.text.c_str()) + 18;
    Txt(pg, at.x + 56, ny, 24, Alpha(cDim, t), sConfirm.note, nullptr, w - 112);
    float bw = 240, bh = 66, by = b.y - 40 - bh;
    bool hy = false, hn = false;
    bool yes = Button(pg, kW / 2 - 20 - bw, by, bw, bh, sConfirm.yes, sConfirm.yesSelected, t, 30, &hy);
    bool no = Button(pg, kW / 2 + 20, by, bw, bh, sConfirm.no, !sConfirm.yesSelected, t, 30, &hn);
    if (yes) {
        CloseConfirm(true);
    } else if (no) {
        CloseConfirm(false);
    } else if (PointerMoved() && (hy || hn) && sConfirm.yesSelected != hy) {
        sConfirm.yesSelected = hy;
        Audio_PlaySfx(NA_SE_SY_CURSOR);
    }
}

// ----- Android: the on-screen controls stay up, but the camera's look-around area ignores touches over the menu.

void SyncTouchControls(bool covering) {
#ifdef __ANDROID__
    static bool sShown = false;
    if (covering == sShown) {
        return;
    }
    sShown = covering;
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (env == nullptr || activity == nullptr) {
        return;
    }
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(activityClass, "SetTitleMenuShown", "(Z)V");
    if (method != nullptr) {
        env->CallVoidMethod(activity, method, (jboolean)covering);
    } else {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
#else
    (void)covering;
#endif
}

// ----- One GUI frame of the menu.

std::string sHelp; // the help line of the screen shown, kept from its last frame for the frame around it

void DrawScreen(const Page& pg, float dt, float a) {
    Frame& f = Cur();
    Screen screen = f.screen;
    size_t depth = sStack.size();
    std::string help = f.screen == Screen::Files ? FilesHelp(f) : sHelp;
    DrawFrame(pg, a, f, help, f.screen != Screen::Edition);
    if (sStack.size() != depth || Cur().screen != screen) {
        return; // Voltar was tapped: the screen under is drawn from the next frame
    }
    // Each screen slides in from the side it was opened from.
    float t = Ease(sScreenT);
    Page in{ pg.dl, ImVec2(pg.origin.x + (1.0f - t) * sScreenDir * 60.0f * pg.k, pg.origin.y), pg.k };
    float ca = a * t;
    switch (screen) {
        case Screen::Files:
            DrawFiles(in, ca, f);
            break;
        case Screen::Settings:
            sHelp = DrawSettings(in, ca, f);
            break;
        case Screen::Controls:
            sHelp = DrawControls(in, ca, f);
            break;
        case Screen::Extras:
            sHelp = DrawExtras(in, ca, f);
            break;
        case Screen::Gallery:
            sHelp = DrawGallery(in, ca, f);
            break;
        case Screen::Edition:
            sHelp = DrawEdition(in, ca);
            break;
        case Screen::Content:
            sHelp = DrawContent(in, ca, f);
            break;
        case Screen::Credits:
            sHelp = DrawCredits(in, ca, f);
            break;
        default:
            break;
    }
}

void DrawMenu() {
    float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    sTime += dt;
    bool shown = sActive && sFileSelect != nullptr;
    sFade = std::clamp(sFade + (shown ? dt * 4.0f : -dt * 6.0f), 0.0f, 1.0f);
    SyncTouchControls(shown && !sLeaving);
    if (sFade <= 0.0f || sStack.empty()) {
        return;
    }
    if (QuestUi::gBodyFont == nullptr && OTRGlobals::Instance != nullptr) {
        QuestUi::gBodyFont = OTRGlobals::Instance->fontQuestBody;
        QuestUi::gTitleFont = OTRGlobals::Instance->fontQuestTitle;
    }
    ImVec2 pos, size;
    if (!GameRect(pos, size)) {
        return;
    }
    float a = sFade;
    float k = std::min(size.x / kW, size.y / kH);
    ImVec2 origin(pos.x + (size.x - kW * k) / 2, pos.y + (size.y - kH * k) / 2);

    bool inputs = shown && !sLeaving && !OtherWindowOpen() && !IntroVideo::Active();
    if (inputs) {
        KeyboardNav(dt);
    }
    // For the log: what keeps the menu from its controls, once it has gone on for a few seconds.
    {
        static float sBlocked = 0.0f;
        auto deck = Ship::Context::GetInstance()->GetControlDeck();
        bool keyboard = deck != nullptr && deck->KeyboardGameInputBlocked();
        bool pad = deck != nullptr && deck->GamepadGameInputBlocked();
        if (shown && (!inputs || keyboard || pad)) {
            sBlocked += dt;
            if (sBlocked >= 4.0f) {
                sBlocked = -1000.0f;
                auto gui = Ship::Context::GetInstance()->GetWindow()->GetGui();
                auto editor = InputEditor();
                SPDLOG_INFO("Title menu: input blocked (leaving {}, settings menu {}, input editor {}, opening {}, "
                            "keyboard {}, pad {})",
                            sLeaving, gui->GetMenu() != nullptr && gui->GetMenu()->IsVisible(),
                            editor != nullptr && editor->IsVisible(), IntroVideo::Active(), keyboard, pad);
            }
        } else {
            sBlocked = 0.0f; // logged again the next time it happens
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    bool pointer = inputs && sFade >= 0.6f;
    sTapTaken = false;
    if (pointer && io.MouseClicked[0]) {
#ifdef __ANDROID__
        sTouchMode = true;
#else
        sTouchMode = io.MouseSource == ImGuiMouseSource_TouchScreen;
#endif
    }
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##TitleMenu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollWithMouse | (inputs ? 0 : ImGuiWindowFlags_NoInputs));
    if (inputs) {
        // Keep the menu in front of the game's window (and so its taps away from the game) while it is in use.
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Page pg{ dl, origin, k };
    RefreshModsTab(false);
    sScreenT = std::min(1.0f, sScreenT + dt * 5.0f);

    bool zoom = Cur().screen == Screen::Gallery && Cur().zoom;
    // Under the question or the enlarged picture, the screen does not take taps.
    sPointerInputs = pointer && !sConfirm.open && !zoom;
    DrawBackground(dl, pos, size, pg, a);
    // After the opening's moon: the menu builds up from it, the veil first and then the rows one by one.
    static bool sIntroWasOn = false;
    bool introOn = IntroVideo::Active();
    if (sIntroWasOn && !introOn) {
        sBuild = 0.0f;
    }
    sIntroWasOn = introOn;
    if (introOn) {
        sBuild = 0.0f;
    } else {
        sBuild = std::min(1.0f, sBuild + dt / 1.6f);
    }
    if (Cur().screen == Screen::Main) {
        DrawVeil(pg, a * Ease(std::min(1.0f, sBuild * 2.5f)));
        float t = Ease(sScreenT);
        Page rows{ dl, ImVec2(origin.x + (1.0f - t) * sScreenDir * 40.0f * k, origin.y), k };
        DrawMain(rows, dt, a * t);
    } else {
        DrawScreen(pg, dt, a);
    }
    sPointerInputs = pointer && !sConfirm.open;
    if (Cur().screen == Screen::Gallery && Cur().zoom) {
        DrawZoom(pg, a, Cur());
    }
    sPointerInputs = pointer;
    DrawToast(pg, dt, a);
    if (sConfirm.open) {
        DrawConfirm(pg, dt, a);
    }

    // A save is loading: fade to black with the file select.
    if (sLeaving && sFileSelect != nullptr) {
        int fill = std::clamp((int)sFileSelect->screenFillAlpha, 0, 255);
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, fill));
    }
    // Fade in from black when the menu appears.
    if (shown && a < 1.0f) {
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, (int)((1.0f - a) * 255)));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ----- The game's side, once per game frame.

// Back from one of the game's own screens (name entry, sound options): the menu takes over where it was.
void Resume() {
    sActive = true;
    sLeaving = false;
    sHandedOver = 0;
    sScreenT = 0.0f;
    sScreenDir = -1.0f;
    if (sStack.empty()) {
        Show();
    }
}

// Erasing a file, the same steps as the game's own Erase: clear it, write it, then wait for the write to finish.
void UpdateErase(FileSelectState* fs) {
    SramContext* sramCtx = &fs->sramCtx;
    int slot = sEraseSlot;
    if (!sEraseWriting) {
        Sram_EraseSave(fs, sramCtx, slot);
        if (gSaveContext.flashSaveAvailable) {
            Sram_SetFlashPagesDefault(sramCtx, gFlashSaveStartPages[slot * FLASH_SAVE_MAIN_MULTIPLIER],
                                      gFlashSpecialSaveNumPages[slot * FLASH_SAVE_MAIN_MULTIPLIER]);
            Sram_StartWriteToFlashDefault(sramCtx);
            sEraseWriting = true;
            return;
        }
    } else {
        Sram_UpdateWriteToFlashDefault(sramCtx);
        if (sramCtx->status != 0) {
            return;
        }
    }
    for (int i = 0; i < 6; i++) {
        fs->newf[slot][i] = gSaveContext.save.saveInfo.playerData.newf[i];
    }
    if (CVarGetInteger(CVAR_LAST_SLOT, -1) == slot) {
        CVarClear(CVAR_LAST_SLOT);
        SaveCVars();
    }
    sEraseSlot = -1;
    sEraseWriting = false;
    Rumble_Request(200.0f, 255, 20, 150);
    Toast("Arquivo excluído.");
    if (!sStack.empty()) {
        Cur().focus = 0;
        Cur().action = 0;
    }
}

void RunPending(FileSelectState* fs) {
    if (sPending == Pending::None) {
        return;
    }
    if (sPending == Pending::Quit) {
        sPending = Pending::None;
        Ship::Context::GetInstance()->GetWindow()->Close();
        return;
    }
    if (!FileSelectReady(fs)) {
        return;
    }
    Pending pending = sPending;
    sPending = Pending::None;
    switch (pending) {
        case Pending::Load:
            // The same steps as confirming a file on the game's own screen, without the screens in between.
            fs->buttonIndex = fs->selectedFileIndex = sPendingSlot;
            fs->menuMode = FS_MENU_MODE_SELECT;
            fs->selectMode = SM_FADE_OUT;
            fs->confirmButtonIndex = FS_BTN_CONFIRM_YES;
            Audio_MuteAllSeqExceptSystemAndOcarina(15);
            sLeaving = true;
            break;
        case Pending::NewGame:
            // Press A on the empty file, so the game opens its own name entry and makes the new file as usual.
            fs->buttonIndex = sPendingSlot;
            fs->warningLabel = FS_WARNING_NONE;
            sAwaitNewFile = sPendingSlot;
            sActive = false;
            sHandedOver = 0;
            sInjectA = 30;
            break;
        case Pending::GameOptions:
            fs->buttonIndex = FS_BTN_MAIN_OPTIONS;
            fs->warningLabel = FS_WARNING_NONE;
            sActive = false;
            sHandedOver = 0;
            sInjectA = 30;
            break;
        case Pending::OriginalTitle:
            Audio_PlaySfx(NA_SE_SY_FSEL_DECIDE_L);
            sAllowOriginalTitle = true;
            sActive = false;
            gSaveContext.gameMode = GAMEMODE_TITLE_SCREEN;
            STOP_GAMESTATE(&fs->state);
            SET_NEXT_GAMESTATE(&fs->state, TitleSetup_Init, sizeof(TitleSetupState));
            break;
        default:
            break;
    }
}

void OnGameFrame() {
    FileSelectState* fs = gFileSelectState;
    if (fs == nullptr || gGameState != &fs->state) {
        if (sLeaving) {
            sFade = 0.0f; // the game is loading behind a black screen; do not show the menu again on the way out
        }
        sFileSelect = nullptr;
        sActive = false;
        sLeaving = false;
        sPending = Pending::None;
        sInjectA = 0;
        sAwaitNewFile = -1;
        sEraseSlot = -1;
        sEraseWriting = false;
        return;
    }
    if (fs != sFileSelect) {
        // A new file select: after boot, after B on the original title screen, after "save and quit"...
        sFileSelect = fs;
        sInjectA = 0;
        sAwaitNewFile = -1;
        sEraseSlot = -1;
        sEraseWriting = false;
        if (!Enabled()) {
            sActive = false;
            return;
        }
        Show();
        sFade = 0.0f;
        PlayMenuMusic();
    }

    // The opening's moon brings the menu's music in: it starts over from there, fading in (BenPort.cpp).
    static bool sWasMuted = false;
    bool muted = Intro_GameMuted();
    if (sWasMuted && !muted && sActive && !sLeaving) {
        PlayMenuMusic();
    }
    sWasMuted = muted;

    Input* input = CONTROLLER1(&fs->state);
    if (sInjectA > 0) {
        if (FileSelectReady(fs)) {
            input->press.button |= BTN_A;
            sInjectA = 0;
        } else {
            sInjectA--;
        }
    }
    if (sEraseSlot >= 0) {
        UpdateErase(fs);
    }
    if (!sActive) {
        if (sLeaving || !Enabled() || sStack.empty()) {
            return;
        }
        // The game's own screen is up. Once it is closed and the file select is back on its main screen, start
        // the file that was just named, or come back to the menu.
        sHandedOver++;
        if (sInjectA == 0 && sHandedOver > 10 && FileSelectReady(fs)) {
            int slot = sAwaitNewFile;
            sAwaitNewFile = -1;
            Resume();
            if (slot >= 0 && SlotOccupied(fs, slot)) {
                sStack.resize(1);
                LoadSlot(slot);
            } else {
                PlayMenuMusic();
            }
        }
        return;
    }
    if (!sLeaving && !OtherWindowOpen() && !IntroVideo::Active()) {
        HandlePad(input);
    }
    RunPending(fs);
    // Whatever the menu read this frame stays with the menu, even when it just handed over to the file select.
    ClearInput(input);
}

} // namespace
} // namespace TitleMenu

namespace TitleMenu {
bool MenuScene(Scene& out) {
    ImVec2 pos, size;
    if (!GameRect(pos, size)) {
        return false;
    }
    const Tex& bg = Load("background.jpg");
    const Tex& moon = Load("moon.png");
    if (bg.id == nullptr || moon.id == nullptr) {
        return false;
    }
    out.k = std::min(size.x / kW, size.y / kH);
    out.origin = ImVec2(pos.x + (size.x - kW * out.k) / 2, pos.y + (size.y - kH * out.k) / 2);
    out.backgroundTex = bg.id;
    out.moonTex = moon.id;
    // The same drift as DrawBackground's.
    float t = sTime;
    bool fx = Effects();
    float scale = fx ? 1.009f + 0.005f * std::sin(t * 0.45f) : 1.0f;
    float dx = fx ? 1.2f * std::sin(t * 0.23f) : 0.0f;
    float dy = fx ? -1.5f + 1.5f * std::sin(t * 0.31f) : 0.0f;
    auto moonPoint = [&](ImVec2 p) {
        return ImVec2(kMoonCX + (p.x - kMoonCX) * scale + dx, kMoonCY + (p.y - kMoonCY) * scale + dy);
    };
    out.moon0 = moonPoint(ImVec2(kMoonX0, 0));
    out.moon1 = moonPoint(ImVec2(kMoonX0 + moon.size.x, moon.size.y));
    out.eyes[0] = moonPoint(kMoonEyes[0]);
    out.eyes[1] = moonPoint(kMoonEyes[1]);
    return true;
}
} // namespace TitleMenu

void TitleMenuWindow::Draw() {
    TitleMenu::DrawMenu();
    IntroVideo::Draw(); // PC: the opening, on top of everything while the game boots
}

void TitleMenuWindow::InitElement() {
}

void TitleMenuWindow::DrawElement() {
}

void TitleMenuWindow::UpdateElement() {
}

static void RegisterTitleMenu() {
    // Instead of the title screen, go to the file select, where the menu takes over. Normally the title scene sets
    // these up; this is what the "Skip to File Select" option does after the boot logo.
    COND_VB_SHOULD(VB_START_TITLE_SCREEN, CVarGetInteger(CVAR_ENABLED, 1), {
        if (TitleMenu::sAllowOriginalTitle) {
            TitleMenu::sAllowOriginalTitle = false;
        } else {
            *should = false;
            Rand_Seed(osGetTime());
            gSaveContext.seqId = NA_BGM_DISABLED;
            gSaveContext.ambienceId = AMBIENCE_ID_DISABLED;
            Sram_InitNewSave();
            gSaveContext.save.time = CLOCK_TIME(8, 0);
            gSaveContext.save.day = 1;
            gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;
            gSaveContext.gameMode = GAMEMODE_FILE_SELECT;
            STOP_GAMESTATE(gGameState);
            SET_NEXT_GAMESTATE(gGameState, FileSelect_Init, sizeof(FileSelectState));
        }
    });
    // B on the file select's main screen comes back to the menu instead of the title screen.
    COND_VB_SHOULD(VB_FILE_SELECT_EXIT_TO_TITLE, CVarGetInteger(CVAR_ENABLED, 1), {
        if (TitleMenu::sFileSelect != nullptr) {
            *should = false;
            Audio_PlaySfx(NA_SE_SY_FSEL_CLOSE);
            TitleMenu::Resume();
        }
    });
    COND_HOOK(OnGameStateMainStart, true, TitleMenu::OnGameFrame);
    // Remember the file that was played, for Continuar.
    COND_HOOK(OnSaveLoad, true, [](s16 fileNum) {
        if (gSaveContext.gameMode == GAMEMODE_NORMAL && fileNum >= 0 && fileNum < 3 &&
            CVarGetInteger(CVAR_LAST_SLOT, -1) != fileNum) {
            CVarSetInteger(CVAR_LAST_SLOT, fileNum);
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
    });
}

static RegisterShipInitFunc sTitleMenuInit(RegisterTitleMenu, { CVAR_ENABLED });
