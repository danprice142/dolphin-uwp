// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/InputMappingHost.h"
#include "DolphinUWP/WiiControllerDatabase.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <span>
#include <utility>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Common/UWP/Device.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GBAPadEmu.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCKeyboardEmu.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "InputCommon/InputConfig.h"

namespace DolphinUWP
{
namespace
{
using UwpImGuiFrontend::ControllerMappingDraft;
using UwpImGuiFrontend::ControllerPreviewLayout;
using UwpImGuiFrontend::ControllerProfileDraft;
using UwpImGuiFrontend::EmulatedControlDescriptor;
using UwpImGuiFrontend::InputControlKind;
using UwpImGuiFrontend::InputDeviceSettingsDraft;
using UwpImGuiFrontend::InputSettingDraft;
using UwpImGuiFrontend::InputSettingKind;
using UwpImGuiFrontend::MappingSaveResult;
using Section = Common::IniFile::Section;

constexpr int MAX_PLAYERS = 4;
constexpr std::string_view KEYBOARD_DEVICE = "UWP/0/Keyboard";
constexpr std::string_view GAMEPAD_SOURCE = "WGInput";

enum class ControllerType
{
  GameCube,
  WiiRemote,
  WiiNunchuk,
  Classic,
  Guitar,
  Drums,
  Turntable,
  UDraw,
  Drawsome,
  TaikoDrum,
  Shinkansen,
  GameBoyAdvance,
  GameCubeKeyboard,
};

struct TypeInfo
{
  ControllerType type;
  std::string_view id;
  std::string_view label;
  ControllerPreviewLayout layout;
  std::array<std::string_view, 4> face_buttons;
  std::string_view extension;
};

constexpr std::array<TypeInfo, 13> CONTROLLER_TYPES{{
    {ControllerType::GameCube,
     "gamecube",
     "GameCube Controller",
     ControllerPreviewLayout::Generic,
     {"Y", "X", "A", "B"},
     ""},
    {ControllerType::WiiRemote,
     "wii-remote",
     "Wii Remote",
     ControllerPreviewLayout::Remote,
     {"A", "B", "1", "2"},
     "None"},
    {ControllerType::WiiNunchuk,
     "wii-remote-nunchuk",
     "Wii Remote + Nunchuk",
     ControllerPreviewLayout::Remote,
     {"A", "B", "1", "2"},
     "Nunchuk"},
    {ControllerType::Classic,
     "classic-controller",
     "Classic Controller",
     ControllerPreviewLayout::Generic,
     {"X", "A", "B", "Y"},
     "Classic"},
    {ControllerType::Guitar,
     "guitar",
     "Guitar",
     ControllerPreviewLayout::Generic,
     {"Blue", "Red", "Green", "Yellow"},
     "Guitar"},
    {ControllerType::Drums,
     "drums",
     "Drum Kit",
     ControllerPreviewLayout::Generic,
     {"Blue", "Red", "Green", "Yellow"},
     "Drums"},
    {ControllerType::Turntable,
     "turntable",
     "DJ Turntable",
     ControllerPreviewLayout::Generic,
     {"Blue Left", "Red Left", "Green Left", "Euphoria"},
     "Turntable"},
    {ControllerType::UDraw,
     "udraw",
     "uDraw GameTablet",
     ControllerPreviewLayout::Generic,
     {"", "", "", ""},
     "uDraw"},
    {ControllerType::Drawsome,
     "drawsome",
     "Drawsome Tablet",
     ControllerPreviewLayout::Generic,
     {"", "", "", ""},
     "Drawsome"},
    {ControllerType::TaikoDrum,
     "taiko-drum",
     "Taiko Drum",
     ControllerPreviewLayout::Generic,
     {"", "", "Center", ""},
     "TaTaCon"},
    {ControllerType::Shinkansen,
     "shinkansen",
     "Shinkansen Controller",
     ControllerPreviewLayout::Generic,
     {"C", "B", "A", "D"},
     "Shinkansen"},
    {ControllerType::GameBoyAdvance,
     "game-boy-advance",
     "Game Boy Advance",
     ControllerPreviewLayout::Generic,
     {"L", "R", "A", "B"},
     ""},
    {ControllerType::GameCubeKeyboard,
     "gamecube-keyboard",
     "GameCube Keyboard",
     ControllerPreviewLayout::Keyboard,
     {"", "", "", ""},
     ""},
}};

struct GroupMapping
{
  std::string_view owner;
  std::string_view group;
  std::string_view label;
  InputControlKind kind;
};

constexpr std::array GAMECUBE_GROUPS{
    GroupMapping{"", "Buttons", "Buttons", InputControlKind::Button},
    GroupMapping{"", "Main Stick", "Left Stick", InputControlKind::Axis},
    GroupMapping{"", "C-Stick", "Right Stick", InputControlKind::Axis},
    GroupMapping{"", "Triggers", "Triggers", InputControlKind::Trigger},
    GroupMapping{"", "D-Pad", "D-Pad", InputControlKind::Button},
    GroupMapping{"", "Triforce", "Triforce", InputControlKind::Button},
};

constexpr std::array GBA_GROUPS{
    GroupMapping{"", "Buttons", "Buttons", InputControlKind::Button},
    GroupMapping{"", "D-Pad", "D-Pad", InputControlKind::Button},
};

// Every row of keys of the GameCube keyboard is a group named Keys.
constexpr std::array GC_KEYBOARD_GROUPS{
    GroupMapping{"", "Keys", "Keys", InputControlKind::Button},
};

constexpr std::array WII_REMOTE_GROUPS{
    GroupMapping{"", "Buttons", "Buttons", InputControlKind::Button},
    GroupMapping{"", "D-Pad", "D-Pad", InputControlKind::Button},
    GroupMapping{"", "IR", "Point", InputControlKind::Pointer},
    GroupMapping{"", "Swing", "Swing", InputControlKind::Accelerometer},
    GroupMapping{"", "Shake", "Shake", InputControlKind::Accelerometer},
    GroupMapping{"", "Tilt", "Tilt", InputControlKind::Gyroscope},
};

constexpr std::array NUNCHUK_GROUPS{
    GroupMapping{"", "Buttons", "Buttons", InputControlKind::Button},
    GroupMapping{"", "D-Pad", "D-Pad", InputControlKind::Button},
    GroupMapping{"Nunchuk", "Buttons", "Nunchuk Buttons", InputControlKind::Button},
    GroupMapping{"Nunchuk", "Stick", "Nunchuk Stick", InputControlKind::Axis},
    GroupMapping{"", "IR", "Point", InputControlKind::Pointer},
    GroupMapping{"", "Swing", "Swing", InputControlKind::Accelerometer},
    GroupMapping{"", "Shake", "Shake", InputControlKind::Accelerometer},
    GroupMapping{"Nunchuk", "Shake", "Nunchuk Shake", InputControlKind::Accelerometer},
    GroupMapping{"", "Tilt", "Tilt", InputControlKind::Gyroscope},
    GroupMapping{"Nunchuk", "Tilt", "Nunchuk Tilt", InputControlKind::Gyroscope},
};

constexpr std::array CLASSIC_GROUPS{
    GroupMapping{"Classic", "Buttons", "Buttons", InputControlKind::Button},
    GroupMapping{"Classic", "Left Stick", "Left Stick", InputControlKind::Axis},
    GroupMapping{"Classic", "Right Stick", "Right Stick", InputControlKind::Axis},
    GroupMapping{"Classic", "Triggers", "Triggers", InputControlKind::Trigger},
    GroupMapping{"Classic", "D-Pad", "D-Pad", InputControlKind::Button},
};

constexpr std::array GUITAR_GROUPS{
    GroupMapping{"Guitar", "Frets", "Frets", InputControlKind::Button},
    GroupMapping{"Guitar", "Strum", "Strum", InputControlKind::Button},
    GroupMapping{"Guitar", "Buttons", "Buttons", InputControlKind::Button},
    GroupMapping{"Guitar", "Stick", "Stick", InputControlKind::Axis},
    GroupMapping{"Guitar", "Whammy", "Whammy", InputControlKind::Trigger},
    GroupMapping{"Guitar", "Slider Bar", "Slider Bar", InputControlKind::Axis},
    GroupMapping{"", "Buttons", "Wii Remote", InputControlKind::Button},
};

constexpr std::array DRUMS_GROUPS{
    GroupMapping{"Drums", "Pads", "Pads", InputControlKind::Button},
    GroupMapping{"Drums", "Buttons", "Buttons", InputControlKind::Button},
    GroupMapping{"Drums", "Stick", "Stick", InputControlKind::Axis},
    GroupMapping{"", "Buttons", "Wii Remote", InputControlKind::Button},
};

constexpr std::array TURNTABLE_GROUPS{
    GroupMapping{"Turntable", "Buttons", "Buttons", InputControlKind::Button},
    GroupMapping{"Turntable", "Table Left", "Left Table", InputControlKind::Axis},
    GroupMapping{"Turntable", "Table Right", "Right Table", InputControlKind::Axis},
    GroupMapping{"Turntable", "Crossfade", "Crossfade", InputControlKind::Axis},
    GroupMapping{"Turntable", "Effect", "Effect", InputControlKind::Axis},
    GroupMapping{"Turntable", "Stick", "Stick", InputControlKind::Axis},
    GroupMapping{"", "Buttons", "Wii Remote", InputControlKind::Button},
};

constexpr std::array UDRAW_GROUPS{
    GroupMapping{"uDraw", "Buttons", "Buttons", InputControlKind::Button},
    GroupMapping{"uDraw", "Stylus", "Stylus", InputControlKind::Axis},
    GroupMapping{"uDraw", "Touch", "Touch", InputControlKind::Trigger},
    GroupMapping{"", "Buttons", "Wii Remote", InputControlKind::Button},
};

constexpr std::array DRAWSOME_GROUPS{
    GroupMapping{"Drawsome", "Stylus", "Stylus", InputControlKind::Axis},
    GroupMapping{"Drawsome", "Touch", "Touch", InputControlKind::Trigger},
    GroupMapping{"", "Buttons", "Wii Remote", InputControlKind::Button},
};

constexpr std::array TAIKO_DRUM_GROUPS{
    GroupMapping{"TaTaCon", "Center", "Center", InputControlKind::Button},
    GroupMapping{"TaTaCon", "Rim", "Rim", InputControlKind::Button},
    GroupMapping{"", "Buttons", "Wii Remote", InputControlKind::Button},
};

constexpr std::array SHINKANSEN_GROUPS{
    GroupMapping{"Shinkansen", "Buttons", "Buttons", InputControlKind::Button},
    GroupMapping{"Shinkansen", "Levers", "Levers", InputControlKind::Trigger},
    GroupMapping{"", "Buttons", "Wii Remote", InputControlKind::Button},
};

struct Binding
{
  std::string_view key;
  std::string_view expression;
};

constexpr std::array GBA_GAMEPAD{
    Binding{"Buttons/A", "`Button A`"}, Binding{"Buttons/B", "`Button B`"},
    Binding{"Buttons/L", "`Bumper L`"}, Binding{"Buttons/R", "`Bumper R`"},
    Binding{"Buttons/SELECT", "View"},  Binding{"Buttons/START", "Menu"},
    Binding{"D-Pad/Up", "`Pad N`"},     Binding{"D-Pad/Down", "`Pad S`"},
    Binding{"D-Pad/Left", "`Pad W`"},   Binding{"D-Pad/Right", "`Pad E`"},
};

constexpr std::array GAMECUBE_GAMEPAD{
    Binding{"Buttons/A", "`Button A`"},
    Binding{"Buttons/B", "`Button X`"},
    Binding{"Buttons/X", "`Button B`"},
    Binding{"Buttons/Y", "`Button Y`"},
    Binding{"Buttons/Z", "`Bumper R`"},
    Binding{"Buttons/Start", "Menu"},
    Binding{"Main Stick/Up", "`Left Y+`"},
    Binding{"Main Stick/Down", "`Left Y-`"},
    Binding{"Main Stick/Left", "`Left X-`"},
    Binding{"Main Stick/Right", "`Left X+`"},
    Binding{"C-Stick/Up", "`Right Y+`"},
    Binding{"C-Stick/Down", "`Right Y-`"},
    Binding{"C-Stick/Left", "`Right X-`"},
    Binding{"C-Stick/Right", "`Right X+`"},
    Binding{"Triggers/L", "`Trigger L`| `Bumper L`"},
    Binding{"Triggers/R", "`Trigger R`"},
    Binding{"Triggers/L-Analog", "`Trigger L`"},
    Binding{"Triggers/R-Analog", "`Trigger R`"},
    Binding{"D-Pad/Up", "`Pad N`"},
    Binding{"D-Pad/Down", "`Pad S`"},
    Binding{"D-Pad/Left", "`Pad W`"},
    Binding{"D-Pad/Right", "`Pad E`"},
    Binding{"Triforce/Coin", "View"},
    Binding{"Triforce/Service", "`Thumb L`"},
    Binding{"Triforce/Test", "`Thumb R`"},
    Binding{"Rumble/Motor", "`Motor L` | `Motor R`"},
};

constexpr std::array WII_REMOTE_GAMEPAD{
    Binding{"Buttons/A", "`Button A`"},
    Binding{"Buttons/B", "`Trigger R`"},
    Binding{"Buttons/1", "`Button Y`"},
    Binding{"Buttons/2", "`Button B`"},
    Binding{"Buttons/-", "View"},
    Binding{"Buttons/+", "Menu"},
    Binding{"Buttons/Home", "`Thumb R`"},
    Binding{"D-Pad/Up", "`Pad N`"},
    Binding{"D-Pad/Down", "`Pad S`"},
    Binding{"D-Pad/Left", "`Pad W`"},
    Binding{"D-Pad/Right", "`Pad E`"},
    Binding{"IR/Up", "`Right Y+`"},
    Binding{"IR/Down", "`Right Y-`"},
    Binding{"IR/Left", "`Right X-`"},
    Binding{"IR/Right", "`Right X+`"},
    Binding{"IR/Relative Input", "True"},
    Binding{"IR/Dead Zone", "10.00"},
    Binding{"IR/Total Yaw", "37.50"},
    Binding{"IR/Total Pitch", "30.00"},
    Binding{"Shake/X", "`Button X`"},
    Binding{"Shake/Y", "`Button X`"},
    Binding{"Shake/Z", "`Button X`"},
    Binding{"Tilt/Forward", "`Left Y+`"},
    Binding{"Tilt/Backward", "`Left Y-`"},
    Binding{"Tilt/Left", "`Left X-`"},
    Binding{"Tilt/Right", "`Left X+`"},
    Binding{"Swing/Forward", "`Trigger L`"},
    Binding{"Swing/Up", "`Bumper L`"},
    Binding{"Rumble/Motor", "`Motor L` | `Motor R`"},
};

constexpr std::array NUNCHUK_GAMEPAD{
    Binding{"Buttons/A", "`Button A`"},
    Binding{"Buttons/B", "`Trigger R`"},
    Binding{"Buttons/1", "`Button Y`"},
    Binding{"Buttons/2", "`Button B`"},
    Binding{"Buttons/-", "View"},
    Binding{"Buttons/+", "Menu"},
    Binding{"Buttons/Home", "`Thumb L`"},
    Binding{"D-Pad/Up", "`Pad N`"},
    Binding{"D-Pad/Down", "`Pad S`"},
    Binding{"D-Pad/Left", "`Pad W`"},
    Binding{"D-Pad/Right", "`Pad E`"},
    Binding{"IR/Up", "!toggle(`Thumb R`) & `Right Y+`"},
    Binding{"IR/Down", "!toggle(`Thumb R`) & `Right Y-`"},
    Binding{"IR/Left", "!toggle(`Thumb R`) & `Right X-`"},
    Binding{"IR/Right", "!toggle(`Thumb R`) & `Right X+`"},
    Binding{"IR/Hide", "toggle(`Thumb R`)"},
    Binding{"IR/Relative Input", "True"},
    Binding{"IR/Dead Zone", "10.00"},
    Binding{"IR/Total Yaw", "37.50"},
    Binding{"IR/Total Pitch", "30.00"},
    Binding{"Shake/X", "`Button X`"},
    Binding{"Shake/Y", "`Button X`"},
    Binding{"Shake/Z", "`Button X`"},
    Binding{"Tilt/Forward", "toggle(`Thumb R`) & `Right Y+`"},
    Binding{"Tilt/Backward", "toggle(`Thumb R`) & `Right Y-`"},
    Binding{"Tilt/Left", "toggle(`Thumb R`) & `Right X-`"},
    Binding{"Tilt/Right", "toggle(`Thumb R`) & `Right X+`"},
    Binding{"Nunchuk/Buttons/C", "`Bumper L`"},
    Binding{"Nunchuk/Buttons/Z", "`Trigger L`"},
    Binding{"Nunchuk/Stick/Up", "`Left Y+`"},
    Binding{"Nunchuk/Stick/Down", "`Left Y-`"},
    Binding{"Nunchuk/Stick/Left", "`Left X-`"},
    Binding{"Nunchuk/Stick/Right", "`Left X+`"},
    Binding{"Nunchuk/Shake/X", "`Bumper R`"},
    Binding{"Nunchuk/Shake/Y", "`Bumper R`"},
    Binding{"Nunchuk/Shake/Z", "`Bumper R`"},
    Binding{"Rumble/Motor", "`Motor L` | `Motor R`"},
};

constexpr std::array CLASSIC_GAMEPAD{
    Binding{"Classic/Buttons/A", "`Button B`"},
    Binding{"Classic/Buttons/B", "`Button A`"},
    Binding{"Classic/Buttons/X", "`Button Y`"},
    Binding{"Classic/Buttons/Y", "`Button X`"},
    Binding{"Classic/Buttons/ZL", "`Trigger L`"},
    Binding{"Classic/Buttons/ZR", "`Trigger R`"},
    Binding{"Classic/Buttons/-", "View"},
    Binding{"Classic/Buttons/+", "Menu"},
    Binding{"Classic/Buttons/Home", "`Thumb R`"},
    Binding{"Classic/Left Stick/Up", "`Left Y+`"},
    Binding{"Classic/Left Stick/Down", "`Left Y-`"},
    Binding{"Classic/Left Stick/Left", "`Left X-`"},
    Binding{"Classic/Left Stick/Right", "`Left X+`"},
    Binding{"Classic/Right Stick/Up", "`Right Y+`"},
    Binding{"Classic/Right Stick/Down", "`Right Y-`"},
    Binding{"Classic/Right Stick/Left", "`Right X-`"},
    Binding{"Classic/Right Stick/Right", "`Right X+`"},
    Binding{"Classic/Triggers/L", "`Bumper L`"},
    Binding{"Classic/Triggers/R", "`Bumper R`"},
    Binding{"Classic/Triggers/L-Analog", "`Bumper L`"},
    Binding{"Classic/Triggers/R-Analog", "`Bumper R`"},
    Binding{"Classic/D-Pad/Up", "`Pad N`"},
    Binding{"Classic/D-Pad/Down", "`Pad S`"},
    Binding{"Classic/D-Pad/Left", "`Pad W`"},
    Binding{"Classic/D-Pad/Right", "`Pad E`"},
    Binding{"Rumble/Motor", "`Motor L` | `Motor R`"},
};

constexpr std::array GUITAR_GAMEPAD{
    Binding{"Guitar/Frets/Green", "`Button A`"},  Binding{"Guitar/Frets/Red", "`Button B`"},
    Binding{"Guitar/Frets/Yellow", "`Button Y`"}, Binding{"Guitar/Frets/Blue", "`Button X`"},
    Binding{"Guitar/Frets/Orange", "`Bumper L`"}, Binding{"Guitar/Strum/Up", "`Pad N`"},
    Binding{"Guitar/Strum/Down", "`Pad S`"},      Binding{"Guitar/Buttons/-", "View"},
    Binding{"Guitar/Buttons/+", "Menu"},          Binding{"Guitar/Stick/Up", "`Left Y+`"},
    Binding{"Guitar/Stick/Down", "`Left Y-`"},    Binding{"Guitar/Stick/Left", "`Left X-`"},
    Binding{"Guitar/Stick/Right", "`Left X+`"},   Binding{"Guitar/Whammy/Bar", "`Trigger R`"},
    Binding{"Buttons/Home", "`Thumb R`"},
};

constexpr std::array DRUMS_GAMEPAD{
    Binding{"Drums/Pads/Red", "`Button B`"},   Binding{"Drums/Pads/Yellow", "`Button Y`"},
    Binding{"Drums/Pads/Blue", "`Button X`"},  Binding{"Drums/Pads/Orange", "`Bumper R`"},
    Binding{"Drums/Pads/Green", "`Button A`"}, Binding{"Drums/Pads/Bass", "`Trigger L`"},
    Binding{"Drums/Buttons/-", "View"},        Binding{"Drums/Buttons/+", "Menu"},
    Binding{"Drums/Stick/Up", "`Left Y+`"},    Binding{"Drums/Stick/Down", "`Left Y-`"},
    Binding{"Drums/Stick/Left", "`Left X-`"},  Binding{"Drums/Stick/Right", "`Left X+`"},
    Binding{"Buttons/Home", "`Thumb R`"},
};

constexpr std::array TURNTABLE_GAMEPAD{
    Binding{"Turntable/Buttons/Green Left", "`Button A`"},
    Binding{"Turntable/Buttons/Red Left", "`Button B`"},
    Binding{"Turntable/Buttons/Blue Left", "`Button X`"},
    Binding{"Turntable/Buttons/Green Right", "`Pad S`"},
    Binding{"Turntable/Buttons/Red Right", "`Pad E`"},
    Binding{"Turntable/Buttons/Blue Right", "`Pad W`"},
    Binding{"Turntable/Buttons/-", "View"},
    Binding{"Turntable/Buttons/+", "Menu"},
    Binding{"Turntable/Buttons/Euphoria", "`Button Y`"},
    Binding{"Turntable/Table Left/Left", "`Left X-`"},
    Binding{"Turntable/Table Left/Right", "`Left X+`"},
    Binding{"Turntable/Table Right/Left", "`Right X-`"},
    Binding{"Turntable/Table Right/Right", "`Right X+`"},
    Binding{"Turntable/Crossfade/Left", "`Bumper L`"},
    Binding{"Turntable/Crossfade/Right", "`Bumper R`"},
    Binding{"Turntable/Effect/Left", "`Trigger L`"},
    Binding{"Turntable/Effect/Right", "`Trigger R`"},
    Binding{"Buttons/Home", "`Thumb R`"},
};

constexpr std::array UDRAW_GAMEPAD{
    Binding{"uDraw/Buttons/Rocker Up", "`Bumper R`"},
    Binding{"uDraw/Buttons/Rocker Down", "`Bumper L`"},
    Binding{"uDraw/Stylus/Up", "`Left Y+`"},
    Binding{"uDraw/Stylus/Down", "`Left Y-`"},
    Binding{"uDraw/Stylus/Left", "`Left X-`"},
    Binding{"uDraw/Stylus/Right", "`Left X+`"},
    Binding{"uDraw/Touch/Pressure", "`Trigger R`"},
    Binding{"uDraw/Touch/Lift", "`Trigger L`"},
    Binding{"Buttons/A", "`Button B`"},
    Binding{"Buttons/B", "`Button X`"},
    Binding{"Buttons/+", "Menu"},
    Binding{"Buttons/-", "View"},
    Binding{"Buttons/Home", "`Thumb R`"},
};

constexpr std::array DRAWSOME_GAMEPAD{
    Binding{"Drawsome/Stylus/Up", "`Left Y+`"},
    Binding{"Drawsome/Stylus/Down", "`Left Y-`"},
    Binding{"Drawsome/Stylus/Left", "`Left X-`"},
    Binding{"Drawsome/Stylus/Right", "`Left X+`"},
    Binding{"Drawsome/Touch/Pressure", "`Trigger R`"},
    Binding{"Drawsome/Touch/Lift", "`Trigger L`"},
    Binding{"Buttons/A", "`Button A`"},
    Binding{"Buttons/B", "`Button B`"},
    Binding{"Buttons/+", "Menu"},
    Binding{"Buttons/-", "View"},
    Binding{"Buttons/Home", "`Thumb R`"},
};

constexpr std::array TAIKO_DRUM_GAMEPAD{
    Binding{"TaTaCon/Center/Left", "`Pad S`"},
    Binding{"TaTaCon/Center/Right", "`Button A`"},
    Binding{"TaTaCon/Rim/Left", "`Bumper L`"},
    Binding{"TaTaCon/Rim/Right", "`Bumper R`"},
    Binding{"Buttons/A", "`Button B`"},
    Binding{"Buttons/+", "Menu"},
    Binding{"Buttons/-", "View"},
    Binding{"Buttons/Home", "`Thumb R`"},
};

constexpr std::array SHINKANSEN_GAMEPAD{
    Binding{"Shinkansen/Buttons/Up", "`Pad N`"},   Binding{"Shinkansen/Buttons/Down", "`Pad S`"},
    Binding{"Shinkansen/Buttons/Left", "`Pad W`"}, Binding{"Shinkansen/Buttons/Right", "`Pad E`"},
    Binding{"Shinkansen/Buttons/A", "`Button A`"}, Binding{"Shinkansen/Buttons/B", "`Button B`"},
    Binding{"Shinkansen/Buttons/C", "`Button X`"}, Binding{"Shinkansen/Buttons/D", "`Button Y`"},
    Binding{"Shinkansen/Buttons/SELECT", "View"},  Binding{"Shinkansen/Buttons/START", "Menu"},
    Binding{"Shinkansen/Levers/L", "`Trigger L`"}, Binding{"Shinkansen/Levers/R", "`Trigger R`"},
    Binding{"Buttons/Home", "`Thumb R`"},
};

constexpr std::array WII_REMOTE_KEYBOARD{
    Binding{"Buttons/A", "X"},         Binding{"Buttons/B", "Z"},
    Binding{"Buttons/1", "C"},         Binding{"Buttons/2", "V"},
    Binding{"Buttons/-", "Q"},         Binding{"Buttons/+", "E"},
    Binding{"Buttons/Home", "RETURN"}, Binding{"D-Pad/Up", "UP"},
    Binding{"D-Pad/Down", "DOWN"},     Binding{"D-Pad/Left", "LEFT"},
    Binding{"D-Pad/Right", "RIGHT"},   Binding{"IR/Up", "I"},
    Binding{"IR/Down", "K"},           Binding{"IR/Left", "J"},
    Binding{"IR/Right", "L"},          Binding{"Shake/X", "R"},
    Binding{"Shake/Y", "R"},           Binding{"Shake/Z", "R"},
};

constexpr std::array WII_REMOTE_MOUSE{
    Binding{"Buttons/A", "X | `UWP/0/Mouse:Click 0`"},
    Binding{"Buttons/B", "Z | `UWP/0/Mouse:Click 1`"},
    Binding{"IR/Up", "`UWP/0/Mouse:Cursor Y-`"},
    Binding{"IR/Down", "`UWP/0/Mouse:Cursor Y+`"},
    Binding{"IR/Left", "`UWP/0/Mouse:Cursor X-`"},
    Binding{"IR/Right", "`UWP/0/Mouse:Cursor X+`"},
};

constexpr std::array NUNCHUK_KEYBOARD{
    Binding{"Buttons/A", "X"},
    Binding{"Buttons/B", "Z"},
    Binding{"Buttons/1", "C"},
    Binding{"Buttons/2", "V"},
    Binding{"Buttons/-", "Q"},
    Binding{"Buttons/+", "E"},
    Binding{"Buttons/Home", "RETURN"},
    Binding{"D-Pad/Up", "T"},
    Binding{"D-Pad/Down", "G"},
    Binding{"D-Pad/Left", "F"},
    Binding{"D-Pad/Right", "H"},
    Binding{"IR/Up", "I"},
    Binding{"IR/Down", "K"},
    Binding{"IR/Left", "J"},
    Binding{"IR/Right", "L"},
    Binding{"Shake/X", "R"},
    Binding{"Shake/Y", "R"},
    Binding{"Shake/Z", "R"},
    Binding{"Nunchuk/Buttons/C", "LSHIFT"},
    Binding{"Nunchuk/Buttons/Z", "SPACE"},
    Binding{"Nunchuk/Stick/Up", "UP"},
    Binding{"Nunchuk/Stick/Down", "DOWN"},
    Binding{"Nunchuk/Stick/Left", "LEFT"},
    Binding{"Nunchuk/Stick/Right", "RIGHT"},
};

constexpr std::array CLASSIC_KEYBOARD{
    Binding{"Classic/Buttons/A", "X"},
    Binding{"Classic/Buttons/B", "Z"},
    Binding{"Classic/Buttons/X", "S"},
    Binding{"Classic/Buttons/Y", "A"},
    Binding{"Classic/Buttons/ZL", "Q"},
    Binding{"Classic/Buttons/ZR", "W"},
    Binding{"Classic/Buttons/-", "N"},
    Binding{"Classic/Buttons/+", "M"},
    Binding{"Classic/Buttons/Home", "RETURN"},
    Binding{"Classic/Left Stick/Up", "UP"},
    Binding{"Classic/Left Stick/Down", "DOWN"},
    Binding{"Classic/Left Stick/Left", "LEFT"},
    Binding{"Classic/Left Stick/Right", "RIGHT"},
    Binding{"Classic/Right Stick/Up", "I"},
    Binding{"Classic/Right Stick/Down", "K"},
    Binding{"Classic/Right Stick/Left", "J"},
    Binding{"Classic/Right Stick/Right", "L"},
    Binding{"Classic/Triggers/L", "E"},
    Binding{"Classic/Triggers/R", "R"},
    Binding{"Classic/D-Pad/Up", "T"},
    Binding{"Classic/D-Pad/Down", "G"},
    Binding{"Classic/D-Pad/Left", "F"},
    Binding{"Classic/D-Pad/Right", "H"},
};

struct ControllerSettingSpec
{
  std::string_view key;
  std::string_view label;
  std::string_view group;
};

constexpr std::array GAMECUBE_SETTINGS{
    ControllerSettingSpec{"Options/Always Connected", "Always connected", "Controller"},
    ControllerSettingSpec{"Triggers/Threshold", "Trigger threshold", "Controller"},
    ControllerSettingSpec{"Main Stick/Dead Zone", "Left stick dead zone", "Tuning"},
    ControllerSettingSpec{"C-Stick/Dead Zone", "Right stick dead zone", "Tuning"},
    ControllerSettingSpec{"Triggers/Dead Zone", "Trigger dead zone", "Tuning"},
};

constexpr std::array WII_REMOTE_SETTINGS{
    ControllerSettingSpec{"Options/Sideways Wiimote", "Sideways Wii Remote", "Controller"},
    ControllerSettingSpec{"Options/Upright Wiimote", "Upright Wii Remote", "Controller"},
    ControllerSettingSpec{"Extension/Attach MotionPlus", "Attach MotionPlus", "Controller"},
    ControllerSettingSpec{"IR/Auto-Hide", "Auto-hide pointer", "Pointer"},
    ControllerSettingSpec{"IR/Vertical Offset", "Pointer vertical offset", "Pointer"},
    ControllerSettingSpec{"IR/Total Yaw", "Pointer width", "Pointer"},
    ControllerSettingSpec{"IR/Total Pitch", "Pointer height", "Pointer"},
    ControllerSettingSpec{"Tilt/Angle", "Tilt angle", "Tuning"},
};

constexpr std::array NUNCHUK_SETTINGS{
    ControllerSettingSpec{"Options/Sideways Wiimote", "Sideways Wii Remote", "Controller"},
    ControllerSettingSpec{"Options/Upright Wiimote", "Upright Wii Remote", "Controller"},
    ControllerSettingSpec{"Extension/Attach MotionPlus", "Attach MotionPlus", "Controller"},
    ControllerSettingSpec{"IR/Auto-Hide", "Auto-hide pointer", "Pointer"},
    ControllerSettingSpec{"IR/Vertical Offset", "Pointer vertical offset", "Pointer"},
    ControllerSettingSpec{"IR/Total Yaw", "Pointer width", "Pointer"},
    ControllerSettingSpec{"IR/Total Pitch", "Pointer height", "Pointer"},
    ControllerSettingSpec{"Tilt/Angle", "Tilt angle", "Tuning"},
    ControllerSettingSpec{"Nunchuk/Stick/Dead Zone", "Nunchuk stick dead zone", "Tuning"},
};

constexpr std::array CLASSIC_SETTINGS{
    ControllerSettingSpec{"Extension/Attach MotionPlus", "Attach MotionPlus", "Controller"},
    ControllerSettingSpec{"Classic/Triggers/Threshold", "Trigger threshold", "Controller"},
    ControllerSettingSpec{"Classic/Left Stick/Dead Zone", "Left stick dead zone", "Tuning"},
    ControllerSettingSpec{"Classic/Right Stick/Dead Zone", "Right stick dead zone", "Tuning"},
};

constexpr std::array GUITAR_SETTINGS{
    ControllerSettingSpec{"Guitar/Stick/Dead Zone", "Stick dead zone", "Tuning"},
    ControllerSettingSpec{"Guitar/Whammy/Dead Zone", "Whammy dead zone", "Tuning"},
};

struct StickKeys
{
  std::string_view left;
  std::string_view right;
  std::string_view triggers;
};

StickKeys GetStickKeys(ControllerType type)
{
  switch (type)
  {
  case ControllerType::GameCube:
    return {"Main Stick", "C-Stick", "Triggers"};
  case ControllerType::WiiNunchuk:
    return {"Nunchuk/Stick", "", ""};
  case ControllerType::Classic:
    return {"Classic/Left Stick", "Classic/Right Stick", "Classic/Triggers"};
  case ControllerType::Guitar:
    return {"Guitar/Stick", "", "Guitar/Whammy"};
  case ControllerType::Drums:
    return {"Drums/Stick", "", ""};
  case ControllerType::Turntable:
    return {"Turntable/Stick", "", ""};
  case ControllerType::UDraw:
    return {"uDraw/Stylus", "", "uDraw/Touch"};
  case ControllerType::Drawsome:
    return {"Drawsome/Stylus", "", "Drawsome/Touch"};
  default:
    return {};
  }
}

const TypeInfo& GetTypeInfo(ControllerType type)
{
  return *std::ranges::find(CONTROLLER_TYPES, type, &TypeInfo::type);
}

std::optional<ControllerType> FindType(std::string_view id)
{
  const auto found = std::ranges::find(CONTROLLER_TYPES, id, &TypeInfo::id);
  if (found == CONTROLLER_TYPES.end())
    return std::nullopt;
  return found->type;
}

bool IsWii(ControllerType type)
{
  return type != ControllerType::GameCube && type != ControllerType::GameBoyAdvance &&
         type != ControllerType::GameCubeKeyboard;
}

bool HasPointer(ControllerType type)
{
  return type == ControllerType::WiiRemote || type == ControllerType::WiiNunchuk;
}

std::span<const GroupMapping> GetGroups(ControllerType type)
{
  switch (type)
  {
  case ControllerType::GameCube:
    return GAMECUBE_GROUPS;
  case ControllerType::GameBoyAdvance:
    return GBA_GROUPS;
  case ControllerType::GameCubeKeyboard:
    return GC_KEYBOARD_GROUPS;
  case ControllerType::WiiRemote:
    return WII_REMOTE_GROUPS;
  case ControllerType::WiiNunchuk:
    return NUNCHUK_GROUPS;
  case ControllerType::Classic:
    return CLASSIC_GROUPS;
  case ControllerType::Guitar:
    return GUITAR_GROUPS;
  case ControllerType::Drums:
    return DRUMS_GROUPS;
  case ControllerType::Turntable:
    return TURNTABLE_GROUPS;
  case ControllerType::UDraw:
    return UDRAW_GROUPS;
  case ControllerType::Drawsome:
    return DRAWSOME_GROUPS;
  case ControllerType::TaikoDrum:
    return TAIKO_DRUM_GROUPS;
  case ControllerType::Shinkansen:
    return SHINKANSEN_GROUPS;
  }
  return {};
}

std::span<const Binding> GetGamepadBindings(ControllerType type)
{
  switch (type)
  {
  case ControllerType::GameCube:
    return GAMECUBE_GAMEPAD;
  case ControllerType::GameBoyAdvance:
    return GBA_GAMEPAD;
  case ControllerType::WiiRemote:
    return WII_REMOTE_GAMEPAD;
  case ControllerType::WiiNunchuk:
    return NUNCHUK_GAMEPAD;
  case ControllerType::Classic:
    return CLASSIC_GAMEPAD;
  case ControllerType::Guitar:
    return GUITAR_GAMEPAD;
  case ControllerType::Drums:
    return DRUMS_GAMEPAD;
  case ControllerType::Turntable:
    return TURNTABLE_GAMEPAD;
  case ControllerType::UDraw:
    return UDRAW_GAMEPAD;
  case ControllerType::Drawsome:
    return DRAWSOME_GAMEPAD;
  case ControllerType::TaikoDrum:
    return TAIKO_DRUM_GAMEPAD;
  case ControllerType::Shinkansen:
    return SHINKANSEN_GAMEPAD;
  default:
    return {};
  }
}

std::span<const Binding> GetKeyboardBindings(ControllerType type)
{
  switch (type)
  {
  case ControllerType::WiiRemote:
    return WII_REMOTE_KEYBOARD;
  case ControllerType::WiiNunchuk:
    return NUNCHUK_KEYBOARD;
  case ControllerType::Classic:
    return CLASSIC_KEYBOARD;
  default:
    return {};
  }
}

std::span<const ControllerSettingSpec> GetSettingSpecs(ControllerType type)
{
  switch (type)
  {
  case ControllerType::GameCube:
    return GAMECUBE_SETTINGS;
  case ControllerType::Classic:
    return CLASSIC_SETTINGS;
  case ControllerType::WiiRemote:
    return WII_REMOTE_SETTINGS;
  case ControllerType::WiiNunchuk:
    return NUNCHUK_SETTINGS;
  case ControllerType::Guitar:
    return GUITAR_SETTINGS;
  default:
    return {};
  }
}

InputConfig* GetInputConfig(ControllerType type)
{
  if (type == ControllerType::GameBoyAdvance)
    return Pad::GetGBAConfig();
  if (type == ControllerType::GameCubeKeyboard)
    return Keyboard::GetConfig();
  return IsWii(type) ? Wiimote::GetConfig() : Pad::GetConfig();
}

ControllerEmu::EmulatedController* GetController(ControllerType type, int player)
{
  InputConfig* const config = GetInputConfig(type);
  if (!config || player < 0 || player >= config->GetControllerCount())
    return nullptr;
  return config->GetController(player);
}

Config::Info<std::string> GetPersistedTypeInfo(int player)
{
  return {{Config::System::Main, "UWP", fmt::format("ControllerType{}", player + 1)},
          std::string(CONTROLLER_TYPES.front().id)};
}

ControllerType TypeFromExtension(std::string_view extension)
{
  const auto found = std::ranges::find_if(CONTROLLER_TYPES, [extension](const TypeInfo& info) {
    return IsWii(info.type) && info.extension == extension;
  });
  return found != CONTROLLER_TYPES.end() ? found->type : ControllerType::WiiRemote;
}

ControllerEmu::ControlGroupContainer* FindContainer(ControllerEmu::EmulatedController* controller,
                                                    std::string_view owner)
{
  if (!controller || owner.empty())
    return controller;

  for (const auto& group : controller->groups)
  {
    if (group->type != ControllerEmu::GroupType::Attachments)
      continue;
    for (const auto& attachment :
         static_cast<ControllerEmu::Attachments*>(group.get())->GetAttachmentList())
    {
      if (attachment->GetName() == owner)
        return attachment.get();
    }
  }
  return nullptr;
}

ControllerEmu::ControlGroup* FindGroup(ControllerEmu::ControlGroupContainer* container,
                                       std::string_view name)
{
  if (!container)
    return nullptr;
  const auto found = std::ranges::find_if(
      container->groups, [name](const auto& group) { return group->name == name; });
  return found == container->groups.end() ? nullptr : found->get();
}

ControllerEmu::NumericSettingBase* FindNumericSetting(ControllerEmu::EmulatedController* controller,
                                                      std::string_view key)
{
  const size_t name_separator = key.rfind('/');
  if (name_separator == std::string_view::npos)
    return nullptr;
  std::string_view group_path = key.substr(0, name_separator);
  const std::string_view name = key.substr(name_separator + 1);

  std::string_view owner;
  if (const size_t owner_separator = group_path.find('/');
      owner_separator != std::string_view::npos)
  {
    owner = group_path.substr(0, owner_separator);
    group_path = group_path.substr(owner_separator + 1);
  }

  ControllerEmu::ControlGroup* const group =
      FindGroup(FindContainer(controller, owner), group_path);
  if (!group)
    return nullptr;
  const auto found = std::ranges::find_if(group->numeric_settings, [name](const auto& setting) {
    return setting->GetININame() == name;
  });
  return found == group->numeric_settings.end() ? nullptr : found->get();
}

bool IsSkippedControl(std::string_view name)
{
  return name == "Modifier" || name == "Relative Input Hold";
}

std::vector<EmulatedControlDescriptor> BuildControls(ControllerType type, int player)
{
  std::vector<EmulatedControlDescriptor> controls;
  ControllerEmu::EmulatedController* const controller = GetController(type, player);
  if (!controller)
    return controls;

  for (const GroupMapping& mapping : GetGroups(type))
  {
    ControllerEmu::ControlGroupContainer* const container =
        FindContainer(controller, mapping.owner);
    if (!container)
      continue;

    const std::string prefix =
        mapping.owner.empty() ? std::string() : fmt::format("{}/", mapping.owner);
    for (const auto& group : container->groups)
    {
      if (group->name != mapping.group)
        continue;
      for (const auto& control : group->controls)
      {
        if (!control->control_ref->IsInput() || IsSkippedControl(control->name))
          continue;

        InputControlKind kind = mapping.kind;
        if (kind == InputControlKind::Trigger && (control->name == "L" || control->name == "R"))
          kind = InputControlKind::Button;
        controls.push_back({fmt::format("{}{}/{}", prefix, group->name, control->name),
                            control->ui_name, std::string(mapping.label), kind});
      }
    }
  }
  return controls;
}

std::vector<std::string> GetMappingKeys(ControllerType type, int player)
{
  std::vector<std::string> keys;
  for (const EmulatedControlDescriptor& control : BuildControls(type, player))
    keys.push_back(control.id);
  return keys;
}

InputControlKind ClassifyDeviceInput(std::string_view name)
{
  if (name.starts_with("Accel "))
    return InputControlKind::Accelerometer;
  if (name.starts_with("Gyro "))
    return InputControlKind::Gyroscope;
  if (name.starts_with("Cursor ") || name.starts_with("Touch"))
    return InputControlKind::Pointer;
  if (name.starts_with("Trigger"))
    return InputControlKind::Trigger;
  if (name.ends_with('+') || name.ends_with('-'))
    return InputControlKind::Axis;
  return InputControlKind::Button;
}

std::shared_ptr<ciface::Core::Device> FindDevice(std::string_view id)
{
  ciface::Core::DeviceQualifier qualifier;
  qualifier.FromString(std::string(id));
  return g_controller_interface.FindDevice(qualifier);
}

bool DeviceSupportsMotion(const ciface::Core::Device& device)
{
  return std::ranges::any_of(device.Inputs(), [](const auto* input) {
    return ClassifyDeviceInput(input->GetName()) == InputControlKind::Accelerometer;
  });
}

std::string FindDefaultGamepad(int player = 0)
{
  constexpr std::string_view prefix = "WGInput/";
  const auto devices = g_controller_interface.GetAllDeviceStrings();

  if (player >= 0)
  {
    // Search for the device matching this player's index (e.g. WGInput/0/..., WGInput/1/...)
    for (const std::string& id : devices)
    {
      if (id.starts_with(prefix))
      {
        size_t number = 0;
        if (std::from_chars(id.data() + prefix.size(), id.data() + id.size(), number).ec ==
                std::errc{} &&
            number == static_cast<size_t>(player))
        {
          return id;
        }
      }
    }
  }

  // Fallback to the first available gamepad only for player 0 or unassigned player (-1)
  if (player <= 0)
  {
    for (const std::string& id : devices)
    {
      if (id.starts_with(GAMEPAD_SOURCE))
        return id;
    }
  }
  return {};
}

std::string FindKeyboard()
{
  for (const std::string& id : g_controller_interface.GetAllDeviceStrings())
  {
    if (id.starts_with(KEYBOARD_DEVICE))
      return id;
  }
  return {};
}

bool IsWordStart(char c)
{
  return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

bool IsWordChar(char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string ToFrontendReference(std::string_view reference, std::string_view default_device,
                                std::vector<std::string>* devices)
{
  const size_t separator = reference.find(':');
  if (separator == std::string_view::npos)
    return UwpImGuiFrontend::EscapeInputControlReference(reference);

  const std::string_view device = reference.substr(0, separator);
  const std::string_view control = reference.substr(separator + 1);
  if (device == default_device)
    return UwpImGuiFrontend::EscapeInputControlReference(control);

  if (devices && std::ranges::find(*devices, device) == devices->end())
    devices->emplace_back(device);
  return UwpImGuiFrontend::EscapeInputControlReference(fmt::format("{}/{}", device, control));
}

std::string DolphinToFrontend(std::string_view expression, std::string_view default_device,
                              std::vector<std::string>* devices)
{
  const std::string_view trimmed = StripWhitespace(expression);
  if (trimmed.empty())
    return {};

  constexpr std::string_view operator_chars = "`|&!+-*/%^<>=(),?:~$";
  if (trimmed.find_first_of(operator_chars) == std::string_view::npos &&
      !std::isdigit(static_cast<unsigned char>(trimmed.front())))
  {
    return UwpImGuiFrontend::EscapeInputControlReference(trimmed);
  }

  std::string result;
  for (size_t i = 0; i < trimmed.size();)
  {
    const char c = trimmed[i];
    if (c == '`')
    {
      const size_t end = trimmed.find('`', i + 1);
      if (end == std::string_view::npos)
      {
        result.append(trimmed.substr(i));
        break;
      }
      result += ToFrontendReference(trimmed.substr(i + 1, end - i - 1), default_device, devices);
      i = end + 1;
    }
    else if (c == '$')
    {
      size_t end = i + 1;
      while (end < trimmed.size() && IsWordChar(trimmed[end]))
        ++end;
      result.append(trimmed.substr(i, end - i));
      i = end;
    }
    else if (IsWordStart(c))
    {
      size_t end = i + 1;
      while (end < trimmed.size() && IsWordChar(trimmed[end]))
        ++end;
      const std::string_view word = trimmed.substr(i, end - i);
      size_t next = end;
      while (next < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[next])))
        ++next;
      if (next < trimmed.size() && trimmed[next] == '(')
        result.append(word);
      else
        result += UwpImGuiFrontend::EscapeInputControlReference(word);
      i = end;
    }
    else
    {
      result.push_back(c);
      ++i;
    }
  }
  return result;
}

std::string FrontendToDolphin(std::string_view expression, const std::string& default_device,
                              const std::vector<std::string>& known_devices)
{
  ciface::Core::DeviceQualifier default_qualifier;
  default_qualifier.FromString(default_device);

  std::string result;
  for (size_t i = 0; i < expression.size();)
  {
    if (expression[i] != '`')
    {
      result.push_back(expression[i++]);
      continue;
    }

    ++i;
    std::string reference;
    while (i < expression.size() && expression[i] != '`')
    {
      if (expression[i] == '\\' && i + 1 < expression.size())
        ++i;
      reference.push_back(expression[i++]);
    }
    ++i;

    std::string device = default_device;
    std::string control = reference;
    size_t matched = 0;
    for (const std::string& known : known_devices)
    {
      if (known.size() > matched && reference.size() > known.size() &&
          reference[known.size()] == '/' && reference.starts_with(known))
      {
        matched = known.size();
        device = known;
        control = reference.substr(known.size() + 1);
      }
    }
    if (matched == 0)
    {
      if (const size_t separator = reference.rfind('/'); separator != std::string::npos)
      {
        device = reference.substr(0, separator);
        control = reference.substr(separator + 1);
      }
    }

    ciface::Core::DeviceQualifier qualifier;
    qualifier.FromString(device);
    result += ciface::MappingCommon::GetExpressionForControl(control, qualifier, default_qualifier);
  }
  return result;
}

std::string GetRumbleExpression(const std::string& device_id, const std::string& default_device)
{
  const auto device = FindDevice(device_id);
  if (!device || device->Outputs().empty())
    return {};

  ciface::Core::DeviceQualifier qualifier;
  qualifier.FromString(device_id);
  ciface::Core::DeviceQualifier default_qualifier;
  default_qualifier.FromString(default_device);

  const bool dual_motor = device->FindOutput("Motor L") && device->FindOutput("Motor R");
  if (dual_motor)
  {
    return fmt::format(
        "{} | {}",
        ciface::MappingCommon::GetExpressionForControl("Motor L", qualifier, default_qualifier),
        ciface::MappingCommon::GetExpressionForControl("Motor R", qualifier, default_qualifier));
  }
  return ciface::MappingCommon::GetExpressionForControl(device->Outputs().front()->GetName(),
                                                        qualifier, default_qualifier);
}

Section ReadLiveSection(ControllerType type, int player)
{
  Section section;
  if (ControllerEmu::EmulatedController* const controller = GetController(type, player))
    controller->SaveConfig(&section);
  return section;
}

std::optional<Section> ReadProfileFile(const std::string& path)
{
  Common::IniFile ini;
  if (!ini.Load(path))
    return std::nullopt;
  const Section* const section = ini.GetSection("Profile");
  if (!section)
    return std::nullopt;
  return *section;
}

bool WriteProfileFile(const std::string& path, const Section& section)
{
  Common::IniFile ini;
  Section* const target = ini.GetOrCreateSection("Profile");
  for (const auto& [key, value] : section.GetValues())
    target->Set(key, value);
  File::CreateFullPath(path);
  return ini.Save(path);
}

std::string GetProfileDirectory(ControllerType type)
{
  return GetInputConfig(type)->GetUserProfileDirectoryPath();
}

std::string GetTitleProfileKey(ControllerType type, int player)
{
  return fmt::format("{}Profile{}", GetInputConfig(type)->GetProfileKey(), player + 1);
}

std::string GetTitleProfileSetting(const std::string& game_id, int player)
{
  return fmt::format("UWP/{}-P{}", game_id, player + 1);
}

std::string GetGameIniPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
}

// A change to the mapping of the game that runs now applies at once.
bool IsRunningGame(const std::string& game_id)
{
  const Core::State state = Core::GetState(Core::System::GetInstance());
  return !game_id.empty() &&
         (state == Core::State::Running || state == Core::State::Paused ||
          state == Core::State::Starting) &&
         SConfig::GetInstance().GetGameID() == game_id;
}

std::string GetIniPath(ControllerType type)
{
  const std::string_view name = type == ControllerType::GameBoyAdvance   ? "GBA" :
                                type == ControllerType::GameCubeKeyboard ? "GCKeyNew" :
                                IsWii(type)                              ? WIIMOTE_INI_NAME :
                                                                           "GCPadNew";
  return fmt::format("{}{}.ini", File::GetUserPath(D_CONFIG_IDX), name);
}

// The mapping of every game, which a running game with a profile of its own doesn't use.
Section ReadGlobalSection(ControllerType type, int player)
{
  ControllerEmu::EmulatedController* const controller = GetController(type, player);
  if (!controller)
    return {};
  Common::IniFile ini;
  ini.Load(GetIniPath(type));
  const Section* const section = ini.GetSection(controller->GetName());
  return section ? *section : Section();
}

// Writes the section of one player only, as the controllers of the others can hold the profiles
// of a running game.
bool SaveGlobalSection(ControllerType type, int player, const Section& section)
{
  ControllerEmu::EmulatedController* const controller = GetController(type, player);
  if (!controller)
    return false;
  const std::string path = GetIniPath(type);
  File::CreateFullPath(path);
  Common::IniFile ini;
  ini.Load(path);
  ini.DeleteSection(controller->GetName());
  Section* const target = ini.GetOrCreateSection(controller->GetName());
  for (const auto& [key, value] : section.GetValues())
    target->Set(key, value);
  return ini.Save(path);
}

std::optional<Section> ReadTitleSection(ControllerType type, int player, const std::string& game_id)
{
  Common::IniFile game_ini;
  game_ini.Load(GetGameIniPath(game_id));
  std::string setting;
  if (!game_ini.GetIfExists("Controls", GetTitleProfileKey(type, player), &setting) ||
      setting.empty())
  {
    return std::nullopt;
  }
  return ReadProfileFile(GetProfileDirectory(type) + setting + ".ini");
}

bool UsesGameProfile(ControllerType type, int player)
{
  const std::string& game_id = SConfig::GetInstance().GetGameID();
  return IsRunningGame(game_id) && ReadTitleSection(type, player, game_id).has_value();
}

std::string SanitizeProfileName(std::string_view name)
{
  std::string result;
  for (const char c : StripWhitespace(name))
  {
    if (std::string_view("\\/:*?\"<>|").find(c) == std::string_view::npos)
      result.push_back(c);
  }
  return result;
}

InputSettingDraft MakeUnavailableSetting(std::string_view id, std::string_view label)
{
  InputSettingDraft setting;
  setting.id = id;
  setting.label = label;
  setting.kind = InputSettingKind::Choice;
  setting.enabled = false;
  setting.choiceValue = "none";
  setting.choices = {{"none", "Not available"}};
  return setting;
}

InputSettingDraft MakeNumberSetting(std::string_view id, std::string_view label, float value,
                                    float minimum, float maximum, float step)
{
  InputSettingDraft setting;
  setting.id = id;
  setting.label = label;
  setting.kind = InputSettingKind::Number;
  setting.numberValue = std::clamp(value, minimum, maximum);
  setting.minimum = minimum;
  setting.maximum = maximum;
  setting.step = step;
  setting.suffix = "%";
  return setting;
}

InputSettingDraft MakeToggleSetting(std::string_view id, std::string_view label, bool value,
                                    bool enabled = true)
{
  InputSettingDraft setting;
  setting.id = id;
  setting.label = label;
  setting.kind = InputSettingKind::Toggle;
  setting.boolValue = value;
  setting.enabled = enabled;
  return setting;
}

std::vector<std::string> GetStickControlKeys(ControllerType type, int player,
                                             std::string_view group_path)
{
  std::vector<std::string> keys;
  if (group_path.empty())
    return keys;
  for (const std::string& key : GetMappingKeys(type, player))
  {
    if (key.size() > group_path.size() && key.starts_with(group_path) &&
        key[group_path.size()] == '/')
    {
      keys.push_back(key);
    }
  }
  return keys;
}

float ReadRange(const Section& section, const std::vector<std::string>& keys,
                std::string_view filter = {})
{
  for (const std::string& key : keys)
  {
    if (!filter.empty() && !key.ends_with(filter))
      continue;
    double range = 100.0;
    section.Get(key + "/Range", &range, 100.0);
    return static_cast<float>(range);
  }
  return 100.0f;
}

void WriteRange(Section& section, const std::vector<std::string>& keys, float value,
                std::string_view filter = {})
{
  for (const std::string& key : keys)
  {
    if (filter.empty() || key.ends_with(filter))
      section.Set(key + "/Range", static_cast<double>(value), 100.0);
  }
}

// The directions of the Wii Remote pointer, whose range sets the pointer speed.
std::vector<std::string> GetPointerKeys(ControllerType type, int player)
{
  std::vector<std::string> keys;
  for (const std::string& key : GetStickControlKeys(type, player, "IR"))
  {
    if (key == "IR/Up" || key == "IR/Down" || key == "IR/Left" || key == "IR/Right")
      keys.push_back(key);
  }
  return keys;
}

float ReadDeadZone(const Section& section, std::string_view group_path)
{
  double value = 0.0;
  section.Get(fmt::format("{}/Dead Zone", group_path), &value, 0.0);
  return static_cast<float>(value);
}

bool HasMotionMapping(const Section& section, std::string_view device_id,
                      std::string_view default_device)
{
  std::string expression;
  section.Get("IMUAccelerometer/Up", &expression, "");
  if (expression.empty())
    return false;
  if (device_id == default_device)
    return expression.find(':') == std::string::npos;
  return expression.find(device_id) != std::string::npos;
}

InputDeviceSettingsDraft BuildDeviceSettings(std::string_view device_id, ControllerType type,
                                             int player, const Section& section,
                                             std::string_view default_device)
{
  InputDeviceSettingsDraft settings;
  settings.deviceId = device_id;

  const auto device = FindDevice(device_id);
  const bool has_outputs = device && !device->Outputs().empty();
  std::string rumble;
  section.Get("Rumble/Motor", &rumble, "");
  settings.settings.push_back(
      MakeToggleSetting("rumble", "Rumble", has_outputs && !rumble.empty(), has_outputs));

  const StickKeys sticks = GetStickKeys(type);
  if (!sticks.left.empty())
  {
    settings.settings.push_back(MakeNumberSetting("axis_deadzone", "Left stick dead zone",
                                                  ReadDeadZone(section, sticks.left), 0, 50, 1));
    settings.settings.push_back(MakeNumberSetting(
        "axis_range", "Left stick range",
        ReadRange(section, GetStickControlKeys(type, player, sticks.left)), 50, 150, 5));
  }
  else
  {
    settings.settings.push_back(MakeUnavailableSetting("axis_deadzone", "Left stick dead zone"));
    settings.settings.push_back(MakeUnavailableSetting("axis_range", "Left stick range"));
  }

  if (!sticks.right.empty())
  {
    settings.settings.push_back(MakeNumberSetting("rotation_deadzone", "Right stick dead zone",
                                                  ReadDeadZone(section, sticks.right), 0, 50, 1));
    settings.settings.push_back(MakeNumberSetting(
        "rotation_range", "Right stick range",
        ReadRange(section, GetStickControlKeys(type, player, sticks.right)), 50, 150, 5));
  }
  else
  {
    settings.settings.push_back(
        MakeUnavailableSetting("rotation_deadzone", "Right stick dead zone"));
    settings.settings.push_back(MakeUnavailableSetting("rotation_range", "Right stick range"));
  }

  if (!sticks.triggers.empty())
  {
    settings.settings.push_back(MakeNumberSetting(
        "trigger_deadzone", "Trigger dead zone", ReadDeadZone(section, sticks.triggers), 0, 25, 1));
    settings.settings.push_back(MakeNumberSetting(
        "trigger_range", "Trigger range",
        ReadRange(section, GetStickControlKeys(type, player, sticks.triggers), "-Analog"), 50, 150,
        5));
  }
  else
  {
    settings.settings.push_back(MakeUnavailableSetting("trigger_deadzone", "Trigger dead zone"));
    settings.settings.push_back(MakeUnavailableSetting("trigger_range", "Trigger range"));
  }

  const bool motion_capable = IsWii(type) && device && DeviceSupportsMotion(*device);
  settings.settings.push_back(MakeToggleSetting(
      "motion", "Use motion sensors",
      motion_capable && HasMotionMapping(section, device_id, default_device), motion_capable));

  bool relative = false;
  section.Get("IR/Relative Input", &relative, false);
  if (HasPointer(type))
  {
    settings.settings.push_back(MakeToggleSetting("pointer_enabled", "Relative pointer", relative));
    settings.settings.push_back(MakeNumberSetting("pointer_speed", "Pointer speed",
                                                  ReadRange(section, GetPointerKeys(type, player)),
                                                  25, 300, 5));
    settings.settings.push_back(MakeNumberSetting("pointer_deadzone", "Pointer dead zone",
                                                  ReadDeadZone(section, "IR"), 0, 50, 1));
  }
  else
  {
    settings.settings.push_back(MakeUnavailableSetting("pointer_enabled", "Relative pointer"));
    settings.settings.push_back(MakeUnavailableSetting("pointer_speed", "Pointer speed"));
    settings.settings.push_back(MakeUnavailableSetting("pointer_deadzone", "Pointer dead zone"));
  }
  return settings;
}

const InputSettingDraft* FindSetting(const InputDeviceSettingsDraft& settings, std::string_view id)
{
  const auto found = std::ranges::find(settings.settings, id, &InputSettingDraft::id);
  return found == settings.settings.end() ? nullptr : &*found;
}

std::vector<InputSettingDraft> BuildControllerSettings(ControllerType type, int player,
                                                       const Section& section)
{
  std::vector<InputSettingDraft> settings;
  ControllerEmu::EmulatedController* const controller = GetController(type, player);
  if (!controller)
    return settings;

  for (const ControllerSettingSpec& spec : GetSettingSpecs(type))
  {
    ControllerEmu::NumericSettingBase* const base = FindNumericSetting(controller, spec.key);
    if (!base)
      continue;

    InputSettingDraft setting;
    setting.id = spec.key;
    setting.label = spec.label;
    setting.group = spec.group;
    switch (base->GetType())
    {
    case ControllerEmu::SettingType::Bool:
    {
      const auto* typed = static_cast<ControllerEmu::NumericSetting<bool>*>(base);
      bool value = typed->GetDefaultValue();
      section.Get(std::string(spec.key), &value, typed->GetDefaultValue());
      setting.kind = InputSettingKind::Toggle;
      setting.boolValue = value;
      break;
    }
    case ControllerEmu::SettingType::Double:
    {
      const auto* typed = static_cast<ControllerEmu::NumericSetting<double>*>(base);
      double value = typed->GetDefaultValue();
      section.Get(std::string(spec.key), &value, typed->GetDefaultValue());
      setting.kind = InputSettingKind::Number;
      setting.minimum = static_cast<float>(typed->GetMinValue());
      setting.maximum = static_cast<float>(typed->GetMaxValue());
      setting.numberValue = std::clamp(static_cast<float>(value), setting.minimum, setting.maximum);
      setting.step = 1.0f;
      setting.suffix = base->GetUISuffix() ? base->GetUISuffix() : "";
      break;
    }
    case ControllerEmu::SettingType::Int:
    {
      const auto* typed = static_cast<ControllerEmu::NumericSetting<int>*>(base);
      int value = typed->GetDefaultValue();
      section.Get(std::string(spec.key), &value, typed->GetDefaultValue());
      setting.kind = InputSettingKind::Number;
      setting.minimum = static_cast<float>(typed->GetMinValue());
      setting.maximum = static_cast<float>(typed->GetMaxValue());
      setting.numberValue = std::clamp(static_cast<float>(value), setting.minimum, setting.maximum);
      setting.step = 1.0f;
      setting.suffix = base->GetUISuffix() ? base->GetUISuffix() : "";
      break;
    }
    }
    settings.push_back(std::move(setting));
  }
  return settings;
}

ControllerProfileDraft BuildDraft(ControllerType type, int player,
                                  std::optional<std::uint64_t> title_id, const Section& section,
                                  std::string name)
{
  ControllerProfileDraft draft;
  draft.name = std::move(name);
  draft.player = static_cast<std::uint8_t>(player);
  draft.titleId = title_id;
  draft.emulatedControllerTypeId = GetTypeInfo(type).id;

  std::string device;
  section.Get("Device", &device, "");
  draft.defaultDeviceId = device;

  std::vector<std::string> devices;
  if (!device.empty())
    devices.push_back(device);

  for (const EmulatedControlDescriptor& control : BuildControls(type, player))
  {
    std::string expression;
    section.Get(control.id, &expression, "");
    draft.mappings.push_back({control.id, DolphinToFrontend(expression, device, &devices), {}, {}});
  }

  draft.attachedDeviceIds = devices;
  draft.controllerSettings = BuildControllerSettings(type, player, section);
  for (const std::string& attached : devices)
    draft.deviceSettings.push_back(BuildDeviceSettings(attached, type, player, section, device));
  return draft;
}

void ApplyBindings(Section& section, ControllerType type, int player,
                   std::span<const Binding> bindings, const std::string& device)
{
  for (const std::string& key : GetMappingKeys(type, player))
    section.Delete(key);
  section.Delete("Rumble/Motor");
  for (const Binding& binding : bindings)
    section.Set(std::string(binding.key), std::string(binding.expression));
  section.Set("Device", device);
  if (IsWii(type))
    section.Set("Extension", std::string(GetTypeInfo(type).extension));
}

Section BuildDefaultSection(ControllerType type, int player, const std::string& device)
{
  Section section = ReadLiveSection(type, player);
  if (device.starts_with(KEYBOARD_DEVICE))
  {
    if (type == ControllerType::GameCube)
    {
      GCPad defaults(static_cast<unsigned int>(player));
      defaults.LoadDefaults(g_controller_interface);
      defaults.SetDefaultDevice(device);
      Section keyboard;
      defaults.SaveConfig(&keyboard);
      return keyboard;
    }
    if (type == ControllerType::GameBoyAdvance)
    {
      GBAPad defaults(static_cast<unsigned int>(player));
      defaults.LoadDefaults(g_controller_interface);
      defaults.SetDefaultDevice(device);
      Section keyboard;
      defaults.SaveConfig(&keyboard);
      return keyboard;
    }
    if (type == ControllerType::GameCubeKeyboard)
    {
      GCKeyboard defaults(static_cast<unsigned int>(player));
      defaults.LoadDefaults(g_controller_interface);
      defaults.SetDefaultDevice(device);
      Section keyboard;
      defaults.SaveConfig(&keyboard);
      return keyboard;
    }
    ApplyBindings(section, type, player, GetKeyboardBindings(type), device);
    // On PC, the mouse points and clicks with the Wii Remote.
    if (!Common::UWP::IsXboxConsole() &&
        (type == ControllerType::WiiRemote || type == ControllerType::WiiNunchuk))
    {
      for (const Binding& binding : WII_REMOTE_MOUSE)
        section.Set(std::string(binding.key), std::string(binding.expression));
    }
    return section;
  }

  if (device.starts_with(GAMEPAD_SOURCE))
  {
    ApplyBindings(section, type, player, GetGamepadBindings(type), device);
    return section;
  }

  ApplyBindings(section, type, player, {}, device);
  return section;
}

void ApplyDraft(Section& section, ControllerType type, int player,
                const ControllerProfileDraft& draft)
{
  const std::vector<std::string> known_devices = g_controller_interface.GetAllDeviceStrings();
  section.Set("Device", draft.defaultDeviceId);
  if (IsWii(type))
    section.Set("Extension", std::string(GetTypeInfo(type).extension));

  for (const ControllerMappingDraft& mapping : draft.mappings)
  {
    if (!mapping.validationError.empty())
      continue;
    const std::string expression =
        FrontendToDolphin(mapping.expression, draft.defaultDeviceId, known_devices);
    if (expression.empty())
      section.Delete(mapping.emulatedControlId);
    else
      section.Set(mapping.emulatedControlId, expression);
  }

  for (const InputSettingDraft& setting : draft.controllerSettings)
  {
    if (setting.kind == InputSettingKind::Toggle)
      section.Set(setting.id, setting.boolValue);
    else if (setting.kind == InputSettingKind::Number)
      section.Set(setting.id, static_cast<double>(setting.numberValue));
  }

  const auto primary = std::ranges::find(draft.deviceSettings, draft.defaultDeviceId,
                                         &InputDeviceSettingsDraft::deviceId);
  const InputDeviceSettingsDraft* const settings =
      primary != draft.deviceSettings.end() ?
          &*primary :
          (draft.deviceSettings.empty() ? nullptr : &draft.deviceSettings.front());
  if (settings)
  {
    const StickKeys sticks = GetStickKeys(type);
    const auto number = [settings](std::string_view id) -> std::optional<float> {
      const InputSettingDraft* const setting = FindSetting(*settings, id);
      if (!setting || setting->kind != InputSettingKind::Number || !setting->enabled)
        return std::nullopt;
      return setting->numberValue;
    };

    if (!sticks.left.empty())
    {
      if (const auto value = number("axis_deadzone"))
        section.Set(fmt::format("{}/Dead Zone", sticks.left), static_cast<double>(*value), 0.0);
      if (const auto value = number("axis_range"))
        WriteRange(section, GetStickControlKeys(type, player, sticks.left), *value);
    }
    if (!sticks.right.empty())
    {
      if (const auto value = number("rotation_deadzone"))
        section.Set(fmt::format("{}/Dead Zone", sticks.right), static_cast<double>(*value), 0.0);
      if (const auto value = number("rotation_range"))
        WriteRange(section, GetStickControlKeys(type, player, sticks.right), *value);
    }
    if (!sticks.triggers.empty())
    {
      if (const auto value = number("trigger_deadzone"))
        section.Set(fmt::format("{}/Dead Zone", sticks.triggers), static_cast<double>(*value), 0.0);
      if (const auto value = number("trigger_range"))
        WriteRange(section, GetStickControlKeys(type, player, sticks.triggers), *value, "-Analog");
    }

    const InputSettingDraft* const rumble = FindSetting(*settings, "rumble");
    if (rumble && rumble->enabled)
    {
      const std::string expression =
          rumble->boolValue ? GetRumbleExpression(draft.defaultDeviceId, draft.defaultDeviceId) :
                              "";
      if (expression.empty())
        section.Delete("Rumble/Motor");
      else
        section.Set("Rumble/Motor", expression);
    }

    const InputSettingDraft* const pointer = FindSetting(*settings, "pointer_enabled");
    if (pointer && pointer->enabled && pointer->kind == InputSettingKind::Toggle)
      section.Set("IR/Relative Input", pointer->boolValue, false);
    if (HasPointer(type))
    {
      if (const auto value = number("pointer_speed"))
        WriteRange(section, GetPointerKeys(type, player), *value);
      if (const auto value = number("pointer_deadzone"))
        section.Set("IR/Dead Zone", static_cast<double>(*value), 0.0);
    }
  }

  if (!IsWii(type))
    return;

  std::string motion_device;
  for (const InputDeviceSettingsDraft& device_settings : draft.deviceSettings)
  {
    const InputSettingDraft* const motion = FindSetting(device_settings, "motion");
    if (motion && motion->enabled && motion->boolValue)
    {
      motion_device = device_settings.deviceId;
      break;
    }
  }

  ControllerEmu::EmulatedController* const controller = GetController(type, player);
  ciface::Core::DeviceQualifier default_qualifier;
  default_qualifier.FromString(draft.defaultDeviceId);
  ciface::Core::DeviceQualifier motion_qualifier;
  motion_qualifier.FromString(motion_device);
  for (const auto& [group_name, prefix] :
       {std::pair{"IMUAccelerometer", "Accel"}, std::pair{"IMUGyroscope", "Gyro"}})
  {
    ControllerEmu::ControlGroup* const group = FindGroup(controller, group_name);
    if (!group)
      continue;
    for (const auto& control : group->controls)
    {
      const std::string key = fmt::format("{}/{}", group_name, control->name);
      if (motion_device.empty())
      {
        section.Delete(key);
        continue;
      }
      section.Set(key, ciface::MappingCommon::GetExpressionForControl(
                           fmt::format("{} {}", prefix, control->name), motion_qualifier,
                           default_qualifier));
    }
  }
}

void SetLiveExtension(ControllerType type, int player)
{
  if (!IsWii(type))
    return;
  ControllerEmu::EmulatedController* const controller = GetController(type, player);
  if (!controller)
    return;

  const std::string_view target_extension = GetTypeInfo(type).extension;
  for (const auto& group : controller->groups)
  {
    if (group->type != ControllerEmu::GroupType::Attachments)
      continue;
    auto* attachments = static_cast<ControllerEmu::Attachments*>(group.get());
    const auto& list = attachments->GetAttachmentList();
    for (u32 i = 0; i < list.size(); ++i)
    {
      if (list[i]->GetName() == target_extension)
      {
        attachments->SetSelectedAttachment(i);
        break;
      }
    }
  }
}

void LoadIntoController(ControllerType type, int player, const Section& section, bool save = true)
{
  ControllerEmu::EmulatedController* const controller = GetController(type, player);
  if (!controller)
    return;
  Section copy = section;
  controller->LoadConfig(&copy);
  if (IsWii(type))
    SetLiveExtension(type, player);
  controller->UpdateReferences(g_controller_interface);
  if (save)
    GetInputConfig(type)->SaveConfig();
}

// Saves the mapping of a player as the profile of a game, or as the mapping of every game when no
// game is given. A running game uses it at once. A profile that replaces the other kind of
// controller removes the profile of that kind, as a player of a game uses one kind.
MappingSaveResult StoreSection(ControllerType type, int player, const Section& section,
                               const std::string& game_id, bool replaces_other_kind = false)
{
  SaveGlobalSection(type, player, section);

  if (game_id.empty())
  {
    // A running game with a profile of its own keeps using it.
    if (!UsesGameProfile(type, player))
      LoadIntoController(type, player, section, false);
    return {true, {}};
  }

  const std::string setting = GetTitleProfileSetting(game_id, player);
  if (!WriteProfileFile(GetProfileDirectory(type) + setting + ".ini", section))
    return {false, "The per-game controller profile could not be written"};

  Common::IniFile game_ini;
  const std::string game_ini_path = GetGameIniPath(game_id);
  File::CreateFullPath(game_ini_path);
  game_ini.Load(game_ini_path);
  game_ini.GetOrCreateSection("Controls")->Set(GetTitleProfileKey(type, player), setting);
  if (replaces_other_kind && type != ControllerType::GameBoyAdvance &&
      type != ControllerType::GameCubeKeyboard)
  {
    const ControllerType other = IsWii(type) ? ControllerType::GameCube : ControllerType::WiiRemote;
    game_ini.DeleteKey("Controls", GetTitleProfileKey(other, player));
  }
  if (!game_ini.Save(game_ini_path))
    return {false, "The game settings file could not be written"};

  // The mapping of every game stays as it is.
  if (IsRunningGame(game_id))
    LoadIntoController(type, player, section, false);
  return {true, {}};
}

// The section that a change to the mapping of a player starts from.
Section ReadSectionToChange(ControllerType type, int player, const std::string& game_id)
{
  if (!game_id.empty())
  {
    if (std::optional<Section> section = ReadTitleSection(type, player, game_id))
      return std::move(*section);
  }
  else if (UsesGameProfile(type, player))
  {
    return ReadGlobalSection(type, player);
  }
  return ReadLiveSection(type, player);
}

std::span<const Binding> GetDefaultBindings(ControllerType type, std::string_view device)
{
  if (device.starts_with(KEYBOARD_DEVICE))
    return GetKeyboardBindings(type);
  if (device.starts_with(GAMEPAD_SOURCE))
    return GetGamepadBindings(type);
  return {};
}

void ConnectPort(ControllerType type, int player)
{
  if (IsWii(type))
  {
    const auto& source = Config::GetInfoForWiimoteSource(player);
    if (Config::Get(source) == WiimoteSource::None)
      Config::SetBaseOrCurrent(source, WiimoteSource::Emulated);
    // Disconnect the GameCube port to prevent double input when using a Wii controller
    const auto& port = Config::GetInfoForSIDevice(player);
    Config::SetBaseOrCurrent(port, SerialInterface::SIDEVICE_NONE);
    return;
  }

  const auto& device = Config::GetInfoForSIDevice(player);
  if (type == ControllerType::GameBoyAdvance)
  {
    Config::SetBaseOrCurrent(device, SerialInterface::SIDEVICE_GC_GBA_EMULATED);
    return;
  }
  if (type == ControllerType::GameCubeKeyboard)
  {
    Config::SetBaseOrCurrent(device, SerialInterface::SIDEVICE_GC_KEYBOARD);
    return;
  }
  // The other port types, such as the steering wheel and the Triforce baseboard, use the
  // GameCube controller mapping.
  switch (Config::Get(device))
  {
  case SerialInterface::SIDEVICE_NONE:
  case SerialInterface::SIDEVICE_GC_GBA:
  case SerialInterface::SIDEVICE_GC_GBA_EMULATED:
  case SerialInterface::SIDEVICE_GC_KEYBOARD:
    Config::SetBaseOrCurrent(device, SerialInterface::SIDEVICE_GC_CONTROLLER);
    break;
  default:
    break;
  }
  // Disconnect the Wii Remote to prevent double input when using a GameCube controller
  const auto& source = Config::GetInfoForWiimoteSource(player);
  Config::SetBaseOrCurrent(source, WiimoteSource::None);
}

ControllerType GetWiiRemoteType(int player)
{
  std::string extension;
  ReadLiveSection(ControllerType::WiiRemote, player).Get("Extension", &extension, "None");
  if (extension.empty() || extension == "None")
  {
    const std::string game_id = InputMappingHost::GetRunningGameId();
    if (!game_id.empty() && Core::System::GetInstance().IsWii())
    {
      const ControllerPreference pref = GetWiiControllerPreference(game_id);
      if (pref == ControllerPreference::Classic)
        return ControllerType::Classic;
      if (pref == ControllerPreference::WiiRemoteNunchuk)
        return ControllerType::WiiNunchuk;
    }
  }
  return TypeFromExtension(extension);
}

ControllerType GetPersistedType(int player)
{
  const std::optional<ControllerType> type = FindType(Config::Get(GetPersistedTypeInfo(player)));
  if (!type || !IsWii(*type))
    return type.value_or(ControllerType::GameCube);
  return GetWiiRemoteType(player);
}
}  // namespace

