#include "SetupScreen.h"

#if !defined(__ANDROID__) && !defined(__SWITCH__) && !defined(__WIIU__)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <functional>
#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_opengl3_loader.h>
#include <imgui_impl_sdl2.h>
#include <stb_image.h>
#include <zip.h>
#include <ship/Context.h>

namespace SetupScreen {

namespace {

constexpr float kW = 1280.0f, kH = 720.0f; // layout units; the window scales them

struct Tex {
    GLuint id = 0;
    int w = 0, h = 0;
};

SDL_Window* sWindow = nullptr;
SDL_GLContext sGl = nullptr;
ImGuiContext* sImGui = nullptr;
ImFont* sTitleFont = nullptr;
ImFont* sBodyFont = nullptr;
Tex sBackground, sMoon;
bool sFailed = false;
bool sAllowed = false;
bool sClosed = false;
float sTime = 0.0f;
std::vector<SDL_GameController*> sPads;

// A file from the port's own archive (2ship.o2r), which is beside the game before the ROM is extracted.
std::vector<unsigned char> ReadFromPortArchive(const char* name) {
    std::vector<unsigned char> data;
    std::string path = Ship::Context::GetPathRelativeToAppBundle("2ship.o2r");
    int err = 0;
    zip_t* zip = zip_open(path.c_str(), ZIP_RDONLY, &err);
    if (zip == nullptr) {
        return data;
    }
    zip_stat_t st;
    if (zip_stat(zip, name, 0, &st) == 0 && (st.valid & ZIP_STAT_SIZE)) {
        zip_file_t* f = zip_fopen(zip, name, 0);
        if (f != nullptr) {
            data.resize(st.size);
            zip_int64_t got = zip_fread(f, data.data(), st.size);
            if (got != (zip_int64_t)st.size) {
                data.clear();
            }
            zip_fclose(f);
        }
    }
    zip_close(zip);
    return data;
}

Tex LoadTexture(const char* name) {
    Tex t;
    std::vector<unsigned char> file = ReadFromPortArchive(name);
    if (file.empty()) {
        return t;
    }
    int n = 0;
    unsigned char* px = stbi_load_from_memory(file.data(), (int)file.size(), &t.w, &t.h, &n, 4);
    if (px == nullptr) {
        return t;
    }
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return t;
}

ImFont* LoadFont(const char* name, float size) {
    std::vector<unsigned char> file = ReadFromPortArchive(name);
    if (file.empty()) {
        return nullptr;
    }
    void* copy = IM_ALLOC(file.size()); // the atlas owns and frees it
    memcpy(copy, file.data(), file.size());
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(copy, (int)file.size(), size, &cfg,
                                                      ImGui::GetIO().Fonts->GetGlyphRangesDefault());
}

ImU32 Col(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

const ImU32 cGold = Col(244, 204, 98);
const ImU32 cGoldBright = Col(255, 232, 150);
const ImU32 cGoldDark = Col(150, 105, 40);
const ImU32 cText = Col(240, 228, 204);
const ImU32 cDim = Col(196, 178, 206);

// The layout: kW x kH units, centered in the window and scaled to fit.
struct View {
    ImDrawList* dl;
    ImVec2 origin;
    float k;
    ImVec2 P(float x, float y) const {
        return ImVec2(origin.x + x * k, origin.y + y * k);
    }
    float S(float v) const {
        return v * k;
    }
};

View MakeView() {
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float k = std::min(size.x / kW, size.y / kH);
    return View{ ImGui::GetBackgroundDrawList(), ImVec2((size.x - kW * k) / 2, (size.y - kH * k) / 2), k };
}

void TextCentered(const View& v, ImFont* font, float size, float cx, float y, ImU32 col, const char* text,
                  float wrap = 0.0f) {
    if (font == nullptr) {
        font = ImGui::GetFont();
    }
    ImVec2 ts = font->CalcTextSizeA(v.S(size), FLT_MAX, wrap > 0 ? v.S(wrap) : 0.0f, text);
    if (wrap > 0) {
        // Centre each wrapped line.
        const char* s = text;
        const char* end = text + strlen(text);
        float yy = v.P(0, y).y;
        while (s < end) {
            const char* lineEnd = font->CalcWordWrapPositionA(v.S(size) / font->FontSize, s, end, v.S(wrap));
            if (lineEnd == s) {
                lineEnd++;
            }
            const char* nl = (const char*)memchr(s, '\n', lineEnd - s);
            if (nl != nullptr) {
                lineEnd = nl;
            }
            ImVec2 lw = font->CalcTextSizeA(v.S(size), FLT_MAX, 0.0f, s, lineEnd);
            v.dl->AddText(font, v.S(size), ImVec2(v.P(cx, 0).x - lw.x / 2 + v.S(1.5f), yy + v.S(2)), Col(0, 0, 0, 150),
                          s, lineEnd);
            v.dl->AddText(font, v.S(size), ImVec2(v.P(cx, 0).x - lw.x / 2, yy), col, s, lineEnd);
            yy += v.S(size) * 1.2f;
            s = lineEnd;
            while (s < end && (*s == ' ' || *s == '\n')) {
                s++;
            }
        }
        return;
    }
    ImVec2 p(v.P(cx, y).x - ts.x / 2, v.P(cx, y).y);
    v.dl->AddText(font, v.S(size), ImVec2(p.x + v.S(1.5f), p.y + v.S(2)), Col(0, 0, 0, 150), text);
    v.dl->AddText(font, v.S(size), p, col, text);
}

float WrappedHeight(const View& v, ImFont* font, float size, float wrap, const char* text) {
    if (font == nullptr) {
        font = ImGui::GetFont();
    }
    // Same line breaking as TextCentered, counted in lines.
    const char* s = text;
    const char* end = text + strlen(text);
    int lines = 0;
    while (s < end) {
        const char* lineEnd = font->CalcWordWrapPositionA(v.S(size) / font->FontSize, s, end, v.S(wrap));
        if (lineEnd == s) {
            lineEnd++;
        }
        const char* nl = (const char*)memchr(s, '\n', lineEnd - s);
        if (nl != nullptr) {
            lineEnd = nl;
        }
        lines++;
        s = lineEnd;
        if (s < end && *s == '\n' && s + 1 < end && s[1] == '\n') {
            lines++; // a blank line between paragraphs
        }
        while (s < end && (*s == ' ' || *s == '\n')) {
            s++;
        }
    }
    return lines * size * 1.2f;
}

void GoldFrame(const View& v, ImVec2 a, ImVec2 b, float r, float w, bool glow) {
    if (glow) {
        for (int i = 5; i >= 1; i--) {
            v.dl->AddRect(v.P(a.x - i * 2, a.y - i * 2), v.P(b.x + i * 2, b.y + i * 2), Col(255, 210, 90, 20 * (6 - i)),
                          v.S(r + i * 2), 0, v.S(3));
        }
    }
    v.dl->AddRect(v.P(a.x, a.y), v.P(b.x, b.y), Col(40, 20, 10, 230), v.S(r), 0, v.S(w + 2));
    v.dl->AddRect(v.P(a.x, a.y), v.P(b.x, b.y), cGold, v.S(r), 0, v.S(w));
    v.dl->AddRect(v.P(a.x + w * 0.5f, a.y + w * 0.5f), v.P(b.x - w * 0.5f, b.y - w * 0.5f), Col(255, 240, 180, 110),
                  v.S(r), 0, v.S(1));
}

void Panel(const View& v, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom, float r) {
    v.dl->AddRectFilled(v.P(a.x, a.y), v.P(b.x, b.y), bottom, v.S(r));
    v.dl->AddRectFilledMultiColor(v.P(a.x + r * 0.3f, a.y + r * 0.3f), v.P(b.x - r * 0.3f, b.y - r), top, top, bottom,
                                  bottom);
}

void Diamond(const View& v, float x, float y, float s, ImU32 col) {
    v.dl->AddQuadFilled(v.P(x, y - s), v.P(x + s, y), v.P(x, y + s), v.P(x - s, y), col);
}

// The title menu's scene: its background, darkened, and the moon over it, slowly drifting.
void DrawScene(const View& v) {
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = v.dl;
    dl->AddRectFilled(ImVec2(0, 0), size, Col(10, 6, 18));
    if (sBackground.id != 0) {
        float s = std::max(size.x / sBackground.w, size.y / sBackground.h);
        float w = sBackground.w * s, h = sBackground.h * s;
        ImVec2 p((size.x - w) / 2, (size.y - h) / 2);
        dl->AddImage((ImTextureID)(intptr_t)sBackground.id, p, ImVec2(p.x + w, p.y + h));
    }
    dl->AddRectFilled(ImVec2(0, 0), size, Col(8, 4, 16, 120));
    if (sMoon.id != 0) {
        float mh = kH * 0.62f, mw = mh * sMoon.w / sMoon.h;
        float bob = std::sin(sTime * 0.6f) * 4.0f;
        ImVec2 a = v.P(kW - mw * 0.82f, -mh * 0.18f + bob);
        dl->AddImage((ImTextureID)(intptr_t)sMoon.id, a, ImVec2(a.x + v.S(mw), a.y + v.S(mh)), ImVec2(0, 0),
                     ImVec2(1, 1), Col(255, 255, 255, 235));
    }
    // A soft vignette so the panel reads well over any part of the picture.
    ImU32 clear = Col(0, 0, 0, 0), dark = Col(0, 0, 0, 170);
    dl->AddRectFilledMultiColor(ImVec2(0, size.y * 0.55f), size, clear, clear, dark, dark);
    TextCentered(v, sTitleFont, 30, kW / 2, 28, cGold, "MAJORA'S MASK  ·  DEFINITIVE EDITION");
}

void DrawFrame(const std::string& title, const std::string& text, float panelH,
               const std::function<void(const View&, float top, float bottom)>& content) {
    View v = MakeView();
    DrawScene(v);
    float w = 820, x0 = (kW - w) / 2, x1 = x0 + w;
    // Below the game logo of the background picture, while there is room.
    float y0 = std::clamp(kH * 0.62f - panelH / 2, 60.0f, std::max(60.0f, kH - 24 - panelH)), y1 = y0 + panelH;
    v.dl->AddRectFilled(v.P(x0 + 6, y0 + 10), v.P(x1 + 6, y1 + 10), Col(0, 0, 0, 110), v.S(14));
    Panel(v, ImVec2(x0, y0), ImVec2(x1, y1), Col(66, 30, 96, 245), Col(26, 10, 40, 245), 14);
    GoldFrame(v, ImVec2(x0, y0), ImVec2(x1, y1), 14, 3, false);
    for (auto [x, y] : { std::pair{ x0 + 18, y0 + 18 }, { x1 - 18, y0 + 18 }, { x0 + 18, y1 - 18 }, { x1 - 18, y1 - 18 } }) {
        Diamond(v, x, y, 6, cGold);
    }
    TextCentered(v, sTitleFont, 44, kW / 2, y0 + 26, cGoldBright, title.c_str());
    v.dl->AddLine(v.P(kW / 2 - 200, y0 + 84), v.P(kW / 2 + 200, y0 + 84), Col(244, 204, 98, 150), v.S(1.5f));
    Diamond(v, kW / 2, y0 + 84, 5, cGold);
    TextCentered(v, sBodyFont, 26, kW / 2, y0 + 104, cText, text.c_str(), w - 120);
    float textH = WrappedHeight(v, sBodyFont, 26, w - 120, text.c_str());
    content(v, y0 + 104 + textH + 18, y1);
}

void BeginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    sTime += ImGui::GetIO().DeltaTime;
}

void EndFrame() {
    ImGui::Render();
    int w, h;
    SDL_GL_GetDrawableSize(sWindow, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(sWindow);
}

enum class Key { None, Left, Right, Accept, Back, Close };

// Events of one frame: the keyboard, the controller and the window; the mouse goes through ImGui.
std::vector<Key> PollKeys() {
    std::vector<Key> keys;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL2_ProcessEvent(&e);
        switch (e.type) {
            case SDL_QUIT:
                keys.push_back(Key::Close);
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                    keys.push_back(Key::Close);
                }
                break;
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                    case SDLK_LEFT:
                    case SDLK_UP:
                    case SDLK_a:
                        keys.push_back(Key::Left);
                        break;
                    case SDLK_RIGHT:
                    case SDLK_DOWN:
                    case SDLK_d:
                    case SDLK_TAB:
                        keys.push_back(Key::Right);
                        break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                    case SDLK_SPACE:
                        keys.push_back(Key::Accept);
                        break;
                    case SDLK_ESCAPE:
                    case SDLK_BACKSPACE:
                        keys.push_back(Key::Back);
                        break;
                    default:
                        break;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED: {
                SDL_GameController* pad = SDL_GameControllerOpen(e.cdevice.which);
                if (pad != nullptr) {
                    sPads.push_back(pad);
                }
                break;
            }
            case SDL_CONTROLLERBUTTONDOWN:
                switch (e.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:
                        keys.push_back(Key::Left);
                        break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                        keys.push_back(Key::Right);
                        break;
                    case SDL_CONTROLLER_BUTTON_A:
                    case SDL_CONTROLLER_BUTTON_START:
                        keys.push_back(Key::Accept);
                        break;
                    case SDL_CONTROLLER_BUTTON_B:
                        keys.push_back(Key::Back);
                        break;
                    default:
                        break;
                }
                break;
            case SDL_CONTROLLERAXISMOTION: {
                static int sLastDir = 0;
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                    int dir = e.caxis.value < -16000 ? -1 : e.caxis.value > 16000 ? 1 : 0;
                    if (dir != 0 && dir != sLastDir) {
                        keys.push_back(dir < 0 ? Key::Left : Key::Right);
                    }
                    sLastDir = dir;
                }
                break;
            }
            default:
                break;
        }
    }
    return keys;
}

} // namespace

