/* ================================================================================================
 * File: input.cpp
 * Brief: Gamepad + keyboard input backend, replacing null/in_null.c. Drives a single
 *        GamePad (see pad.h) and maps its state onto the engine: buttons become key
 *        events routed by input focus (menu/console navigation vs. rebindable game
 *        action keys), the right stick rotates the camera and the left stick moves
 *        the player - following the axis handling of the original win32/in_win.c.
 *        The pad's vibration motors play the force feedback from rumble.cpp.
 *
 *        A USB keyboard (see keyboard.h) is optional and gated by the in_keyboard
 *        cvar: when present its keys are forwarded verbatim, so the stock default.cfg
 *        binds work as they do on the PC. The gamepad is unaffected either way, and
 *        both devices can be used at the same time.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/input/keyboard.h"
#include "ps2/input/pad.h"
#include "ps2/input/rumble.h"

// The input backend is client code (the engine's own win32/in_win.c is the same):
// sticks write straight into cl.viewangles and the outgoing usercmd_t, and button
// routing depends on cls.key_dest, so it reaches into the client internals. The
// legacy headers redeclare a few q_common.h functions, hence the pragma.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
extern "C" {
    #include "client/client.h"
}
#pragma GCC diagnostic pop

#include <cmath>
#include <cstdio>

extern "C" {
// Timestamp of the current input frame, maintained by Sys_SendKeyEvents (sys.cpp);
// the engine has no declaration for it (cl_input.c declares its own extern).
extern unsigned sys_frame_time;

// Referenced by the options menu (menu.c). Gates stick movement only - buttons
// keep working regardless, or the menus would become unnavigable.
cvar_t * in_joystick = nullptr;
} // extern "C"

namespace {

// ------------------------------------------------------------------------------------------------
// Button -> key mapping
// ------------------------------------------------------------------------------------------------

// Each pad button sends one of two keys depending on where input is focused when
// it goes down: menus/console/message mode get UI navigation keys, gameplay gets
// JOY/AUX keys driven by the default binds below (rebindable once configs exist).
// START and SELECT behave the same in both contexts: ESCAPE toggles the menu and
// '`' is the hardcoded console toggle in keys.c.
struct ButtonMapping
{
    u16 padButton;           // PAD_* bit
    int uiKey;               // key sent while a menu/the console has focus
    int gameKey;             // key sent while playing
    const char * gameBind;   // default binding for gameKey (nullptr = leave unbound)
};

constexpr ButtonMapping kButtonMap[] = {
    { PAD_UP,       K_UPARROW,    K_AUX5,   "invuse"     },
    { PAD_DOWN,     K_DOWNARROW,  K_AUX6,   "inven"      },
    { PAD_LEFT,     K_LEFTARROW,  K_AUX7,   "invprev"    },
    { PAD_RIGHT,    K_RIGHTARROW, K_AUX8,   "invnext"    },
    { PAD_CROSS,    K_ENTER,      K_JOY1,   "+moveup"    }, // jump
    { PAD_CIRCLE,   K_ESCAPE,     K_JOY2,   "+movedown"  }, // crouch
    { PAD_SQUARE,   K_ENTER,      K_JOY3,   "+use"       },
    { PAD_TRIANGLE, K_ESCAPE,     K_JOY4,   "cmd help"   },
    { PAD_L2,       K_PGUP,       K_AUX2,   "+speed"     },
    { PAD_R2,       K_PGDN,       K_AUX1,   "+attack"    },
    { PAD_R1,       K_AUX4,       K_AUX4,   "weapprev"   },
    { PAD_L1,       K_AUX3,       K_AUX3,   "weapnext"   },
    { PAD_L3,       K_AUX9,       K_AUX9,   nullptr      },
    { PAD_R3,       K_AUX10,      K_AUX10,  "centerview" },
    { PAD_START,    K_ESCAPE,     K_ESCAPE, nullptr      },
    { PAD_SELECT,   '`',          '`',      nullptr      }
};

constexpr int kNumButtons = static_cast<int>(sizeof(kButtonMap) / sizeof(kButtonMap[0]));

// ------------------------------------------------------------------------------------------------
// Backend state
// ------------------------------------------------------------------------------------------------

// The one gamepad, polled once per frame in IN_Frame and queried by the seam below.
static ps2::input::GamePad s_gamepad;

// Edge-detection state for turning the button mask into key up/down events.
static u16 s_oldButtons = 0;

// Key delivered when each button was pressed, so the release sends the same key
// even if input focus changed while the button was held.
static int s_heldKeys[kNumButtons] = {};

// The optional USB keyboard, polled alongside the pad in IN_Frame.
static ps2::input::Keyboard s_keyboard;

// Mirrors in_keyboard, and whether we have already tried to bring the driver up
// (a one-shot: the IOP modules must not be loaded twice, and a failed attempt is
// not worth repeating every frame).
static bool s_keyboardEnabled = false;
static bool s_keyboardInitTried = false;

// Quake keys currently held down via the keyboard, so they can be released if the
// keyboard is switched off mid-press - otherwise a held +attack would stick.
static bool s_keyboardHeld[256] = {};

// Stick tuning (cvar names and defaults follow the original win32 joystick code;
// negate a sensitivity to invert that axis).
static const cvar_t * s_yawSensitivity;
static const cvar_t * s_pitchSensitivity;
static const cvar_t * s_forwardSensitivity;
static const cvar_t * s_sideSensitivity;
static const cvar_t * s_yawThreshold;
static const cvar_t * s_pitchThreshold;
static const cvar_t * s_forwardThreshold;
static const cvar_t * s_sideThreshold;

// Gates the keyboard only; the gamepad keeps working either way.
static const cvar_t * s_inKeyboard;

// ------------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------------

void InstallDefaultBinds()
{
    for (const ButtonMapping & mapping : kButtonMap)
    {
        if (mapping.gameBind == nullptr || keybindings[mapping.gameKey] != nullptr)
        {
            continue; // Nothing to bind, or the user's config already bound this key.
        }
        // Key_SetBinding has a legacy non-const signature but copies the string;
        // go through a local buffer to keep -Wwrite-strings happy.
        char bind[64];
        std::snprintf(bind, sizeof(bind), "%s", mapping.gameBind);
        Key_SetBinding(mapping.gameKey, bind);
    }
}

// Releases every key the keyboard still has down. Called when the keyboard is
// switched off, since no up event will ever arrive for those presses.
void ReleaseHeldKeyboardKeys()
{
    for (int key = 0; key < ps2::ArrayLength(s_keyboardHeld); ++key)
    {
        if (s_keyboardHeld[key])
        {
            s_keyboardHeld[key] = false;
            Key_Event(key, false, sys_frame_time);
        }
    }
}

// Applies the current in_keyboard setting. Switching it on the first time brings
// the driver up - the IOP modules load on demand, so a user who never wants a
// keyboard never pays for one - and switching it off drops any held keys.
void SyncKeyboardEnabled()
{
    const bool enabled = (s_inKeyboard->value != 0.0f);
    if (enabled == s_keyboardEnabled)
    {
        return;
    }
    s_keyboardEnabled = enabled;

    if (!enabled)
    {
        ReleaseHeldKeyboardKeys();
        return;
    }

    if (!s_keyboardInitTried)
    {
        s_keyboardInitTried = true;
        if (s_keyboard.Init())
        {
            Com_Printf("USB keyboard input initialised.\n");
        }
    }
}

} // namespace

extern "C" {

// ------------------------------------------------------------------------------------------------
// IN_Init / IN_Shutdown
// ------------------------------------------------------------------------------------------------

void IN_Init()
{
    in_joystick          = Cvar_Get("in_joystick",            "1",    CVAR_ARCHIVE);
    s_yawSensitivity     = Cvar_Get("joy_yawsensitivity",     "1",    0);
    s_pitchSensitivity   = Cvar_Get("joy_pitchsensitivity",   "1",    0);
    s_forwardSensitivity = Cvar_Get("joy_forwardsensitivity", "1",    0);
    s_sideSensitivity    = Cvar_Get("joy_sidesensitivity",    "1",    0);
    s_yawThreshold       = Cvar_Get("joy_yawthreshold",       "0.15", 0);
    s_pitchThreshold     = Cvar_Get("joy_pitchthreshold",     "0.15", 0);
    s_forwardThreshold   = Cvar_Get("joy_forwardthreshold",   "0.15", 0);
    s_sideThreshold      = Cvar_Get("joy_sidethreshold",      "0.15", 0);
    s_inKeyboard         = Cvar_Get("in_keyboard",            "1",    CVAR_ARCHIVE);

    ps2::input::InitRumble(s_gamepad);

    if (s_gamepad.Init())
    {
        InstallDefaultBinds();
        Com_Printf("Gamepad input initialised.\n");
    }

    // Quiet when no keyboard driver comes up: it is optional hardware, and the
    // pad binds installed above are what the console actually ships with.
    SyncKeyboardEnabled();
}

void IN_Shutdown()
{
    s_gamepad.Shutdown();
    s_keyboard.Shutdown();
}

// ------------------------------------------------------------------------------------------------
// IN_Frame - once per client frame: advance the device state machines and poll them
// ------------------------------------------------------------------------------------------------

void IN_Frame()
{
    s_gamepad.Update();
    ps2::input::UpdateRumble();

    SyncKeyboardEnabled();
    if (s_keyboardEnabled)
    {
        s_keyboard.Update();
    }
}

// ------------------------------------------------------------------------------------------------
// IN_Commands - emit key events for button state changes and keystrokes
// ------------------------------------------------------------------------------------------------

void IN_Commands()
{
    const u16 buttons = s_gamepad.Buttons();
    const u16 changed = static_cast<u16>(buttons ^ s_oldButtons);
    if (changed != 0)
    {
        s_oldButtons = buttons;

        const bool uiFocus = (cls.key_dest != key_game);

        for (int i = 0; i < kNumButtons; ++i)
        {
            const ButtonMapping & mapping = kButtonMap[i];
            if ((changed & mapping.padButton) == 0)
            {
                continue;
            }

            if ((buttons & mapping.padButton) != 0)
            {
                const int key = uiFocus ? mapping.uiKey : mapping.gameKey;
                s_heldKeys[i] = key;
                Key_Event(key, true, sys_frame_time);
            }
            else if (s_heldKeys[i] != 0)
            {
                Key_Event(s_heldKeys[i], false, sys_frame_time);
                s_heldKeys[i] = 0;
            }
        }
    }

    // Keystrokes go straight through: unlike the pad there is no input focus
    // routing to do, since the keys the engine binds are the keys we send.
    if (s_keyboardEnabled)
    {
        const int numEvents = s_keyboard.NumEvents();
        for (int i = 0; i < numEvents; ++i)
        {
            const ps2::input::Keyboard::Event & event = s_keyboard.GetEvent(i);
            s_keyboardHeld[event.key] = event.down;
            Key_Event(event.key, event.down, sys_frame_time);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// IN_Move - sticks: right rotates the camera, left moves the player
// ------------------------------------------------------------------------------------------------

void IN_Move(usercmd_t * cmd)
{
    if (!s_gamepad.AnalogValid() || in_joystick->value == 0.0f)
    {
        return;
    }
    if (cls.key_dest != key_game)
    {
        return; // Sticks shouldn't drive the player while a menu/the console has focus.
    }

    const bool running = (((in_speed.state & 1) ^ static_cast<int>(cl_run->value)) != 0);
    const float speed = running ? 2.0f : 1.0f;
    const float angleSpeed = speed * cls.frametime;

    // Right stick rotates the camera. Stick right = turn right (yaw decreases),
    // stick up = look up (pitch decreases); flip a sensitivity cvar to invert.
    const float yaw = s_gamepad.RightStickX();
    if (std::fabs(yaw) > s_yawThreshold->value)
    {
        cl.viewangles[YAW] -= yaw * s_yawSensitivity->value * angleSpeed * cl_yawspeed->value;
    }

    const float pitch = s_gamepad.RightStickY();
    if (std::fabs(pitch) > s_pitchThreshold->value)
    {
        cl.viewangles[PITCH] += pitch * s_pitchSensitivity->value * angleSpeed * cl_pitchspeed->value;
    }

    // Left stick moves the player. CL_FinishMove clamps the pitch and packs the
    // final angles/moves after IN_Move returns.
    const float forward = s_gamepad.LeftStickY();
    if (std::fabs(forward) > s_forwardThreshold->value)
    {
        cmd->forwardmove = static_cast<short>(static_cast<float>(cmd->forwardmove) -
            (forward * s_forwardSensitivity->value * speed * cl_forwardspeed->value));
    }

    const float side = s_gamepad.LeftStickX();
    if (std::fabs(side) > s_sideThreshold->value)
    {
        cmd->sidemove = static_cast<short>(static_cast<float>(cmd->sidemove) +
            (side * s_sideSensitivity->value * speed * cl_sidespeed->value));
    }
}

// ------------------------------------------------------------------------------------------------
// IN_Activate
// ------------------------------------------------------------------------------------------------

void IN_Activate(qboolean active)
{
    (void)active; // No window focus on the PS2.
}

} // extern "C"
