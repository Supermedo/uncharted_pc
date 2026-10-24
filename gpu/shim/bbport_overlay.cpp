// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include <miniz.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f, last_present_ms = 0.0f;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    // The widget runs first: as one argument list, v could be read before it changed.
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin("Uncharted — settings  (Insert / L3+R3)", &keep_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  (%.1f ms)", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg);

    ImGui::SeparatorText("Temporal upscaler");
    static const char* upscalers[] = {"Off", "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
                                     "TAA (native anti-aliasing)", "DLSS (NVIDIA RTX)", "XeSS (Intel)"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo("Upscaler", upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                : i == BbSettings::UpscalerXess   ? s.xess_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("— not supported by this GPU");
            }
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.dlss_problem.load(); problem && upscaler == BbSettings::UpscalerDlss) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS: %s", problem);
    }
    if (const char* problem = s.xess_problem.load(); problem && upscaler == BbSettings::UpscalerXess) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "XeSS: %s", problem);
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "FSR 4 unavailable: %s", problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted("The mode selected above is active. FSR 4 can be selected again.");
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint("FSR 4.1.1 in INT8 mode: the model from AMD's 4.1.1 DLL, reproduced in Vulkan "
                 "(output matches the DLL). One model for Native..Performance and a separate one "
                 "for Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (needs the DLLs and Proton).");
        } else {
            Hint("FSR 4 in INT8 mode (the v07 model from the AMD FidelityFX SDK sources). Higher quality "
                 "than FSR 3.1, but a heavier pass. Changing the preset rebuilds the model (a short "
                 "pause). Assets: tools/fetch_fsr4_assets.sh.");
        }
        Checkbox("FSR 4: auto exposure", s.fsr4_auto_exposure);
        Checkbox("FSR 4: invert jitter sign", s.fsr4_invert_jitter);
        Hint("Checks for ghosting: the FSR 4 network normalizes color by exposure and uses it to decide "
             "when to discard previous frames. Changes apply immediately, without a restart.");
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo("Preset", preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(label, sizeof(label), "%s (x%.1f, render %dx%d)",
                          BbSettings::PresetName(i), scale,
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("TAA anti-aliases the scene at the output resolution, without an FSR model or upscaling. "
                           "The saved FSR preset comes back when FSR is selected.");
    }
    ImGui::Text("Active scene render: %d x %d", s.active_render_width.load(),
                s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text("Preset at startup: %s", BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint("With an output other than 1080p the whole game renders at the preset's size (a startup "
                 "patch): fastest on the Steam Deck and weaker GPUs. Preset or output resolution "
                 "changes need a restart. The \"Live resolution changes\" option below allows "
                 "changes without a restart (post-processing then stays at 1080p, slower).");
        } else {
            Hint("BB_RENDER_RES fixes the scene size at startup. Remove that variable to change "
                 "resolution and presets without restarting the game.");
        }
    } else {
        Hint("Native AA: FSR works as anti-aliasing. The other presets lower the scene's render "
             "resolution relative to the output. The UI is drawn at the output resolution. "
             "The preset applies from the next frame, without restarting the game.");
    }
    Checkbox("Sharpening (RCAS)", s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider("Sharpening strength", s.sharpness, 0.0f, 2.0f);
    Hint("Up to 1: the upscaler's own sharpening (RCAS). Above 1 an extra RCAS pass is added. "
         "Ctrl+click the slider to type an exact value.");
    ImGui::EndDisabled();
    Checkbox("Sub-pixel jitter", s.jitter);
    Hint("Each frame the scene shifts by a fraction of a pixel, and the upscaler gathers more "
         "detail from several frames. Without it you only get history-based smoothing.");

    ImGui::SeparatorText("Reactive mask");
    ImGui::BeginDisabled(taa);
    Checkbox("Enable mask", s.reactive);
    Hint("Marks transparent effects (particles, haze) so the upscaler relies less on previous "
         "frames. Fewer trails behind effects, but shimmer returns underneath them.");
    ImGui::BeginDisabled(!s.reactive);
    Slider("Scale", s.reactive_scale, 0.0f, 4.0f);
    Slider("Threshold", s.reactive_threshold, 0.0f, 1.0f);
    Slider("Maximum", s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox("Show mask (debug)", &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox("Character motion vectors", s.object_motion);
    Hint("Exact vectors for animated objects: clothing and weapons break up less in motion. "
         "A static scene gets no extra pass. "
         "The change applies after restarting the game.");
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox("Show motion vectors (debug)", &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint("Red/green: horizontal/vertical motion (8 pixels = full brightness). "
         "Blue: the pixel got an exact object vector, not just camera motion. "
         "A moving object with neither blue nor red/green is treated by the upscaler "
         "as still, which causes trails.");
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText("Frame generation");
    static const char* frame_gens[] = {"Off", "FSR 3 (any GPU)",
                                       "DLSS (NVIDIA RTX 40/50)"};
    const int frame_gen = s.frame_gen;
    if (ImGui::BeginCombo("Frame generation", frame_gens[frame_gen])) {
        for (int i = 0; i < BbSettings::FrameGenCount; ++i) {
            const bool supported = i != BbSettings::FrameGenDlss || s.dlssg_max_multiplier > 1;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(frame_gens[i], i == frame_gen)) {
                Store(s.frame_gen, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("— not supported by this GPU");
            }
        }
        ImGui::EndCombo();
    }
    Hint("Frames between two game frames: 60 FPS on screen becomes 120. Needs Mailbox or "
         "Immediate presentation and a monitor with that refresh rate. Adds a little input "
         "latency. Menus and videos are not generated.");
    const int max_multiplier = s.frame_gen == BbSettings::FrameGenDlss ? s.dlssg_max_multiplier.load() : 2;
    ImGui::BeginDisabled(s.frame_gen == BbSettings::FrameGenOff || max_multiplier <= 2);
    int multiplier = std::min(s.frame_gen_multiplier.load(), max_multiplier);
    if (ImGui::SliderInt("Multiplier", &multiplier, 2, std::max(max_multiplier, 2), "%dx")) {
        Store(s.frame_gen_multiplier, multiplier, true);
    }
    ImGui::EndDisabled();
    if (const char* problem = s.frame_gen_problem.load()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", problem);
    }

    ImGui::SeparatorText("Output resolution");
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    if (ImGui::BeginCombo("Output resolution", outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint("Size of the finished frame and the UI. The preset sets the scene size relative to "
             "the output: 4K Performance = 1920x1080. Applies after restarting the game.");
    } else {
        Hint("The size of the finished frame and UI changes at the next frame boundary. "
             "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
             "Changing it resets FSR's history and may cause a short pause.");
    }
    static const char* live_modes[] = {"Auto (by GPU)", "Off (faster)", "On"};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo("Live resolution changes", live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("On: output resolution and preset change without a restart, but the game's post-processing "
         "stays at 1080p, noticeably slower on the Steam Deck and older GPUs. "
         "Off: everything renders at the preset's size, changes need a restart. Auto turns "
         "it on for strong discrete GPUs. Applies after restarting the game.");
    // No game-effect, cheat or model-detail toggles here: those were Bloodborne patches
    // (scripts/patches.py, patches/Bloodborne.xml), which the Uncharted launch never applies.
    bool restart = s.object_motion != s.startup_object_motion ||
                   s.live_resolution != s.startup_live_resolution ||
                   BbSettings::ResolutionNeedsRestart();
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Changes apply after restarting the game");
        if (ImGui::Button("Apply and restart the game")) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::SeparatorText("Other");
    Checkbox("FPS counter in the corner", s.show_fps);

    ImGui::Spacing();
    if (ImGui::Button("Close")) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Settings are saved to bbport.ini");
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text("%.0f FPS  %.1f ms  %s", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4 ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa ? "TAA"
                : s.upscaler == BbSettings::UpscalerDlss ? "DLSS"
                : s.upscaler == BbSettings::UpscalerXess ? "XeSS"
                                                         : "");
    ImGui::End();
}


// ---- Trophy notices (NotifyTrophy): the console's "trophy earned" box, one at a time ----------
struct Toast {
    int grade = 4;
    std::string game, name;
    std::vector<unsigned char> png;
};
std::mutex toast_mutex;
std::deque<Toast> toast_queue;       // guarded by toast_mutex
std::atomic<bool> toast_busy{false}; // something is queued or on screen
bool toast_on = false;
Toast toast_now;
std::chrono::steady_clock::time_point toast_start;
ImTextureData* toast_icon = nullptr; // one 256x256 texture, rewritten for every notice
ImVec2 toast_icon_uv{1.0f, 1.0f};
constexpr float ToastSeconds = 6.0f;
constexpr int ToastIconSize = 256;

// 8-bit RGB/RGBA PNG without interlacing (what the trophy packs hold) to RGBA.
bool DecodePng(const std::vector<unsigned char>& png, std::vector<unsigned char>& rgba, int& width,
               int& height) {
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 33 || std::memcmp(png.data(), signature, 8) != 0) {
        return false;
    }
    const auto be32 = [](const unsigned char* p) {
        return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
    };
    std::vector<unsigned char> idat;
    int color_type = -1;
    for (size_t at = 8; at + 12 <= png.size();) {
        const u32 length = be32(&png[at]);
        const unsigned char* type = &png[at + 4];
        if (length > png.size() || at + 12 + length > png.size()) {
            return false;
        }
        const unsigned char* body = &png[at + 8];
        if (std::memcmp(type, "IHDR", 4) == 0 && length >= 13) {
            width = int(be32(body));
            height = int(be32(body + 4));
            color_type = body[9];
            if (body[8] != 8 || (color_type != 2 && color_type != 6) || body[12] != 0) {
                return false;
            }
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        at += 12 + length;
    }
    if (color_type < 0 || width <= 0 || height <= 0 || width > ToastIconSize ||
        height > ToastIconSize || idat.empty()) {
        return false;
    }
    const int bpp = color_type == 6 ? 4 : 3;
    const size_t stride = size_t(width) * bpp;
    std::vector<unsigned char> raw((stride + 1) * height);
    mz_ulong raw_size = mz_ulong(raw.size());
    if (mz_uncompress(raw.data(), &raw_size, idat.data(), mz_ulong(idat.size())) != MZ_OK ||
        raw_size != raw.size()) {
        return false;
    }
    std::vector<unsigned char> pixels(stride * height);
    for (int y = 0; y < height; ++y) {
        const unsigned char* in = &raw[y * (stride + 1)];
        const int filter = *in++;
        unsigned char* row = &pixels[y * stride];
        const unsigned char* up = y ? row - stride : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(bpp) ? row[x - bpp] : 0;
            const int b = up ? up[x] : 0;
            const int c = (up && x >= size_t(bpp)) ? up[x - bpp] : 0;
            int predictor = 0;
            switch (filter) {
            case 0: break;
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b),
                          pc = std::abs(p - c);
                predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: return false;
            }
            row[x] = static_cast<unsigned char>(in[x] + predictor);
        }
    }
    rgba.assign(size_t(width) * height * 4, 255);
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        std::memcpy(&rgba[i * 4], &pixels[i * bpp], bpp);
    }
    return true;
}

// Puts the notice's icon into the shared texture (the first notice creates it).
void SetToastIcon(const std::vector<unsigned char>& png) {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    const bool ok = !png.empty() && DecodePng(png, rgba, w, h);
    if (!ok && !toast_icon) {
        return;
    }
    const bool created = toast_icon == nullptr;
    if (created) {
        toast_icon = IM_NEW(ImTextureData)();
        toast_icon->Create(ImTextureFormat_RGBA32, ToastIconSize, ToastIconSize);
        toast_icon->UseColors = true;
    }
    std::memset(toast_icon->Pixels, 0, size_t(ToastIconSize) * ToastIconSize * 4);
    if (ok) {
        for (int y = 0; y < h; ++y) {
            std::memcpy(toast_icon->GetPixelsAt(0, y), &rgba[size_t(y) * w * 4], size_t(w) * 4);
        }
        toast_icon_uv = ImVec2(float(w) / ToastIconSize, float(h) / ToastIconSize);
    } else {
        toast_icon_uv = ImVec2(0.0f, 0.0f); // nothing to show: an empty square
    }
    if (created) {
        ImGui::RegisterUserTexture(toast_icon);
    } else {
        toast_icon->UpdateRect = {0, 0, ToastIconSize, ToastIconSize};
        toast_icon->Updates.push_back(toast_icon->UpdateRect);
        toast_icon->Status = ImTextureStatus_WantUpdates;
    }
}

void TrophyToast() {
    const auto now = std::chrono::steady_clock::now();
    if (!toast_on) {
        std::scoped_lock lock{toast_mutex};
        if (toast_queue.empty()) {
            toast_busy = false;
            return;
        }
        toast_now = std::move(toast_queue.front());
        toast_queue.pop_front();
        toast_on = true;
        toast_start = now;
        SetToastIcon(toast_now.png);
        toast_now.png = {};
    }
    const float t = std::chrono::duration<float>(now - toast_start).count();
    if (t >= ToastSeconds) {
        toast_on = false;
        return;
    }
    const float s = base_scale;
    const float slide = std::min(t / 0.35f, 1.0f);
    const float eased = 1.0f - (1.0f - slide) * (1.0f - slide);
    const float alpha = std::min(1.0f, (ToastSeconds - t) / 0.5f);
    static const ImVec4 colors[] = {ImVec4(0.62f, 0.82f, 0.96f, 1.0f), ImVec4(0.96f, 0.78f, 0.28f, 1.0f),
                                    ImVec4(0.80f, 0.83f, 0.88f, 1.0f), ImVec4(0.82f, 0.52f, 0.24f, 1.0f)};
    static const char* const names[] = {"Platinum trophy earned", "Gold trophy earned",
                                        "Silver trophy earned", "Bronze trophy earned"};
    const int grade = std::clamp(toast_now.grade, 1, 4) - 1;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + (18.0f - (1.0f - eased) * 110.0f) * s),
        ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f * s, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * s, 12.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f * s);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.08f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, colors[grade]);
    ImGui::Begin("##trophy_toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (toast_icon) {
        ImGui::Image(toast_icon->GetTexRef(), ImVec2(64.0f * s, 64.0f * s), ImVec2(0.0f, 0.0f),
                     toast_icon_uv);
        ImGui::SameLine(0.0f, 12.0f * s);
    }
    ImGui::BeginGroup();
    ImGui::TextColored(colors[grade], "%s", names[grade]);
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted(toast_now.name.c_str());
    ImGui::SetWindowFontScale(1.0f);
    if (!toast_now.game.empty()) {
        ImGui::TextDisabled("%s", toast_now.game.c_str());
    }
    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized &&
           (menu_open || text_entry_active || BbSettings::Get().show_fps || toast_busy);
}

