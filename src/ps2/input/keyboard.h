#pragma once
/* ================================================================================================
 * File: keyboard.h
 * Brief: USB keyboard abstraction over the ps2kbd IOP driver. The Keyboard class owns
 *        the driver bring-up (the usbd + ps2kbd IRX modules) and per-frame polling,
 *        translating the driver's raw USB HID scan codes into Quake key up/down events.
 *        The Quake input seam (input.cpp) drives a single static instance, gated by the
 *        in_keyboard cvar; a keyboard is optional and the gamepad is unaffected by it.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::input {

class Keyboard final
{
public:
    // One key transition, ready to hand to Key_Event.
    struct Event
    {
        int key; // Quake key code: a K_* constant or a lowercase ASCII character
        bool down;
    };

    // Starts the IOP-side keyboard driver and opens it in raw (scan code) mode.
    // Returns false when the driver can't be brought up, after which the keyboard
    // stays silent forever - Update() then reports no events. Note that success
    // only means the driver is running: no keyboard needs to be plugged in.
    bool Init();
    void Shutdown();

    // Drains the driver's queue into the event buffer below, dropping keys with no
    // Quake equivalent. Call exactly once per client frame before reading the events;
    // a frame that isn't a poll reports none (see kIdlePollMsec). Set in_keyboarddebug
    // to echo the raw scan codes as they arrive - which USB usage a physical key sends
    // depends on the keyboard's layout, so that trace is what to reach for when a
    // particular key appears to do nothing.
    void Update();

    // Key transitions collected by the last Update(), in the order they arrived.
    int NumEvents() const { return m_numEvents; }
    const Event & GetEvent(int index) const { return m_events[index]; }

private:
    // A frame's worth of transitions. The driver sends two events per keystroke
    // (down + up) and doesn't auto-repeat in raw mode, so this only ever fills up
    // after a long stall - and then dropping the overflow is the right call anyway.
    static constexpr int kMaxEventsPerFrame = 32;

    // How often the driver is polled. Every poll is a file read - an IOP round trip,
    // which also waits behind any other file I/O in flight - and the driver can't say
    // whether a keyboard is plugged in at all: its read comes back empty either way.
    // So a keyboard nobody is using is polled every kIdlePollMsec, which puts the first
    // key after a quiet spell up to that late, and one in use every frame: until
    // kActiveMsec after the last key, and for as long as any key is held, so letting go
    // after a long hold (running forward) is never the late one.
    static constexpr int kActiveMsec   = 10 * 1000;
    static constexpr int kIdlePollMsec = 250;

    bool m_available = false;
    int m_numEvents = 0;
    Event m_events[kMaxEventsPerFrame] = {};

    bool m_held[256]    = {};    // by raw USB usage, as the driver reports them
    int  m_numHeld      = 0;     // how many of m_held are set
    bool m_keySeen      = false; // whether m_lastKeyMsec holds anything yet
    int  m_lastKeyMsec  = 0;     // Sys_Milliseconds of the last raw event, mapped or not
    int  m_lastPollMsec = 0;     // ... of the last read actually made
};

} // namespace ps2::input
