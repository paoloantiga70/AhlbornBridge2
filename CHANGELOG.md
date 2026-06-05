# AhlbornBridge2 Changelog

## 1.0.109
- New 'Process Mgr' tab in Settings: start, stop, install and uninstall the Process Manager service directly from the app.
- AhlbornBridge Virtual Port is now always visible on first launch (no second restart needed).
- Settings window no longer opens automatically on startup unless it is a first install.
- Service uninstall now waits for the service to fully stop before removing it.

## 1.0.100
- Process Manager service now auto-starts when needed.
- Hauptwerk priority policy: REALTIME only when an organ is loaded; HIGH in standby.
- Improved Process Manager diagnostics in startup logs.
- More robust bridge-to-service pipe connectivity and retries.