void NotifyTrophy(int grade, std::string game, std::string name, std::vector<unsigned char> png) {
    std::scoped_lock lock{toast_mutex};
    if (toast_queue.size() < 32) {
        toast_queue.push_back({grade, std::move(game), std::move(name), std::move(png)});
        toast_busy = true;
    }
}

bool CapturesInput() {
    return menu_open || text_entry_active;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

// Called for every presented frame: the interval for the FPS readout and BB_FPS_LOG.
void NotePresent() {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    last_present_ms = ms;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    // BB_FPS_LOG=1: presents per second and the slowest frame, every 5 seconds (test runs).
    static const bool fps_log = [] {
        const char* env = std::getenv("BB_FPS_LOG");
        return env && env[0] == '1';
    }();
    if (fps_log) {
        static auto window_start = now;
        static int frames = 0;
        static float worst = 0.0f;
        ++frames;
        worst = std::max(worst, ms < 1000.0f ? ms : 0.0f);
        const float elapsed = std::chrono::duration<float>(now - window_start).count();
        if (elapsed >= 5.0f) {
            std::printf("Frame rate: %.1f FPS, slowest frame %.1f ms\n", frames / elapsed, worst);
            window_start = now;
            frames = 0;
            worst = 0.0f;
        }
    }
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    const float ms = last_present_ms;
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    TrophyToast();
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
