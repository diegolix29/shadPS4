// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <map>
#include <string>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <toml.hpp>
#include "common/elf_info.h"

#include "common/assert.h"
#include "common/config.h"
#include "core/libraries/pad/pad.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"

#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif
#include <input/input_handler.h>

namespace Frontend {

using Libraries::Pad::OrbisPadButtonDataOffset;
std::map<std::string, u32> outputkey_map = {
    {"dpad_down", OrbisPadButtonDataOffset::Down},
    {"dpad_up", OrbisPadButtonDataOffset::Up},
    {"dpad_left", OrbisPadButtonDataOffset::Left},
    {"dpad_right", OrbisPadButtonDataOffset::Right},
    {"cross", OrbisPadButtonDataOffset::Cross},
    {"triangle", OrbisPadButtonDataOffset::Triangle},
    {"square", OrbisPadButtonDataOffset::Square},
    {"circle", OrbisPadButtonDataOffset::Circle},
    {"options", OrbisPadButtonDataOffset::Options},
    {"L1", OrbisPadButtonDataOffset::L1},
    {"R1", OrbisPadButtonDataOffset::R1},
    {"L3", OrbisPadButtonDataOffset::L3},
    {"R3", OrbisPadButtonDataOffset::R3},
    {"L2", OrbisPadButtonDataOffset::L2},
    {"R2", OrbisPadButtonDataOffset::R2},
    {"lstickup", OrbisPadButtonDataOffset::LeftStickUp},        // = 2,000,001
    {"lstickdown", OrbisPadButtonDataOffset::LeftStickDown},    // = 2000002
    {"lstickleft", OrbisPadButtonDataOffset::LeftStickLeft},    // = 2000003
    {"lstickright", OrbisPadButtonDataOffset::LeftStickRight},  // = 2000004
    {"rstickup", OrbisPadButtonDataOffset::RightStickUp},       // = 2000005
    {"rstickdown", OrbisPadButtonDataOffset::RightStickDown},   // = 2000006
    {"rstickleft", OrbisPadButtonDataOffset::RightStickLeft},   // = 2000007
    {"rstickright", OrbisPadButtonDataOffset::RightStickRight}, // = 2000008
};

static std::string Amap = "cross";
static std::string Ymap = "triangle";
static std::string Xmap = "square";
static std::string Bmap = "circle";
static std::string LBmap = "L1";
static std::string RBmap = "R1";
static std::string dupmap = "dpad_up";
static std::string ddownmap = "dpad_down";
static std::string dleftmap = "dpad_left";
static std::string drightmap = "dpad_right";
static std::string rstickmap = "R3";
static std::string lstickmap = "L3";
static std::string startmap = "options";
static std::string LTmap = "L2";
static std::string RTmap = "R2";
static std::string Lstickupmap = "lstickup";
static std::string Lstickdownmap = "lstickdown";
static std::string Lstickleftmap = "lstickleft";
static std::string Lstickrightmap = "lstickright";
static bool Lstickbuttons = false;
static bool Lstickswap = false;
static bool LstickinvertY = false;
static bool LstickinvertX = false;
static std::string Rstickupmap = "rstickup";
static std::string Rstickdownmap = "rstickdown";
static std::string Rstickleftmap = "rstickleft";
static std::string Rstickrightmap = "rstickright";
static bool Rstickbuttons = false;
static bool Rstickswap = false;
static bool RstickinvertY = false;
static bool RstickinvertX = false;

static Uint32 SDLCALL PollController(void* userdata, SDL_TimerID timer_id, Uint32 interval) {
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    return controller->Poll();
}

WindowSDL::WindowSDL(s32 width_, s32 height_, Input::GameController* controller_,
                     std::string_view window_title)
    : width{width_}, height{height_}, controller{controller_} {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        UNREACHABLE_MSG("Failed to initialize SDL video subsystem: {}", SDL_GetError());
    }
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          std::string(window_title).c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(props, "flags", SDL_WINDOW_VULKAN);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (window == nullptr) {
        UNREACHABLE_MSG("Failed to create window handle: {}", SDL_GetError());
    }

    SDL_SetWindowFullscreen(window, Config::isFullscreenMode());

    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    controller->TryOpenSDLController();

#if defined(SDL_PLATFORM_WIN32)
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#elif defined(SDL_PLATFORM_LINUX)
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        window_info.render_surface = (void*)SDL_GetNumberProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    } else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        window_info.render_surface = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    }
#elif defined(SDL_PLATFORM_MACOS)
    window_info.type = WindowSystemType::Metal;
    window_info.render_surface = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