InputMappingHost::InputMappingHost() = default;

InputMappingHost::~InputMappingHost()
{
  ReleaseTextures();
}

void InputMappingHost::SetTextureCallbacks(std::filesystem::path frontend_resources,
                                           std::filesystem::path system_resources,
                                           TextureLoader load, TextureReleaser release)
{
  ReleaseTextures();
  m_frontend_resources = std::move(frontend_resources);
  m_system_resources = std::move(system_resources);
  m_load_texture = std::move(load);
  m_release_texture = std::move(release);
}

void InputMappingHost::ReleaseTextures()
{
  for (const auto& [id, texture] : m_previews)
  {
    if (texture && m_release_texture)
      m_release_texture(texture);
  }
  m_previews.clear();
}

void InputMappingHost::Tick()
{
  if (!m_rumble_deadline || std::chrono::steady_clock::now() < *m_rumble_deadline)
    return;

  m_rumble_deadline.reset();
  if (const auto device = FindDevice(m_rumble_device))
  {
    for (ciface::Core::Device::Output* const output : device->Outputs())
      output->SetState(0.0);
  }
}

bool InputMappingHost::ApplyDefaultGamepadMappings(int player, const std::string& device)
{
  if (device.empty() || player < 0 || player >= MAX_PLAYERS)
    return false;

  LoadIntoController(ControllerType::GameCube, player,
                     BuildDefaultSection(ControllerType::GameCube, player, device));
  LoadIntoController(ControllerType::WiiNunchuk, player,
                     BuildDefaultSection(ControllerType::WiiNunchuk, player, device));
  LoadIntoController(ControllerType::GameBoyAdvance, player,
                     BuildDefaultSection(ControllerType::GameBoyAdvance, player, device));
  return true;
}

