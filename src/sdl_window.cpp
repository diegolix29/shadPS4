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
#include "common/assert.h"
#include "common/config.h"
#include "common/version.h"
#include "core/libraries/pad/pad.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"

#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif

namespace Frontend {

    using Libraries::Pad::OrbisPadButtonDataOffset;
    std::map<std::string, u32> outputkey_map = {
        {"dpad_down", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_DOWN},
        {"dpad_up", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_UP},
        {"dpad_left", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_LEFT},
        {"dpad_right", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_RIGHT},
        {"cross", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CROSS},
        {"triangle", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TRIANGLE},
        {"square", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_SQUARE},
        {"circle", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CIRCLE},
        {"options", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_OPTIONS},
        {"L1", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L1},
        {"R1", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R1},
        {"L3", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L3},
        {"R3", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R3},
        {"L2", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L2},
        {"R2", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R2},
        {"lstickup", OrbisPadButtonDataOffset::LEFT_ANALOG_STICKUP}, // OrbisPadButtonDataOffset::LEFT_ANALOG_STICKUP =  2,000,001
        {"lstickdown", OrbisPadButtonDataOffset::LEFT_ANALOG_STICKDOWN}, // = 2000002
        {"lstickleft", OrbisPadButtonDataOffset::LEFT_ANALOG_STICKLEFT}, // = 2000003
        {"lstickright", OrbisPadButtonDataOffset::LEFT_ANALOG_STICKRIGHT}, // = 2000004
        {"rstickup", OrbisPadButtonDataOffset::RIGHT_ANALOG_STICKUP}, // = 2000005
        {"rstickdown", OrbisPadButtonDataOffset::RIGHT_ANALOG_STICKDOWN}, // = 2000006
        {"rstickleft", OrbisPadButtonDataOffset::RIGHT_ANALOG_STICKLEFT}, // = 2000007
        {"rstickright", OrbisPadButtonDataOffset::RIGHT_ANALOG_STICKRIGHT}, // = 2000008
        };


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

}

WindowSDL::~WindowSDL() = default;

void WindowSDL::waitEvent() {
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
        onResize();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = event.type == SDL_EVENT_WINDOW_EXPOSED;
        onResize();
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        onKeyPress(&event);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        onGamepadEvent(&event);
        break;
    case SDL_EVENT_QUIT:
        is_open = false;
        break;
    default:
        break;
    }
}

void WindowSDL::initTimers() {
    SDL_AddTimer(100, &PollController, controller);
}

void WindowSDL::onResize() {
    SDL_GetWindowSizeInPixels(window, &width, &height);
    ImGui::Core::OnResize();
}

void WindowSDL::onKeyPress(const SDL_Event* event) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

#ifdef __APPLE__
    // Use keys that are more friendly for keyboards without a keypad.
    // Once there are key binding options this won't be necessary.
    constexpr SDL_Keycode CrossKey = SDLK_N;
    constexpr SDL_Keycode CircleKey = SDLK_B;
    constexpr SDL_Keycode SquareKey = SDLK_V;
    constexpr SDL_Keycode TriangleKey = SDLK_C;
#else
    constexpr SDL_Keycode CrossKey = SDLK_KP_2;
    constexpr SDL_Keycode CircleKey = SDLK_KP_6;
    constexpr SDL_Keycode SquareKey = SDLK_KP_4;
    constexpr SDL_Keycode TriangleKey = SDLK_KP_8;
#endif

    u32 button = 0;
    Input::Axis axis = Input::Axis::AxisMax;
    int axisvalue = 0;
    int ax = 0;
    std::string backButtonBehavior = Config::getBackButtonBehavior();
    switch (event->key.key) {
    case SDLK_UP:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_UP;
        break;
    case SDLK_DOWN:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_DOWN;
        break;
    case SDLK_LEFT:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_LEFT;
        break;
    case SDLK_RIGHT:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_RIGHT;
        break;
    case TriangleKey:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TRIANGLE;
        break;
    case CircleKey:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CIRCLE;
        break;
    case CrossKey:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CROSS;
        break;
    case SquareKey:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_SQUARE;
        break;
    case SDLK_RETURN:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_OPTIONS;
        break;
    case SDLK_A:
        axis = Input::Axis::LeftX;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += -127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_D:
        axis = Input::Axis::LeftX;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_W:
        axis = Input::Axis::LeftY;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += -127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_S:
        axis = Input::Axis::LeftY;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_J:
        axis = Input::Axis::RightX;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += -127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_L:
        axis = Input::Axis::RightX;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_I:
        axis = Input::Axis::RightY;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += -127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_K:
        axis = Input::Axis::RightY;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_X:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L3;
        break;
    case SDLK_M:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R3;
        break;
    case SDLK_Q:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L1;
        break;
    case SDLK_U:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R1;
        break;
    case SDLK_E:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L2;
        axis = Input::Axis::TriggerLeft;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 255;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(0, 0x80, axisvalue);
        break;
    case SDLK_O:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R2;
        axis = Input::Axis::TriggerRight;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 255;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(0, 0x80, axisvalue);
        break;
    case SDLK_SPACE:
        if (backButtonBehavior != "none") {
            float x = backButtonBehavior == "left" ? 0.25f
                                                   : (backButtonBehavior == "right" ? 0.75f : 0.5f);
            // trigger a touchpad event so that the touchpad emulation for back button works
            controller->SetTouchpadState(0, true, x, 0.5f);
            button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TOUCH_PAD;
        } else {
            button = 0;
        }
        break;
    case SDLK_F11:
        if (event->type == SDL_EVENT_KEY_DOWN) {
            {
                SDL_WindowFlags flag = SDL_GetWindowFlags(window);
                bool is_fullscreen = flag & SDL_WINDOW_FULLSCREEN;
                SDL_SetWindowFullscreen(window, !is_fullscreen);
            }
        }
        break;
    case SDLK_F12:
        if (event->type == SDL_EVENT_KEY_DOWN) {
            // Trigger rdoc capture
            VideoCore::TriggerCapture();
        }
        break;
    default:
        break;
    }
    if (button != 0) {
        controller->CheckButton(0, button, event->type == SDL_EVENT_KEY_DOWN);
    }
    if (axis != Input::Axis::AxisMax) {
        controller->Axis(0, axis, ax);
    }
}