#endif

    Input::CheckRemapFile();
    // input handler init-s
    Input::ControllerOutput::SetControllerOutputController(controller);
    Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));
    RefreshMappings();
}

WindowSDL::~WindowSDL() = default;

void WindowSDL::WaitEvent() {
    // Called on main thread
    SDL_Event event;

    if (!SDL_WaitEvent(&event)) {
        return;
    }

    if (ImGui::Core::ProcessEvent(&event)) {
        return;
    }

    switch (event.type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        OnResize();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = event.type == SDL_EVENT_WINDOW_EXPOSED;
        OnResize();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_WHEEL_OFF:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        OnKeyboardMouseInput(&event);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        controller->TryOpenSDLController();
        break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        controller->SetTouchpadState(event.gtouchpad.finger,
                                     event.type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP, event.gtouchpad.x,
                                     event.gtouchpad.y);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        OnGamepadEvent(&event);
        break;
    case SDL_EVENT_QUIT:
        is_open = false;
        break;
    default:
        break;
    }
}

void WindowSDL::InitTimers() {
    SDL_AddTimer(100, &PollController, controller);
    SDL_AddTimer(33, Input::MousePolling, (void*)controller);
}

void WindowSDL::RequestKeyboard() {
    if (keyboard_grab == 0) {
        SDL_StartTextInput(window);
    }
    keyboard_grab++;
}

void WindowSDL::ReleaseKeyboard() {
    ASSERT(keyboard_grab > 0);
    keyboard_grab--;
    if (keyboard_grab == 0) {
        SDL_StopTextInput(window);
    }
}

void WindowSDL::OnResize() {
    SDL_GetWindowSizeInPixels(window, &width, &height);
    ImGui::Core::OnResize();
}

Uint32 wheelOffCallback(void* og_event, Uint32 timer_id, Uint32 interval) {
    SDL_Event off_event = *(SDL_Event*)og_event;
    off_event.type = SDL_EVENT_MOUSE_WHEEL_OFF;
    SDL_PushEvent(&off_event);
    delete (SDL_Event*)og_event;
    return 0;
}

void WindowSDL::OnKeyboardMouseInput(const SDL_Event* event) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

    // get the event's id, if it's keyup or keydown
    bool input_down = event->type == SDL_EVENT_KEY_DOWN ||
                      event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                      event->type == SDL_EVENT_MOUSE_WHEEL;
    u32 input_id = Input::InputBinding::GetInputIDFromEvent(*event);

    // Handle window controls outside of the input maps
    if (event->type == SDL_EVENT_KEY_DOWN) {
        // Reparse kbm inputs
        if (input_id == SDLK_F8) {
            Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));
            return;
        }
        // Toggle mouse capture and movement input
        else if (input_id == SDLK_F7) {
            Input::ToggleMouseEnabled();
            SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(),
                                           !SDL_GetWindowRelativeMouseMode(this->GetSDLWindow()));
            return;
        }
        // Toggle fullscreen
        else if (input_id == SDLK_F11) {
            SDL_WindowFlags flag = SDL_GetWindowFlags(window);
            bool is_fullscreen = flag & SDL_WINDOW_FULLSCREEN;
            SDL_SetWindowFullscreen(window, !is_fullscreen);
            return;
        }
        // Trigger rdoc capture
        else if (input_id == SDLK_F12) {
            VideoCore::TriggerCapture();
            return;
        }
    }

    // if it's a wheel event, make a timer that turns it off after a set time
    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        const SDL_Event* copy = new SDL_Event(*event);
        SDL_AddTimer(33, wheelOffCallback, (void*)copy);
    }

    // add/remove it from the list
    bool inputs_changed = Input::UpdatePressedKeys(input_id, input_down);

    // update bindings
    if (inputs_changed) {
        Input::ActivateOutputsFromInputs();
    }
}

