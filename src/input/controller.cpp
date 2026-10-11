// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <SDL3/SDL.h>
#include <common/singleton.h>
#include "common/config.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/system/userservice.h"
#include "input/controller.h"
#include "video_core/renderdoc.h"

static std::string SelectedGamepad = "";
static std::mutex SelectedGamepadMutex;

namespace Input {

// Input recording/playback for automated testing.
// SHADPS4_INPUT_RECORD=<file> records player 1 button/axis changes with timestamps (ms since
// process start); pressing the capture key adds a 'C' marker. SHADPS4_INPUT_PLAY=<file> replays
// them on player 1; at the marker it takes screenshots, exits, or triggers a RenderDoc capture.
namespace {
const auto g_input_t0 = std::chrono::steady_clock::now();
std::mutex g_record_mutex;
std::atomic_bool g_playing{false};

long long InputElapsedMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 g_input_t0)
        .count();
}

std::FILE* InputRecordFile() {
    static std::FILE* file = []() -> std::FILE* {
        const char* path = std::getenv("SHADPS4_INPUT_RECORD");
        return path && *path ? std::fopen(path, "w") : nullptr;
    }();
    return file;
}

// UI file loads mark menu screen transitions. Inputs are recorded relative to the latest one
// so playback follows the game instead of the clock (loading speed varies between runs).
struct AnchorState {
    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>> opens;
    std::string last_path;
};

AnchorState& Anchors() {
    static AnchorState state;
    return state;
}

// Delay between the 'C' marker and SHADPS4_EXIT_AT_MARKER's exit (SHADPS4_CAPTURE_DELAY_MS).
u32 MarkerDelayMs() {
    static const u32 delay = [] {
        const char* env = std::getenv("SHADPS4_CAPTURE_DELAY_MS");
        return env && *env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 120000u;
    }();
    return delay;
}

bool InputScriptActive() {
    static const bool active = [] {
        const char* rec = std::getenv("SHADPS4_INPUT_RECORD");
        const char* play = std::getenv("SHADPS4_INPUT_PLAY");
        return (rec && *rec) || (play && *play);
    }();
    return active;
}

void RecordInputEvent(const GameController* controller, char kind, u32 a, s32 b) {
    std::FILE* file = InputRecordFile();
    if (!file || g_playing) {
        return;
    }
    if (controller && controller != (*Common::Singleton<GameControllers>::Instance())[0]) {
        return;
    }
    std::string anchor = "-";
    size_t anchor_count = 0;
    long long anchor_dt = 0;
    {
        auto& anchors = Anchors();
        std::scoped_lock lock{anchors.mutex};
        if (!anchors.last_path.empty()) {
            const auto& times = anchors.opens[anchors.last_path];
            anchor = anchors.last_path;
            anchor_count = times.size();
            anchor_dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - times.back())
                            .count();
        }
    }
    std::scoped_lock lock{g_record_mutex};
    std::fprintf(file, "%lld %c %u %d %zu %lld %s\n", InputElapsedMs(), kind, a, b, anchor_count,
                 anchor_dt, anchor.c_str());
    std::fflush(file);
}