void InputMappingHost::DisconnectGamepad(int player)
{
  if (player <= 0 || player >= MAX_PLAYERS || NetPlay::IsNetPlayRunning() ||
      Core::System::GetInstance().GetMovie().IsMovieActive())
  {
    return;
  }

  Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::None);
  Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_NONE);
  Config::Save();
}

void InputMappingHost::AssignGamepad(int player, const std::string& device)
{
  if (player < 0 || player >= MAX_PLAYERS || !device.starts_with(GAMEPAD_SOURCE) ||
      NetPlay::IsNetPlayRunning() || Core::System::GetInstance().GetMovie().IsMovieActive())
  {
    return;
  }

  const auto is_unmapped = [player](ControllerType type) {
    const Section section = ReadGlobalSection(type, player);
    std::string current_device;
    section.Get("Device", &current_device, "");
    // If player > 0 still points to Player 1's gamepad or has no device set, consider it unmapped
    if (player > 0 && (current_device.empty() || current_device == FindDefaultGamepad(0)))
      return true;
    return std::ranges::none_of(GetMappingKeys(type, player), [&section](const std::string& key) {
      std::string expression;
      section.Get(key, &expression, "");
      return !expression.empty();
    });
  };
  if (is_unmapped(ControllerType::GameCube) && is_unmapped(ControllerType::WiiNunchuk))
    ApplyDefaultGamepadMappings(player, device);

  const std::string game_id = GetRunningGameId();
  if (!game_id.empty())
  {
    // A game is currently running: connect only the port appropriate for this game
    const bool has_gc = ReadTitleSection(ControllerType::GameCube, player, game_id).has_value();
    const bool has_wii = ReadTitleSection(ControllerType::WiiRemote, player, game_id).has_value();
    const bool is_wii = Core::System::GetInstance().IsWii();

    if (has_gc && !has_wii)
    {
      Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_GC_CONTROLLER);
      Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::None);
      if (std::optional<Section> title_sec = ReadTitleSection(ControllerType::GameCube, player, game_id))
        LoadIntoController(ControllerType::GameCube, player, *title_sec, false);
    }
    else if (has_wii && !has_gc)
    {
      Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::Emulated);
      Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_NONE);
      if (std::optional<Section> title_sec = ReadTitleSection(ControllerType::WiiRemote, player, game_id))
      {
        std::string ext;
        title_sec->Get("Extension", &ext, "");
        if (ext.empty() || ext == "None")
        {
          const ControllerPreference pref = GetWiiControllerPreference(game_id);
          if (pref == ControllerPreference::Classic)
          {
            ext = "Classic";
            title_sec->Set("Extension", "Classic");
            StoreSection(ControllerType::Classic, player, *title_sec, game_id);
          }
          else if (pref == ControllerPreference::WiiRemoteNunchuk)
          {
            ext = "Nunchuk";
            title_sec->Set("Extension", "Nunchuk");
            StoreSection(ControllerType::WiiNunchuk, player, *title_sec, game_id);
          }
        }
        const ControllerType ext_type = TypeFromExtension(ext);
        LoadIntoController(ext_type, player, *title_sec, false);
        SetLiveExtension(ext_type, player);
      }
    }
    else if (!is_wii)
    {
      Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_GC_CONTROLLER);
      Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::None);
      Section section = BuildDefaultSection(ControllerType::GameCube, player, device);
      StoreSection(ControllerType::GameCube, player, section, game_id, true);
      LoadIntoController(ControllerType::GameCube, player, section, true);
    }
    else
    {
      const ControllerPreference pref = GetWiiControllerPreference(game_id);
      if (pref == ControllerPreference::Classic)
      {
        Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::Emulated);
        Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_NONE);
        Section section = BuildDefaultSection(ControllerType::Classic, player, device);
        section.Set("Extension", "Classic");
        StoreSection(ControllerType::Classic, player, section, game_id, true);
        LoadIntoController(ControllerType::Classic, player, section, true);
        SetLiveExtension(ControllerType::Classic, player);
      }
      else if (pref == ControllerPreference::GameCube)
      {
        Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_GC_CONTROLLER);
        Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::None);
        Section section = BuildDefaultSection(ControllerType::GameCube, player, device);
        StoreSection(ControllerType::GameCube, player, section, game_id, true);
        LoadIntoController(ControllerType::GameCube, player, section, true);
        SetLiveExtension(ControllerType::WiiRemote, player);
      }
      else
      {
        Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::Emulated);
        Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_NONE);
        Section section = BuildDefaultSection(ControllerType::WiiNunchuk, player, device);
        section.Set("Extension", "Nunchuk");
        StoreSection(ControllerType::WiiNunchuk, player, section, game_id, true);
        LoadIntoController(ControllerType::WiiNunchuk, player, section, true);
        SetLiveExtension(ControllerType::WiiNunchuk, player);
      }
    }
  }
  else
  {
    const auto& port = Config::GetInfoForSIDevice(player);
    if (Config::Get(port) == SerialInterface::SIDEVICE_NONE)
      Config::SetBaseOrCurrent(port, SerialInterface::SIDEVICE_GC_CONTROLLER);
    const auto& source = Config::GetInfoForWiimoteSource(player);
    if (Config::Get(source) == WiimoteSource::None)
      Config::SetBaseOrCurrent(source, WiimoteSource::Emulated);
  }
  Config::Save();
}

