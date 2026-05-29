#pragma once

// ---------------------------------------------------------------
// Single source of truth for the Stream Deck plugin version.
// Update these values before each release.  The pre-build step
// auto-increments the patch number and patches manifest.json.
// The post-build step patches the Inno Setup installer script.
// ---------------------------------------------------------------
#define PLUGIN_VERSION_MAJOR 1
#define PLUGIN_VERSION_MINOR 0
#define PLUGIN_VERSION_PATCH 83
#define PLUGIN_VERSION "1.0.83"

// GitHub repository for auto-update checks.
// Change these to match your GitHub repository.
#define PLUGIN_GITHUB_REPO_OWNER L"paoloantiga70"
#define PLUGIN_GITHUB_REPO_NAME  L"AhlbornBridgeSD2"
