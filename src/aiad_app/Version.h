// The app's version and build. deploy.ps1 increments kAppBuild (and commits it) before every deployment, so each
// package carries its own number: release v<kAppVersion>.<kAppBuild>.
#pragma once

namespace aiad::app
{

inline constexpr const char* kAppVersion = "0.1.0";
inline constexpr int kAppBuild = 0;

}  // namespace aiad::app
