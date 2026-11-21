// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <UwpImGuiFrontend/Input.h>
#include <UwpImGuiFrontend/InputMapper.h>
#include <UwpImGuiFrontend/Views.h>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"
#include "Common/WindowSystemInfo.h"
#include "DolphinUWP/DebuggerView.h"
#include "DolphinUWP/GBAScreens.h"
#include "DolphinUWP/HotkeyScheduler.h"
#include "DolphinUWP/InputMappingHost.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/QuickMenu.h"
#include "DolphinUWP/TasView.h"

struct BootParameters;
struct ImFont;
struct ImGuiContext;

namespace DolphinUWP
{
struct ShellServices;

class EmulationSession
{
public:
  using Notify = std::function<void(std::string)>;

  EmulationSession();
  ~EmulationSession();

  EmulationSession(const EmulationSession&) = delete;
  EmulationSession& operator=(const EmulationSession&) = delete;

  void SetNotify(Notify notify) { m_notify = std::move(notify); }
  void SetOverlayFont(std::span<const u8> font_data) { m_font_data = font_data; }
  void SetThemeIndex(int theme_index) { m_theme_index = theme_index; }
  void SetChatSender(QuickMenu::ChatSender sender) { m_menu.SetChatSender(std::move(sender)); }
  void SetExitLabel(std::string label) { m_menu.SetExitLabel(std::move(label)); }
  // Asks every NetPlay player to stop before this player leaves the game.
  void SetNetPlayStopper(std::function<void()> stopper) { m_netplay_stopper = std::move(stopper); }
  void SetGolfRequester(std::function<void()> requester)
  {
    m_golf_requester = std::move(requester);
  }
  void SetServices(const ShellServices* services);
  void SetMapperResources(std::filesystem::path frontend_resources,
                          std::filesystem::path system_resources,
                          InputMappingHost::DsuServerEnumerator dsu_servers,
                          InputMappingHost::ProfileLocationGetter profile_location);

  bool Start(std::unique_ptr<BootParameters> boot, std::string title, const WindowSystemInfo& wsi);
  void Update(const UwpImGuiFrontend::FrameInput& input, bool quick_menu_requested,
              bool input_held);
  void RequestStop();
  // Stops after asking first, when the setting to confirm stopping is on.
  void ConfirmStop();
  void HandleHotkey(HotkeyScheduler::Action action, int argument);
  // Follows a change of the running title, such as a game that the Wii Menu starts.
  void OnTitleChanged();
  void Shutdown();
  void Suspend();
  // Writes save data that is still in memory, such as recent memory card saves, to its files.
  void FlushSaveData();
  void Resume();
  // Pauses while the app is in the background, when the setting is enabled.
  void SetFocused(bool focused);

  bool IsActive() const { return m_active; }
  // True while an in-game overlay or a file picker owns the controller.
  bool IsOverlayActive() const
  {
    return m_overlay != Overlay::None || m_prompt_visible || m_picker_open;
  }
  bool ConsumeFinished() { return std::exchange(m_finished, false); }

  // Keeps controller input away from the game until every button is released.
  void HoldInputUntilReleased();

private:
  enum class Overlay
  {
    None,
    Menu,
    Debugger,
    Tas,
    Mapper,
  };

  struct OverlayFrame
  {
    Overlay overlay = Overlay::None;
    UwpImGuiFrontend::QuickMenuModel menu;
    DebuggerSnapshot debugger;
    TasSnapshot tas;
    std::optional<PromptService::Prompt> prompt;
    UwpImGuiFrontend::FrameInput input;
    bool input_held = false;
    bool mapper_game_scope = false;
    u64 focus_generation = 0;
    u64 open_generation = 0;
    int theme_index = 0;
  };

  struct OverlayResult
  {
    u64 focus_generation = 0;
    UwpImGuiFrontend::QuickMenuResult menu;
    std::optional<std::pair<u64, bool>> answer;
    bool mapper_closed = false;
    std::vector<UwpImGuiFrontend::ControllerProfileDraft> mapper_saved;
    std::string mapper_error;
  };

  struct DirectionRepeat
  {
    bool Update(bool held, float delta_seconds);

    bool was_held = false;
    float elapsed = 0.0f;
    float next_repeat = 0.30f;
  };

  void OpenQuickMenu();
  void OpenDebugger();
  void OpenTasInput(TasView::Target target);
  void CloseTasInput();
  void OpenMapper();
  // Saves what the mapper saved, and shows the quick menu again.
  void CloseMapper(const OverlayResult& result);
  // Shows a picker while the game is paused, and resumes it when the picker closes.
  void PickWhilePaused(bool folder, std::vector<std::wstring> file_types, std::wstring token,
                       std::function<void(const std::string&)> picked);
  void PickDisc();
  void PickStateToLoad();
  void PickStateToSave();
  void PickFileToStart();
  void PickGBAFile(QuickMenu::Effect effect);
  // Shows a picker for the debugger, which stays open while the CPU is stopped.
  void PickForDebugger(DebuggerView::PickRequest request);
  void HandleGBAROMHotkey(bool load);
  void CloseOverlay();
  void RebuildMenu(bool refocus);
  void ApplyMenuEffect(QuickMenu::Effect effect);
  void ApplyResult(const OverlayResult& result);
  void UpdatePrompt(const std::optional<PromptService::Prompt>& prompt);
  static bool IsEmulationWaitingForAnswer();
  void PublishOverlay(const UwpImGuiFrontend::FrameInput& input,
                      const std::optional<PromptService::Prompt>& prompt, bool input_held);
  UwpImGuiFrontend::FrameInput MakeEdgeInput(const UwpImGuiFrontend::FrameInput& input);
  void RequestPresent(bool cpu_blocked);
  void FinishStop();

