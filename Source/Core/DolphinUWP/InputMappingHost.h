// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <UwpImGuiFrontend/InputMapping.h>

#include "Common/CommonTypes.h"

namespace DolphinUWP
{
class InputMappingHost final : public UwpImGuiFrontend::IInputMappingHost
{
public:
  using GameIdResolver = std::function<std::string(std::uint64_t)>;
  using DsuServerEnumerator = std::function<std::vector<UwpImGuiFrontend::DsuServerModel>()>;
  using TextureLoader =
      std::function<UwpImGuiFrontend::TextureHandle(const std::filesystem::path&)>;
  using TextureReleaser = std::function<void(UwpImGuiFrontend::TextureHandle)>;
  using SaveForwarder = std::function<void(UwpImGuiFrontend::ControllerProfileDraft)>;
  using ProfileLocationGetter = std::function<UwpImGuiFrontend::InputProfileLocation()>;
  using ProfileLocationSetter =
      std::function<UwpImGuiFrontend::MappingSaveResult(std::string_view choice)>;

  struct WiiRemoteOptions
  {
    bool sideways = false;
    bool upright = false;
    // The emulated controller that the attached extension makes, such as "wii-remote-nunchuk".
    std::string type_id;
  };

  InputMappingHost();
  ~InputMappingHost() override;

  void SetGameIdResolver(GameIdResolver resolver) { m_game_id_resolver = std::move(resolver); }
  void SetDsuServerEnumerator(DsuServerEnumerator enumerator)
  {
    m_dsu_servers = std::move(enumerator);
  }
  void SetTextureCallbacks(std::filesystem::path frontend_resources,
                           std::filesystem::path system_resources, TextureLoader load,
                           TextureReleaser release);
  void ReleaseTextures();
  void Tick();
  // The game overlay draws the mapper on the video thread, where saving could deadlock, as saving
  // can pause the CPU thread. The overlay hands the profiles to the UI thread to save instead.
  void SetSaveForwarder(SaveForwarder forwarder) { m_save_forwarder = std::move(forwarder); }
  // Without a setter, the mapper shows the location of the profiles without changing it.
  void SetProfileLocationHandlers(ProfileLocationGetter get, ProfileLocationSetter set = {})
  {
    m_get_profile_location = std::move(get);
    m_set_profile_location = std::move(set);
  }

  // The running game, whose own controller profiles the game overlay changes. Empty when no game
  // runs, or the game has no ID to keep profiles under.
  static std::string GetRunningGameId();
  // The extensions of an emulated Wii Remote, as controller types and labels.
  static std::vector<std::pair<std::string, std::string>> GetWiiRemoteExtensions();
  static WiiRemoteOptions GetWiiRemoteOptions(int player);
  // Changes a player's emulated Wii Remote in the running game and keeps the change.
  static UwpImGuiFrontend::MappingSaveResult SetWiiRemoteOrientation(int player, bool sideways,
                                                                     bool upright);
  static UwpImGuiFrontend::MappingSaveResult SetWiiRemoteExtension(int player,
                                                                   std::string_view type_id);

  static bool ApplyDefaultGamepadMappings(int player, const std::string& device);
  // Plays a gamepad that connected as the player with its number: gives the player the default
  // mappings of the gamepad when they have none, and plugs in their controller and Wii Remote.
  static void AssignGamepad(int player, const std::string& device);
  // Unplugs the controller and Wii Remote when a gamepad disconnects.
  static void DisconnectGamepad(int player);
  // Configures controller ports and defaults according to the running game's compatibility.
  static void OnGameStarted(const std::string& game_id);
  // Installs from before the integrated GBA have no Game Boy Advance mappings. Gives each player
  // with a gamepad the default GBA mapping of that gamepad, as a new install has.
  static void AddMissingGBAMappings();

  std::vector<UwpImGuiFrontend::InputDeviceDescriptor> EnumerateInputDevices() override;
  void RefreshInputDevices() override;
  std::vector<UwpImGuiFrontend::DsuServerModel> EnumerateDsuServers() override;
  std::vector<UwpImGuiFrontend::EmulatedControllerDescriptor>
  EnumerateEmulatedControllerTypes(std::uint8_t player) override;
  UwpImGuiFrontend::TextureHandle
  GetControllerPreviewTexture(std::string_view controller_type_id) override;
  std::vector<UwpImGuiFrontend::EmulatedControlDescriptor>
  EnumerateEmulatedControls(std::uint8_t player, std::string_view controller_type_id) override;
  std::vector<UwpImGuiFrontend::InputSettingDraft>
  CreateControllerSettings(std::uint8_t player, std::string_view controller_type_id) override;
  UwpImGuiFrontend::InputDeviceSettingsDraft
  CreateInputDeviceSettings(std::string_view device_id) override;
  std::vector<std::string> EnumerateProfileNames(std::uint8_t player) override;
  UwpImGuiFrontend::ControllerProfileDraft
  LoadProfileDraft(std::uint8_t player, std::optional<std::uint64_t> title_id) override;
  UwpImGuiFrontend::ControllerProfileDraft
  LoadNamedProfileDraft(std::uint8_t player, std::string_view profile_name,
                        std::optional<std::uint64_t> title_id) override;
  UwpImGuiFrontend::ControllerProfileDraft
  CreateDefaultProfile(std::uint8_t player, std::optional<std::uint64_t> title_id,
                       std::string_view controller_type_id = {},
                       std::string_view preferred_device_id = {}) override;
  UwpImGuiFrontend::MappingSaveResult
  SaveProfileDraft(const UwpImGuiFrontend::ControllerProfileDraft& draft) override;
  UwpImGuiFrontend::MappingSaveResult DeleteNamedProfile(std::string_view profile_name) override;
  UwpImGuiFrontend::InputProfileLocation GetProfileLocation() override;
  UwpImGuiFrontend::MappingSaveResult SetProfileLocation(std::string_view choice) override;
  std::vector<UwpImGuiFrontend::InputCaptureSample> ReadLiveInputs() override;
  void TestRumble(std::string_view device_id, float strength,
                  std::chrono::milliseconds duration) override;
  void ResetMappingRuntime() override;

private:
  std::string ResolveGameId(std::optional<std::uint64_t> title_id) const;

  GameIdResolver m_game_id_resolver;
  DsuServerEnumerator m_dsu_servers;
  std::filesystem::path m_frontend_resources;
  std::filesystem::path m_system_resources;
  TextureLoader m_load_texture;
  TextureReleaser m_release_texture;
  SaveForwarder m_save_forwarder;
  ProfileLocationGetter m_get_profile_location;
  ProfileLocationSetter m_set_profile_location;
  std::unordered_map<std::string, UwpImGuiFrontend::TextureHandle> m_previews;
  std::string m_rumble_device;
  std::optional<std::chrono::steady_clock::time_point> m_rumble_deadline;
  std::string m_last_type;
};
}  // namespace DolphinUWP
