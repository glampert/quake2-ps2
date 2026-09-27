#pragma once
/* ================================================================================================
 * File: pad.h
 * Brief: DualShock gamepad abstraction over libpad. The GamePad class owns the pad
 *        connection lifecycle and per-frame polling, exposing the button mask and
 *        the normalised analog sticks, and drives the two vibration motors. The Quake
 *        input seam (input.cpp) drives a single static instance and maps its state
 *        onto key events and movement; rumble.cpp decides what the motors do.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>
#include <libpad.h> // padButtonStatus + PAD_* button bits

namespace ps2::input {

class GamePad final
{
public:
    // Brings up SIF RPC, loads the IOP pad modules and opens connector 1. Returns
    // false if the gamepad is unavailable, after which the accessors report a
    // neutral, disconnected pad forever.
    bool Init();
    void Shutdown();

    // Advances the connection state machine and, once connected, polls the pad.
    // Call exactly once per client frame before querying the state below.
    void Update();

    // Currently-pressed buttons as an active-high mask of PAD_* bits
    // (0 while disconnected).
    u16 Buttons() const { return m_buttons; }

    // True when the analog sticks carry meaningful data: a DualShock in analog
    // mode that polled successfully this frame. Digital pads and the brief
    // connect/mode-switch window report false.
    bool AnalogValid() const { return m_analogValid; }

    // Analog sticks normalised to [-1, +1] in raw hardware orientation: X grows to
    // the right, Y grows downward. Only meaningful while AnalogValid().
    float LeftStickX() const;
    float LeftStickY() const;
    float RightStickX() const;
    float RightStickY() const;

    // Runs the vibration motors: the small one is either on or off, the large one
    // spins at a speed (0 = stopped). A no-op for pads without motors. Only reaches
    // the IOP when the values change, since each call there is a blocking RPC.
    void SetMotors(bool smallOn, u8 largeSpeed);

private:
    enum class Status : u8
    {
        Unavailable,   // IOP modules or the port failed - pad permanently off
        Disconnected,  // waiting for a pad to connect and stabilise
        SettingMode,   // analog (DualShock) mode requested, awaiting completion
        SettingMotors, // vibration motor mapping requested, awaiting completion
        Ready          // connected and delivering data
    };

    // m_sentSmall value meaning "not known": nothing guarantees what padman drives
    // the motors with after a (re)connect, so the next SetMotors always goes out.
    static constexpr u8 kMotorsUnknown = 0xFF;

    static bool Connected(int state);

    Status m_status = Status::Unavailable;
    padButtonStatus m_data{};    // last good padRead() result
    u16 m_buttons = 0; // active-high pressed mask
    bool m_analogValid = false;

    bool m_hasMotors = false;
    u8 m_sentSmall = kMotorsUnknown; // motor values last sent to the IOP
    u8 m_sentLarge = 0;

    // libpad DMA transfer area: 256 bytes, 64-byte aligned.
    alignas(64) char m_dmaArea[256];
};

} // namespace ps2::input