void Allow(bool allowed) {
    sAllowed = allowed;
}

bool Ensure() {
    if (sWindow != nullptr) {
        return true;
    }
    if (sFailed || !sAllowed) {
        return false;
    }
    sFailed = true; // until everything below works
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    sWindow = SDL_CreateWindow("Majora's Mask Definitive Edition", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (sWindow == nullptr) {
        return false;
    }
    sGl = SDL_GL_CreateContext(sWindow);
    if (sGl == nullptr) {
        SDL_DestroyWindow(sWindow);
        sWindow = nullptr;
        return false;
    }
    SDL_GL_MakeCurrent(sWindow, sGl);
    SDL_GL_SetSwapInterval(1);

    ImGuiContext* previous = ImGui::GetCurrentContext();
    sImGui = ImGui::CreateContext();
    ImGui::SetCurrentContext(sImGui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui_ImplSDL2_InitForOpenGL(sWindow, sGl);
    ImGui_ImplOpenGL3_Init("#version 150");
    (void)previous;

    // The title menu's fonts and pictures, from the port's archive; ImGui's own font when they are missing.
    sBodyFont = LoadFont("fonts/Alegreya-Regular.ttf", 52.0f);
    sTitleFont = LoadFont("fonts/Alegreya-Bold.ttf", 72.0f);
    if (sBodyFont == nullptr) {
        sBodyFont = io.Fonts->AddFontDefault();
    }
    if (sTitleFont == nullptr) {
        sTitleFont = sBodyFont;
    }
    sBackground = LoadTexture("overlays/titlemenu/background.jpg");
    sMoon = LoadTexture("overlays/titlemenu/moon.png");
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* pad = SDL_GameControllerOpen(i);
            if (pad != nullptr) {
                sPads.push_back(pad);
            }
        }
    }
    sFailed = false;
    return true;
}