void InputPlaybackThread(std::string path) {
    struct Event {
        long long time;
        char kind;
        u32 a;
        s32 b;
        size_t anchor_count;
        long long anchor_dt;
        std::string anchor;
    };
    std::vector<Event> events;
    std::ifstream in{path};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss{line};
        Event e{};
        if (ss >> e.time >> e.kind >> e.a >> e.b) {
            if (!(ss >> e.anchor_count >> e.anchor_dt >> e.anchor) || e.anchor == "-") {
                e.anchor_count = 0;
            }
            events.push_back(e);
        }
    }
    g_playing = true;
    for (size_t i = 0; i < events.size(); ++i) {
        const auto& e = events[i];
        bool synced = false;
        if (e.anchor_count > 0) {
            auto& anchors = Anchors();
            std::unique_lock lock{anchors.mutex};
            const bool reached = anchors.cv.wait_for(lock, std::chrono::seconds(60), [&] {
                const auto it = anchors.opens.find(e.anchor);
                return it != anchors.opens.end() && it->second.size() >= e.anchor_count;
            });
            if (reached) {
                const auto base = anchors.opens[e.anchor][e.anchor_count - 1];
                lock.unlock();
                std::this_thread::sleep_until(base + std::chrono::milliseconds(e.anchor_dt));
                synced = true;
            } else {
                LOG_WARNING(Input, "InputPlayback: anchor {} #{} not reached, using recorded time",
                            e.anchor, e.anchor_count);
            }
        }
        if (!synced) {
            std::this_thread::sleep_until(g_input_t0 + std::chrono::milliseconds(e.time));
        }
        auto* controller = (*Common::Singleton<GameControllers>::Instance())[0];
        switch (e.kind) {
        case 'B':
            controller->CheckButton(0, static_cast<Libraries::Pad::OrbisPadButtonDataOffset>(e.a),
                                    e.b != 0);
            break;
        case 'A':
            controller->Axis(static_cast<Input::Axis>(e.a), e.b);
            break;
        case 'C':
            LOG_INFO(Input, "InputPlayback: capture marker at {} ms (event {}/{})", e.time, i + 1,
                     events.size());
            // SHADPS4_SCREENSHOT_SERIES=count:interval_ms:delay_ms takes plain screenshots.
            if (const char* series = std::getenv("SHADPS4_SCREENSHOT_SERIES"); series && *series) {
                unsigned count = 8, interval = 300, delay = 10000;
                std::sscanf(series, "%u:%u:%u", &count, &interval, &delay);
                LOG_INFO(Input, "InputPlayback: {} screenshots every {} ms after {} ms", count,
                         interval, delay);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                for (unsigned n = 0; n < count; ++n) {
                    VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
                    std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                }
                LOG_INFO(Input, "InputPlayback: screenshot series requested");
            }
            if (const char* exit_env = std::getenv("SHADPS4_EXIT_AT_MARKER");
                exit_env && *exit_env == '1') {
                const u32 delay = MarkerDelayMs();
                LOG_INFO(Input, "InputPlayback: exiting in {} ms (performance run)", delay);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                std::_Exit(0);
            }
            if (VideoCore::IsRenderDocLoaded()) {
                VideoCore::TriggerCapture();
            }
            break;
        default:
            break;
        }
    }
    LOG_INFO(Input, "InputPlayback: finished {} events", events.size());
}

const bool g_input_playback_started = [] {
    const char* path = std::getenv("SHADPS4_INPUT_PLAY");
    if (!path || !*path) {
        return false;
    }
    std::thread(InputPlaybackThread, std::string{path}).detach();
    return true;
}();
} // namespace

void RecordInputMarker(char kind) {
    RecordInputEvent(nullptr, kind, 0, 0);
}

void NotifyGameFileOpen(const char* path) {
    if (!InputScriptActive() || !path) {
        return;
    }
    const std::string_view view{path};
    if (view.find("/ui/") == std::string_view::npos) {
        return;
    }
    auto& anchors = Anchors();
    {
        std::scoped_lock lock{anchors.mutex};
        anchors.opens[std::string{view}].push_back(std::chrono::steady_clock::now());
        anchors.last_path = std::string{view};
    }
    anchors.cv.notify_all();
}

GameController::GameController() {
    m_states_num = 0;
    m_last_state = State();
}

void GameController::ReadState(State* state, bool* isConnected, int* connectedCount) {
    std::scoped_lock lock{m_mutex};

    *isConnected = m_connected;
    *connectedCount = m_connected_count;
    *state = GetLastState();
}

int GameController::ReadStates(State* states, int states_num, bool* isConnected,
                               int* connectedCount) {
    std::scoped_lock lock{m_mutex};

    *isConnected = m_connected;
    *connectedCount = m_connected_count;

    int ret_num = 0;

    if (m_connected) {
        if (m_states_num == 0) {
            ret_num = 1;
            states[0] = m_last_state;
        } else {
            for (uint32_t i = 0; i < m_states_num; i++) {
                if (ret_num >= states_num) {
                    break;
                }
                auto index = (m_first_state + i) % MAX_STATES;
                if (!m_private[index].obtained) {
                    m_private[index].obtained = true;

                    states[ret_num++] = m_states[index];
                }
            }
        }
    }

    return ret_num;
}

State GameController::GetLastState() const {
    if (m_states_num == 0) {
        return m_last_state;
    }
    const u32 last = (m_first_state + m_states_num - 1) % MAX_STATES;
    return m_states[last];
}

