#pragma once

// The screen that prepares the game files before the opening, when the game's mm.o2r is missing: it asks for the
// Majora's Mask ROM, checks it and shows the extraction's progress, in the look of the Definitive Edition's title
// menu (the menu's background and moon, its fonts, purple panels with gold edges). It runs before the game's own
// window exists, in a window of its own, and closes before the game starts.
//
// PC only (SDL + OpenGL + ImGui, with a context of its own). Android draws the same screen in Java
// (MainActivity / SetupView), and when the window cannot be made the extractor falls back to message boxes.

#include <string>
#include <vector>

namespace SetupScreen {

// The screen is only used while the game files are being prepared (BenPort.cpp InitOTR), never once the game's own
// window and menus run: errors from then on keep the system's message box.
void Allow(bool allowed);

// Opens the window on first use. False when it cannot or is not allowed now (then the caller uses a message box).
bool Ensure();

// Shows a question with up to four buttons and waits for one. Returns the button's index, or `cancel` for Esc / B
// or the window's close button.
int Ask(const std::string& title, const std::string& text, const std::vector<std::string>& buttons, int def = 0,
        int cancel = -1);

// Draws one frame of the progress screen: fraction 0..1, or below 0 while the amount of work is not known yet.
// Returns false when the window was closed.
bool Progress(const std::string& title, const std::string& text, float fraction);

// Closes the window (before the game's window opens) and ends the preparation (Allow(false)).
void Shutdown();

bool Active();

} // namespace SetupScreen