void WindowSDL::onGamepadEvent(const SDL_Event* event) {
   
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
        try {
            std::ifstream ifs;
            ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
            const toml::value data = toml::parse("Controller.toml");
        } catch (std::exception& ex) {
            fmt::print("Got exception trying to load controller config file 'Controller.toml'. Exception: {}\n", ex.what());
            return;
        } 
        button = sdlGamepadToOrbisButton(event->gbutton.button);
        // if button is mapped to axis, convert to axis inputs, axes are mapped to values 2000001-8
        if (button > 2000000 ) {
            if (button == OrbisPadButtonDataOffset::LEFT_ANALOG_STICKUP) {
                axis = Input::Axis::LeftY;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += -127;
                } else {
                    axisvalue = 0;
                }
            } else if (button == OrbisPadButtonDataOffset::LEFT_ANALOG_STICKDOWN) {
                axis = Input::Axis::LeftY;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += 127;
                } else {
                    axisvalue = 0;
                }
            } else if (button == OrbisPadButtonDataOffset::LEFT_ANALOG_STICKLEFT) {
                axis = Input::Axis::LeftX;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += -127;
                } else {
                    axisvalue = 0;
                }
            } else if (button == OrbisPadButtonDataOffset::LEFT_ANALOG_STICKRIGHT) {
                axis = Input::Axis::LeftX;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += 127;
                } else {
                    axisvalue = 0;
                }
            } else if (button == OrbisPadButtonDataOffset::RIGHT_ANALOG_STICKUP) {
                axis = Input::Axis::RightY;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += -127;
                } else {
                    axisvalue = 0;
                }
            } else if (button == OrbisPadButtonDataOffset::RIGHT_ANALOG_STICKDOWN) {
                axis = Input::Axis::RightY;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += 127;
                } else {
                    axisvalue = 0;
                }
            } else if (OrbisPadButtonDataOffset::RIGHT_ANALOG_STICKLEFT) {
                axis = Input::Axis::RightX;
                if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    axisvalue += -127;
                } else {
                    axisvalue = 0;
                }
            } else if (OrbisPadButtonDataOffset::RIGHT_ANALOG_STICKRIGHT) {
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

    try {
        std::ifstream ifs;
        ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        const toml::value data = toml::parse("Controller.toml");
    } catch (std::exception& ex) {
        fmt::print("Got exception trying to load controller config file 'Controller.toml'. Exception: {}\n", ex.what());
        return;
    }

    const toml::value data = toml::parse("Controller.toml");
    std::string LTmap = toml::find<std::string>(data, "Left_trigger", "remap");
    std::string RTmap = toml::find<std::string>(data, "Right_trigger", "remap");
    std::string Lstickupmap = toml::find<std::string>(data, "If_Left_analog_stick_mapped_to_buttons", "Left_stick_up_remap");
    std::string Lstickdownmap = toml::find<std::string>(data, "If_Left_analog_stick_mapped_to_buttons", "Left_stick_down_remap");
    std::string Lstickleftmap = toml::find<std::string>(data, "If_Left_analog_stick_mapped_to_buttons", "Left_stick_left_remap");
    std::string Lstickrightmap = toml::find<std::string>(data, "If_Left_analog_stick_mapped_to_buttons", "Left_stick_right_remap");
    std::string Lstickbehavior = toml::find<std::string>(data, "Left_analog_stick_behavior", "Analog_stick_or_buttons");
    std::string Lstickswap = toml::find<std::string>(data, "Left_analog_stick_behavior", "Swap_sticks");
    std::string LstickinvertY = toml::find<std::string>(data, "Left_analog_stick_behavior", "Invert_movement_vertical");
    std::string LstickinvertX = toml::find<std::string>(data, "Left_analog_stick_behavior", "Invert_movement_horizontal");
    std::string Rstickupmap = toml::find<std::string>(data, "If_Right_analog_stick_mapped_to_buttons", "Right_stick_up_remap");
    std::string Rstickdownmap = toml::find<std::string>(data, "If_Right_analog_stick_mapped_to_buttons", "Right_stick_down_remap");
    std::string Rstickleftmap = toml::find<std::string>(data, "If_Right_analog_stick_mapped_to_buttons", "Right_stick_left_remap");
    std::string Rstickrightmap = toml::find<std::string>(data, "If_Right_analog_stick_mapped_to_buttons", "Right_stick_right_remap");
    std::string Rstickbehavior = toml::find<std::string>(data, "Right_analog_stick_behavior", "Analog_stick_or_buttons");
    std::string Rstickswap = toml::find<std::string>(data, "Right_analog_stick_behavior", "Swap_sticks");
    std::string RstickinvertY = toml::find<std::string>(data, "Right_analog_stick_behavior", "Invert_movement_vertical");
    std::string RstickinvertX = toml::find<std::string>(data, "Right_analog_stick_behavior", "Invert_movement_horizontal"); 

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
            // if trigger is not mapped to axis, convert to button inputs, axes are mapped to values 2000000+
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
            if (Rstickbehavior == "buttons") {
                button = outputkey_map[Rstickupmap];
                controller->CheckButton(0, button, event->gaxis.value < -15000);
                button = outputkey_map[Rstickdownmap];
                controller->CheckButton(0, button, event->gaxis.value > 15000);
            } else {
                if (Rstickswap == "Yes") {
                    axis = Input::Axis::LeftY;
                }
                if (RstickinvertY == "Yes") {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, negaxisvalue)); 
                } else { 
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
                }
            }
        }
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX) {
            if (Rstickbehavior == "buttons") {
                button = outputkey_map[Rstickleftmap];
                controller->CheckButton(0, button, event->gaxis.value < -15000);
                button = outputkey_map[Rstickrightmap];
                controller->CheckButton(0, button, event->gaxis.value > 15000);
            } else {
                if (Rstickswap == "Yes") {
                    axis = Input::Axis::LeftX;
                }
                if (RstickinvertX == "Yes") {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, negaxisvalue)); 
                } else {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
                }
            }
        }
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            if (Lstickbehavior == "buttons") {
                button = outputkey_map[Lstickupmap];
                controller->CheckButton(0, button, event->gaxis.value < -15000);
                button = outputkey_map[Lstickdownmap];
                controller->CheckButton(0, button, event->gaxis.value > 15000);
            } else {
                if (Lstickswap == "Yes") {
                    axis = Input::Axis::RightY;
                }
                if (LstickinvertY == "Yes") {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, negaxisvalue)); 
                } else {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
                }
            } 
        } 
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            if (Lstickbehavior == "buttons") {
                button = outputkey_map[Lstickleftmap];
                controller->CheckButton(0, button, event->gaxis.value < -15000);
                button = outputkey_map[Lstickrightmap];
                controller->CheckButton(0, button, event->gaxis.value > 15000);
            } else {
                if (Lstickswap == "Yes") {
                    axis = Input::Axis::RightX;
                }
                if (LstickinvertX == "Yes") {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, negaxisvalue));  
                } else {
                    controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
                }
            }
        }    
    }
}

