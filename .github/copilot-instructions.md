# Copilot Instructions

## Project Guidelines
- Use a Hauptwerk-style Win32 classic preferences UI with tabs for the Settings window instead of the current simple dialog.
- Allow restructuring sections and adding new preferences within the Settings UI.
- Implement the redesign incrementally, starting with a first "Model" tab.

## MIDI Device Configuration
- Use a separate window for MIDI device configuration.
- Use two treeview pairs for the device-assignment UI: one pair for Inputs (available and assigned) and one pair for Outputs (available and assigned).
- Support drag-and-drop between and within treeviews to assign devices and set the order for multi-device input and output assignments.
- Persist assigned device lists (including order) by device name in Settings.xml.
- Open all assigned devices at application startup.
- Allow assigned devices to be removable and reorderable in the configuration UI.