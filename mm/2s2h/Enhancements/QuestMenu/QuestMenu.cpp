#include "QuestMenu.h"
#include "QuestMenuBridge.h"

#include "QuestData.h"
#include "QuestRenderer.h"
#include "QuestState.h"

#include <algorithm>
#include <cmath>
#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/controller/controldeck/ControlDeck.h>
#include <ship/controller/physicaldevice/ConnectedPhysicalDeviceManager.h>

#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"
#include "2s2h/Enhancements/Controls/ButtonPrompts.h"
#ifndef QUEST_MENU_PREVIEW
#include "2s2h/BenPort.h"
#endif

extern "C" {
#include <z64.h>
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
void KaleidoScope_RequestQuitToTitle(void);
bool KaleidoScope_QuittingToTitle(void);
}

using namespace QuestUi;

namespace QuestMenu {

void MarkQuestsSeen();

namespace {

constexpr int kCategories = (int)Category::Count;

// ----- Layout of the 1100x700 page: a carved frame holding the category buttons and an open book, a title plaque
// with the crest above it and a hint plaque below.

constexpr float kFrameX0 = 6, kFrameY0 = 84, kFrameX1 = 1094, kFrameY1 = 646;
constexpr float kSideX0 = 58, kSideX1 = 262, kSideY0 = 146, kSideButtonH = 58, kSideGap = 6;
constexpr float kLeftX0 = 272, kLeftX1 = 566, kRightX0 = 562, kRightX1 = 1042, kBookY0 = 134, kBookY1 = 600;
constexpr float kRowsY0 = 206, kRowH = 44;
constexpr int kRowsPerPage = 8;
constexpr float kRX0 = 600, kRX1 = 1022; // right page content
constexpr int kTilesShown = 6;
constexpr float kTile = 56, kTileGap = 8;

// ----- State. The game thread (input hook) and the GUI draw run one after the other on the same thread.

bool sOpen = false;
int sPendingOpen = 0; // frames left to wait for the pause menu after RequestOpen
bool sStartInjected = false;
float sAnim = 0.0f; // 0 closed .. 1 open
float sTime = 0.0f;
int sCategory = 0;
int sSelected[kCategories] = {};
float sCursorY = -1.0f; // highlight position, follows the selection
bool sDetailFocus = false;
int sTile = 0; // step shown under the progress tiles
bool sTileFollowsProgress = true;
float sDetailFade = 1.0f;
int sRepeatDir = 0, sRepeatTimer = 0;
int sRepeatDirX = 0, sRepeatTimerX = 0;
bool sBackHeld = false;

// ----- "Tela inicial" on the pause menu's bottom bar: back to the title menu, after a yes / no question.
constexpr float kQuitHold = 1.0f;
bool sQuitAsk = false;   // the question is on screen
bool sQuitYes = false;   // the answer under the cursor
float sQuitAnim = 0.0f;  // 0..1, the question fading in
bool sQuitStick = false; // the stick was already pushed sideways

void AskQuitToTitle() {
    if (sQuitAsk || KaleidoScope_QuittingToTitle()) {
        return;
    }
    sQuitAsk = true;
    sQuitYes = false;
    Audio_PlaySfx(NA_SE_SY_WIN_OPEN);
}

void AnswerQuit(bool yes) {
    sQuitAsk = false;
    if (yes) {
        Audio_PlaySfx(NA_SE_SY_DECIDE);
        KaleidoScope_RequestQuitToTitle();
    } else {
        Audio_PlaySfx(NA_SE_SY_CANCEL);
    }
}

int QuestCount(int category) {
    return (int)Quests((Category)category).size();
}

void SelectionChanged() {
    sDetailFade = 0.0f;
    sTileFollowsProgress = true;
}

void ChangeCategory(int delta) {
    sCategory = (sCategory + delta + kCategories) % kCategories;
    sDetailFocus = false;
    sCursorY = -1.0f;
    SelectionChanged();
    Audio_PlaySfx(NA_SE_SY_WIN_SCROLL_LEFT);
}

void Select(int index) {
    int count = QuestCount(sCategory);
    if (count == 0) {
        return;
    }
    index = std::clamp(index, 0, count - 1);
    if (index != sSelected[sCategory]) {
        if (index / kRowsPerPage != sSelected[sCategory] / kRowsPerPage) {
            sCursorY = -1.0f;
        }
        sSelected[sCategory] = index;
        SelectionChanged();
        Audio_PlaySfx(NA_SE_SY_CURSOR);
    }
}

void MoveTile(int delta) {
    const auto& quests = Quests((Category)sCategory);
    if (quests.empty()) {
        return;
    }
    int steps = (int)quests[sSelected[sCategory]].objectives.size();
    int next = std::clamp(sTile + delta, 0, std::max(0, steps - 1));
    if (next != sTile) {
        sTile = next;
        sTileFollowsProgress = false;
        Audio_PlaySfx(NA_SE_SY_CURSOR);
    }
}

bool PauseReady() {
    if (gPlayState == nullptr || !SaveLoaded()) {
        return false;
    }
    PauseContext* pauseCtx = &gPlayState->pauseCtx;
    return pauseCtx->state == PAUSE_STATE_MAIN && pauseCtx->mainState == PAUSE_MAIN_STATE_IDLE &&
           pauseCtx->debugEditor == DEBUG_EDITOR_NONE;
}

void Open() {
    sOpen = true;
    MarkQuestsSeen();
    sPendingOpen = 0;
    sDetailFocus = false;
    sCursorY = -1.0f;
    SelectionChanged();
    Audio_PlaySfx(NA_SE_SY_DECIDE);
}

// Held-direction repeat: the first press moves at once, then again after a pause and steadily while held.
bool Repeat(int dir, int& lastDir, int& timer) {
    if (dir == 0) {
        lastDir = 0;
        return false;
    }
    if (dir != lastDir) {
        lastDir = dir;
        timer = 8;
        return true;
    }
    if (--timer <= 0) {
        timer = 3;
        return true;
    }
    return false;
}

// Reads the game's controller while the menu is open. The presses are then cleared so that nothing behind the menu
// reacts to them (the pause menu is also held still through VB_KALEIDO_UPDATE).
void HandlePad(Input* input) {
    u16 press = input->press.button, cur = input->cur.button;
    s8 stickX = input->cur.stick_x, stickY = input->cur.stick_y;

    if (CHECK_BTN_ANY(press, BTN_START)) {
        Close();
        return;
    }
    if (CHECK_BTN_ANY(press, BTN_B)) {
        if (sDetailFocus) {
            sDetailFocus = false;
            Audio_PlaySfx(NA_SE_SY_CANCEL);
        } else {
            Close();
        }
        return;
    }
    if (CHECK_BTN_ANY(press, BTN_A) && !sDetailFocus && QuestCount(sCategory) > 0) {
        sDetailFocus = true;
        Audio_PlaySfx(NA_SE_SY_DECIDE);
    }
    if (CHECK_BTN_ANY(press, BTN_L | BTN_Z)) {
        ChangeCategory(-1);
    } else if (CHECK_BTN_ANY(press, BTN_R)) {
        ChangeCategory(1);
    }

    int dirY = (stickY > 30 || CHECK_BTN_ANY(cur, BTN_DUP))      ? -1
               : (stickY < -30 || CHECK_BTN_ANY(cur, BTN_DDOWN)) ? 1
                                                                 : 0;
    int dirX = (stickX < -30 || CHECK_BTN_ANY(cur, BTN_DLEFT))   ? -1
               : (stickX > 30 || CHECK_BTN_ANY(cur, BTN_DRIGHT)) ? 1
                                                                 : 0;
    if (Repeat(dirY, sRepeatDir, sRepeatTimer) && !sDetailFocus) {
        Select(sSelected[sCategory] + dirY);
    }
    if (Repeat(dirX, sRepeatDirX, sRepeatTimerX)) {
        if (sDetailFocus) {
            MoveTile(dirX); // through the steps of the quest
        } else {
            Select(sSelected[sCategory] + dirX * kRowsPerPage); // turn the page
        }
    }
}

void ClearInput(Input* input) {
    input->press.button = 0;
    input->rel.button = 0;
    input->rel.stick_x = input->rel.stick_y = 0;
    input->press.stick_x = input->press.stick_y = 0;
}

void OnGameFrame() {
    if (gPlayState == nullptr) {
        sOpen = false;
        sPendingOpen = 0;
        return;
    }
    Input* input = CONTROLLER1(&gPlayState->state);
    PauseContext* pauseCtx = &gPlayState->pauseCtx;

    // The quit question takes the controller while it is up; the pause menu waits under it.
    if (sQuitAsk) {
        if (!PauseReady()) {
            sQuitAsk = false;
        } else {
            u16 press = input->press.button;
            if (CHECK_BTN_ANY(press, BTN_A | BTN_START)) {
                AnswerQuit(sQuitYes);
            } else if (CHECK_BTN_ANY(press, BTN_B)) {
                AnswerQuit(false);
            } else {
                bool stick = std::abs(input->rel.stick_x) > 30;
                if (CHECK_BTN_ANY(press, BTN_DLEFT | BTN_DRIGHT) || (stick && !sQuitStick)) {
                    sQuitYes = !sQuitYes;
                    Audio_PlaySfx(NA_SE_SY_CURSOR);
                }
                sQuitStick = stick;
            }
            ClearInput(input);
            return;
        }
    }
    if (KaleidoScope_QuittingToTitle()) {
        ClearInput(input);
        return;
    }

    if (sPendingOpen > 0) {
        sPendingOpen--;
        if (PauseReady()) {
            Open();
        } else if (pauseCtx->state == PAUSE_STATE_OFF && !sStartInjected) {
            // Pause the game the way the player would, so all of the game's own checks apply.
            input->press.button |= BTN_START;
            sStartInjected = true;
        }
    }
    // L on the pause screen opens the menu; the pause menu itself has no use for it. On the Song of Time it is
    // left to the Song of Time variants, and with the debug tools on to the inventory editor.
    if (!sOpen && PauseReady() && CHECK_BTN_ANY(input->press.button, BTN_L) &&
        !CVarGetInteger("gDeveloperTools.DebugEnabled", 0) &&
        !(pauseCtx->pageIndex == PAUSE_QUEST && pauseCtx->cursorSpecialPos == 0 &&
          pauseCtx->cursorPoint[PAUSE_QUEST] == QUEST_SONG_TIME)) {
        Open();
        ClearInput(input);
        return;
    }
    if (!sOpen) {
        return;
    }
    if (pauseCtx->state != PAUSE_STATE_MAIN) {
        sOpen = false;
        return;
    }
    HandlePad(input);
    ClearInput(input);
}

// N on the keyboard, or Back / View / Select / - on a controller: a tap opens or closes the quest menu; held for a
// second on the pause screen it asks to go back to the title menu (the "Tela inicial" button on the bottom bar).
float sShortcutHeld = -1.0f; // seconds the shortcut has been held, -1 when it is up
bool sShortcutLong = false;  // this hold already asked to quit

bool ShortcutPressed() {
    bool held = !ImGui::GetIO().WantTextInput && ImGui::IsKeyDown(ImGuiKey_N);
    // With controller navigation of the menus on, Back opens the settings menu instead.
    if (!CVarGetInteger(CVAR_IMGUI_CONTROLLER_NAV, 0)) {
        auto gamepads = Ship::Context::GetInstance()
                            ->GetControlDeck()
                            ->GetConnectedPhysicalDeviceManager()
                            ->GetConnectedSDLGamepadsForPort(0);
        for (const auto& [id, gamepad] : gamepads) {
            held = held || SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_BACK);
        }
    }
    bool pressed = false;
    if (held) {
        sShortcutHeld = sShortcutHeld < 0.0f ? 0.0f : sShortcutHeld + ImGui::GetIO().DeltaTime;
        if (!sShortcutLong && !sOpen && sShortcutHeld >= kQuitHold && PauseReady()) {
            sShortcutLong = true;
            AskQuitToTitle();
        }
    } else {
        // Released: a short press is the quest menu shortcut. Off the pause screen it acts on the press, as before.
        pressed = sShortcutHeld >= 0.0f && !sShortcutLong && PauseReady();
        sShortcutHeld = -1.0f;
        sShortcutLong = false;
    }
    if (held && !sBackHeld && !PauseReady() && !sOpen) {
        pressed = true;
        sShortcutLong = true; // already used
    }
    sBackHeld = held;
    return pressed;
}

// ----- Drawing.

float Ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

ImU32 Alpha(ImU32 col, float a) {
    int alpha = (int)(((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
    return (col & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
}

ImU32 White(float a) {
    return Alpha(IM_COL32_WHITE, a);
}

const ImU32 cCream = Col(250, 236, 200);
const ImU32 cTitleInk = Col(96, 30, 52);
const ImU32 cHeadInk = Col(92, 40, 120);

// A button prompt: the icon of the physical button when the button icons are on, otherwise an N64-style badge.
void Prompt(const Page& pg, ButtonPrompt prompt, float cx, float cy, float s) {
    const char* tex = ButtonPrompts_Icon(prompt);
    if (tex != nullptr && GameIcon(pg, tex, cx, cy, s)) {
        return;
    }
    ImU32 face = Col(120, 120, 130);
    const char* label = "";
    switch (prompt) {
        case BUTTON_PROMPT_A:
            face = Col(60, 90, 220);
            label = "A";
            break;
        case BUTTON_PROMPT_B:
            face = Col(40, 160, 60);
            label = "B";
            break;
        case BUTTON_PROMPT_L:
            label = "L";
            break;
        case BUTTON_PROMPT_R:
            label = "R";
            break;
        case BUTTON_PROMPT_Z:
            label = "Z";
            break;
        case BUTTON_PROMPT_START:
            face = Col(200, 40, 40);
            label = "S";
            break;
        case BUTTON_PROMPT_STICK:
            face = Col(150, 150, 160);
            label = "+";
            break;
        default:
            break;
    }
    pg.dl->AddCircleFilled(pg.P(cx, cy), pg.S(s * 0.5f), Col(20, 10, 20, 200));
    pg.dl->AddCircleFilled(pg.P(cx, cy), pg.S(s * 0.44f), face);
    float size = s * 0.66f;
    TextCentered(pg, cx, cy - size * 0.6f, size, Col(255, 255, 255), label, false, TitleFont());
}

void ProgressBar(const Page& pg, float x0, float y0, float x1, float y1, float fraction, ImU32 fill) {
    float r = (y1 - y0) * 0.5f;
    pg.dl->AddRectFilled(pg.P(x0, y0), pg.P(x1, y1), Col(60, 36, 30, 120), pg.S(r));
    if (fraction > 0.0f) {
        float xf = x0 + (x1 - x0) * std::clamp(fraction, 0.0f, 1.0f);
        pg.dl->AddRectFilled(pg.P(x0, y0), pg.P(std::max(xf, x0 + r * 2), y1), fill, pg.S(r));
    }
    pg.dl->AddRect(pg.P(x0, y0), pg.P(x1, y1), Col(150, 105, 40, 200), pg.S(r), 0, pg.S(1));
}

bool UsesPortraits(Category category) {
    return category == Category::Side || category == Category::People;
}

// A quest's picture: the game's icon or notebook photo (from the player's files), our PNG, or the category's.
void QuestIcon(const Page& pg, const Quest& quest, float cx, float cy, float s, float alpha, bool round = false) {
    ImU32 tint = White(alpha);
    if (quest.icon != nullptr) {
        ImTextureID id = Texture(quest.icon);
        if (id != nullptr) {
            if (round) {
                pg.dl->AddImageRounded(id, pg.P(cx - s / 2, cy - s / 2), pg.P(cx + s / 2, cy + s / 2), ImVec2(0, 0),
                                       ImVec2(1, 1), tint, pg.S(s / 2));
            } else {
                pg.dl->AddImage(id, pg.P(cx - s / 2, cy - s / 2), pg.P(cx + s / 2, cy + s / 2), ImVec2(0, 0),
                                ImVec2(1, 1), tint);
            }
            return;
        }
    }
    const char* asset = quest.iconAsset != nullptr ? quest.iconAsset : CategoryAsset((Category)sCategory);
    Png(pg, asset, ImVec2(cx - s / 2, cy - s / 2), ImVec2(cx + s / 2, cy + s / 2), tint);
}

ImU32 StatusColor(Status status) {
    switch (status) {
        case Status::Locked:
            return Col(130, 110, 120);
        case Status::Available:
            return Col(196, 140, 40);
        case Status::InProgress:
            return Col(70, 120, 200);
        case Status::Done:
            return Col(60, 150, 50);
    }
    return cGold;
}

// ----- Frame, title, clock.

void DrawFrame(const Page& pg, float a) {
    if (!NineSlice(pg, "book_frame", ImVec2(kFrameX0, kFrameY0), ImVec2(kFrameX1, kFrameY1), 88, 50, White(a))) {
        Panel(pg, ImVec2(kFrameX0, kFrameY0), ImVec2(kFrameX1, kFrameY1), Alpha(cPurple, a), Alpha(cPurpleDeep, a), 18);
        GoldFrame(pg, ImVec2(kFrameX0, kFrameY0), ImVec2(kFrameX1, kFrameY1), 18, 4);
    }
    // the stone behind the buttons and the book
    pg.dl->AddRectFilled(pg.P(kFrameX0 + 44, kFrameY0 + 44), pg.P(kFrameX1 - 44, kFrameY1 - 44),
                         Col(30, 10, 44, (int)(235 * a)), pg.S(8));
}

void DrawTitle(const Page& pg, float a) {
    if (!Png(pg, "title_plaque", ImVec2(318, 56), ImVec2(782, 138), White(a))) {
        Panel(pg, ImVec2(330, 66), ImVec2(770, 130), cWine, cPurpleDeep, 12);
    }
    const char* title = CategoryName((Category)sCategory);
    float size = 36;
    while (size > 22 && TextWidth(pg, size, title, TitleFont()) > 380) {
        size -= 2;
    }
    TextCentered(pg, 550, 97 - size * 0.62f, size, Alpha(cCream, a), title, true, TitleFont());
    // the crest sits on the plaque
    float bob = sinf(sTime * 1.6f) * 1.5f;
    Png(pg, "emblem", ImVec2(484, -10 + bob), ImVec2(616, 89 + bob), White(a));

    // L and R keycaps beside the plaque, clickable.
    struct Key {
        float x;
        ButtonPrompt prompt;
        const char* label;
        int delta;
        const char* id;
    } keys[] = { { 270, BUTTON_PROMPT_L, "L", -1, "##qm_prev" }, { 782, BUTTON_PROMPT_R, "R", 1, "##qm_next" } };
    for (const Key& k : keys) {
        if (ButtonPrompts_Icon(k.prompt) != nullptr) {
            Prompt(pg, k.prompt, k.x + 24, 98, 44);
        } else if (Png(pg, "keycap", ImVec2(k.x, 74), ImVec2(k.x + 48, 122), White(a))) {
            TextCentered(pg, k.x + 24, 76, 30, Alpha(Col(40, 30, 50), a), k.label, false, TitleFont());
        } else {
            Prompt(pg, k.prompt, k.x + 24, 98, 44);
        }
        if (Hit(pg, k.id, k.x, 74, 48, 48)) {
            ChangeCategory(k.delta);
        }
    }
}

void DrawClock(const Page& pg, float a) {
    if (!SaveLoaded()) {
        return;
    }
    float x0 = 856, y0 = 18, x1 = 1086, y1 = 70;
    if (!Png(pg, "clock_box", ImVec2(x0, y0), ImVec2(x1, y1), White(a))) {
        Panel(pg, ImVec2(x0, y0), ImVec2(x1, y1), cPurple, cPurpleDeep, 10);
    }
    int minutes = (int)TIME_TO_MINUTES_F(CURRENT_TIME);
    bool night = minutes < 6 * 60 || minutes >= 18 * 60;
    char day[16], clock[16];
    snprintf(day, sizeof(day), "Dia %d", std::max(1, (int)CURRENT_DAY));
    snprintf(clock, sizeof(clock), "%d:%02d", minutes / 60, minutes % 60);
    float cy = (y0 + y1) / 2;
    IconSun(pg, x0 + 34, cy, 26);
    Text(pg, x0 + 52, cy - 15, 26, Alpha(cCream, a), day, 0, TitleFont());
    if (night) {
        IconMoon(pg, x0 + 146, cy, 24, Col(40, 22, 80));
    } else {
        IconClock(pg, x0 + 146, cy, 24);
    }
    Text(pg, x0 + 164, cy - 15, 26, Alpha(cCream, a), clock, 0, TitleFont());
}

void DrawCloseButton(const Page& pg, float a) {
    float cx = 40, cy = 44;
    pg.dl->AddCircleFilled(pg.P(cx, cy), pg.S(22), Col(60, 20, 40, (int)(230 * a)));
    pg.dl->AddCircle(pg.P(cx, cy), pg.S(22), Alpha(cGold, a), 0, pg.S(2.5f));
    pg.dl->AddLine(pg.P(cx - 8, cy - 8), pg.P(cx + 8, cy + 8), Alpha(cGold, a), pg.S(3.5f));
    pg.dl->AddLine(pg.P(cx + 8, cy - 8), pg.P(cx - 8, cy + 8), Alpha(cGold, a), pg.S(3.5f));
    if (Hit(pg, "##qm_close", cx - 24, cy - 24, 48, 48)) {
        Close();
    }
}

// ----- Category buttons.

void DrawSidebar(const Page& pg, float a) {
    float pulse = 0.5f + 0.5f * sinf(sTime * 4.0f);
    for (int c = 0; c < kCategories; c++) {
        float y0 = kSideY0 + c * (kSideButtonH + kSideGap), y1 = y0 + kSideButtonH;
        bool selected = c == sCategory;
        bool hovered = false;
        ImGui::PushID(c);
        if (Hit(pg, "##qm_cat", kSideX0, y0, kSideX1 - kSideX0, kSideButtonH, &hovered) && !selected) {
            ChangeCategory(c - sCategory);
        }
        ImGui::PopID();

        float x0 = kSideX0, x1 = kSideX1;
        if (selected) {
            for (int i = 4; i >= 1; i--) {
                pg.dl->AddRect(pg.P(x0 - i * 2.5f, y0 - i * 2.5f), pg.P(x1 + i * 2.5f, y1 + i * 2.5f),
                               Col(255, 214, 90, (int)((26 + 34 * pulse) * (5 - i) * a)), pg.S(12 + i * 2.5f), 0,
                               pg.S(3));
            }
        }
        if (!NineSlice(pg, selected ? "button_selected" : "button", ImVec2(x0, y0), ImVec2(x1, y1), 28, 18,
                       White(a) & (hovered && !selected ? Col(255, 236, 255) : IM_COL32_WHITE))) {
            Panel(pg, ImVec2(x0, y0), ImVec2(x1, y1), selected ? cWine : cPurpleLight, cPurpleDeep, 10);
            GoldFrame(pg, ImVec2(x0, y0), ImVec2(x1, y1), 10, selected ? 3.0f : 1.5f);
        }
        Png(pg, CategoryAsset((Category)c), ImVec2(x0 + 8, y0 + 6), ImVec2(x0 + 54, y0 + 52), White(a));
        const char* name = CategoryName((Category)c);
        float size = 21;
        float wrap = x1 - x0 - 66;
        float h = WrappedHeight(pg, size, wrap, name, TitleFont());
        if (h > 30) {
            size = 18;
            h = WrappedHeight(pg, size, wrap, name, TitleFont());
        }
        Text(pg, x0 + 60, (y0 + y1) / 2 - h / 2 - 1, size, Alpha(selected ? Col(255, 228, 140) : cCream, a), name, wrap,
             TitleFont());
    }
}

// ----- The left page: the list, with portraits or icons and a check box for each entry.

void DrawLeftPage(const Page& pg, float a, const std::vector<QuestProgress>& progress) {
    if (!Png(pg, "page_left", ImVec2(kLeftX0, kBookY0), ImVec2(kLeftX1, kBookY1), White(a))) {
        Panel(pg, ImVec2(kLeftX0, kBookY0), ImVec2(kLeftX1, kBookY1), cPurple, cPurpleDeep, 8);
    }
    GoldFrame(pg, ImVec2(kLeftX0, kBookY0), ImVec2(kLeftX1 - 6, kBookY1), 6, 2.5f);

    // Heading: the category's picture and its subtitle.
    Png(pg, CategoryAsset((Category)sCategory), ImVec2(kLeftX0 + 14, kBookY0 + 12), ImVec2(kLeftX0 + 58, kBookY0 + 56),
        White(a));
    TextShadow(pg, kLeftX0 + 64, kBookY0 + 18, 26, Alpha(Col(255, 222, 150), a), CategorySubtitle((Category)sCategory),
               TitleFont());
    Png(pg, "divider", ImVec2(kLeftX0 + 12, kBookY0 + 58), ImVec2(kLeftX1 - 18, kBookY0 + 70), White(a));

    const auto& quests = Quests((Category)sCategory);
    int count = (int)quests.size();
    if (count == 0) {
        TextCentered(pg, (kLeftX0 + kLeftX1) / 2, 320, 18, cCream, "Nada por aqui ainda.");
        return;
    }
    int& selected = sSelected[sCategory];
    selected = std::clamp(selected, 0, count - 1);
    int pages = (count + kRowsPerPage - 1) / kRowsPerPage;
    int page = selected / kRowsPerPage;
    int first = page * kRowsPerPage, last = std::min(count, first + kRowsPerPage);

    // Mouse wheel over the list moves the selection.
    if (ImGui::IsMouseHoveringRect(pg.P(kLeftX0, kRowsY0), pg.P(kLeftX1, kRowsY0 + kRowsPerPage * kRowH)) &&
        ImGui::GetIO().MouseWheel != 0) {
        Select(selected + (ImGui::GetIO().MouseWheel > 0 ? -1 : 1));
    }

    float dt = ImGui::GetIO().DeltaTime;
    float targetY = kRowsY0 + (selected - first) * kRowH;
    if (sCursorY < 0) {
        sCursorY = targetY;
    }
    sCursorY += (targetY - sCursorY) * std::min(1.0f, dt * 18.0f);

    float rx0 = kLeftX0 + 12, rx1 = kLeftX1 - 18;
    bool portraits = UsesPortraits((Category)sCategory);
    for (int i = first; i < last; i++) {
        float y = kRowsY0 + (i - first) * kRowH;
        if (i + 1 < last) {
            pg.dl->AddLine(pg.P(rx0 + 50, y + kRowH), pg.P(rx1 - 4, y + kRowH), Col(150, 105, 180, (int)(70 * a)),
                           pg.S(1));
        }
    }

    // Selection: a glowing gold box.
    float pulse = 0.5f + 0.5f * sinf(sTime * 5.0f);
    for (int i = 3; i >= 1; i--) {
        pg.dl->AddRect(pg.P(rx0 - i * 2, sCursorY + 1 - i * 2), pg.P(rx1 + i * 2, sCursorY + kRowH - 1 + i * 2),
                       Col(255, 214, 90, (int)((30 + 40 * pulse) * (4 - i) * a)), pg.S(10 + i * 2), 0, pg.S(3));
    }
    pg.dl->AddRectFilledMultiColor(pg.P(rx0, sCursorY + 1), pg.P(rx1, sCursorY + kRowH - 1),
                                   Col(120, 70, 30, (int)(150 * a)), Col(80, 40, 60, (int)(120 * a)),
                                   Col(80, 40, 60, (int)(120 * a)), Col(120, 70, 30, (int)(150 * a)));
    pg.dl->AddRect(pg.P(rx0, sCursorY + 1), pg.P(rx1, sCursorY + kRowH - 1), Alpha(cGold, a), pg.S(8), 0,
                   pg.S(sDetailFocus ? 1.5f : 2.5f));

    for (int i = first; i < last; i++) {
        const Quest& q = quests[i];
        const QuestProgress& p = progress[i];
        float y = kRowsY0 + (i - first) * kRowH;
        bool locked = p.status == Status::Locked;
        float rowAlpha = a * (locked ? 0.5f : 1.0f);

        ImGui::PushID(i);
        if (Hit(pg, "##qm_row", rx0, y, rx1 - rx0, kRowH)) {
            if (selected == i) {
                sDetailFocus = true;
            } else {
                Select(i);
            }
        }
        ImGui::PopID();

        float cx = rx0 + 26, cy = y + kRowH / 2;
        if (portraits) {
            pg.dl->AddCircleFilled(pg.P(cx, cy), pg.S(19), Col(230, 200, 150, (int)(255 * rowAlpha)));
            QuestIcon(pg, q, cx, cy, 36, rowAlpha, true);
            pg.dl->AddCircle(pg.P(cx, cy), pg.S(19), Alpha(cGoldDark, a), 0, pg.S(1.5f));
        } else {
            QuestIcon(pg, q, cx, cy, 36, rowAlpha);
        }
        float nameSize = 23;
        float maxW = rx1 - rx0 - (p.status == Status::InProgress && p.total > 0 ? 136 : 100);
        while (nameSize > 15 && TextWidth(pg, nameSize, q.name.c_str()) > maxW) {
            nameSize -= 1;
        }
        Text(pg, rx0 + 54, cy - nameSize * 0.62f, nameSize,
             Alpha(i == selected ? Col(255, 240, 200) : cCream, rowAlpha), q.name.c_str());

        // Status on the right: lock, check box, or the steps done so far.
        float bx = rx1 - 34, by = cy - 12;
        if (locked) {
            Png(pg, "state_locked", ImVec2(bx - 1, by - 1), ImVec2(bx + 25, by + 25), White(a * 0.85f));
        } else {
            if (!Png(pg, p.status == Status::Done ? "mark_done" : "mark_todo", ImVec2(bx, by), ImVec2(bx + 24, by + 24),
                     White(a))) {
                CheckSquare(pg, bx, by, 24, p.status == Status::Done);
            }
            if (p.status == Status::InProgress && p.total > 0) {
                char frac[16];
                snprintf(frac, sizeof(frac), "%d/%d", p.done, p.total);
                float w = TextWidth(pg, 15, frac);
                Text(pg, bx - w - 6, cy - 9, 15, Alpha(Col(150, 200, 255), a), frac);
            }
        }
    }

    // Pages: < 1 / 2 >
    if (pages > 1) {
        float cx = (kLeftX0 + kLeftX1) / 2 - 4, cy = kBookY1 - 22;
        char label[16];
        snprintf(label, sizeof(label), "%d / %d", page + 1, pages);
        TextCentered(pg, cx, cy - 13, 22, Alpha(cCream, a), label, true, TitleFont());
        float w = TextWidth(pg, 22, label, TitleFont());
        float lx = cx - w / 2 - 22, rx = cx + w / 2 + 22;
        ImU32 on = Alpha(Col(250, 230, 190), a), off = Alpha(Col(120, 90, 130), a);
        pg.dl->AddTriangleFilled(pg.P(lx - 7, cy), pg.P(lx + 6, cy - 9), pg.P(lx + 6, cy + 9), page > 0 ? on : off);
        pg.dl->AddTriangleFilled(pg.P(rx + 7, cy), pg.P(rx - 6, cy - 9), pg.P(rx - 6, cy + 9),
                                 page < pages - 1 ? on : off);
        if (Hit(pg, "##qm_pgprev", lx - 16, cy - 16, 32, 32) && page > 0) {
            Select((page - 1) * kRowsPerPage);
        }
        if (Hit(pg, "##qm_pgnext", rx - 16, cy - 16, 32, 32) && page < pages - 1) {
            Select((page + 1) * kRowsPerPage);
        }
    }
}

// ----- The right page: the selected quest on parchment.

void SectionHeading(const Page& pg, float x, float y, const char* text, float a) {
    Text(pg, x, y, 25, Alpha(cHeadInk, a), text, 0, TitleFont());
}

void DrawSteps(const Page& pg, const Quest& q, const QuestProgress& p, float y, float a) {
    int steps = (int)q.objectives.size();
    int next = 0;
    while (next < steps && p.objectiveDone[next]) {
        next++;
    }
    if (sTileFollowsProgress) {
        sTile = std::min(next, std::max(0, steps - 1));
    }
    sTile = std::clamp(sTile, 0, std::max(0, steps - 1));

    // Which tiles fit: a window around the one shown.
    int firstTile = std::clamp(sTile - kTilesShown / 2, 0, std::max(0, steps - kTilesShown));
    int lastTile = std::min(steps, firstTile + kTilesShown);
    float pulse = 0.5f + 0.5f * sinf(sTime * 4.5f);
    for (int i = firstTile; i < lastTile; i++) {
        float x = kRX0 + (i - firstTile) * (kTile + kTileGap);
        bool done = p.objectiveDone[i];
        bool current = i == sTile;
        if (current) {
            for (int g = 3; g >= 1; g--) {
                pg.dl->AddRect(pg.P(x - g * 2, y - g * 2), pg.P(x + kTile + g * 2, y + kTile + g * 2),
                               Col(255, 210, 90, (int)((25 + 40 * pulse) * (4 - g) * a)), pg.S(12 + g * 2), 0, pg.S(3));
            }
        }
        if (!NineSlice(pg, current ? "tile_current" : "tile", ImVec2(x, y), ImVec2(x + kTile, y + kTile), 30, 18,
                       White(a))) {
            Panel(pg, ImVec2(x, y), ImVec2(x + kTile, y + kTile), current ? cPurpleLight : cParchment, cParchmentDark,
                  8);
        }
        const Objective& o = q.objectives[i];
        ImTextureID id = o.icon != nullptr ? Texture(o.icon) : nullptr;
        float alpha = a * (done || current ? 1.0f : 0.75f);
        if (id != nullptr) {
            float s = kTile * 0.66f;
            float cx = x + kTile / 2, cy = y + kTile / 2;
            pg.dl->AddImage(id, pg.P(cx - s / 2, cy - s / 2), pg.P(cx + s / 2, cy + s / 2), ImVec2(0, 0), ImVec2(1, 1),
                            White(alpha));
        } else {
            char num[8];
            snprintf(num, sizeof(num), "%d", i + 1);
            TextCentered(pg, x + kTile / 2, y + kTile / 2 - 19, 30, Alpha(current ? cCream : Col(110, 70, 40), alpha),
                         num, false, TitleFont());
        }
        if (done) {
            Png(pg, "mark_done", ImVec2(x + kTile - 22, y + kTile - 22), ImVec2(x + kTile + 4, y + kTile + 4),
                White(a));
        }
        ImGui::PushID(i);
        if (Hit(pg, "##qm_tile", x, y, kTile, kTile)) {
            sTile = i;
            sTileFollowsProgress = false;
            sDetailFocus = true;
        }
        ImGui::PopID();
    }
    if (firstTile > 0) {
        float cy = y + kTile / 2;
        pg.dl->AddTriangleFilled(pg.P(kRX0 - 16, cy), pg.P(kRX0 - 6, cy - 8), pg.P(kRX0 - 6, cy + 8),
                                 Alpha(cTitleInk, a));
    }
    if (lastTile < steps) {
        float x = kRX0 + kTilesShown * (kTile + kTileGap) - 2, cy = y + kTile / 2;
        pg.dl->AddTriangleFilled(pg.P(x + 10, cy), pg.P(x, cy - 8), pg.P(x, cy + 8), Alpha(cTitleInk, a));
    }

    // The step shown, in words.
    if (steps > 0) {
        const Objective& o = q.objectives[sTile];
        char head[32];
        snprintf(head, sizeof(head), "Passo %d de %d", sTile + 1, steps);
        float ty = y + kTile + 10;
        Text(pg, kRX0, ty, 20, Alpha(cTitleInk, a), head, 0, TitleFont());
        float hw = TextWidth(pg, 20, head, TitleFont());
        if (p.objectiveDone[sTile]) {
            Text(pg, kRX0 + hw + 10, ty, 20, Alpha(Col(60, 140, 50), a), "feito");
        }
        Text(pg, kRX0, ty + 24, 20, Alpha(cInk, a), o.text.c_str(), kRX1 - kRX0);
    }
}

void DrawRightPage(const Page& pg, float a, const std::vector<QuestProgress>& progress) {
    float pulse = 0.5f + 0.5f * sinf(sTime * 4.0f);
    if (sDetailFocus) {
        for (int i = 4; i >= 1; i--) {
            pg.dl->AddRect(pg.P(kRightX0 - i * 3, kBookY0 - i * 3), pg.P(kRightX1 + i * 3, kBookY1 + i * 3),
                           Col(255, 210, 90, (int)((18 + 26 * pulse) * (5 - i) * a)), pg.S(10 + i * 3), 0, pg.S(3));
        }
    }
    if (!Png(pg, "page_right", ImVec2(kRightX0, kBookY0), ImVec2(kRightX1, kBookY1), White(a))) {
        Panel(pg, ImVec2(kRightX0, kBookY0), ImVec2(kRightX1, kBookY1), cParchment, cParchmentDark, 8);
    }
    GoldFrame(pg, ImVec2(kRightX0, kBookY0), ImVec2(kRightX1, kBookY1), 6, 2.5f);
    Png(pg, "spine", ImVec2(kLeftX1 - 22, kBookY0 - 6), ImVec2(kLeftX1 + 8, kBookY1 + 6), White(a));

    const auto& quests = Quests((Category)sCategory);
    if (quests.empty()) {
        return;
    }
    if (Hit(pg, "##qm_detail", kRightX0, kBookY0, kRightX1 - kRightX0, 160)) {
        sDetailFocus = true;
    }
    const Quest& q = quests[sSelected[sCategory]];
    const QuestProgress& p = progress[sSelected[sCategory]];
    bool locked = p.status == Status::Locked;

    float dt = ImGui::GetIO().DeltaTime;
    sDetailFade = std::min(1.0f, sDetailFade + dt * 6.0f);
    float fa = a * Ease(sDetailFade);
    float slide = (1.0f - Ease(sDetailFade)) * 12.0f;

    pg.dl->PushClipRect(pg.P(kRightX0 + 8, kBookY0 + 6), pg.P(kRightX1 - 8, kBookY1 - 6), true);

    // Picture in a gold ring, top right.
    float pcx = 950, pcy = 214 + slide, pr = 62;
    pg.dl->AddCircleFilled(pg.P(pcx, pcy), pg.S(pr - 4), Col(236, 214, 170, (int)(255 * fa)));
    QuestIcon(pg, q, pcx, pcy, UsesPortraits((Category)sCategory) ? (pr - 6) * 2 : pr * 1.4f,
              fa * (locked ? 0.45f : 1.0f), UsesPortraits((Category)sCategory));
    Png(pg, "portrait_ring", ImVec2(pcx - pr - 6, pcy - pr - 6), ImVec2(pcx + pr + 6, pcy + pr + 6), White(fa));

    // Title and description.
    float y = kBookY0 + 16 + slide;
    float tw = 870 - kRX0;
    float titleSize = 32;
    while (titleSize > 22 && WrappedHeight(pg, titleSize, tw, q.name.c_str(), TitleFont()) > 80) {
        titleSize -= 2;
    }
    Text(pg, kRX0, y, titleSize, Alpha(cTitleInk, fa), q.name.c_str(), tw, TitleFont());
    y += WrappedHeight(pg, titleSize, tw, q.name.c_str(), TitleFont()) + 2;
    pg.dl->AddLine(pg.P(kRX0, y), pg.P(870, y), Alpha(Col(150, 100, 60), fa), pg.S(1.5f));
    y += 8;
    if (locked) {
        const char* hint = q.lockedHint.empty() ? "Continue a aventura para liberar." : q.lockedHint.c_str();
        Png(pg, "state_locked", ImVec2(kRX0, y), ImVec2(kRX0 + 26, y + 26), White(fa));
        Text(pg, kRX0 + 32, y + 2, 20, Alpha(cWine, fa), hint, tw - 32);
        y += std::max(28.0f, WrappedHeight(pg, 20, tw - 32, hint)) + 6;
    }
    if (!q.description.empty()) {
        float descSize = 21;
        float room = 296 + slide - y;
        while (descSize > 14 && WrappedHeight(pg, descSize, tw, q.description.c_str()) > room) {
            descSize -= 1;
        }
        Text(pg, kRX0, y, descSize, Alpha(cInk, fa), q.description.c_str(), tw);
    }

    // Progress: tiles for the steps, or a bar for a count.
    float py = 300 + slide;
    IconTriforce(pg, kRX0 + 12, py + 13, 22);
    if (!q.objectives.empty()) {
        SectionHeading(pg, kRX0 + 30, py, "Progresso da Missão", fa);
        DrawSteps(pg, q, p, py + 32, fa);
    } else if (q.counter.value) {
        SectionHeading(pg, kRX0 + 30, py, "Progresso", fa);
        char line[32];
        snprintf(line, sizeof(line), "%d / %d", locked ? 0 : p.counterValue, q.counter.max);
        Text(pg, kRX0, py + 40, 30, Alpha(cTitleInk, fa), line, 0, TitleFont());
        ProgressBar(pg, kRX0, py + 84, kRX1, py + 98, locked ? 0.0f : (float)p.counterValue / q.counter.max,
                    Alpha(StatusColor(p.status == Status::Done ? Status::Done : Status::InProgress), fa));
    }
    if (q.counter.value && !q.objectives.empty()) {
        char line[32];
        snprintf(line, sizeof(line), "%d / %d", locked ? 0 : p.counterValue, q.counter.max);
        float w = TextWidth(pg, 18, line, TitleFont());
        Text(pg, kRX1 - w, py + 2, 18, Alpha(cTitleInk, fa), line, 0, TitleFont());
    }

    // When and where.
    float iy = 470 + slide;
    pg.dl->AddLine(pg.P(kRX0, iy - 6), pg.P(kRX1, iy - 6), Alpha(Col(150, 100, 60, 160), fa), pg.S(1));
    float colW = (kRX1 - kRX0) / 2 - 10;
    IconClock(pg, kRX0 + 11, iy + 12, 20);
    SectionHeading(pg, kRX0 + 28, iy, "Horário", fa);
    Text(pg, kRX0 + 4, iy + 28, 19, Alpha(cInk, fa), q.when.empty() ? "Qualquer hora" : q.when.c_str(), colW);
    float lx = kRX0 + colW + 20;
    pg.dl->AddLine(pg.P(lx - 10, iy), pg.P(lx - 10, iy + 60), Alpha(Col(150, 100, 60, 140), fa), pg.S(1));
    IconPin(pg, lx + 10, iy + 12, 20);
    SectionHeading(pg, lx + 26, iy, "Local", fa);
    Text(pg, lx + 4, iy + 28, 19, Alpha(cInk, fa), q.region.empty() ? "Termina" : q.region.c_str(), colW);

    // Reward and status.
    float ry = 540 + slide;
    pg.dl->AddLine(pg.P(kRX0, ry - 6), pg.P(kRX1, ry - 6), Alpha(Col(150, 100, 60, 160), fa), pg.S(1));
    IconChest(pg, kRX0 + 12, ry + 12, 22);
    SectionHeading(pg, kRX0 + 30, ry, "Recompensa", fa);
    Text(pg, kRX0 + 4, ry + 28, 18, Alpha(Col(110, 60, 20), fa), q.reward.empty() ? "-" : q.reward.c_str(), colW);

    float sx = lx - 4, sy = ry - 2;
    pg.dl->AddRectFilled(pg.P(sx, sy), pg.P(kRX1, sy + 50), Col(70, 30, 60, (int)(40 * fa)), pg.S(10));
    pg.dl->AddRect(pg.P(sx, sy), pg.P(kRX1, sy + 50), Alpha(Col(150, 100, 60), fa), pg.S(10), 0, pg.S(1.2f));
    Png(pg, StatusAsset(p.status), ImVec2(sx + 8, sy + 9), ImVec2(sx + 40, sy + 41), White(fa));
    Text(pg, sx + 46, sy + 4, 20, Alpha(StatusColor(p.status), fa), StatusName(p.status), 0, TitleFont());
    if (p.total > 0 && !locked) {
        ProgressBar(pg, sx + 48, sy + 32, kRX1 - 52, sy + 40, (float)p.done / p.total,
                    Alpha(StatusColor(p.status), fa));
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", p.done * 100 / p.total);
        Text(pg, kRX1 - 46, sy + 26, 15, Alpha(cInk, fa), pct);
    }
    pg.dl->PopClipRect();
}

// ----- The plaque under the book: a line about the category and the controls.

void DrawFooter(const Page& pg, float a) {
    float x0 = 196, y0 = 618, x1 = 904, y1 = 700;
    if (!Png(pg, "hint_plaque", ImVec2(x0, y0), ImVec2(x1, y1), White(a))) {
        Panel(pg, ImVec2(x0, y0), ImVec2(x1, y1), cPurple, cPurpleDeep, 12);
    }
    TextCentered(pg, 550, y0 + 12, 22, Alpha(cCream, a), CategoryHint((Category)sCategory), true);

    struct Hint {
        ButtonPrompt prompt;
        const char* text;
    };
    Hint hints[] = { { BUTTON_PROMPT_R, "Categoria" },
                     { BUTTON_PROMPT_STICK, sDetailFocus ? "Passos" : "Escolher" },
                     { BUTTON_PROMPT_A, "Detalhes" },
                     { BUTTON_PROMPT_B, sDetailFocus ? "Voltar" : "Fechar" } };
    float size = 18, icon = 22;
    float total = icon + 4;
    for (const Hint& h : hints) {
        total += icon + 6 + TextWidth(pg, size, h.text) + 18;
    }
    float x = 550 - total / 2, cy = y0 + 54;
    Prompt(pg, BUTTON_PROMPT_L, x + icon / 2, cy, icon);
    x += icon + 4;
    for (const Hint& h : hints) {
        Prompt(pg, h.prompt, x + icon / 2, cy, icon);
        Text(pg, x + icon + 6, cy - 9, size, Alpha(Col(230, 210, 230), a), h.text);
        x += icon + 6 + TextWidth(pg, size, h.text) + 18;
    }
}

bool GameRect(ImVec2& pos, ImVec2& size) {
    ImGuiWindow* gameWindow = ImGui::FindWindowByName("Main Game");
    if (gameWindow == nullptr || gameWindow->Hidden) {
        return false;
    }
    pos = gameWindow->InnerRect.Min;
    size = gameWindow->InnerRect.GetSize();
    return size.x > 0 && size.y > 0;
}

void UseFonts() {
#ifndef QUEST_MENU_PREVIEW
    if (OTRGlobals::Instance != nullptr) {
        gBodyFont = OTRGlobals::Instance->fontQuestBody;
        gTitleFont = OTRGlobals::Instance->fontQuestTitle;
    }
#endif
}

void DrawMenu() {
    float dt = ImGui::GetIO().DeltaTime;
    sTime += dt;
    sAnim = std::clamp(sAnim + (sOpen ? dt * 6.0f : -dt * 8.0f), 0.0f, 1.0f);
    if (sAnim <= 0.0f) {
        return;
    }
    ImVec2 pos, size;
    if (!GameRect(pos, size)) {
        return;
    }
    float a = Ease(sAnim);
    float k = std::min(size.x / kPageW, size.y / kPageH) * (0.93f + 0.06f * a);
    ImVec2 origin(pos.x + (size.x - kPageW * k) / 2, pos.y + (size.y - kPageH * k) / 2);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##QuestMenu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
                     (sOpen ? 0 : ImGuiWindowFlags_NoInputs));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(8, 2, 14, (int)(150 * a)));
    Page pg{ dl, origin, k };

    const auto& quests = Quests((Category)sCategory);
    std::vector<QuestProgress> progress;
    progress.reserve(quests.size());
    for (const Quest& q : quests) {
        progress.push_back(Evaluate(q));
    }

    DrawFrame(pg, a);
    DrawSidebar(pg, a);
    DrawLeftPage(pg, a, progress);
    DrawRightPage(pg, a, progress);
    DrawTitle(pg, a);
    DrawClock(pg, a);
    DrawCloseButton(pg, a);
    DrawFooter(pg, a);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// The "Tela inicial" plaque on the pause menu's bottom bar, right of the name panel, and its question. Laid out in
// the game's 320x240 screen, so it keeps its place at any window size.
bool Tapped(ImVec2 a, ImVec2 b) {
    ImGuiIO& io = ImGui::GetIO();
    return ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MousePos.x >= a.x && io.MousePos.x <= b.x &&
           io.MousePos.y >= a.y && io.MousePos.y <= b.y && ImGui::IsWindowHovered();
}

void DrawPauseBar() {
    bool shown = !sOpen && sAnim <= 0.0f && PauseReady() && SaveLoaded() && !KaleidoScope_QuittingToTitle();
    float dt = ImGui::GetIO().DeltaTime;
    sQuitAnim = std::clamp(sQuitAnim + (sQuitAsk ? dt * 7.0f : -dt * 9.0f), 0.0f, 1.0f);
    if (!shown && sQuitAnim <= 0.0f) {
        return;
    }
    ImVec2 pos, size;
    if (!GameRect(pos, size)) {
        return;
    }
    float k = std::min(size.x / 320.0f, size.y / 240.0f);
    ImVec2 origin(pos.x + (size.x - 320.0f * k) / 2, pos.y + (size.y - 240.0f * k) / 2);
    Page pg{ nullptr, origin, k };

    // The plaque: dark stone like the name panel, with a gold edge that lights up while the shortcut is held.
    ImVec2 a(250, 207), b(314, 229);
    ImGui::SetNextWindowPos(sQuitAsk ? pos : pg.P(a.x - 2, a.y - 2));
    ImGui::SetNextWindowSize(sQuitAsk ? size : ImVec2(pg.S(b.x - a.x + 4), pg.S(b.y - a.y + 4)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##PauseBar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
    pg.dl = ImGui::GetForegroundDrawList();
    if (shown) {
        float hold = sShortcutHeld > 0.15f && !sShortcutLong ? std::min(1.0f, sShortcutHeld / kQuitHold) : 0.0f;
        ImGuiIO& io = ImGui::GetIO();
        bool hover = io.MousePos.x >= pg.P(a.x, a.y).x && io.MousePos.x <= pg.P(b.x, b.y).x &&
                     io.MousePos.y >= pg.P(a.x, a.y).y && io.MousePos.y <= pg.P(b.x, b.y).y;
        pg.dl->AddRectFilled(pg.P(a.x + 1, a.y + 1.5f), pg.P(b.x + 1, b.y + 1.5f), Col(0, 0, 0, 90), pg.S(3));
        Panel(pg, a, b, Col(96, 92, 58, 235), Col(56, 53, 32, 235), 3);
        pg.dl->AddRect(pg.P(a.x, a.y), pg.P(b.x, b.y), Col(28, 26, 14, 255), pg.S(3), 0, pg.S(1.2f));
        pg.dl->AddRect(pg.P(a.x + 1, a.y + 1), pg.P(b.x - 1, b.y - 1),
                       hover || hold > 0 ? Col(244, 204, 98, 255) : Col(150, 142, 96, 200), pg.S(2.5f), 0, pg.S(0.8f));
        if (hold > 0) {
            pg.dl->AddRectFilled(pg.P(a.x + 3, b.y - 3.2f), pg.P(a.x + 3 + (b.x - a.x - 6) * hold, b.y - 2.2f),
                                 Col(255, 214, 110, 255), pg.S(0.5f));
        }
        ImFont* font = TitleFont();
        const char* label = "Tela inicial";
        float tw = TextWidth(pg, 9.5f, label, font);
        float cx = (a.x + b.x) / 2;
        for (int i = 0; i < 8; i++) { // a dark rim around white letters, like the item names
            float ox = std::cos(i * 0.785f) * 0.7f, oy = std::sin(i * 0.785f) * 0.7f;
            Text(pg, cx - tw / 2 + ox, a.y + 1.5f + oy, 9.5f, Col(14, 20, 30, 255), label, 0, font);
        }
        Text(pg, cx - tw / 2, a.y + 1.5f, 9.5f, Col(255, 255, 255, 255), label, 0, font);
        TextCentered(pg, cx, a.y + 12.0f, 6.0f, Col(230, 222, 180, 255), "Segure Select / N", false, BodyFont());
        if (!sQuitAsk && Tapped(pg.P(a.x, a.y), pg.P(b.x, b.y))) {
            AskQuitToTitle();
        }
    }

    // The question, in the middle of the screen.
    if (sQuitAnim > 0.0f) {
        float al = sQuitAnim;
        auto A = [al](ImU32 c) { return (c & 0x00FFFFFF) | ((ImU32)(((c >> 24) & 0xFF) * al) << 24); };
        pg.dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), A(Col(0, 0, 0, 120)));
        float lift = (1.0f - al) * 6.0f;
        ImVec2 qa(80, 82 + lift), qb(240, 150 + lift);
        Panel(pg, qa, qb, A(cParchment), A(cParchmentDark), 5);
        GoldFrame(pg, qa, qb, 5, 2.0f, false);
        TextCentered(pg, 160, qa.y + 9, 12, A(cInk), "Voltar à tela inicial?", false, TitleFont());
        TextCentered(pg, 160, qa.y + 26, 7.5f, A(Col(80, 52, 36)), "O que não foi salvo será perdido.", false,
                     BodyFont());
        const char* names[2] = { "Sim", "Não" };
        for (int i = 0; i < 2; i++) {
            bool yes = i == 0;
            bool sel = sQuitYes == yes;
            ImVec2 ba(yes ? 100 : 170, qa.y + 42), bb(yes ? 150 : 220, qa.y + 60);
            Panel(pg, ba, bb, A(sel ? Col(120, 60, 150) : Col(70, 40, 90)), A(sel ? Col(70, 26, 96) : Col(40, 20, 54)),
                  4);
            if (sel) {
                GoldFrame(pg, ba, bb, 4, 1.2f, true);
            }
            TextCentered(pg, (ba.x + bb.x) / 2, ba.y + 3.5f, 9.5f, A(sel ? Col(255, 244, 210) : Col(200, 186, 210)),
                         names[i], true, TitleFont());
            if (sQuitAsk && Tapped(pg.P(ba.x, ba.y), pg.P(bb.x, bb.y))) {
                AnswerQuit(yes);
            }
        }
        TextCentered(pg, 160, qb.y + 4, 6.5f, A(Col(240, 230, 200)), "A confirma   ·   B volta", true, BodyFont());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace

void RequestOpen() {
    if (sOpen) {
        return;
    }
    if (PauseReady()) {
        Open();
        return;
    }
    if (gPlayState != nullptr && SaveLoaded()) {
        sPendingOpen = 90;
        sStartInjected = false;
    }
}

void Close() {
    if (!sOpen) {
        return;
    }
    sOpen = false;
    sPendingOpen = 0;
    Audio_PlaySfx(NA_SE_SY_CANCEL);
}

bool IsOpen() {
    return sOpen;
}

} // namespace QuestMenu

// The window is only a door: showing it (from the menu) opens the quest menu, and it draws the menu every frame.
void QuestMenuWindow::Draw() {
    QuestMenu::UseFonts();
    if (IsVisible()) {
        ToggleVisibility();
        QuestMenu::RequestOpen();
    }
    if (QuestMenu::ShortcutPressed()) {
        if (QuestMenu::IsOpen()) {
            QuestMenu::Close();
        } else {
            QuestMenu::RequestOpen();
        }
    }
    QuestMenu::DrawMenu();
    QuestMenu::DrawPauseBar();
}

void QuestMenuWindow::InitElement() {
}

void QuestMenuWindow::DrawElement() {
}

void QuestMenuWindow::UpdateElement() {
}

static void RegisterQuestMenu() {
    COND_VB_SHOULD(VB_KALEIDO_UPDATE, true, {
        if (QuestMenu::IsOpen()) {
            *should = false;
        }
    });
    COND_HOOK(OnGameStateMainStart, true, QuestMenu::OnGameFrame);
    // A new game in a slot (Sram_InitSave) starts with no quests seen, whatever the erased file there had.
    COND_HOOK(OnSaveInit, true, [](s16 fileNum) { QuestMenu_ForgetSave(fileNum); });
}

static RegisterShipInitFunc sQuestMenuInit(RegisterQuestMenu, {});

// The QUESTS scroll on the pause menu's quest page, between the quiver and the bomb bag (see z_kaleido_collect.c).
extern "C" bool QuestMenu_PauseEntryShown(void) {
    return CVarGetInteger("gEnhancements.QuestMenu.PauseEntry", 1) != 0;
}

extern "C" void QuestMenu_OpenFromPause(void) {
    QuestMenu::RequestOpen();
}

// "New" means more quests open (not locked) now than when the menu was last opened in this save. The count is
// worked out at most twice a second (kNewsRefreshMs, by the clock: the game's frame count runs at different rates),
// since every quest's checks read the save.
//
// The "seen" count is kept per save slot (gEnhancements.QuestMenu.Seen<slot>). It belongs to the save in that slot,
// not to the slot: it is cleared when the slot's save is erased or a new game is started in it, and copied with the
// save by Copy, so a new file never shows (or hides) news from the file that was there before.
namespace {

constexpr uint32_t kNewsRefreshMs = 500;

int OpenQuestCount() {
    int count = 0;
    for (int c = 0; c < (int)QuestMenu::Category::Count; c++) {
        for (const QuestMenu::Quest& q : QuestMenu::Quests((QuestMenu::Category)c)) {
            count += QuestMenu::Evaluate(q).status != QuestMenu::Status::Locked ? 1 : 0;
        }
    }
    return count;
}

std::string SeenCVar(int fileNum) {
    return "gEnhancements.QuestMenu.Seen" + std::to_string(fileNum);
}

std::string SeenCVar() {
    return SeenCVar((int)gSaveContext.fileNum);
}

int sNewsCount = -1;
int sNewsFile = -1; // the save slot sNewsCount was counted for
uint32_t sNewsTicks = 0;

void SaveSeenCVars() {
#ifndef QUEST_MENU_PREVIEW
    Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
#endif
}

} // namespace

extern "C" bool QuestMenu_HasNews(void) {
    if (!QuestMenu::SaveLoaded()) {
        return false;
    }
    uint32_t now = SDL_GetTicks();
    if (sNewsCount < 0 || sNewsFile != (int)gSaveContext.fileNum || now - sNewsTicks >= kNewsRefreshMs) {
        sNewsCount = OpenQuestCount();
        sNewsFile = (int)gSaveContext.fileNum;
        sNewsTicks = now;
    }
    std::string cvar = SeenCVar();
    int seen = CVarGetInteger(cvar.c_str(), -1);
    if (seen < 0) {
        // First look at this save: what is open now is not news.
        CVarSetInteger(cvar.c_str(), sNewsCount);
        SaveSeenCVars();
        return false;
    }
    return sNewsCount > seen;
}

// The save in `fileNum` was erased or replaced by a new game: its quest menu state goes with it.
extern "C" void QuestMenu_ForgetSave(int fileNum) {
    if (fileNum < 0) {
        return;
    }
    CVarClear(SeenCVar(fileNum).c_str());
    if (sNewsFile == fileNum) {
        sNewsCount = -1;
        sNewsFile = -1;
    }
    SaveSeenCVars();
}

// The save in `from` was copied into `to`: the copy has the same quests seen.
extern "C" void QuestMenu_CopySave(int from, int to) {
    if (from < 0 || to < 0 || from == to) {
        return;
    }
    int seen = CVarGetInteger(SeenCVar(from).c_str(), -1);
    if (seen < 0) {
        CVarClear(SeenCVar(to).c_str());
    } else {
        CVarSetInteger(SeenCVar(to).c_str(), seen);
    }
    if (sNewsFile == to) {
        sNewsCount = -1;
        sNewsFile = -1;
    }
    SaveSeenCVars();
}

namespace QuestMenu {
void MarkQuestsSeen() {
    if (SaveLoaded()) {
        sNewsCount = OpenQuestCount();
        sNewsFile = (int)gSaveContext.fileNum;
        sNewsTicks = SDL_GetTicks();
        CVarSetInteger(SeenCVar().c_str(), sNewsCount);
        SaveSeenCVars();
    }
}
} // namespace QuestMenu