void InputMappingHost::OnGameStarted(const std::string& game_id)
{
  if (game_id.empty() || Core::System::GetInstance().GetMovie().IsMovieActive())
  {
    return;
  }

  const bool is_wii = Core::System::GetInstance().IsWii();
  const bool is_netplay = NetPlay::IsNetPlayRunning();

  for (int player = 0; player < MAX_PLAYERS; ++player)
  {
    std::string device = FindDefaultGamepad(player);
    if (device.empty())
    {
      if (player == 0)
      {
        device = "WGInput/0/Xbox One Game Controller";
      }
      else
      {
        if (!is_netplay)
        {
          Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::None);
          Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_NONE);
        }
        continue;
      }
    }

    const bool has_gc_profile =
        ReadTitleSection(ControllerType::GameCube, player, game_id).has_value();
    const bool has_wii_profile =
        ReadTitleSection(ControllerType::WiiRemote, player, game_id).has_value();

    if (has_gc_profile || has_wii_profile)
    {
      if (!is_netplay)
      {
        // The user previously saved an explicit profile for this game. Respect their preference.
        if (has_gc_profile && !has_wii_profile)
        {
          Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player),
                                   SerialInterface::SIDEVICE_GC_CONTROLLER);
          Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::None);
          if (std::optional<Section> title_sec = ReadTitleSection(ControllerType::GameCube, player, game_id))
            LoadIntoController(ControllerType::GameCube, player, *title_sec, false);
        }
        else if (has_wii_profile && !has_gc_profile)
        {
          Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::Emulated);
          Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_NONE);
          if (std::optional<Section> title_sec = ReadTitleSection(ControllerType::WiiRemote, player, game_id))
          {
            std::string ext;
            title_sec->Get("Extension", &ext, "");
            if (ext.empty() || ext == "None")
            {
              const ControllerPreference pref = GetWiiControllerPreference(game_id);
              if (pref == ControllerPreference::Classic)
              {
                ext = "Classic";
                title_sec->Set("Extension", "Classic");
                StoreSection(ControllerType::Classic, player, *title_sec, game_id);
              }
              else if (pref == ControllerPreference::WiiRemoteNunchuk)
              {
                ext = "Nunchuk";
                title_sec->Set("Extension", "Nunchuk");
                StoreSection(ControllerType::WiiNunchuk, player, *title_sec, game_id);
              }
            }
            const ControllerType ext_type = TypeFromExtension(ext);
            LoadIntoController(ext_type, player, *title_sec, false);
            SetLiveExtension(ext_type, player);
          }
        }
        else if (has_gc_profile && has_wii_profile)
        {
          const ControllerPreference pref = is_wii ? GetWiiControllerPreference(game_id) : ControllerPreference::GameCube;
          if (pref == ControllerPreference::GameCube)
          {
            Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_GC_CONTROLLER);
            Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::None);
            if (std::optional<Section> title_sec = ReadTitleSection(ControllerType::GameCube, player, game_id))
              LoadIntoController(ControllerType::GameCube, player, *title_sec, false);
          }
          else
          {
            Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::Emulated);
            Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_NONE);
            if (std::optional<Section> title_sec = ReadTitleSection(ControllerType::WiiRemote, player, game_id))
            {
              std::string ext;
              title_sec->Get("Extension", &ext, "");
              if (ext.empty() || ext == "None")
                ext = pref == ControllerPreference::Classic ? "Classic" : "Nunchuk";
              const ControllerType ext_type = TypeFromExtension(ext);
              LoadIntoController(ext_type, player, *title_sec, false);
              SetLiveExtension(ext_type, player);
            }
          }
        }
      }
      continue;
    }

    if (!is_wii)
    {
      if (!is_netplay)
      {
        // GameCube game: connect GameCube port, disconnect Wii Remote
        Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player),
                                 SerialInterface::SIDEVICE_GC_CONTROLLER);
        Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::None);
      }
      Section section = BuildDefaultSection(ControllerType::GameCube, player, device);
      StoreSection(ControllerType::GameCube, player, section, game_id, true);
      LoadIntoController(ControllerType::GameCube, player, section, true);
      continue;
    }

    // Wii game without a saved profile: apply the recommended controller preference
    const ControllerPreference pref = GetWiiControllerPreference(game_id);

    if (pref == ControllerPreference::Classic)
    {
      if (!is_netplay)
      {
        Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::Emulated);
        Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_NONE);
      }
      Section section = BuildDefaultSection(ControllerType::Classic, player, device);
      section.Set("Extension", "Classic");
      StoreSection(ControllerType::Classic, player, section, game_id, true);
      LoadIntoController(ControllerType::Classic, player, section, true);
      SetLiveExtension(ControllerType::Classic, player);
    }
    else if (pref == ControllerPreference::GameCube)
    {
      if (!is_netplay)
      {
        Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player),
                                 SerialInterface::SIDEVICE_GC_CONTROLLER);
        Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::None);
      }
      Section section = BuildDefaultSection(ControllerType::GameCube, player, device);
      StoreSection(ControllerType::GameCube, player, section, game_id, true);
      LoadIntoController(ControllerType::GameCube, player, section, true);
      SetLiveExtension(ControllerType::WiiRemote, player);
    }
    else
    {
      if (!is_netplay)
      {
        Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(player), WiimoteSource::Emulated);
        Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(player), SerialInterface::SIDEVICE_NONE);
      }
      Section section = BuildDefaultSection(ControllerType::WiiNunchuk, player, device);
      section.Set("Extension", "Nunchuk");
      StoreSection(ControllerType::WiiNunchuk, player, section, game_id, true);
      LoadIntoController(ControllerType::WiiNunchuk, player, section, true);
      SetLiveExtension(ControllerType::WiiNunchuk, player);
    }
  }

  if (!is_netplay)
    Config::Save();
}

