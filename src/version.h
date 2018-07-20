#pragma once

// Versioning is performed according to the standard at <https://semver.org/>
// Creating a new version should be performed using scripts/bumpversion.py.

#include <string>
#include "version.inc"

std::uint32_t GetVersionInt(int major = LC0_VERSION_MAJOR,
                            int minor = LC0_VERSION_MINOR,
                            int patch = LC0_VERSION_PATCH);

std::string GetVersionStr(int major = LC0_VERSION_MAJOR,
                          int minor = LC0_VERSION_MINOR,
                          int patch = LC0_VERSION_PATCH,
                          const std::string& postfix = LC0_VERSION_POSTFIX);
