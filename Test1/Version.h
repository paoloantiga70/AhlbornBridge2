#pragma once

// ---------------------------------------------------------------
// Single source of truth for the application version.
// Update these values before each release.  The post-build step
// reads APP_VERSION from this header and patches the Inno Setup
// installer script automatically.
// ---------------------------------------------------------------
#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 4
#define APP_VERSION_PATCH 255
#define APP_VERSION "0.4.255"

// GitHub repository for auto-update checks.
// Change these to match your GitHub repository.
#define GITHUB_REPO_OWNER L"paoloantiga70"
#define GITHUB_REPO_NAME  L"AhlbornBridge2"
