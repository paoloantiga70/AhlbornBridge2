# Copilot Instructions

## Project Guidelines
- Use a Hauptwerk-style Win32 classic preferences UI with tabs for the Settings window instead of the current simple dialog.
- Allow restructuring sections and adding new preferences within the Settings UI.
- Implement the redesign incrementally, starting with a first "Model" tab.

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
- Allow users to remove or reassign the auto-included device from the Hauptwerk outputs via the configuration UI; reflect changes in persisted Settings.xml