void InputMappingHost::AddMissingGBAMappings()
{
  if (File::Exists(File::GetUserPath(D_CONFIG_IDX) + "GBA.ini"))
    return;

  for (int player = 0; player < MAX_PLAYERS; ++player)
  {
    std::string device;
    {
      const auto lock = ControllerEmu::EmulatedController::GetStateLock();
      device = Pad::GetConfig()->GetController(player)->GetDefaultDevice().ToString();
    }
    if (device.starts_with(GAMEPAD_SOURCE))
    {
      LoadIntoController(ControllerType::GameBoyAdvance, player,
                         BuildDefaultSection(ControllerType::GameBoyAdvance, player, device));
    }
  }
}

std::string InputMappingHost::ResolveGameId(std::optional<std::uint64_t> title_id) const
{
  if (!title_id || !m_game_id_resolver)
    return {};
  return m_game_id_resolver(*title_id);
}

std::vector<UwpImGuiFrontend::InputDeviceDescriptor> InputMappingHost::EnumerateInputDevices()
{
  std::vector<UwpImGuiFrontend::InputDeviceDescriptor> devices;
  for (const std::string& id : g_controller_interface.GetAllDeviceStrings())
  {
    const auto device = FindDevice(id);
    if (!device)
      continue;

    UwpImGuiFrontend::InputDeviceDescriptor descriptor;
    descriptor.id = id;
    descriptor.label = device->GetId() > 0 ?
                           fmt::format("{} {}", device->GetName(), device->GetId() + 1) :
                           device->GetName();
    descriptor.api = device->GetSource();
    descriptor.connected = device->IsValid();
    descriptor.supportsRumble = !device->Outputs().empty();
    for (const ciface::Core::Device::Input* const input : device->Inputs())
    {
      if (input->IsHidden())
        continue;
      const std::string name = input->GetName();
      const InputControlKind kind = ClassifyDeviceInput(name);
      descriptor.supportsMotion |=
          kind == InputControlKind::Accelerometer || kind == InputControlKind::Gyroscope;
      descriptor.supportsPointer |= kind == InputControlKind::Pointer;
      descriptor.controls.push_back({name, name, kind, 0.0f, 1.0f, false});
    }
    devices.push_back(std::move(descriptor));
  }
  return devices;
}