  void DrawOverlay();
  std::optional<bool> DrawPrompt(const PromptService::Prompt& prompt,
                                 const UwpImGuiFrontend::ShellTheme& theme, bool focus_default);
  ImFont* GetOverlayFont();
  UwpImGuiFrontend::FrameInput FilterMapperInput(const OverlayFrame& frame);
  UwpImGuiFrontend::FrameInput MakeMapperInput(const UwpImGuiFrontend::FrameInput& input);
  bool DrawMapper(const OverlayFrame& frame, const UwpImGuiFrontend::FrameInput& input,
                  ImFont* font, OverlayResult* result);
  UwpImGuiFrontend::TextureHandle LoadMapperTexture(const std::filesystem::path& path);
  void ResetMapperTextures();
  void DrawGBAScreens();
  ImTextureData* UpdateGBATexture(size_t index, bool blending);
  void ResetGBAScreens();

  Notify m_notify;
  std::function<void()> m_netplay_stopper;
  std::function<void()> m_golf_requester;
  const ShellServices* m_services = nullptr;
  std::span<const u8> m_font_data;
  int m_theme_index = 0;
  std::string m_title;

  Common::EventHook m_state_hook;
  Common::EventHook m_overlay_hook;

  std::atomic<bool> m_active = false;
  std::atomic<bool> m_core_stopped = false;
  std::atomic<Overlay> m_overlay = Overlay::None;
  std::atomic<bool> m_prompt_visible = false;
  std::atomic<bool> m_present_pending = false;
  bool m_picker_open = false;
  bool m_finished = false;
  bool m_stop_requested = false;
  bool m_stop_confirming = false;
  std::string m_title_game_id;
  int m_wii_extension_sync_frame = 0;
  bool m_wii_extension_synced = false;
  bool m_paused_by_menu = false;
  bool m_paused_by_suspend = false;
  bool m_paused_by_user = false;
  bool m_paused_by_focus = false;
  bool m_release_barrier = false;
  std::optional<std::chrono::steady_clock::time_point> m_force_stop_deadline;
  std::chrono::steady_clock::time_point m_last_present_request;
  std::chrono::steady_clock::time_point m_last_debugger_refresh;

  QuickMenu m_menu;
  DebuggerView m_debugger;
  TasView m_tas;
  UwpImGuiFrontend::QuickMenuModel m_menu_model;
  DebuggerSnapshot m_debugger_snapshot;
  TasSnapshot m_tas_snapshot;
  bool m_model_dirty = false;
  u64 m_focus_generation = 0;
  u64 m_open_generation = 0;
  u64 m_prompt_id = 0;

  UwpImGuiFrontend::FrameInput m_previous_input;
  std::array<DirectionRepeat, 4> m_repeat;

  std::mutex m_overlay_mutex;
  OverlayFrame m_overlay_frame;
  std::vector<OverlayResult> m_results;

  UwpImGuiFrontend::FrameInput m_overlay_previous_input;
  u64 m_drawn_focus_generation = 0;
  u64 m_drawn_open_generation = 0;
  ImGuiContext* m_overlay_font_context = nullptr;
  ImFont* m_overlay_font = nullptr;

  // The controller mapper of the overlay saves on the UI thread, and the rest of it runs on the
  // video thread.
  InputMappingHost m_mapping;
  InputMappingHost m_overlay_mapping;
  bool m_mapper_game_scope = false;
  UwpImGuiFrontend::InputMapperView m_mapper;
  std::vector<UwpImGuiFrontend::ControllerProfileDraft> m_mapper_saved;
  u64 m_mapper_open_generation = 0;
  bool m_mapper_release_barrier = false;
  std::array<DirectionRepeat, 4> m_mapper_repeat;
  std::chrono::steady_clock::time_point m_mapper_last_draw;
  ImGuiContext* m_mapper_context = nullptr;
  std::unordered_map<std::string, std::unique_ptr<ImTextureData>> m_mapper_textures;

  // Drawn on the video thread. The textures belong to the ImGui context of the video backend.
  ImGuiContext* m_gba_context = nullptr;
  std::array<std::unique_ptr<ImTextureData>, GBAScreens::MAX_SCREENS> m_gba_textures;
  std::array<u64, GBAScreens::MAX_SCREENS> m_gba_frames{};
  std::array<std::vector<u32>, GBAScreens::MAX_SCREENS> m_gba_previous;
  std::vector<u32> m_gba_pixels;
};
}  // namespace DolphinUWP