bool Active() {
    return sWindow != nullptr;
}

int Ask(const std::string& title, const std::string& text, const std::vector<std::string>& buttons, int def,
        int cancel) {
    if (!Ensure() || buttons.empty()) {
        return cancel;
    }
    SDL_RaiseWindow(sWindow);
    int n = (int)buttons.size();
    int sel = std::clamp(def, 0, n - 1);
    int chosen = -2;
    float appear = 0.0f;
    while (chosen == -2) {
        for (Key key : PollKeys()) {
            switch (key) {
                case Key::Left:
                    sel = (sel + n - 1) % n;
                    break;
                case Key::Right:
                    sel = (sel + 1) % n;
                    break;
                case Key::Accept:
                    chosen = sel;
                    break;
                case Key::Back:
                    if (cancel >= 0) {
                        chosen = cancel;
                    }
                    break;
                case Key::Close:
                    chosen = cancel >= 0 ? cancel : n - 1;
                    break;
                default:
                    break;
            }
        }
        BeginFrame();
        appear = std::min(1.0f, appear + ImGui::GetIO().DeltaTime * 5.0f);
        float bw = std::min(250.0f, (760.0f - (n - 1) * 20.0f) / n), bh = 58;
        View probe = MakeView();
        float textH = WrappedHeight(probe, sBodyFont, 26, 700, text.c_str());
        float panelH = std::max(300.0f, 104 + textH + 18 + bh + 70);
        DrawFrame(title, text, panelH, [&](const View& v, float top, float bottom) {
            float total = n * bw + (n - 1) * 20.0f;
            float x = kW / 2 - total / 2, y = bottom - bh - 44;
            ImVec2 mouse = ImGui::GetIO().MousePos;
            for (int i = 0; i < n; i++) {
                ImVec2 a(x + i * (bw + 20), y), b(a.x + bw, y + bh);
                ImVec2 pa = v.P(a.x, a.y), pb = v.P(b.x, b.y);
                bool hover = mouse.x >= pa.x && mouse.x <= pb.x && mouse.y >= pa.y && mouse.y <= pb.y;
                if (hover && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) {
                    sel = i;
                }
                if (hover && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    chosen = i;
                }
                bool on = sel == i;
                float pulse = on ? 0.5f + 0.5f * std::sin(sTime * 4.0f) : 0.0f;
                Panel(v, a, b, on ? Col(128, 66, 160) : Col(74, 40, 96), on ? Col(76, 28, 104) : Col(42, 20, 58), 10);
                GoldFrame(v, a, b, 10, on ? 2.5f : 1.5f, on);
                if (on) {
                    v.dl->AddRect(v.P(a.x - 3, a.y - 3), v.P(b.x + 3, b.y + 3), Col(255, 226, 140, (int)(90 * pulse)),
                                  v.S(12), 0, v.S(2));
                }
                TextCentered(v, sTitleFont, 30, (a.x + b.x) / 2, a.y + 12, on ? Col(255, 246, 214) : cDim,
                             buttons[i].c_str());
            }
            TextCentered(v, sBodyFont, 19, kW / 2, bottom - 30, Col(200, 184, 150),
                         "Setas ou D-pad escolhem  ·  Enter ou A confirma  ·  Esc ou B volta");
        });
        EndFrame();
    }
    return chosen;
}