void WindowSDL::OnGamepadEvent(const SDL_Event* event) {

    u32 button = 0;
    std::string buttonanalogmap = "default";
    Input::Axis axis = Input::Axis::AxisMax;
    int axisvalue = 0;
    int ax = 0;

    switch (event->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        controller->TryOpenSDLController();
        break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        controller->SetTouchpadState(event->gtouchpad.finger,
                                     event->type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP,
                                     event->gtouchpad.x, event->gtouchpad.y);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        button = sdlGamepadToOrbisButton(event->gbutton.button);
        // if button is mapped to axis, convert to axis inputs, axes are mapped to values 2000001-8
        if (button > 2000000) {
            if (button == OrbisPadButtonDataOffset::LeftStickUp) {
                axis = Input::Axis::LeftY;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += -127;
                } else {
                    axisvalue = 0;
                }
            } else if (button == OrbisPadButtonDataOffset::LeftStickDown) {
                axis = Input::Axis::LeftY;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += 127;
                } else {
                    axisvalue = 0;
                }
            } else if (button == OrbisPadButtonDataOffset::LeftStickLeft) {
                axis = Input::Axis::LeftX;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += -127;
                } else {
                    axisvalue = 0;
                }
            } else if (button == OrbisPadButtonDataOffset::LeftStickRight) {
                axis = Input::Axis::LeftX;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += 127;
                } else {
                    axisvalue = 0;
                }
            } else if (button == OrbisPadButtonDataOffset::RightStickUp) {
                axis = Input::Axis::RightY;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += -127;
                } else {
                    axisvalue = 0;
                }
            } else if (button == OrbisPadButtonDataOffset::RightStickDown) {
                axis = Input::Axis::RightY;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += 127;
                } else {
                    axisvalue = 0;
                }
            } else if (OrbisPadButtonDataOffset::RightStickLeft) {
                axis = Input::Axis::RightX;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += -127;
                } else {
                    axisvalue = 0;
                }
            } else if (OrbisPadButtonDataOffset::RightStickRight) {
                axis = Input::Axis::RightX;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += 127;
                } else {
                    axisvalue = 0;
                }
            }
            ax = Input::GetAxis(-0x80, 0x80, axisvalue);
            controller->Axis(0, axis, ax);
            break;
        } else {
            if (event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                std::string backButtonBehavior = Config::getBackButtonBehavior();
                if (backButtonBehavior != "none") {
                    float x = backButtonBehavior == "left"
                                  ? 0.25f
                                  : (backButtonBehavior == "right" ? 0.75f : 0.5f);
                    // trigger a touchpad event so that the touchpad emulation for back button works
                    controller->SetTouchpadState(0, true, x, 0.5f);
                    controller->CheckButton(0, button,
                                            event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                }
            } else {
                controller->CheckButton(0, button, event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            }
        }
        break;

    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        int negaxisvalue = event->gaxis.value * -1;
        enum Input::Axis OutputLeftTrig;
        enum Input::Axis OutputRightTrig;

        if (LTmap == "R2") {
            OutputLeftTrig = Input::Axis::TriggerRight;
        } else if (LTmap == "L2") {
            OutputLeftTrig = Input::Axis::TriggerLeft;
        } else if (LTmap == "lstickup" || LTmap == "lstickdown") {
            OutputLeftTrig = Input::Axis::LeftY;
        } else if (LTmap == "lstickleft" || LTmap == "lstickright") {
            OutputLeftTrig = Input::Axis::LeftX;
        } else if (LTmap == "rstickup" || LTmap == "rstickdown") {
            OutputLeftTrig = Input::Axis::RightY;
        } else if (LTmap == "rstickleft" || LTmap == "rstickright") {
            OutputLeftTrig = Input::Axis::RightX;
        } else {
            OutputLeftTrig = Input::Axis::AxisMax;
        }

        if (RTmap == "R2") {
            OutputRightTrig = Input::Axis::TriggerRight;
        } else if (RTmap == "L2") {
            OutputRightTrig = Input::Axis::TriggerLeft;
        } else if (RTmap == "lstickup" || RTmap == "lstickdown") {
            OutputRightTrig = Input::Axis::LeftY;
        } else if (RTmap == "lstickleft" || RTmap == "lstickright") {
            OutputRightTrig = Input::Axis::LeftX;
        } else if (RTmap == "rstickup" || RTmap == "rstickdown") {
            OutputRightTrig = Input::Axis::RightY;
        } else if (RTmap == "rstickleft" || RTmap == "rstickright") {
            OutputRightTrig = Input::Axis::RightX;
        } else {
            OutputRightTrig = Input::Axis::AxisMax;
        }

        axis = event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX           ? Input::Axis::LeftX
               : event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY         ? Input::Axis::LeftY
               : event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX        ? Input::Axis::RightX
               : event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTY        ? Input::Axis::RightY
               : event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER  ? OutputLeftTrig
               : event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? OutputRightTrig
                                                                     : Input::Axis::AxisMax;
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            if (LTmap == "L2" || LTmap == "R2") {
                controller->Axis(0, axis, Input::GetAxis(0, 0x8000, event->gaxis.value));
                // if trigger is not mapped to axis, convert to button inputs, axes are mapped to
                // values 2000000+
            } else if (outputkey_map[LTmap] < 2000000) {
                button = outputkey_map[LTmap];
                controller->CheckButton(0, button, event->gaxis.value > 120);
            } else if (LTmap == "lstickup" || LTmap == "lstickleft" || LTmap == "rstickup" ||
                       LTmap == "rstickleft") {
                controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, negaxisvalue));
            } else if (axis != Input::Axis::AxisMax) {
                controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
            }
        }
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            if (RTmap == "L2" || RTmap == "R2") {
                controller->Axis(0, axis, Input::GetAxis(0, 0x8000, event->gaxis.value));
            } else if (outputkey_map[RTmap] < 2000000) {
                button = outputkey_map[RTmap];
                controller->CheckButton(0, button, event->gaxis.value > 120);
            } else if (RTmap == "lstickup" || RTmap == "lstickleft" || RTmap == "rstickup" ||
                       RTmap == "rstickleft") {
                controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, negaxisvalue));
            } else if (axis != Input::Axis::AxisMax) {
                controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
            }
        }
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTY) {
            if (Rstickbuttons) {
                button = outputkey_map[Rstickupmap];
                controller->CheckButton(0, button, event->gaxis.value < -15000);
                button = outputkey_map[Rstickdownmap];
                controller->CheckButton(0, button, event->gaxis.value > 15000);
            } else {
                if (Rstickswap) {
                    axis = Input::Axis::LeftY;
                }
                if (RstickinvertY) {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, negaxisvalue));
                } else {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
                }
            }
        }
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX) {
            if (Rstickbuttons) {
                button = outputkey_map[Rstickleftmap];
                controller->CheckButton(0, button, event->gaxis.value < -15000);
                button = outputkey_map[Rstickrightmap];
                controller->CheckButton(0, button, event->gaxis.value > 15000);
            } else {
                if (Rstickswap) {
                    axis = Input::Axis::LeftX;
                }
                if (RstickinvertX) {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, negaxisvalue));
                } else {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
                }
            }
        }
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            if (Lstickbuttons) {
                button = outputkey_map[Lstickupmap];
                controller->CheckButton(0, button, event->gaxis.value < -15000);
                button = outputkey_map[Lstickdownmap];
                controller->CheckButton(0, button, event->gaxis.value > 15000);
            } else {
                if (Lstickswap) {
                    axis = Input::Axis::RightY;
                }
                if (LstickinvertY) {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, negaxisvalue));
                } else {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
                }
            }
        }
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            if (Lstickbuttons) {
                button = outputkey_map[Lstickleftmap];
                controller->CheckButton(0, button, event->gaxis.value < -15000);
                button = outputkey_map[Lstickrightmap];
                controller->CheckButton(0, button, event->gaxis.value > 15000);
            } else {
                if (Lstickswap) {
                    axis = Input::Axis::RightX;
                }
                if (LstickinvertX) {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, negaxisvalue));
                } else {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
                }
            }
        }
    }
}