void GameController::AddState(const State& state) {
    if (m_states_num >= MAX_STATES) {
        m_first_state = (m_first_state + 1) % MAX_STATES;
    } else {
        m_states_num++;
    }

    const u32 index = (m_first_state + m_states_num - 1) % MAX_STATES;
    m_states[index] = state;
    m_last_state = state;
    m_private[index].obtained = false;
}

void GameController::CheckButton(int id, Libraries::Pad::OrbisPadButtonDataOffset button,
                                 bool is_pressed) {
    RecordInputEvent(this, 'B', static_cast<u32>(button), is_pressed ? 1 : 0);
    std::scoped_lock lock{m_mutex};
    auto state = GetLastState();
    state.time = Libraries::Kernel::sceKernelGetProcessTime();
    if (is_pressed) {
        state.buttonsState |= button;
    } else {
        state.buttonsState &= ~button;
    }

    AddState(state);
}

void GameController::Axis(Input::Axis axis, int value, bool smooth) {
    RecordInputEvent(this, 'A', static_cast<u32>(axis), value);
    std::scoped_lock lock{m_mutex};
    auto state = GetLastState();
    state.time = Libraries::Kernel::sceKernelGetProcessTime();

    auto const i = static_cast<size_t>(axis);

    state.axes[i] = axis_smoothing_end_values[i];

    axis_smoothing_start_times[i] = state.time;
    axis_smoothing_start_values[i] = state.axes[i];
    axis_smoothing_end_values[i] = value;
    axis_smoothing_flags[i] = smooth;

    const auto toggle = [&](const auto button) {
        if (value > 0) {
            state.buttonsState |= button;
        } else {
            state.buttonsState &= ~button;
        }
    };

    switch (axis) {
    case Input::Axis::TriggerLeft:
        toggle(Libraries::Pad::OrbisPadButtonDataOffset::L2);
        break;
    case Input::Axis::TriggerRight:
        toggle(Libraries::Pad::OrbisPadButtonDataOffset::R2);
        break;
    default:
        break;
    }

    AddState(state);
}

void GameController::UpdateAxisSmoothing() {
    std::scoped_lock lock{m_mutex};
    auto state = GetLastState();
    state.time = Libraries::Kernel::sceKernelGetProcessTime();
    bool state_changed = false;

    for (size_t i = 0; i < static_cast<size_t>(Input::Axis::AxisMax); i++) {
        if (!axis_smoothing_flags[i] ||
            std::abs(state.axes[i] - axis_smoothing_end_values[i]) < 8) {
            if (state.axes[i] != axis_smoothing_end_values[i]) {
                state.axes[i] = axis_smoothing_end_values[i];
                state_changed = true;
            }
            continue;
        }
        auto now = state.time;
        f32 t =
            std::clamp((now - axis_smoothing_start_times[i]) / f32{axis_smoothing_time}, 0.f, 1.f);
        s32 new_val =
            s32(axis_smoothing_start_values[i] * (1 - t) + axis_smoothing_end_values[i] * t);

        if (state.axes[i] != new_val) {
            state.axes[i] = new_val;
            state_changed = true;
        }
    }

    if (state_changed) {
        AddState(state);
    }
}

void GameController::Gyro(int id, const float gyro[3]) {
    std::scoped_lock lock{m_mutex};
    auto state = GetLastState();
    state.time = Libraries::Kernel::sceKernelGetProcessTime();

    // Update the angular velocity (gyro data)
    state.angularVelocity.x = gyro[0]; // X-axis
    state.angularVelocity.y = gyro[1]; // Y-axis
    state.angularVelocity.z = gyro[2]; // Z-axis

    AddState(state);
}
void GameController::Acceleration(int id, const float acceleration[3]) {
    std::scoped_lock lock{m_mutex};
    auto state = GetLastState();
    state.time = Libraries::Kernel::sceKernelGetProcessTime();

    // Update the acceleration values
    state.acceleration.x = acceleration[0]; // X-axis
    state.acceleration.y = acceleration[1]; // Y-axis
    state.acceleration.z = acceleration[2]; // Z-axis

    AddState(state);
}

