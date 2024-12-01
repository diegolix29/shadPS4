// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <iostream>
#include <map>
#include <string>
#include "INIReader.h"
#include "SDL3/SDL_events.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_properties.h"
#include "SDL3/SDL_timer.h"
#include "SDL3/SDL_video.h"
#include "common/assert.h"
#include "common/config.h"
#include "common/elf_info.h"
#include "common/version.h"
#include "core/libraries/pad/pad.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"

#ifdef __APPLE__
#include "SDL3/SDL_metal.h"
#endif

namespace Frontend {

/* placeholder for on-demand config parsing function
case SDLK_F5:
    if (event->type == SDL_EVENT_KEY_DOWN) {
        void parseconfig();
    }
void parseconfig() {
using Libraries::Pad::OrbisPadButtonDataOffset;
inih::INIReader r{"./user/remap.ini"};
const std::string Amapping = r.Get<std::string>("A button", "remap");
const std::string Ymapping = r.Get<std::string>("Y button", "remap");
const std::string Xmapping = r.Get<std::string>("X button", "remap");
const std::string Bmapping = r.Get<std::string>("B button", "remap");
const std::string LBmapping = r.Get<std::string>("Left_bumper", "remap");
const std::string RBmapping = r.Get<std::string>("Right_bumper", "remap");
const std::string dupmapping = r.Get<std::string>("dpad_up", "remap");
const std::string ddownmapping = r.Get<std::string>("dpad_down", "remap");
const std::string dleftmapping = r.Get<std::string>("dpad_left", "remap");
const std::string drightmapping = r.Get<std::string>("dpad_right", "remap");
const std::string rstickmapping = r.Get<std::string>("Right_stick_button", "remap");
const std::string lstickmapping = r.Get<std::string>("Left_stick_button", "remap");
const std::string startmapping = r.Get<std::string>("Start", "remap");
const std::string LTmapping = r.Get<std::string>("Left_trigger", "remap");
const std::string RTmapping = r.Get<std::string>("Right_trigger", "remap");
std::map<std::string, u32> outputkey_map = {
{"dpad_down", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_DOWN},
{"dpad_up", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_UP},
{"dpad_left", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_LEFT},
{"dpad_right", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_RIGHT},
{"cross", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CROSS},
{"triangle", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TRIANGLE},
{"square", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_SQUARE},
{"circle", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CIRCLE},
{"L1", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L1},
{"R1", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R1},
{"L3", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L3},
{"R3", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R3},
{"options", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_OPTIONS},
{"touchpad", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TOUCH_PAD},
{"L2", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L2},
{"R2", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R2},
};
}
*/
/* todo: autogenerate config
 */
/* todo: use controls button
 */

using namespace Libraries::Pad;

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
    // input handler init-s
    Input::ControllerOutput::SetControllerOutputController(controller);
    Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));
    checkremapinifile();
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

    bool input_down = event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION ||
                      event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                      event->type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN ||
                      event->type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION;
    u32 input_id = Input::InputBinding::GetInputIDFromEvent(*event);

    bool inputs_changed = Input::UpdatePressedKeys(input_id, input_down);

    if (inputs_changed) {
        Input::ActivateOutputsFromInputs();
    }
}

std::string WindowSDL::sdlButtonToAnalog(u8 button) {

    inih::INIReader r{"remap.ini"};
    const std::string Amap = r.Get<std::string>("A button", "remap");
    const std::string Ymap = r.Get<std::string>("Y button", "remap");
    const std::string Xmap = r.Get<std::string>("X button", "remap");
    const std::string Bmap = r.Get<std::string>("B button", "remap");
    const std::string LBmap = r.Get<std::string>("Left bumper", "remap");
    const std::string RBmap = r.Get<std::string>("Right bumper", "remap");
    const std::string dupmap = r.Get<std::string>("dpad up", "remap");
    const std::string ddownmap = r.Get<std::string>("dpad down", "remap");
    const std::string dleftmap = r.Get<std::string>("dpad left", "remap");
    const std::string drightmap = r.Get<std::string>("dpad right", "remap");
    const std::string rstickmap = r.Get<std::string>("Right stick button", "remap");
    const std::string lstickmap = r.Get<std::string>("Left stick button", "remap");
    const std::string startmap = r.Get<std::string>("Start", "remap");

    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return ddownmap;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return dupmap;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return dleftmap;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return drightmap;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return Amap;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return Ymap;
    case SDL_GAMEPAD_BUTTON_WEST:
        return Xmap;
    case SDL_GAMEPAD_BUTTON_EAST:
        return Bmap;
    case SDL_GAMEPAD_BUTTON_START:
        return startmap;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return LBmap;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return RBmap;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return lstickmap;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return rstickmap;
    default:
        return "null";
    }
}

void WindowSDL::checkremapinifile() {
    const std::string defaultremap =
        R"(; Edit only after equal signs ***other edits to the file may cause crashes***
; See syntax at the bottom of the file
; Close ini file before returning to game to avoid stability issues
[Sample binding]
remap=desired_PS4_button_output

[A button]
remap=cross

[Y button]
remap=triangle

[X button]
remap=square

[B button]
remap=circle

[Left bumper]
remap=L1

[Right bumper]
remap=R1

[Left trigger]
remap=L2

[Right trigger]
remap=R2

[dpad up]
remap=dpad_up

[dpad down]
remap=dpad_down

[dpad left]
remap=dpad_left

[dpad right]
remap=dpad_right

[Left stick button]
remap=L3

[Right stick button]
remap=R3

[Start]
remap=options

[Left analog stick behavior]
Analog stick or buttons=analog_stick
Swap sticks=No
Invert movement vertical=No
Invert movement horizontal=No

[If Left analog stick mapped to buttons]
Left stick up remap=dpad_up
Left stick down remap=dpad_down
Left stick left remap=dpad_left
Left stick right remap=dpad_right

[Right analog stick behavior]
Analog stick or buttons=analog_stick
Swap sticks=No
Invert movement vertical=No
Invert movement horizontal=No

[If Right analog stick mapped to buttons]
Right stick up remap=triangle
Right stick down remap=cross
Right stick left remap=square
Right stick right remap=circle

[Syntax and defaults, do not edit]
A button=cross
Y button=triangle
X button=square
B button=circle
Left bumper=L1
Right bumper=R1
Left trigger=L2
Right trigger=R2
dpad up=dpad_up
dpad down=dpad_down
dpad left=dpad_left
dpad right=dpad_right
Left stick button=L3
Right stick button=R3
Left stick up=lstickup
Left stick down=lstickdown
Left stick left=lstickleft
Left stick right=lstickright
Right stick up=rstickup
Right stick down=rstickdown
Right stick left=rstickleft
Right stick right=rstickright
Start=options

[Syntax for stick settings, do not edit]
Swap sticks (default)=No
Swap sticks (swap)=Yes
Invert movement (default)=No
Invert movement (invert)=Yes)";

    if (!std::filesystem::exists("remap.ini")) {
        std::ofstream remapfile("remap.ini");
        remapfile << defaultremap;
        remapfile.close();
    }
}

} // namespace Frontend