int WindowSDL::sdlGamepadToOrbisButton(u8 button) {

    try {
        std::ifstream ifs;
        ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        const toml::value data = toml::parse("Controller.toml");
    } catch (std::exception& ex) {
        fmt::print("Got exception trying to load controller config file 'Controller.toml'. Exception: {}\n", ex.what());
        return;
    }

    using Libraries::Pad::OrbisPadButtonDataOffset;
    const toml::value data = toml::parse("Controller.toml");
    std::string Amap = toml::find<std::string>(data, "A_button", "remap");
    std::string Ymap = toml::find<std::string>(data, "Y_button", "remap");
    std::string Xmap = toml::find<std::string>(data, "X_button", "remap");
    std::string Bmap = toml::find<std::string>(data, "B_button", "remap");
    std::string LBmap = toml::find<std::string>(data, "Left_bumper", "remap");
    std::string RBmap = toml::find<std::string>(data, "Right_bumper", "remap");
    std::string dupmap = toml::find<std::string>(data, "dpad_up", "remap");
    std::string ddownmap = toml::find<std::string>(data, "dpad_down", "remap");
    std::string dleftmap = toml::find<std::string>(data, "dpad_left", "remap");
    std::string drightmap = toml::find<std::string>(data, "dpad_right", "remap");
    std::string rstickmap = toml::find<std::string>(data, "Right_stick_button", "remap");
    std::string lstickmap = toml::find<std::string>(data, "Left_stick_button", "remap");
    std::string startmap = toml::find<std::string>(data, "Start", "remap");

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
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TOUCH_PAD;
    case SDL_GAMEPAD_BUTTON_BACK:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TOUCH_PAD;
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

} // namespace Frontend