void GameController::CalculateOrientation(Libraries::Pad::OrbisFVector3& acceleration,
                                          Libraries::Pad::OrbisFVector3& angularVelocity,
                                          float deltaTime,
                                          Libraries::Pad::OrbisFQuaternion& lastOrientation,
                                          Libraries::Pad::OrbisFQuaternion& orientation) {
    constexpr float MAX_DELTA_TIME = 0.1f;
    if (deltaTime > MAX_DELTA_TIME) {
        deltaTime = MAX_DELTA_TIME;
    }

    if (deltaTime <= 0.0f || deltaTime < 0.0001f) {
        orientation = lastOrientation;
        return;
    }

    Libraries::Pad::OrbisFQuaternion q = lastOrientation;
    Libraries::Pad::OrbisFQuaternion ω = {angularVelocity.x, angularVelocity.y, angularVelocity.z,
                                          0.0f};

    Libraries::Pad::OrbisFQuaternion qω = {q.w * ω.x + q.x * ω.w + q.y * ω.z - q.z * ω.y,
                                           q.w * ω.y + q.y * ω.w + q.z * ω.x - q.x * ω.z,
                                           q.w * ω.z + q.z * ω.w + q.x * ω.y - q.y * ω.x,
                                           q.w * ω.w - q.x * ω.x - q.y * ω.y - q.z * ω.z};

    Libraries::Pad::OrbisFQuaternion qDot = {0.5f * qω.x, 0.5f * qω.y, 0.5f * qω.z, 0.5f * qω.w};

    q.x += qDot.x * deltaTime;
    q.y += qDot.y * deltaTime;
    q.z += qDot.z * deltaTime;
    q.w += qDot.w * deltaTime;

    float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (norm > 0.0001f) {
        q.x /= norm;
        q.y /= norm;
        q.z /= norm;
        q.w /= norm;
    } else {
        q = {0.0f, 0.0f, 0.0f, 1.0f};
    }

    constexpr float ACCEL_CORRECTION_WEIGHT = 0.02f;

    float accelNorm = std::sqrt(acceleration.x * acceleration.x + acceleration.y * acceleration.y +
                                acceleration.z * acceleration.z);

    if (accelNorm > 0.5f && accelNorm < 1.5f) {
        float accelX = acceleration.x / accelNorm;
        float accelY = acceleration.y / accelNorm;
        float accelZ = acceleration.z / accelNorm;

        constexpr float gravityX = 0.0f;
        constexpr float gravityY = -1.0f;
        constexpr float gravityZ = 0.0f;

        float predX = (1 - 2 * q.y * q.y - 2 * q.z * q.z) * gravityX +
                      (2 * q.x * q.y + 2 * q.w * q.z) * gravityY +
                      (2 * q.x * q.z - 2 * q.w * q.y) * gravityZ;
        float predY = (2 * q.x * q.y - 2 * q.w * q.z) * gravityX +
                      (1 - 2 * q.x * q.x - 2 * q.z * q.z) * gravityY +
                      (2 * q.y * q.z + 2 * q.w * q.x) * gravityZ;
        float predZ = (2 * q.x * q.z + 2 * q.w * q.y) * gravityX +
                      (2 * q.y * q.z - 2 * q.w * q.x) * gravityY +
                      (1 - 2 * q.x * q.x - 2 * q.y * q.y) * gravityZ;

        float errorX = accelY * predZ - accelZ * predY;
        float errorY = accelZ * predX - accelX * predZ;
        float errorZ = accelX * predY - accelY * predX;

        float correctionScale = ACCEL_CORRECTION_WEIGHT * deltaTime;
        q.x += correctionScale * errorX;
        q.y += correctionScale * errorY;
        q.z += correctionScale * errorZ;

        norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (norm > 0.0001f) {
            q.x /= norm;
            q.y /= norm;
            q.z /= norm;
            q.w /= norm;
        }
    }

    orientation.x = q.x;
    orientation.y = q.y;
    orientation.z = q.z;
    orientation.w = q.w;
    LOG_DEBUG(Lib_Pad, "Calculated orientation: {:.2f} {:.2f} {:.2f} {:.2f}", orientation.x,
              orientation.y, orientation.z, orientation.w);
}

void GameController::SetLightBarRGB(u8 r, u8 g, u8 b) {
    if (m_sdl_gamepad != nullptr) {
        SDL_SetGamepadLED(m_sdl_gamepad, r, g, b);
    }
}

bool GameController::SetVibration(u8 smallMotor, u8 largeMotor) {
    if (m_sdl_gamepad != nullptr) {
        return SDL_RumbleGamepad(m_sdl_gamepad, (smallMotor / 255.0f) * 0xFFFF,
                                 (largeMotor / 255.0f) * 0xFFFF, -1);
    }
    return true;
}

