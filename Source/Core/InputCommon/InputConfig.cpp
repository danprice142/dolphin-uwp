// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/InputConfig.h"

#include <utility>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputProfile.h"

InputConfig::InputConfig(std::string ini_name, std::string gui_name,
                         std::string profile_directory_name, std::string profile_key)
    : m_ini_name(std::move(ini_name)), m_gui_name(std::move(gui_name)),
      m_profile_directory_name(std::move(profile_directory_name)),
      m_profile_key(std::move(profile_key))
{
}

InputConfig::~InputConfig() = default;

bool InputConfig::LoadConfig()
{
  Common::IniFile inifile;
  bool useProfile[MAX_BBMOTES] = {false, false, false, false, false};
  static constexpr std::array<std::string_view, MAX_BBMOTES> num = {"1", "2", "3", "4", "BB"};
  std::string profile[MAX_BBMOTES];

  if (SConfig::GetInstance().GetGameID() != "00000000")
  {
    const std::string profile_directory = GetUserProfileDirectoryPath();

    Common::IniFile game_ini = SConfig::GetInstance().LoadGameIni();
    auto* control_section = game_ini.GetOrCreateSection("Controls");

    for (int i = 0; i < 4; i++)
    {
      const auto profile_name = fmt::format("{}Profile{}", GetProfileKey(), num[i]);

      if (control_section->Exists(profile_name))
      {
        std::string profile_setting;
        if (control_section->Get(profile_name, &profile_setting))
        {
          auto profiles = InputProfile::GetProfilesFromSetting(profile_setting, profile_directory);

          if (profiles.empty())
          {
            // TODO: PanicAlert shouldn't be used for this.
            PanicAlertFmtT("No profiles found for game setting '{0}'", profile_setting);
            continue;
          }

          // Use the first profile by default
          profile[i] = profiles[0];
          useProfile[i] = true;
        }
      }
    }
  }

  const bool inifile_loaded =
      inifile.Load(File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini") &&
      !inifile.GetSections().empty();

  int n = 0;
  for (auto& controller : m_controllers)
  {
    Common::IniFile::Section config;
    bool loaded = false;
    if (n < MAX_BBMOTES && useProfile[n])
    {
      std::string base;
      SplitPath(profile[n], nullptr, &base, nullptr);
      Core::DisplayMessage("Loading game specific input profile '" + base + "' for device '" +
                               controller->GetName() + "'",
                           6000);

      Common::IniFile profile_ini;
      if (profile_ini.Load(profile[n]))
      {
        config = *profile_ini.GetOrCreateSection("Profile");
        loaded = true;
      }
    }

    if (!loaded && inifile_loaded)
    {
      config = *inifile.GetOrCreateSection(controller->GetName());
      loaded = true;
    }

    if (loaded)
    {
      controller->LoadConfig(&config);
      controller->UpdateReferences(g_controller_interface);
    }
    else
    {
      // Only load the default profile for the first controller and clear the others,
      // otherwise they would all share the same mappings on the same (default) device
      if (n == 0)
        controller->LoadDefaults(g_controller_interface);
      else
        controller->EmulatedController::LoadDefaults(g_controller_interface);
      controller->UpdateReferences(g_controller_interface);
    }

    n++;
  }
  return inifile_loaded;
}

void InputConfig::SaveConfig()
{
  std::string ini_filename = File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini";

  Common::IniFile inifile;
  inifile.Load(ini_filename);

  for (auto& controller : m_controllers)
  {
    controller->SaveConfig(inifile.GetOrCreateSection(controller->GetName()));
  }

  inifile.Save(ini_filename);
}

ControllerEmu::EmulatedController* InputConfig::GetController(int index) const
{
  return m_controllers.at(index).get();
}

void InputConfig::ClearControllers()
{
  m_controllers.clear();
}

bool InputConfig::ControllersNeedToBeCreated() const
{
  return m_controllers.empty();
}

std::string InputConfig::GetUserProfileDirectoryPath() const
{
  return fmt::format("{}{}/", File::GetUserPath(D_CONTROLLERPROFILES_IDX),
                     GetProfileDirectoryName());
}

std::string InputConfig::GetSysProfileDirectoryPath() const
{
  return fmt::format("{}Profiles/{}/", File::GetSysDirectory(), GetProfileDirectoryName());
}

int InputConfig::GetControllerCount() const
{
  return static_cast<int>(m_controllers.size());
}

void InputConfig::RegisterHotplugCallback()
{
  // Update control references on all controllers
  // as configured devices may have been added or removed.
  m_hotplug_event_hook = g_controller_interface.RegisterDevicesChangedCallback([this] {
    for (auto& controller : m_controllers)
      controller->UpdateReferences(g_controller_interface);
  });
}

void InputConfig::UnregisterHotplugCallback()
{
  m_hotplug_event_hook.reset();
}

bool InputConfig::IsControllerControlledByGamepadDevice(int index) const
{
  if (static_cast<size_t>(index) >= m_controllers.size())
    return false;

  const auto& controller = m_controllers.at(index).get()->GetDefaultDevice();

  // By default on Android, no device is selected
  if (controller.source == "")
    return false;

  // Filter out anything which obviously not a gamepad
  return !((controller.source == "Quartz")      // OSX Quartz Keyboard/Mouse
           || (controller.source == "XInput2")  // Linux and BSD Keyboard/Mouse
           || (controller.source == "Android" && controller.cid <= 0)  // Android non-gamepad device
           || (controller.source == "DInput" &&
               controller.name == "Keyboard Mouse"));  // Windows Keyboard/Mouse
}

void InputConfig::GenerateControllerTextures(const Common::IniFile& file)
{
  m_dynamic_input_tex_config_manager.Load();

  std::vector<std::string> controller_names;
  for (auto& controller : m_controllers)
  {
    controller_names.push_back(controller->GetName());
  }

  m_dynamic_input_tex_config_manager.GenerateTextures(file, controller_names);
}

void InputConfig::GenerateControllerTextures()
{
  const std::string ini_filename = File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini";

  Common::IniFile inifile;
  inifile.Load(ini_filename);

  for (auto& controller : m_controllers)
  {
    controller->SaveConfig(inifile.GetOrCreateSection(controller->GetName()));
  }

  GenerateControllerTextures(inifile);
}