bool Progress(const std::string& title, const std::string& text, float fraction) {
    if (!Ensure()) {
        return true;
    }
    for (Key key : PollKeys()) {
        if (key == Key::Close) {
            sClosed = true;
        }
    }
    BeginFrame();
    View probe = MakeView();
    float textH = WrappedHeight(probe, sBodyFont, 26, 700, text.c_str());
    float panelH = std::max(300.0f, 104 + textH + 18 + 130);
    DrawFrame(title, text, panelH, [&](const View& v, float top, float bottom) {
        float x0 = kW / 2 - 320, x1 = kW / 2 + 320, y = bottom - 112;
        v.dl->AddRectFilled(v.P(x0, y), v.P(x1, y + 30), Col(16, 6, 24, 255), v.S(15));
        if (fraction >= 0.0f) {
            float f = std::clamp(fraction, 0.0f, 1.0f);
            if (f > 0.0f) {
                v.dl->AddRectFilledMultiColor(v.P(x0 + 3, y + 3), v.P(x0 + 3 + (x1 - x0 - 6) * f, y + 27), cGoldBright,
                                              cGold, cGoldDark, cGoldDark);
            }
            // A light sweeping along the filled part.
            float sweep = std::fmod(sTime * 0.6f, 1.0f) * f;
            float sx = x0 + 3 + (x1 - x0 - 6) * sweep;
            v.dl->AddRectFilled(v.P(std::max(x0 + 3, sx - 30), y + 3), v.P(sx, y + 27), Col(255, 255, 230, 60));
            char pct[16];
            snprintf(pct, sizeof(pct), "%d%%", (int)std::lround(f * 100));
            TextCentered(v, sTitleFont, 30, kW / 2, y + 40, cGoldBright, pct);
        } else {
            // Not known yet: a gold piece going back and forth.
            float t = 0.5f + 0.5f * std::sin(sTime * 2.4f);
            float px = x0 + 3 + (x1 - x0 - 126) * t;
            v.dl->AddRectFilled(v.P(px, y + 3), v.P(px + 120, y + 27), cGold, v.S(12));
            TextCentered(v, sBodyFont, 24, kW / 2, y + 42, cDim, "Preparando...");
        }
        GoldFrame(v, ImVec2(x0, y), ImVec2(x1, y + 30), 15, 2, false);
        TextCentered(v, sBodyFont, 19, kW / 2, bottom - 30, Col(200, 184, 150),
                     "Não feche o jogo até terminar.");
    });
    EndFrame();
    return !sClosed;
}

