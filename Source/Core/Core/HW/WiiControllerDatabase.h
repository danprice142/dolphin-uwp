// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace Core::HW
{
enum class ControllerPreference
{
  Classic,
  GameCube,
  WiiRemoteNunchuk,
};

// Returns the recommended controller preference for a Wii game based on GameTDB compatibility.
ControllerPreference GetWiiControllerPreference(std::string_view game_id);
}  // namespace Core::HW

namespace DolphinUWP
{
using Core::HW::ControllerPreference;
using Core::HW::GetWiiControllerPreference;
}  // namespace DolphinUWP