void InputMappingHost::RefreshInputDevices()
{
  g_controller_interface.RefreshDevices();
}

std::vector<UwpImGuiFrontend::DsuServerModel> InputMappingHost::EnumerateDsuServers()
{
  return m_dsu_servers ? m_dsu_servers() : std::vector<UwpImGuiFrontend::DsuServerModel>{};
}

std::vector<UwpImGuiFrontend::EmulatedControllerDescriptor>
InputMappingHost::EnumerateEmulatedControllerTypes(std::uint8_t player)
{
  std::vector<UwpImGuiFrontend::EmulatedControllerDescriptor> types;
  if (player >= MAX_PLAYERS)
    return types;

  for (const TypeInfo& info : CONTROLLER_TYPES)
  {
    UwpImGuiFrontend::EmulatedControllerDescriptor descriptor;
    descriptor.id = info.id;
    descriptor.label = info.label;
    descriptor.previewLayout = info.layout;
    for (size_t i = 0; i < info.face_buttons.size(); ++i)
      descriptor.faceButtonLabels[i] = info.face_buttons[i];
    types.push_back(std::move(descriptor));
  }
  return types;
}

UwpImGuiFrontend::TextureHandle
InputMappingHost::GetControllerPreviewTexture(std::string_view controller_type_id)
{
  const std::optional<ControllerType> type = FindType(controller_type_id);
  if (!type || !m_load_texture)
    return {};

  const std::string key(controller_type_id);
  if (const auto found = m_previews.find(key); found != m_previews.end())
    return found->second;

  std::filesystem::path path;
  switch (*type)
  {
  case ControllerType::GameCube:
  case ControllerType::GameCubeKeyboard:
    path = m_system_resources / "Platform_Gamecube@4x.png";
    break;
  case ControllerType::Classic:
    path = m_system_resources / "Platform_Wii@4x.png";
    break;
  default:
    path = m_frontend_resources / "controller-previews" / "wii-remote.png";
    break;
  }
  // A loader can hand out a texture later, once its renderer has created it.
  const UwpImGuiFrontend::TextureHandle texture = m_load_texture(path);
  if (texture)
    m_previews.emplace(key, texture);
  return texture;
}