int WindowSDL::sdlGamepadToOrbisButton(u8 button) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return outputkey_map[ddownmap];
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return outputkey_map[dupmap];
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return outputkey_map[dleftmap];
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return outputkey_map[drightmap];
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return outputkey_map[Amap];
    case SDL_GAMEPAD_BUTTON_NORTH:
        return outputkey_map[Ymap];
    case SDL_GAMEPAD_BUTTON_WEST:
        return outputkey_map[Xmap];
    case SDL_GAMEPAD_BUTTON_EAST:
        return outputkey_map[Bmap];
    case SDL_GAMEPAD_BUTTON_START:
        return outputkey_map[startmap];
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        return OrbisPadButtonDataOffset::TouchPad;
    case SDL_GAMEPAD_BUTTON_BACK:
        return OrbisPadButtonDataOffset::TouchPad;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return outputkey_map[LBmap];
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return outputkey_map[RBmap];
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return outputkey_map[lstickmap];
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return outputkey_map[rstickmap];
    default:
        return 0;
    }
}

void RefreshMappings() {

    try {
        std::ifstream ifs;
        ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        const toml::value data = toml::parse("Controller.toml");
    } catch (std::exception& ex) {
        const std::string ParseErrorMsg =
            fmt::format("Parse Error 'Controller.toml'. Exception: {}", ex.what());
        const char* cParseErrorMsg = ParseErrorMsg.c_str();
        LOG_ERROR(Lib_Pad, cParseErrorMsg);
        return;
    }

    const toml::value data = toml::parse("Controller.toml");
    Amap = toml::find_or<std::string>(data, "A_button", "remap", "cross");
    Ymap = toml::find_or<std::string>(data, "Y_button", "remap", "triangle");
    Xmap = toml::find_or<std::string>(data, "X_button", "remap", "square");
    Bmap = toml::find_or<std::string>(data, "B_button", "remap", "circle");
    LBmap = toml::find_or<std::string>(data, "Left_bumper", "remap", "L1");
    RBmap = toml::find_or<std::string>(data, "Right_bumper", "remap", "R1");
    dupmap = toml::find_or<std::string>(data, "dpad_up", "remap", "dpad_up");
    ddownmap = toml::find_or<std::string>(data, "dpad_down", "remap", "dpad_down");
    dleftmap = toml::find_or<std::string>(data, "dpad_left", "remap", "dpad_left");
    drightmap = toml::find_or<std::string>(data, "dpad_right", "remap", "dpad_right");
    rstickmap = toml::find_or<std::string>(data, "Right_stick_button", "remap", "R3");
    lstickmap = toml::find_or<std::string>(data, "Left_stick_button", "remap", "L3");
    startmap = toml::find_or<std::string>(data, "Start", "remap", "options");
    LTmap = toml::find_or<std::string>(data, "Left_trigger", "remap", "L2");
    RTmap = toml::find_or<std::string>(data, "Right_trigger", "remap", "R2");
    Lstickupmap = toml::find_or<std::string>(data, "If_Left_analog_stick_mapped_to_buttons",
                                             "Left_stick_up_remap", "lstickup");
    Lstickdownmap = toml::find_or<std::string>(data, "If_Left_analog_stick_mapped_to_buttons",
                                               "Left_stick_down_remap", "lstickdown");
    Lstickleftmap = toml::find_or<std::string>(data, "If_Left_analog_stick_mapped_to_buttons",
                                               "Left_stick_left_remap", "lstickleft");
    Lstickrightmap = toml::find_or<std::string>(data, "If_Left_analog_stick_mapped_to_buttons",
                                                "Left_stick_right_remap", "lstickright");
    Lstickbuttons =
        toml::find_or<bool>(data, "Left_analog_stick_behavior", "Mapped_to_buttons", false);
    Lstickswap = toml::find_or<bool>(data, "Left_analog_stick_behavior", "Swap_sticks", false);
    LstickinvertY =
        toml::find_or<bool>(data, "Left_analog_stick_behavior", "Invert_movement_vertical", false);
    LstickinvertX = toml::find_or<bool>(data, "Left_analog_stick_behavior",
                                        "Invert_movement_horizontal", false);
    Rstickupmap = toml::find_or<std::string>(data, "If_Right_analog_stick_mapped_to_buttons",
                                             "Right_stick_up_remap", "rstickup");
    Rstickdownmap = toml::find_or<std::string>(data, "If_Right_analog_stick_mapped_to_buttons",
                                               "Right_stick_down_remap", "rstickdown");
    Rstickleftmap = toml::find_or<std::string>(data, "If_Right_analog_stick_mapped_to_buttons",
                                               "Right_stick_left_remap", "rstickleft");
    Rstickrightmap = toml::find_or<std::string>(data, "If_Right_analog_stick_mapped_to_buttons",
                                                "Right_stick_right_remap", "rstickright");
    Rstickbuttons =
        toml::find_or<bool>(data, "Right_analog_stick_behavior", "Mapped_to_buttons", false);
    Rstickswap = toml::find_or<bool>(data, "Right_analog_stick_behavior", "Swap_sticks", false);
    RstickinvertY =
        toml::find_or<bool>(data, "Right_analog_stick_behavior", "Invert_movement_vertical", false);
    RstickinvertX = toml::find_or<bool>(data, "Right_analog_stick_behavior",
                                        "Invert_movement_horizontal", false);
}

} // namespace Frontend