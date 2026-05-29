# Copilot Instructions

## General Guidelines
- Do not run builds unless the user explicitly asks for one.
- When updating runtime settings, use the user settings file at C:\Users\paolo\AppData\Roaming\AhlbornBridge2\Settings.xml instead of build output copies.
- Respond in English.
- Disable legacy "[MIDI ACTIVITY]" debug log output in MidiInProc; prefer the new MIDI monitor logs instead.
- Format MIDI monitor debug output with a blank line before and after each [MIDI MON] entry for readability.

## Project Guidelines
- Use a Hauptwerk-style Win32 classic preferences UI with tabs for the Settings window instead of the current simple dialog.
- Allow restructuring sections and adding new preferences within the Settings UI.
- Implement the redesign incrementally, starting with a first "Model" tab.
- For Bidule integration, use a dedicated Settings tab, auto-detect the executable with file-picker fallback, and close Bidule automatically when an organ is unloaded. 
- Save Bidule profiles only when an organ is actually loaded; do not trigger Bidule profile save during standby/transient states, and only on real unload cases such as manual close, unload, or FE disconnect.

## MIDI Device Configuration
- Use a separate window for MIDI device configuration.
- Use two treeview pairs for the device-assignment UI: one pair for Inputs (available and assigned) and one pair for Outputs (available and assigned).
- Support drag-and-drop between and within treeviews to assign devices and set the order for multi-device input and output assignments.
- Persist assigned device lists (including order) by device name in Settings.xml; include Hauptwerk output assignments.
- Open all assigned devices at application startup.
- Allow assigned devices to be removable and reorderable in the configuration UI.

### Hauptwerk Output Assignment
- Automatically include the physical MIDI device auto-detected on first installation in Hauptwerk's EnabledMIDIOutputPort list as a required exception.
- Treat the fixed Hauptwerk MIDI output as the same physical console device found during the initial detect (use the same device mapping/identifier, e.g., the device previously recorded as typ=3).
- Keep EnabledMIDIInputPort fixed to the AhlbornBridge Virtual Port (B); do not replace or remove this virtual input mapping when configuring Hauptwerk outputs.
- Exclude bridge/virtual ports (for example, AhlbornBridge Virtual Port) from being written into Hauptwerk's EnabledMIDIOutputPort list; the bridge port must never be added there.
- Do not automatically add input devices that are manually assigned later to the Hauptwerk outputs.
- Allow users to remove or reassign the auto-included device from the Hauptwerk outputs via the configuration UI; reflect changes in persisted Settings.xml.

## Ahlborn Switches Action
- For each stop entry, include the following parameters:
  - `nam`: stop name
  - `h`: channel number
  - `c`: control change number
  - `d`: data value ON
  - `e`: data value OFF
- For Ahlborn Switches button graphics, set badge text as follows:
  - 'PEDALE' for MIDI channel 1
  - 'MANUALE I' for channel 2
  - 'MANUALE II' for channel 3
- For switch-state synchronization, switches must not update when no organ is loaded, and unload reset CC must be ignored only for in-memory restore while avoiding stale ON visuals on the plugin.

## Stream Deck Key State Logic
- OFFLINE must represent pipe-disconnected state (not pipe-connected/no-name state). Stream Deck buttons should show OFFLINE whenever the AhlbornBridge pipe is disconnected, regardless of cached organ names.
- When the pipe is disconnected, the key title text should be blank (no 'Disconnected') and the status dot should use the same offline red color theme.
- Stream Deck status dot mapping: OFFLINE red; pipe connected without Hauptwerk running gray; Hauptwerk running without organ loaded yellow (red border if console connected); Hauptwerk running with organ loaded green (red border if console connected).
- When Hauptwerk is closed but console is connected, the Stream Deck key should be gray with red borders to match status semantics.
- The READY badge must be yellow in the same state as the yellow status dot (Hauptwerk running with no organ loaded), and with a red border when the console is connected.
- When an organ is loaded (VST Link or not), the badge text border must be red if the console is connected; the outer key border must keep its normal color.
- For Stream Deck switch keys, implement toggle mode: each key press flips state (ON to OFF and OFF to ON), not momentary keyDown/keyUp ON/OFF.