std::vector<EmulatedControlDescriptor>
InputMappingHost::EnumerateEmulatedControls(std::uint8_t player,
                                            std::string_view controller_type_id)
{
  const std::optional<ControllerType> type = FindType(controller_type_id);
  if (!type || player >= MAX_PLAYERS)
    return {};
  // The mapper shows this type now, which the device settings it asks for follow.
  m_last_type = GetTypeInfo(*type).id;
  return BuildControls(*type, player);
}

std::vector<InputSettingDraft>
InputMappingHost::CreateControllerSettings(std::uint8_t player, std::string_view controller_type_id)
{
  const std::optional<ControllerType> type = FindType(controller_type_id);
  if (!type || player >= MAX_PLAYERS)
    return {};
  return BuildControllerSettings(*type, player, ReadLiveSection(*type, player));
}

InputDeviceSettingsDraft InputMappingHost::CreateInputDeviceSettings(std::string_view device_id)
{
  const ControllerType type = FindType(m_last_type).value_or(ControllerType::GameCube);
  return BuildDeviceSettings(device_id, type, 0, Section(), {});
}

std::vector<std::string> InputMappingHost::EnumerateProfileNames(std::uint8_t player)
{
  std::vector<std::string> names;
  if (player >= MAX_PLAYERS)
    return names;

  const std::string directory = GetProfileDirectory(GetPersistedType(player));
  for (const std::string& path : Common::DoFileSearch(directory, ".ini", false))
  {
    std::string name;
    SplitPath(path, nullptr, &name, nullptr);
    names.push_back(std::move(name));
  }
  std::ranges::sort(names);
  return names;
}

