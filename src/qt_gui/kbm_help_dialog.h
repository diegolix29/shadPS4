// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QApplication>
#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

class HelpDialog : public QDialog {
    Q_OBJECT
public:
    explicit HelpDialog(QWidget* parent = nullptr);

private:
    QString quickstart() {
        return
            R"(The keyboard remapping backend, GUI and documentation have been written by kalaposfos

In this section, you will find information about the project, its features and help on setting up your ideal setup.
To view the config file's syntax, check out the Syntax tab, for keybind names, visit Normal Keybinds and Special Bindings, and if you are here to view emulator-wide keybinds, you can find it in the FAQ section.
This project started out because I didn't like the original unchangeable keybinds, but rather than waiting for someone else to do it, I implemented this myself. From the default keybinds, you can clearly tell this was a project built for Bloodborne, but ovbiously you can make adjustments however you like.
)";
    }
    QString faq() {
        return
            R"(Q: What are the emulator-wide keybinds?
A: -F12: Triggers Rdoc capture
-F11: Toggles fullscreen
-F10: Toggles FPS counter
-F9: Toggles mouse capture and mouse input
-F8: Reparses the config file while in-game

Q: How do I change between mouse and controller joystick input, and why is it even required?
A: You can switch between them with F9, and it is required, because mouse input is done with polling, which means mouse movement is checked every frame, and if it didn't move, the code manually sets the emulator's virtual controller to 0 (back to the center), even if other input devices would update it.

Q: What happens if I accidentally make a typo in the config?
A: The code recognises the line as wrong, and skip it, so the rest of the file will get parsed, but that line in question will be treated like a comment line.

Q: I want to bind <key> to <input>, but your code doesn't support <key>!
A: Some keys are intentionally omitted, but if you read the bindings through, and you're sure it is not there and isn't one of the intentionally disabled ones, reach out to me by opening an issue on https://github.com/kalaposfos13/shadPS4 or on Discord (@kalaposfos). 
)";
    }
    QString syntax() {
        return
            R"(This is the full list of currently supported mouse and keyboard inputs, and how to use them.
Emulator-reserved keys: F1 through F12, Insert, PrintScreen, Delete, Home, End, PgUp, PgDown

Syntax (aka how a line can look like):
#Comment line
<controller_button> = <key>;
<controller_button> = <key>, <modifier_key>;
<controller_axis> = <key>;
<controller_axis> = <key>, <modifier_key>;

As you can see, there's two main types of input: single-keyed and key-modifier-key pairs. The first is fairly straightforward, it's just a key input, that can be anything, with the only thing to note here is that all modifier keys are also normal keys, but not every normal key is a modifier.
The second type is also easy to understand, it is exactly what it seems.

Examples:
#Interact
cross = e;
#Heavy attack (in BB)
r2 = leftbutton, lshift;
#Move forward
axis_left_y_minus = w;

You can make a comment line by putting # as the first character.
Whitespace doesn't matter, <button>=<key>; is just as valid as <button> = <key>;
)";
    }
    QString bindings() {
        return
            R"(The following names should be interpreted without the '' around them, and for inputs that have left and right versions, only the left one is shown, but the right can be inferred from that.
Example: 'lshift', 'rshift'

Keyboard:
Alphabet: 'a', 'b', ..., 'z'
Numbers: '0', '1', ..., '9'
Keypad: 'kp0', kp1', ..., 'kp9', 'kpperiod', 'kpcomma', 
        'kpdivide', 'kpmultiply', 'kpdivide', 'kpplus', 'kpminus', 'kpenter'
Punctuation and misc:
        'space', 'comma', 'period', 'question', 'semicolon', 'minus', 'plus', 'lparenthesis', 'lbracket', 'lbrace', 'backslash', 'dash',
        'enter', 'tab', backspace', 'escape'
Arrow keys: 'up', 'down', 'left', 'right'
Modifier keys (these can be used both as normal and modifier keys):
        'lctrl', 'lshift', 'lalt', 'lwin' = 'lmeta', 'none' (the same as not adding it at all)

Mouse:
        'leftbutton', 'rightbutton', 'middlebutton'
    The following wheel inputs cannot be bound to axis input, only button:
        'mousewheelup', 'mousewheeldown', 'mousewheelleft', 'mousewheelright'

Controller (this is not for controller remappings (yet), but rather the buttons and axes the above key will be bound to):
    The same left-right rule still applies here.
    Buttons:
        'triangle', 'circle', 'cross', 'square', 'l1', 'l2', 'l3',
        'options', touchpad', 'up', 'down', 'left', 'right'
    Axes:
        'axis_left_x_plus', 'axis_left_x_minus', 'axis_left_y_plus', 'axis_left_y_minus'
        'axis_right_x_plus', ..., 'axis_right_y_minus'
)";
    }
    QString special() {
        return
            R"(There are some extra bindings you can put into the config file, that don't correspond to a controller input, but rather something else.
You can find these here, with detailed comments, examples and suggestions for most of them.

'leftjoystick_halfmode' and 'rightjoystick_halfmode' = <key>;
    These are a pair of input modifiers, that change the way keyboard button bound axes work. By default, those push the joystick to the max in their respective direction, but if their respective joystick_halfmode modifier value is true, they only push it (wait for it)... halfway. With this, you can change from run to walk in games like Bloodborne.

'mouse_to_joystick' = 'none', 'left' or 'right';
    This binds the mouse movement to either joystick. If it recieves a value that is not 'left' or 'right', it defaults to 'none'.

'mouse_movement_params' = float, float, float;
    (If you don't know what a float is, it is a data type that stores non-whole numbers.)
    This controls mouse-to-joystick input smoothness. Let's break each parameter down:
        1st: mouse_deadzone_offset: this value should have a value between 0 and 1 (It gets clamped to that range anyway), with 0 being no offset and 1 being pushing the joystick to the max in the direction the mouse moved.
            This controls the minimum distance the joystick gets moved, when moving the mouse. If set to 0, it will emulate raw mouse input, which doesn't work very well due to deadzones preventing input if the movement is not large enough.
            It defaults to 0.5.
        2nd: mouse_speed: It's just a multiplier to the mouse input speed, which varies between people, so rather than adjusting the mouse speed globally, they can just do it here.
            It defults to 1.0, which is just about fine for my mouse.
            If you input a negative number, the axis directions get reversed.
        3rd: mouse_speed_offset: This also should be in the 0 to 1 range, with 0 being no offset and 1 being offsetting to the max possible value.
            This is best explained through an example: Let's set mouse_deadzone to 0.5, and this to 0: This means that if we move the mousevery slowly, it still inputs a half-strength joystick input, and if we increase the speed, it would stay that way until we move faster than half the max speed. If we instead set this to 0.25, we now only need to move the mouse faster than the 0.5-0.25=0.25=quarter of the max speed, to get an increase in joystick speed. If we set it to 0.5, then even moving the mouse at 1 pixel per frame will result in a faster-than-minimum speed.
)";
    }
};
