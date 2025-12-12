// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#ifdef ENABLE_QT_GUI
#include <QObject>
#endif

#include "common/types.h"
#include "core/libraries/pad/pad.h"
#include "input/controller.h"

struct SDL_Window;
struct SDL_Gamepad;
union SDL_Event;

namespace Input {
class GameController;
}

namespace Frontend {

enum class WindowSystemType : u8 {
    Headless,
    Windows,
    X11,
    Wayland,
    Metal,
};

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL
#ifdef ENABLE_QT_GUI
    : public QObject
#endif
{
#ifdef ENABLE_QT_GUI
    Q_OBJECT
#endif

    int keyboard_grab = 0;

public:
    explicit WindowSDL(s32 width, s32 height, Input::GameControllers* controllers,
                       std::string_view window_title);
    ~WindowSDL();

    s32 GetWidth() const {
        return width;
    }
    s32 GetHeight() const {
        return height;
    }
    bool IsOpen() const {
        return is_open;
    }
    SDL_Window* GetSDLWindow() const {
        return window;
    }
    WindowSystemInfo GetWindowInfo() const {
        return window_info;
    }

    void WaitEvent();
    void InitTimers();

    void RequestKeyboard();
    void ReleaseKeyboard();

#ifdef ENABLE_QT_GUI
signals:
    void gamepadButtonPressed(int button);
    void GamepadButtonPressed(int button);
    void gamepadButtonReleased(int button);
    void gamepadAxisChanged(int axis, float value);
#endif

private:
    void OnResize();
    void OnKeyboardMouseInput(const SDL_Event* event);
    void OnGamepadEvent(const SDL_Event* event);
    void CheckHotkeys();
    void RelaunchEmulator();

    s32 width;
    s32 height;
    Input::GameControllers controllers{};
    WindowSystemInfo window_info{};
    SDL_Window* window{};
    bool is_shown{};
    bool is_open{true};
};

} // namespace Frontend