ControllerProfileDraft InputMappingHost::LoadProfileDraft(std::uint8_t player,
                                                          std::optional<std::uint64_t> title_id)
{
  if (player >= MAX_PLAYERS)
  {
    ControllerProfileDraft draft;
    draft.player = player;
    draft.titleId = title_id;
    return draft;
  }

  ControllerType type = GetPersistedType(player);
  std::optional<Section> section;
  if (const std::string game_id = ResolveGameId(title_id); !game_id.empty())
  {
    // While a game runs, its mapping starts at the controller of its console.
    if (IsRunningGame(game_id) && IsWii(type) != Core::System::GetInstance().IsWii())
      type = IsWii(type) ? ControllerType::GameCube : GetWiiRemoteType(player);
    const std::optional<Section> gamecube =
        ReadTitleSection(ControllerType::GameCube, player, game_id);
    const std::optional<Section> wii = ReadTitleSection(ControllerType::WiiRemote, player, game_id);
    if (wii && (!gamecube || IsWii(type)))
    {
      std::string extension;
      wii->Get("Extension", &extension, "None");
      if (extension.empty() || extension == "None")
      {
        const ControllerPreference pref = GetWiiControllerPreference(game_id);
        if (pref == ControllerPreference::Classic)
          extension = "Classic";
        else if (pref == ControllerPreference::WiiRemoteNunchuk)
          extension = "Nunchuk";
      }
      type = TypeFromExtension(extension);
      section = wii;
    }
    else if (gamecube)
    {
      type = ControllerType::GameCube;
      section = gamecube;
    }
    else if (IsRunningGame(game_id) && Core::System::GetInstance().IsWii())
    {
      const ControllerPreference pref = GetWiiControllerPreference(game_id);
      if (pref == ControllerPreference::Classic)
        type = ControllerType::Classic;
      else if (pref == ControllerPreference::GameCube)
        type = ControllerType::GameCube;
      else
        type = ControllerType::WiiNunchuk;
    }
  }

  if (!section)
    section = ReadSectionToChange(type, player, {});

  m_last_type = GetTypeInfo(type).id;
  ControllerProfileDraft draft = BuildDraft(type, player, title_id, *section, {});

  // For Player 2, 3, and 4, automatically select their corresponding gamepad
  // if the profile is still pointing to Player 1's gamepad or is unset.
  if (player > 0)
  {
    const std::string expected_device = FindDefaultGamepad(player);
    if (!expected_device.empty())
    {
      const std::string default_p1_device = FindDefaultGamepad(0);
      if (draft.defaultDeviceId.empty() || draft.defaultDeviceId == default_p1_device)
      {
        draft.defaultDeviceId = expected_device;
        if (!default_p1_device.empty())
          std::erase(draft.attachedDeviceIds, default_p1_device);
        if (std::ranges::find(draft.attachedDeviceIds, expected_device) ==
            draft.attachedDeviceIds.end())
        {
          draft.attachedDeviceIds.push_back(expected_device);
        }
        for (auto& ds : draft.deviceSettings)
        {
          if (ds.deviceId == default_p1_device)
            ds.deviceId = expected_device;
        }
      }
    }
  }

  return draft;
}

ControllerProfileDraft
InputMappingHost::LoadNamedProfileDraft(std::uint8_t player, std::string_view profile_name,
                                        std::optional<std::uint64_t> title_id)
{
  ControllerProfileDraft draft;
  if (player >= MAX_PLAYERS)
    return draft;

  ControllerType type = GetPersistedType(player);
  const std::string name = SanitizeProfileName(profile_name);
  const std::optional<Section> section = ReadProfileFile(GetProfileDirectory(type) + name + ".ini");
  if (!section)
    return draft;

  if (IsWii(type))
  {
    std::string extension;
    section->Get("Extension", &extension, "None");
    type = TypeFromExtension(extension);
  }
  m_last_type = GetTypeInfo(type).id;
  return BuildDraft(type, player, title_id, *section, name);
}

ControllerProfileDraft InputMappingHost::CreateDefaultProfile(std::uint8_t player,
                                                              std::optional<std::uint64_t> title_id,
                                                              std::string_view controller_type_id,
                                                              std::string_view preferred_device_id)
{
  if (player >= MAX_PLAYERS)
    return {};

  const ControllerType type = FindType(controller_type_id).value_or(GetPersistedType(player));
  std::string device(preferred_device_id);
  if (device.empty())
    device = FindDefaultGamepad(player);
  if (device.empty())
    device = FindKeyboard();

  m_last_type = GetTypeInfo(type).id;
  ControllerProfileDraft draft =
      BuildDraft(type, player, title_id, BuildDefaultSection(type, player, device), {});
  draft.dirty = true;
  return draft;
}

MappingSaveResult InputMappingHost::SaveProfileDraft(const ControllerProfileDraft& draft)
{
  if (draft.player >= MAX_PLAYERS)
    return {true, {}};
  if (m_save_forwarder)
  {
    m_save_forwarder(draft);
    return {true, {}};
  }

  const std::optional<ControllerType> found_type = FindType(draft.emulatedControllerTypeId);
  if (!found_type)
    return {false, "Unknown emulated controller"};

  const ControllerType type = *found_type;
  const int player = draft.player;
  const std::string game_id = ResolveGameId(draft.titleId);

  Section section = ReadSectionToChange(type, player, game_id);
  ApplyDraft(section, type, player, draft);

  if (const std::string name = SanitizeProfileName(draft.name); game_id.empty() && !name.empty())
  {
    if (!WriteProfileFile(GetProfileDirectory(type) + name + ".ini", section))
      return {false, "The controller profile could not be written"};
  }

  const MappingSaveResult result = StoreSection(type, player, section, game_id, true);
  if (!result.succeeded)
    return result;
  if (game_id.empty())
    Config::SetBase(GetPersistedTypeInfo(player), std::string(GetTypeInfo(type).id));
  if (game_id.empty() || IsRunningGame(game_id))
  {
    ConnectPort(type, player);
    Config::Save();
  }
  m_last_type = GetTypeInfo(type).id;
  return result;
}

std::string InputMappingHost::GetRunningGameId()
{
  // Dolphin loads no controller profiles for a game without an ID.
  const std::string game_id = SConfig::GetInstance().GetGameID();
  return IsRunningGame(game_id) && game_id != "00000000" ? game_id : std::string();
}

std::vector<std::pair<std::string, std::string>> InputMappingHost::GetWiiRemoteExtensions()
{
  std::vector<std::pair<std::string, std::string>> extensions;
  for (const TypeInfo& info : CONTROLLER_TYPES)
  {
    if (!IsWii(info.type))
      continue;
    const std::string_view label = info.type == ControllerType::WiiRemote  ? "None" :
                                   info.type == ControllerType::WiiNunchuk ? "Nunchuk" :
                                                                             info.label;
    extensions.emplace_back(info.id, label);
  }
  return extensions;
}

InputMappingHost::WiiRemoteOptions InputMappingHost::GetWiiRemoteOptions(int player)
{
  WiiRemoteOptions options;
  if (player < 0 || player >= MAX_PLAYERS)
    return options;
  const Section section = ReadLiveSection(ControllerType::WiiRemote, player);
  std::string extension;
  section.Get("Options/Sideways Wiimote", &options.sideways, false);
  section.Get("Options/Upright Wiimote", &options.upright, false);
  section.Get("Extension", &extension, "None");

  const std::string game_id = GetRunningGameId();
  if (!game_id.empty() && Core::System::GetInstance().IsWii())
  {
    bool explicit_user_none = false;
    std::string saved_ext;
    if (const auto title_sec = ReadTitleSection(ControllerType::WiiRemote, player, game_id))
    {
      if (title_sec->Get("Extension", &saved_ext, "") && saved_ext == "None" &&
          title_sec->Exists("Extension"))
      {
        explicit_user_none = true;
      }
    }

    if (!explicit_user_none && (extension == "None" || extension.empty()))
    {
      ControllerPreference pref = GetWiiControllerPreference(game_id);
      if (!saved_ext.empty() && saved_ext != "None")
      {
        if (saved_ext == "Classic")
          pref = ControllerPreference::Classic;
        else if (saved_ext == "Nunchuk")
          pref = ControllerPreference::WiiRemoteNunchuk;
      }

      if (pref == ControllerPreference::Classic)
      {
        extension = "Classic";
        SetLiveExtension(ControllerType::Classic, player);
      }
      else if (pref == ControllerPreference::WiiRemoteNunchuk)
      {
        extension = "Nunchuk";
        SetLiveExtension(ControllerType::WiiNunchuk, player);
      }
    }
  }

  options.type_id = GetTypeInfo(TypeFromExtension(extension)).id;
  return options;
}

MappingSaveResult InputMappingHost::SetWiiRemoteOrientation(int player, bool sideways, bool upright)
{
  if (player < 0 || player >= MAX_PLAYERS)
    return {false, "There is no such Wii Remote"};
  const ControllerType type =
      FindType(GetWiiRemoteOptions(player).type_id).value_or(ControllerType::WiiRemote);
  const std::string game_id = GetRunningGameId();
  Section section = ReadSectionToChange(type, player, game_id);
  section.Set("Options/Sideways Wiimote", sideways);
  section.Set("Options/Upright Wiimote", upright);
  return StoreSection(type, player, section, game_id);
}

MappingSaveResult InputMappingHost::SetWiiRemoteExtension(int player, std::string_view type_id)
{
  const std::optional<ControllerType> type = FindType(type_id);
  if (player < 0 || player >= MAX_PLAYERS || !type || !IsWii(*type))
    return {false, "There is no such extension"};

  const std::string game_id = GetRunningGameId();
  Section section = ReadSectionToChange(*type, player, game_id);
  const std::string extension(GetTypeInfo(*type).extension);
  section.Set("Extension", extension);

  // An extension that was never mapped gets the default mapping of the player's device, and the
  // Wii Remote keeps its own mapping.
  const std::string prefix = extension + '/';
  const std::vector<std::string> keys = GetMappingKeys(*type, player);
  const bool mapped = std::ranges::any_of(
      keys, [&](const std::string& key) { return key.starts_with(prefix) && section.Exists(key); });
  if (*type != ControllerType::WiiRemote && !mapped)
  {
    std::string device;
    section.Get("Device", &device, "");
    for (const Binding& binding : GetDefaultBindings(*type, device))
    {
      if (binding.key.starts_with(prefix))
        section.Set(std::string(binding.key), std::string(binding.expression));
    }
  }
  SetLiveExtension(*type, player);
  return StoreSection(*type, player, section, game_id);
}

MappingSaveResult InputMappingHost::DeleteNamedProfile(std::string_view profile_name)
{
  const std::string name = SanitizeProfileName(profile_name);
  if (name.empty())
    return {false, "Choose a named profile to delete"};

  const ControllerType type = FindType(m_last_type).value_or(ControllerType::GameCube);
  const std::string path = GetProfileDirectory(type) + name + ".ini";
  if (!File::Exists(path) || !File::Delete(path))
    return {false, "The controller profile could not be deleted"};
  return {true, {}};
}

UwpImGuiFrontend::InputProfileLocation InputMappingHost::GetProfileLocation()
{
  if (!m_get_profile_location)
    return {};
  UwpImGuiFrontend::InputProfileLocation location = m_get_profile_location();
  location.changeable = static_cast<bool>(m_set_profile_location);
  return location;
}

MappingSaveResult InputMappingHost::SetProfileLocation(std::string_view choice)
{
  if (!m_set_profile_location)
    return {false, "Change the location of the profiles from Controllers on the home screen"};
  return m_set_profile_location(choice);
}

std::vector<UwpImGuiFrontend::InputCaptureSample> InputMappingHost::ReadLiveInputs()
{
  std::vector<UwpImGuiFrontend::InputCaptureSample> samples;
  ControllerInterface::SetCurrentInputChannel(ciface::InputChannel::Host);
  g_controller_interface.UpdateInput();

  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  for (const std::string& id : g_controller_interface.GetAllDeviceStrings())
  {
    const auto device = FindDevice(id);
    if (!device)
      continue;
    for (const ciface::Core::Device::Input* const input : device->Inputs())
    {
      if (input->IsHidden() || !input->IsDetectable())
        continue;
      const std::string name = input->GetName();
      samples.push_back({id, name, static_cast<float>(std::clamp(input->GetState(), 0.0, 1.0)),
                         ClassifyDeviceInput(name)});
    }
  }
  return samples;
}

void InputMappingHost::TestRumble(std::string_view device_id, float strength,
                                  std::chrono::milliseconds duration)
{
  const auto device = FindDevice(device_id);
  if (!device)
    return;

  for (ciface::Core::Device::Output* const output : device->Outputs())
    output->SetState(std::clamp(strength, 0.0f, 1.0f));
  m_rumble_device = device_id;
  m_rumble_deadline = std::chrono::steady_clock::now() + duration;
}

void InputMappingHost::ResetMappingRuntime()
{
  ControllerInterface::SetCurrentInputChannel(ciface::InputChannel::Host);
}
}  // namespace DolphinUWP