void GameController::SetTouchpadState(int touchIndex, bool touchDown, float x, float y) {
    if (touchIndex < 2) {
        std::scoped_lock lock{m_mutex};
        auto state = GetLastState();
        state.time = Libraries::Kernel::sceKernelGetProcessTime();

        state.touchpad[touchIndex].state = touchDown;
        state.touchpad[touchIndex].x = static_cast<u16>(x * 1920);
        state.touchpad[touchIndex].y = static_cast<u16>(y * 941);

        AddState(state);
    }
}

bool is_first_check = true;

void GameControllers::TryOpenSDLControllers(GameControllers& controllers) {
    using namespace Libraries::UserService;
    int controller_count;
    SDL_JoystickID* new_joysticks = SDL_GetGamepads(&controller_count);

    std::unordered_set<SDL_JoystickID> assigned_ids;
    std::array<bool, 4> slot_taken{false, false, false, false};

    for (int i = 0; i < 4; i++) {
        SDL_Gamepad* pad = controllers[i]->m_sdl_gamepad;
        if (pad) {
            SDL_JoystickID id = SDL_GetGamepadID(pad);
            bool still_connected = false;
            for (int j = 0; j < controller_count; j++) {
                if (new_joysticks[j] == id) {
                    still_connected = true;
                    assigned_ids.insert(id);
                    slot_taken[i] = true;
                    break;
                }
            }
            if (!still_connected) {
                AddUserServiceEvent({OrbisUserServiceEventType::Logout, i + 1});
                controllers[i]->m_sdl_gamepad = nullptr;
                SDL_CloseGamepad(pad);
                controllers[i]->user_id = -1;
                slot_taken[i] = false;
            } else {
                controllers[i]->player_index = i;
            }
        }
    }

    for (int j = 0; j < controller_count; j++) {
        SDL_JoystickID id = new_joysticks[j];
        if (assigned_ids.contains(id))
            continue;

        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad)
            continue;

        for (int i = 0; i < 4; i++) {
            if (!slot_taken[i]) {
                auto* c = controllers[i];
                c->m_sdl_gamepad = pad;
                LOG_INFO(Input, "Gamepad registered for slot {}! Handle: {}", i,
                         SDL_GetGamepadID(pad));
                c->user_id = i + 1;
                slot_taken[i] = true;
                c->player_index = i;
                AddUserServiceEvent({OrbisUserServiceEventType::Login, i + 1});

                if (SDL_SetGamepadSensorEnabled(c->m_sdl_gamepad, SDL_SENSOR_GYRO, true)) {
                    c->gyro_poll_rate =
                        SDL_GetGamepadSensorDataRate(c->m_sdl_gamepad, SDL_SENSOR_GYRO);
                    LOG_INFO(Input, "Gyro initialized, poll rate: {}", c->gyro_poll_rate);

                } else {
                    LOG_ERROR(Input, "Failed to initialize gyro controls for gamepad {}",
                              c->user_id);
                }
                if (SDL_SetGamepadSensorEnabled(c->m_sdl_gamepad, SDL_SENSOR_ACCEL, true)) {
                    c->accel_poll_rate =
                        SDL_GetGamepadSensorDataRate(c->m_sdl_gamepad, SDL_SENSOR_ACCEL);
                    LOG_INFO(Input, "Accel initialized, poll rate: {}", c->accel_poll_rate);

                } else {
                    LOG_ERROR(Input, "Failed to initialize accel controls for gamepad {}",
                              c->user_id);
                }
                break;
            }
        }
    }
    if (is_first_check) [[unlikely]] {
        is_first_check = false;
        if (controller_count == 0) {
            controllers[0]->user_id = 1;
            AddUserServiceEvent({OrbisUserServiceEventType::Login, 1});
        }
    }

    if (new_joysticks) {
        SDL_free(new_joysticks);
    }
}

u8 GameController::GetTouchCount() {
    std::scoped_lock lock{m_mutex};
    return m_touch_count;
}

void GameController::SetTouchCount(u8 touchCount) {
    std::scoped_lock lock{m_mutex};
    m_touch_count = touchCount;
}

u8 GameController::GetSecondaryTouchCount() {
    std::scoped_lock lock{m_mutex};
    return m_secondary_touch_count;
}