void Shutdown() {
    sAllowed = false;
    if (sWindow == nullptr) {
        return;
    }
    if (sBackground.id != 0) {
        glDeleteTextures(1, &sBackground.id);
    }
    if (sMoon.id != 0) {
        glDeleteTextures(1, &sMoon.id);
    }
    sBackground = sMoon = Tex();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(sImGui);
    ImGui::SetCurrentContext(nullptr);
    sImGui = nullptr;
    sTitleFont = sBodyFont = nullptr;
    for (SDL_GameController* pad : sPads) {
        SDL_GameControllerClose(pad);
    }
    sPads.clear();
    SDL_GL_DeleteContext(sGl);
    SDL_DestroyWindow(sWindow);
    sGl = nullptr;
    sWindow = nullptr;
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

} // namespace SetupScreen

#elif defined(__ANDROID__)

// Android: the screen is drawn in Java (SetupOverlay.java), over the app; these calls go to MainActivity.
#include <algorithm>
#include <SDL2/SDL.h>
#include <jni.h>

namespace SetupScreen {

namespace {

bool sAllowed = false;
bool sShown = false;

// MainActivity, looked up on the calling thread (the game's, which can see the app's classes).
jclass ActivityClass(JNIEnv* env) {
    jclass c = env->FindClass("com/dishii/mm/MainActivity");
    if (c == nullptr) {
        env->ExceptionClear();
    }
    return c;
}

jmethodID StaticMethod(JNIEnv* env, jclass c, const char* name, const char* sig) {
    jmethodID m = env->GetStaticMethodID(c, name, sig);
    if (m == nullptr) {
        env->ExceptionClear();
    }
    return m;
}

constexpr const char* kAskSig = "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;II)I";

} // namespace

void Allow(bool allowed) {
    sAllowed = allowed;
}

bool Ensure() {
    if (!sAllowed) {
        return false;
    }
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jclass c = env != nullptr ? ActivityClass(env) : nullptr;
    if (c == nullptr) {
        return false;
    }
    bool ok = StaticMethod(env, c, "setupAsk", kAskSig) != nullptr;
    env->DeleteLocalRef(c);
    return ok;
}

int Ask(const std::string& title, const std::string& text, const std::vector<std::string>& buttons, int def,
        int cancel) {
    if (!Ensure() || buttons.empty()) {
        return cancel;
    }
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jclass c = ActivityClass(env);
    jmethodID m = StaticMethod(env, c, "setupAsk", kAskSig);
    jclass stringClass = env->FindClass("java/lang/String");
    int n = (int)std::min<size_t>(buttons.size(), 4);
    jobjectArray array = env->NewObjectArray(n, stringClass, nullptr);
    for (int i = 0; i < n; i++) {
        jstring b = env->NewStringUTF(buttons[i].c_str());
        env->SetObjectArrayElement(array, i, b);
        env->DeleteLocalRef(b);
    }
    jstring jt = env->NewStringUTF(title.c_str());
    jstring jx = env->NewStringUTF(text.c_str());
    jint answer = env->CallStaticIntMethod(c, m, jt, jx, array, (jint)def, (jint)cancel);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        answer = cancel;
    }
    env->DeleteLocalRef(jt);
    env->DeleteLocalRef(jx);
    env->DeleteLocalRef(array);
    env->DeleteLocalRef(stringClass);
    env->DeleteLocalRef(c);
    sShown = true;
    return answer;
}

