// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <fmt/ranges.h>
#include <math.h>

#include "common/param_package.h"
#include "common/settings.h"
#include "common/steady_clock.h"
#include "input_common/drivers/mouse.h"

namespace InputCommon {
constexpr float default_panning_sensitivity = 0.0035f;
constexpr float default_stick_sensitivity = 0.0006f;
constexpr float default_deadzone_counterweight = 0.01f;
constexpr float default_motion_panning_sensitivity = 2.5f;
constexpr float default_motion_sensitivity = 0.416f;
constexpr float maximum_rotation_speed = 2.0f;
constexpr float maximum_stick_range = 1.5f;
constexpr int mouse_axis_x = 0;
constexpr int mouse_axis_y = 1;
constexpr int wheel_axis_x = 2;
constexpr int wheel_axis_y = 3;
constexpr PadIdentifier identifier = {
    .guid = Common::UUID{},
    .port = 0,
    .pad = 0,
};

constexpr PadIdentifier motion_identifier = {
    .guid = Common::UUID{},
    .port = 0,
    .pad = 1,
};

constexpr PadIdentifier real_mouse_identifier = {
    .guid = Common::UUID{},
    .port = 1,
    .pad = 0,
};

constexpr PadIdentifier touch_identifier = {
    .guid = Common::UUID{},
    .port = 2,
    .pad = 0,
};

Mouse::Mouse(std::string input_engine_) : InputEngine(std::move(input_engine_)) {
    PreSetController(identifier);
    PreSetController(real_mouse_identifier);
    PreSetController(touch_identifier);
    PreSetController(motion_identifier);

    // Initialize all mouse axis
    PreSetAxis(identifier, mouse_axis_x);
    PreSetAxis(identifier, mouse_axis_y);
    PreSetAxis(identifier, wheel_axis_x);
    PreSetAxis(identifier, wheel_axis_y);
    PreSetAxis(real_mouse_identifier, mouse_axis_x);
    PreSetAxis(real_mouse_identifier, mouse_axis_y);
    PreSetAxis(touch_identifier, mouse_axis_x);
    PreSetAxis(touch_identifier, mouse_axis_y);

    // Initialize variables
    mouse_origin = {};
    last_mouse_position = {};
    wheel_position = {};
    last_mouse_change = {};
    last_motion_change = {};
}

void Mouse::UpdateStickInput(Common::SteadyClock::time_point timestamp) {
    if (IsMousePanningEnabled()) {
        const float raw_length = last_mouse_change.Length();

        if (raw_length <= 0.0001f) {
            SetAxis(identifier, mouse_axis_x, 0.0f);
            SetAxis(identifier, mouse_axis_y, 0.0f);
            return;
        }

        // If mouse stopped moving for more than 15 ms, snap stick to 0 immediately
        // This eliminates the floaty "sliding on ice" inertia artifact.
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(timestamp - last_move_timestamp).count();
        if (elapsed_us > 15000) {
            last_mouse_change = {};
            has_active_mouse_move = false;
            SetAxis(identifier, mouse_axis_x, 0.0f);
            SetAxis(identifier, mouse_axis_y, 0.0f);
            return;
        }

        // Clamp to max stick range (1.0f)
        const float clamped_length = (std::min)(1.0f, raw_length);
        const auto direction = last_mouse_change / raw_length;

        // Deadzone counterweight: remap range [0, 1] into [deadzone_cw, 1.0]
        // Ensures the game engine immediately responds to subtle 1-pixel micro movements
        const float deadzone_cw = (std::min)(0.90f, Settings::values.mouse_panning_deadzone_counterweight.GetValue() * default_deadzone_counterweight);
        const float remapped_length = deadzone_cw + clamped_length * (1.0f - deadzone_cw);

        SetAxis(identifier, mouse_axis_x, direction[0] * remapped_length);
        SetAxis(identifier, mouse_axis_y, -direction[1] * remapped_length);

        // Progressive decay during active motion
        const float decay_strength = Settings::values.mouse_panning_decay_strength.GetValue();
        const float decay = 1.0f - clamped_length * decay_strength * 0.01f;
        const float min_decay = Settings::values.mouse_panning_min_decay.GetValue();
        const float clamped_decay = (std::min)(1.0f - min_decay / 100.0f, decay);
        last_mouse_change *= clamped_decay;
    }
}

void Mouse::UpdateMotionInput(Common::SteadyClock::time_point timestamp) {
    const float sensitivity =
        IsMousePanningEnabled() ? default_motion_panning_sensitivity : default_motion_sensitivity;

    const float rotation_velocity = std::sqrt(last_motion_change[0] * last_motion_change[0] +
                                              last_motion_change[1] * last_motion_change[1]);

    // Clamp rotation speed
    if (rotation_velocity > maximum_rotation_speed / sensitivity && rotation_velocity > 0.0001f) {
        const float multiplier = maximum_rotation_speed / rotation_velocity / sensitivity;
        last_motion_change[0] = last_motion_change[0] * multiplier;
        last_motion_change[1] = last_motion_change[1] * multiplier;
    }

    SetMotion(motion_identifier, 0, BasicMotion{
        .gyro_x = last_motion_change[0] * sensitivity,
        .gyro_y = last_motion_change[1] * sensitivity,
        .gyro_z = last_motion_change[2] * sensitivity,
        .accel_x = 0,
        .accel_y = 0,
        .accel_z = 0,
        .delta_timestamp = u64(std::chrono::duration_cast<std::chrono::microseconds>(timestamp - last_notify_timestamp).count()),
    });

    last_motion_change = {};
}

void Mouse::Move(int x, int y, int center_x, int center_y) {
    if (IsMousePanningEnabled()) {
        auto const mouse_change_int = Common::Vec<int, 2>(x, y) - Common::Vec<int, 2>(center_x, center_y);
        auto const mouse_change = Common::Vec<float, 2>(float(mouse_change_int[0]), float(mouse_change_int[1]));
        const float delta_len = mouse_change.Length();
        if (delta_len <= 0.0001f) {
            return;
        }

        // Velocity ballistics: dynamic acceleration factor for swift turns
        // Subtle micro-movements stay 1:1, fast sweeps ramp up smoothly
        const float accel = 1.0f + (std::min)(delta_len * 0.025f, 2.5f);

        auto const x_sensitivity = Settings::values.mouse_panning_x_sensitivity.GetValue() * default_panning_sensitivity * accel;
        auto const y_sensitivity = Settings::values.mouse_panning_y_sensitivity.GetValue() * default_panning_sensitivity * accel;

        // Direct velocity mapping
        last_mouse_change[0] = mouse_change[0] * x_sensitivity;
        last_mouse_change[1] = mouse_change[1] * y_sensitivity;

        // Feed mouse delta to motion/gyro for 1:1 Zelda Bow/Scope aiming
        last_motion_change += {-mouse_change[1] * x_sensitivity * 12.0f, -mouse_change[0] * y_sensitivity * 12.0f, 0};

        has_active_mouse_move = true;
        last_move_timestamp = Common::SteadyClock::Now();
    } else {
        if (button_pressed) {
            const auto mouse_move = Common::Vec<int, 2>(x, y) - mouse_origin;
            const float x_sensitivity = Settings::values.mouse_panning_x_sensitivity.GetValue() * default_stick_sensitivity;
            const float y_sensitivity = Settings::values.mouse_panning_y_sensitivity.GetValue() * default_stick_sensitivity;
            SetAxis(identifier, mouse_axis_x, float(mouse_move[0]) * x_sensitivity);
            SetAxis(identifier, mouse_axis_y, float(-mouse_move[1]) * y_sensitivity);
            last_motion_change = {
                float(-mouse_move[1]) * x_sensitivity,
                float(-mouse_move[0]) * y_sensitivity,
                last_motion_change[2],
            };
        }
    }
}

void Mouse::NotifyChanged() {
    auto const timestamp = Common::SteadyClock::Now();
    UpdateStickInput(timestamp);
    UpdateMotionInput(timestamp);
    last_notify_timestamp = timestamp;
}

void Mouse::MouseMove(f32 touch_x, f32 touch_y) {
    SetAxis(real_mouse_identifier, mouse_axis_x, touch_x);
    SetAxis(real_mouse_identifier, mouse_axis_y, touch_y);
}

void Mouse::TouchMove(f32 touch_x, f32 touch_y) {
    SetAxis(touch_identifier, mouse_axis_x, touch_x);
    SetAxis(touch_identifier, mouse_axis_y, touch_y);
}

void Mouse::PressButton(int x, int y, MouseButton button) {
    SetButton(identifier, static_cast<int>(button), true);

    // Set initial analog parameters
    mouse_origin = {x, y};
    last_mouse_position = {x, y};
    button_pressed = true;
}

void Mouse::PressMouseButton(MouseButton button) {
    SetButton(real_mouse_identifier, static_cast<int>(button), true);
}

void Mouse::PressTouchButton(f32 touch_x, f32 touch_y, MouseButton button) {
    SetAxis(touch_identifier, mouse_axis_x, touch_x);
    SetAxis(touch_identifier, mouse_axis_y, touch_y);
    SetButton(touch_identifier, static_cast<int>(button), true);
}

void Mouse::ReleaseButton(MouseButton button) {
    SetButton(identifier, static_cast<int>(button), false);
    SetButton(real_mouse_identifier, static_cast<int>(button), false);
    SetButton(touch_identifier, static_cast<int>(button), false);

    if (!IsMousePanningEnabled()) {
        SetAxis(identifier, mouse_axis_x, 0);
        SetAxis(identifier, mouse_axis_y, 0);
    }

    last_motion_change[0] = 0;
    last_motion_change[1] = 0;

    button_pressed = false;
}

void Mouse::MouseWheelChange(int x, int y) {
    wheel_position[0] += x;
    wheel_position[1] += y;
    last_motion_change[2] += static_cast<f32>(y);
    SetAxis(identifier, wheel_axis_x, f32(wheel_position[0]));
    SetAxis(identifier, wheel_axis_y, f32(wheel_position[1]));
}

void Mouse::ReleaseAllButtons() {
    ResetButtonState();
    button_pressed = false;
}

bool Mouse::IsMousePanningEnabled() {
    // Disable mouse panning when a real mouse is connected
    return Settings::values.mouse_panning && !Settings::values.mouse_enabled;
}

std::vector<Common::ParamPackage> Mouse::GetInputDevices() const {
    std::vector<Common::ParamPackage> devices;
    devices.emplace_back(Common::ParamPackage{
        {"engine", GetEngineName()},
        {"display", "Keyboard/Mouse"},
    });
    return devices;
}

AnalogMapping Mouse::GetAnalogMappingForDevice(
    [[maybe_unused]] const Common::ParamPackage& params) {
    // Only overwrite different buttons from default
    AnalogMapping mapping = {};
    Common::ParamPackage right_analog_params;
    right_analog_params.Set("engine", GetEngineName());
    right_analog_params.Set("axis_x", 0);
    right_analog_params.Set("axis_y", 1);
    right_analog_params.Set("threshold", 0.5f);
    right_analog_params.Set("range", 1.0f);
    right_analog_params.Set("deadzone", 0.0f);
    mapping.insert_or_assign(Settings::NativeAnalog::RStick, std::move(right_analog_params));
    return mapping;
}

Common::Input::ButtonNames Mouse::GetUIButtonName(const Common::ParamPackage& params) const {
    const auto button = static_cast<MouseButton>(params.Get("button", 0));
    switch (button) {
    case MouseButton::Left:
        return Common::Input::ButtonNames::ButtonLeft;
    case MouseButton::Right:
        return Common::Input::ButtonNames::ButtonRight;
    case MouseButton::Wheel:
        return Common::Input::ButtonNames::ButtonMouseWheel;
    case MouseButton::Backward:
        return Common::Input::ButtonNames::ButtonBackward;
    case MouseButton::Forward:
        return Common::Input::ButtonNames::ButtonForward;
    case MouseButton::Task:
        return Common::Input::ButtonNames::ButtonTask;
    case MouseButton::Extra:
        return Common::Input::ButtonNames::ButtonExtra;
    case MouseButton::Undefined:
    default:
        return Common::Input::ButtonNames::Undefined;
    }
}

Common::Input::ButtonNames Mouse::GetUIName(const Common::ParamPackage& params) const {
    if (params.Has("button")) {
        return GetUIButtonName(params);
    }
    if (params.Has("axis")) {
        return Common::Input::ButtonNames::Value;
    }
    if (params.Has("axis_x") && params.Has("axis_y") && params.Has("axis_z")) {
        return Common::Input::ButtonNames::Engine;
    }
    if (params.Has("motion")) {
        return Common::Input::ButtonNames::Engine;
    }

    return Common::Input::ButtonNames::Invalid;
}

} // namespace InputCommon