void GameController::SetSecondaryTouchCount(u8 touchCount) {
    std::scoped_lock lock{m_mutex};
    m_secondary_touch_count = touchCount;
    if (touchCount == 0) {
        m_was_secondary_reset = true;
    }
}

u8 GameController::GetPreviousTouchNum() {
    std::scoped_lock lock{m_mutex};
    return m_previous_touchnum;
}

void GameController::SetPreviousTouchNum(u8 touchNum) {
    std::scoped_lock lock{m_mutex};
    m_previous_touchnum = touchNum;
}

bool GameController::WasSecondaryTouchReset() {
    std::scoped_lock lock{m_mutex};
    return m_was_secondary_reset;
}

void GameController::UnsetSecondaryTouchResetBool() {
    std::scoped_lock lock{m_mutex};
    m_was_secondary_reset = false;
}

void GameController::SetLastOrientation(Libraries::Pad::OrbisFQuaternion& orientation) {
    std::scoped_lock lock{m_mutex};
    m_orientation = orientation;
}

Libraries::Pad::OrbisFQuaternion GameController::GetLastOrientation() {
    std::scoped_lock lock{m_mutex};
    return m_orientation;
}

std::chrono::steady_clock::time_point GameController::GetLastUpdate() {
    std::scoped_lock lock{m_mutex};
    return m_last_update;
}

void GameController::SetLastUpdate(std::chrono::steady_clock::time_point lastUpdate) {
    std::scoped_lock lock{m_mutex};
    m_last_update = lastUpdate;
}

u32 GameController::Poll() {
    std::scoped_lock lock{m_mutex};
    if (m_connected) {
        auto time = Libraries::Kernel::sceKernelGetProcessTime();
        if (m_states_num == 0) {
            auto diff = (time - m_last_state.time) / 1000;
            if (diff >= 100) {
                AddState(GetLastState());
            }
        } else {
            auto index = (m_first_state - 1 + m_states_num) % MAX_STATES;
            auto diff = (time - m_states[index].time) / 1000;
            if (m_private[index].obtained && diff >= 100) {
                AddState(GetLastState());
            }
        }
    }
    return 100;
}

u8 GameControllers::GetGamepadIndexFromJoystickId(SDL_JoystickID id, const GameControllers& ctrls) {
    for (int i = 0; i < 4; i++) {
        if (ctrls.controllers[i]->m_sdl_gamepad &&
            SDL_GetGamepadID(ctrls.controllers[i]->m_sdl_gamepad) == id) {
            return i;
        }
    }
    LOG_WARNING(Input, "Gamepad with joystick ID {} not found, returning 0 as fallback", id);
    return 0; // fallback - should ideally handle this case better
}

} // namespace Input

namespace GamepadSelect {

int GetDefaultGamepad(SDL_JoystickID* gamepadIDs, int gamepadCount) {
    char GUIDbuf[33];
    if (Config::getDefaultControllerID() != "") {
        for (int i = 0; i < gamepadCount; i++) {
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(gamepadIDs[i]), GUIDbuf, 33);
            std::string currentGUID = std::string(GUIDbuf);
            if (currentGUID == Config::getDefaultControllerID()) {
                return i;
            }
        }
    }
    return -1;
}

std::optional<u8> GetControllerIndexFromUserID(s32 user_id) {
    if (user_id < 1 || user_id > 4)
        return std::nullopt;
    return static_cast<u8>(user_id - 1);
}

int GetIndexfromGUID(SDL_JoystickID* gamepadIDs, int gamepadCount, std::string GUID) {
    char GUIDbuf[33];
    for (int i = 0; i < gamepadCount; i++) {
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(gamepadIDs[i]), GUIDbuf, 33);
        std::string currentGUID = std::string(GUIDbuf);
        if (currentGUID == GUID) {
            return i;
        }
    }
    return 0;
}

std::string GetGUIDString(SDL_JoystickID* gamepadIDs, int index) {
    char GUIDbuf[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(gamepadIDs[index]), GUIDbuf, 33);
    std::string GUID = std::string(GUIDbuf);
    return GUID;
}

std::string GetSelectedGamepad() {
    std::scoped_lock lock(SelectedGamepadMutex);
    return SelectedGamepad;
}

void SetSelectedGamepad(std::string GUID) {
    std::scoped_lock lock(SelectedGamepadMutex);
    SelectedGamepad = GUID;
}

} // namespace GamepadSelect