bool Progress(const std::string& title, const std::string& text, float fraction) {
    if (!Ensure()) {
        return true;
    }
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jclass c = ActivityClass(env);
    jmethodID m = StaticMethod(env, c, "setupProgress", "(Ljava/lang/String;Ljava/lang/String;F)V");
    if (m != nullptr) {
        jstring jt = env->NewStringUTF(title.c_str());
        jstring jx = env->NewStringUTF(text.c_str());
        env->CallStaticVoidMethod(c, m, jt, jx, (jfloat)fraction);
        env->ExceptionClear();
        env->DeleteLocalRef(jt);
        env->DeleteLocalRef(jx);
    }
    env->DeleteLocalRef(c);
    sShown = true;
    SDL_Delay(100); // the Java screen animates by itself; this only updates what it shows
    return true;
}

void Shutdown() {
    sAllowed = false;
    sShown = false;
    // Always: the opening waits for this when the game files were missing at launch.
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jclass c = env != nullptr ? ActivityClass(env) : nullptr;
    if (c == nullptr) {
        return;
    }
    jmethodID m = StaticMethod(env, c, "setupFinish", "()V");
    if (m != nullptr) {
        env->CallStaticVoidMethod(c, m);
        env->ExceptionClear();
    }
    env->DeleteLocalRef(c);
}

bool Active() {
    return sShown;
}

} // namespace SetupScreen

#else

namespace SetupScreen {
void Allow(bool) {
}
bool Ensure() {
    return false;
}
int Ask(const std::string&, const std::string&, const std::vector<std::string>&, int, int cancel) {
    return cancel;
}
bool Progress(const std::string&, const std::string&, float) {
    return true;
}
void Shutdown() {
}
bool Active() {
    return false;
}
} // namespace SetupScreen

#endif
