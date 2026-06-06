#pragma once

// ---------------------------------------------------------------
// Single source of truth for the application version.
// Update these values before each release.  The post-build step
// reads APP_VERSION from this header and patches the Inno Setup
// installer script automatically.
// ---------------------------------------------------------------
#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 1
#define APP_VERSION_PATCH 79
inline constexpr const	char* APP_VERSION = "1.1.79";

// GitHub repository for auto-update checks.
// Change these to match your GitHub repository.
#define GITHUB_REPO_OWNER L"paoloantiga70"
#define GITHUB_REPO_NAME  L"AhlbornBridge2"
