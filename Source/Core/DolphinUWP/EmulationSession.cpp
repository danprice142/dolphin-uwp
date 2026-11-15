// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/EmulationSession.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <UwpImGuiFrontend/ImGuiShell.h>
#include <UwpImGuiFrontend/Widgets.h>

#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Common/StringUtil.h"
#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"
#include "DolphinUWP/GBAControls.h"
#include "DolphinUWP/GameLibrary.h"
#include "DolphinUWP/HostState.h"
#include "DolphinUWP/MediaStreamer.h"
#include "DolphinUWP/Recordings.h"
#include "DolphinUWP/ShellServices.h"
#include "DolphinUWP/Translation.h"
#include "DolphinUWP/UWPSettings.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoEvents.h"

namespace DolphinUWP
{
namespace
{
using UwpImGuiFrontend::FrameInput;
using UwpImGuiFrontend::QuickMenuResult;

constexpr u32 STATE_SLOT_COUNT = 10;
constexpr auto GRACEFUL_STOP_TIMEOUT = std::chrono::seconds(5);
constexpr auto PRESENT_INTERVAL = std::chrono::milliseconds(15);
constexpr auto DEBUGGER_REFRESH_INTERVAL = std::chrono::milliseconds(500);
constexpr float OVERLAY_FONT_SIZE = 24.0f;
// Stands for the running game in the overlay's controller mapper.
constexpr std::uint64_t RUNNING_GAME_TITLE = 0;

UwpImGuiFrontend::WidgetInputState ToWidgetInput(const FrameInput& input)
{
  return {input.accept, input.back,  input.up,           input.down,
          input.left,   input.right, input.leftShoulder, input.rightShoulder};
}

const UwpImGuiFrontend::ShellTheme& GetTheme(int index)
{
  const UwpImGuiFrontend::ShellThemeCollection& themes = UwpImGuiFrontend::ShellThemes();
  return themes[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(themes.size()) - 1))];
}
}  // namespace

bool EmulationSession::DirectionRepeat::Update(bool held, float delta_seconds)
{
  if (!held)
  {
    was_held = false;
    elapsed = 0.0f;
    next_repeat = 0.30f;
    return false;
  }
  if (!was_held)
  {
    was_held = true;
    return true;
  }
  elapsed += std::clamp(delta_seconds, 0.0f, 0.1f);
  if (elapsed < next_repeat)
    return false;
  elapsed = 0.0f;
  next_repeat = 0.08f;
  return true;
}

EmulationSession::EmulationSession()
{
  const auto running_game = [](std::uint64_t) { return SConfig::GetInstance().GetGameID(); };
  m_mapping.SetGameIdResolver(running_game);
  m_overlay_mapping.SetGameIdResolver(running_game);
  m_overlay_mapping.SetSaveForwarder([this](UwpImGuiFrontend::ControllerProfileDraft draft) {
    m_mapper_saved.push_back(std::move(draft));
  });
}

EmulationSession::~EmulationSession()
{
  Shutdown();
}

void EmulationSession::SetServices(const ShellServices* services)
{
  m_services = services;
  m_menu.SetLibrary(services ? services->library : nullptr);
}

void EmulationSession::SetMapperResources(std::filesystem::path frontend_resources,
                                          std::filesystem::path system_resources,
                                          InputMappingHost::DsuServerEnumerator dsu_servers,
                                          InputMappingHost::ProfileLocationGetter profile_location)
{
  m_overlay_mapping.SetDsuServerEnumerator(std::move(dsu_servers));
  m_overlay_mapping.SetProfileLocationHandlers(std::move(profile_location));
  m_overlay_mapping.SetTextureCallbacks(
      std::move(frontend_resources), std::move(system_resources),
      [this](const std::filesystem::path& path) { return LoadMapperTexture(path); },
      [](UwpImGuiFrontend::TextureHandle) {});
}

bool EmulationSession::Start(std::unique_ptr<BootParameters> boot, std::string title,
                             const WindowSystemInfo& wsi)
{
  if (m_active || !boot)
    return false;

  m_title = std::move(title);
  m_menu.SetTitle(m_title);
  m_title_game_id.clear();
  m_wii_extension_sync_frame = 0;
  m_wii_extension_synced = false;
  m_core_stopped = false;
  m_finished = false;
  m_stop_requested = false;
  m_stop_confirming = false;
  m_paused_by_menu = false;
  m_paused_by_suspend = false;
  m_paused_by_user = false;
  m_paused_by_focus = false;
  m_release_barrier = false;
  m_force_stop_deadline.reset();
  m_present_pending = false;
  m_picker_open = false;
  m_overlay = Overlay::None;
  m_prompt_visible = false;
  m_prompt_id = 0;
  m_overlay_font_context = nullptr;
  m_overlay_font = nullptr;
  ResetGBAScreens();
  HostState::ConsumeStopRequest();
  HostState::ConsumeDebuggerUpdate();
  HostState::SetControllerInputBlocked(false);

  m_state_hook = Core::AddOnStateChangedCallback([this](Core::State state) {
    if (state == Core::State::Uninitialized)
      m_core_stopped = true;

    // With a separate video thread, the overlay can present a prompt while the CPU thread waits.
    PromptService& prompts = PromptService::GetInstance();
    if (state == Core::State::Running)
      prompts.SetEmulationPromptsAllowed(Core::System::GetInstance().IsDualCoreMode());
    else if (state == Core::State::Stopping || state == Core::State::Uninitialized)
      prompts.SetEmulationPromptsAllowed(false);
  });
  m_overlay_hook = GetVideoEvents().on_screen_ui_event.Register([this] { DrawOverlay(); });
  PromptService::GetInstance().SetEmulationPromptsAllowed(false);

  auto& system = Core::System::GetInstance();
  if (!BootManager::BootCore(system, std::move(boot), wsi))
  {
    m_overlay_hook.reset();
    m_state_hook.reset();
    return false;
  }

  m_active = true;
  OnTitleChanged();
  return true;
}

FrameInput EmulationSession::MakeEdgeInput(const FrameInput& input)
{
  const FrameInput& previous = m_previous_input;
  const bool up = input.up || input.leftStickY >= 0.55f;
  const bool down = input.down || input.leftStickY <= -0.55f;
  const bool left = input.left || input.leftStickX <= -0.55f;
  const bool right = input.right || input.leftStickX >= 0.55f;
  return {
      .deltaSeconds = input.deltaSeconds,
      .up = m_repeat[0].Update(up, input.deltaSeconds),
      .down = m_repeat[1].Update(down, input.deltaSeconds),
      .left = m_repeat[2].Update(left, input.deltaSeconds),
      .right = m_repeat[3].Update(right, input.deltaSeconds),
      .accept = input.accept && !previous.accept,
      .back = input.back && !previous.back,
      .context = input.context && !previous.context,
      .alternate = input.alternate && !previous.alternate,
      .menu = input.menu && !previous.menu,
      .view = input.view && !previous.view,
      .leftShoulder = input.leftShoulder && !previous.leftShoulder,
      .rightShoulder = input.rightShoulder && !previous.rightShoulder,
  };
}

void EmulationSession::Update(const FrameInput& input, bool quick_menu_requested, bool input_held)
{
  if (!m_active)
    return;

  auto& system = Core::System::GetInstance();
  if (HostState::ConsumeStopRequest())
  {
    m_stop_requested = true;
    m_force_stop_deadline.reset();
    PromptService::GetInstance().AnswerAll(true);
    Core::Stop(system);
  }

  if (m_force_stop_deadline && std::chrono::steady_clock::now() >= *m_force_stop_deadline)
  {
    m_force_stop_deadline.reset();
    Core::Stop(system);
  }

  if (m_core_stopped)
  {
    FinishStop();
    return;
  }

  const std::optional<PromptService::Prompt> prompt =
      PromptService::GetInstance().GetActivePrompt();
  UpdatePrompt(prompt);
  const bool overlay_prompt = prompt && !prompt->system_dialog;

  if (!Core::IsRunning(system) && !prompt)
  {
    m_previous_input = input;
    return;
  }

  if (m_active && system.IsWii() && Core::IsRunning(system) && m_overlay == Overlay::None &&
      !m_wii_extension_synced)
  {
    ++m_wii_extension_sync_frame;
    if (m_wii_extension_sync_frame >= 30)
    {
      m_wii_extension_synced = true;
      for (int player = 0; player < 4; ++player)
      {
        InputMappingHost::GetWiiRemoteOptions(player);
      }
    }
  }

  m_menu.Update();
  if (m_menu.ConsumeChanged() && m_overlay == Overlay::Menu)
    RebuildMenu(false);

  if (HostState::ConsumeDebuggerUpdate())
  {
    if (m_overlay == Overlay::Debugger)
    {
      m_debugger.OnCPUStopped();
    }
    else if (Config::IsDebuggingEnabled() && system.GetCPU().IsStepping() &&
             m_overlay == Overlay::None && !m_paused_by_user && !m_paused_by_suspend &&
             !m_stop_requested)
    {
      OpenDebugger();
    }
  }

  // Escape is also Back, which the open menu handles one page at a time.
  if (quick_menu_requested && !m_stop_requested && !prompt)
  {
    if (m_overlay == Overlay::None)
      OpenQuickMenu();
    else if (m_overlay == Overlay::Menu && !input.back)
      CloseOverlay();
  }

  std::vector<OverlayResult> results;
  {
    std::lock_guard lock(m_overlay_mutex);
    results.swap(m_results);
  }
  for (const OverlayResult& result : results)
  {
    if (result.focus_generation == m_focus_generation)
      ApplyResult(result);
  }

  const FrameInput edges = MakeEdgeInput(input);
  if (m_overlay == Overlay::Debugger && !prompt && !m_picker_open)
  {
    if (m_debugger.HandleInput(edges) == DebuggerView::Effect::Close)
    {
      m_paused_by_menu = false;
      CloseOverlay();
    }
    else
    {
      if (std::optional<DebuggerView::PickRequest> request = m_debugger.TakePickRequest())
        PickForDebugger(std::move(*request));
      const auto now = std::chrono::steady_clock::now();
      if (m_debugger.ConsumeChanged() || now - m_last_debugger_refresh > DEBUGGER_REFRESH_INTERVAL)
      {
        m_last_debugger_refresh = now;
        m_debugger_snapshot = m_debugger.BuildSnapshot();
        m_model_dirty = true;
      }
    }
  }
  if (m_overlay == Overlay::Tas && !prompt)
  {
    if (m_tas.HandleInput(edges) == TasView::Effect::Close)
    {
      CloseTasInput();
    }
    else
    {
      const auto now = std::chrono::steady_clock::now();
      if (m_tas.ConsumeChanged() || now - m_last_debugger_refresh > DEBUGGER_REFRESH_INTERVAL)
      {
        m_last_debugger_refresh = now;
        m_tas_snapshot = m_tas.BuildSnapshot();
        m_model_dirty = true;
      }
    }
  }
  m_previous_input = input;

  if (m_overlay != Overlay::None || overlay_prompt)
  {
    // While the system dialog is open, the menu behind it ignores the controller.
    PublishOverlay(prompt && !overlay_prompt ? FrameInput{.deltaSeconds = input.deltaSeconds} :
                                               input,
                   overlay_prompt ? prompt : std::nullopt, input_held);
    if (!prompt || overlay_prompt)
      RequestPresent(overlay_prompt && prompt->from_cpu_thread);
  }
  else if (m_release_barrier && !input_held)
  {
    m_release_barrier = false;
    HostState::SetControllerInputBlocked(false);
  }
}

void EmulationSession::UpdatePrompt(const std::optional<PromptService::Prompt>& prompt)
{
  const u64 id = prompt ? prompt->id : 0;
  if (id == m_prompt_id)
    return;
  m_prompt_id = id;
  m_prompt_visible = prompt.has_value();
  ++m_focus_generation;
  if (prompt)
  {
    HostState::SetControllerInputBlocked(true);
    m_release_barrier = false;
  }
  else if (m_overlay == Overlay::None)
  {
    m_release_barrier = true;
  }
}

void EmulationSession::HoldInputUntilReleased()
{
  if (!m_active)
  {
    HostState::SetControllerInputBlocked(false);
    return;
  }
  HostState::SetControllerInputBlocked(true);
  if (m_overlay == Overlay::None && !m_prompt_visible)
    m_release_barrier = true;
}

bool EmulationSession::IsEmulationWaitingForAnswer()
{
  const std::optional<PromptService::Prompt> prompt =
      PromptService::GetInstance().GetActivePrompt();
  return prompt && prompt->blocks_emulation;
}

void EmulationSession::RequestStop()
{
  if (!m_active || m_stop_requested)
    return;

  auto& system = Core::System::GetInstance();
  if (system.GetMovie().IsRecordingInput())
  {
    const std::string path = Recordings::SaveMovieRecording();
    if (!path.empty() && m_notify)
      m_notify(fmt::format("Saved the input recording to {}", path));
  }

  if (NetPlay::IsNetPlayRunning() && m_netplay_stopper)
    m_netplay_stopper();

  m_stop_requested = true;
  m_stop_confirming = false;
  PromptService::GetInstance().AnswerAll(true);
  if (UICommon::TriggerSTMPowerEvent())
  {
    if (Core::GetState(system) == Core::State::Paused)
      Core::SetState(system, Core::State::Running);
    m_force_stop_deadline = std::chrono::steady_clock::now() + GRACEFUL_STOP_TIMEOUT;
    return;
  }

  Core::Stop(system);
}

void EmulationSession::ConfirmStop()
{
  if (!m_active || m_stop_requested || m_stop_confirming)
    return;
  if (!Config::Get(Config::MAIN_CONFIRM_ON_STOP))
  {
    if (m_overlay != Overlay::None)
      CloseOverlay();
    RequestStop();
    return;
  }

  auto& system = Core::System::GetInstance();
  const bool resume = Core::GetState(system) == Core::State::Running;
  if (resume)
    Core::SetState(system, Core::State::Paused);
  m_stop_confirming = true;
  PromptService::GetInstance().Confirm(
      "Confirm", "Do you want to stop the current emulation?", [this, resume](bool confirmed) {
        if (!std::exchange(m_stop_confirming, false) || !m_active || m_stop_requested)
          return;
        if (confirmed)
        {
          if (m_overlay != Overlay::None)
            CloseOverlay();
          RequestStop();
          return;
        }
        if (resume && m_overlay == Overlay::None && !m_paused_by_user && !m_paused_by_suspend &&
            !m_paused_by_focus)
        {
          Core::SetState(Core::System::GetInstance(), Core::State::Running);
        }
      });
}

void EmulationSession::OnTitleChanged()
{
  if (!m_active)
    return;
  // The first title is the one that was started, which keeps its name from the library.
  const SConfig& config = SConfig::GetInstance();
  const std::string game_id = config.GetGameID();
  if (game_id.empty())
    return;
  const bool is_new_game = m_title_game_id != game_id;
  m_title_game_id = game_id;
  m_wii_extension_sync_frame = 0;
  m_wii_extension_synced = false;
  InputMappingHost::OnGameStarted(game_id);
  const std::string description = config.GetTitleDescription();
  if (!is_new_game || description.empty())
    return;
  m_title = description;
  m_menu.SetTitle(m_title);
}

void EmulationSession::HandleGBAROMHotkey(bool load)
{
  // The ROM can't change during input recordings or NetPlay.
  if (Core::System::GetInstance().GetMovie().IsMovieActive() || NetPlay::IsNetPlayRunning())
  {
    OSD::AddMessage("The GBA ROM can't change during an input recording or NetPlay");
    return;
  }

  // The hotkey acts on the GBA that has a player's controls, or else on the first GBA.
  std::optional<size_t> port;
  for (size_t player = 0; player < GBAScreens::MAX_SCREENS && !port; ++player)
    port = GBAControls::GetControlledGBA(player);
  for (size_t i = 0; i < GBAScreens::MAX_SCREENS && !port; ++i)
  {
    if (GBAScreens::IsRunning(i))
      port = i;
  }
  if (!port)
  {
    OSD::AddMessage("There is no Game Boy Advance");
    return;
  }

  if (load)
  {
    m_menu.SetGBAPort(*port);
    PickGBAFile(QuickMenu::Effect::PickGBAROM);
  }
  else
  {
    GBAScreens::LoadROM(*port, {});
  }
}

void EmulationSession::HandleHotkey(HotkeyScheduler::Action action, int argument)
{
  if (!m_active || m_stop_requested)
    return;

  using Action = HotkeyScheduler::Action;
  auto& system = Core::System::GetInstance();
  const auto show_slot = [this] {
    const u32 slot = m_menu.GetSelectedSlot();
    OSD::AddMessage(
        fmt::format("Selected slot {} - {}", slot, State::GetInfoStringOfSlot(slot, false)));
  };
  const auto open_menu_at = [this](std::string_view id) {
    if (m_overlay != Overlay::None)
      return;
    OpenQuickMenu();
    ApplyMenuEffect(m_menu.OpenAt(id));
  };
  const auto debugger = [this](auto&& operation) {
    if (!Config::IsDebuggingEnabled())
      return;
    if (m_overlay == Overlay::Menu)
      CloseOverlay();
    if (m_overlay != Overlay::Debugger)
      OpenDebugger();
    operation();
  };

  switch (action)
  {
  case Action::TogglePause:
    if (m_overlay != Overlay::None)
      break;
    if (Core::GetState(system) == Core::State::Paused)
    {
      m_paused_by_user = false;
      Core::SetState(system, Core::State::Running);
    }
    else if (Core::GetState(system) == Core::State::Running)
    {
      m_paused_by_user = true;
      Core::SetState(system, Core::State::Paused);
    }
    break;
  case Action::Stop:
    ConfirmStop();
    break;
  case Action::Reset:
    system.GetProcessorInterface().ResetButton_Tap();
    break;
  case Action::Screenshot:
    Core::SaveScreenShot();
    break;
  case Action::OpenFile:
    PickFileToStart();
    break;
  case Action::RefreshLibrary:
    if (m_services && m_services->library)
      m_services->library->Refresh();
    break;
  case Action::EjectDisc:
    if (!system.IsTriforce())
      system.GetDVDInterface().EjectDisc(Core::CPUThreadGuard{system}, DVD::EjectCause::User);
    break;
  case Action::ChangeDisc:
    if (!system.IsTriforce())
      open_menu_at("change-disc");
    break;
  case Action::StartRecording:
    OSD::AddMessage(Recordings::BeginMovieRecording() ? "Input recording started" :
                                                        "Input recording could not be started");
    break;
  case Action::ExportRecording:
  {
    const std::string path = Recordings::SaveMovieRecording();
    OSD::AddMessage(path.empty() ? std::string("There is no input recording to save") :
                                   fmt::format("Saved the input recording to {}", path));
    break;
  }
  case Action::ToggleReadOnly:
  {
    auto& movie = system.GetMovie();
    movie.SetReadOnly(!movie.IsReadOnly());
    OSD::AddMessage(movie.IsReadOnly() ? "Read-only mode" : "Read and write mode");
    break;
  }
  case Action::ConnectWiiRemote:
    if (system.IsWii())
    {
      const Core::CPUThreadGuard guard(system);
      if (const auto bluetooth = WiiUtils::GetBluetoothEmuDevice())
      {
        if (auto* const wiimote = bluetooth->AccessWiimoteByIndex(argument))
          wiimote->Activate(!wiimote->IsConnected());
      }
    }
    break;
  case Action::LoadStateSlot:
    m_menu.SetSelectedSlot(static_cast<u32>(argument));
    State::Load(system, static_cast<u32>(argument));
    break;
  case Action::SaveStateSlot:
    m_menu.SetSelectedSlot(static_cast<u32>(argument));
    State::Save(system, static_cast<u32>(argument));
    break;
  case Action::LoadLastState:
    State::LoadLastSaved(system, argument);
    break;
  case Action::LoadStateFile:
    PickStateToLoad();
    break;
  case Action::SaveStateFile:
    PickStateToSave();
    break;
  case Action::SelectStateSlot:
    m_menu.SetSelectedSlot(static_cast<u32>(argument));
    show_slot();
    break;
  case Action::SaveSelectedState:
    State::Save(system, m_menu.GetSelectedSlot());
    break;
  case Action::LoadSelectedState:
    State::Load(system, m_menu.GetSelectedSlot());
    break;
  case Action::IncrementStateSlot:
    m_menu.SetSelectedSlot(m_menu.GetSelectedSlot() % STATE_SLOT_COUNT + 1);
    show_slot();
    break;
  case Action::DecrementStateSlot:
    m_menu.SetSelectedSlot((m_menu.GetSelectedSlot() + STATE_SLOT_COUNT - 2) % STATE_SLOT_COUNT +
                           1);
    show_slot();
    break;
  case Action::SaveOldestState:
    State::SaveFirstSaved(system);
    break;
  case Action::UndoLoadState:
    State::UndoLoadState(system);
    break;
  case Action::UndoSaveState:
    State::UndoSaveState(system);
    break;
  case Action::OpenSkylanderPortal:
    open_menu_at("skylanders");
    break;
  case Action::OpenInfinityBase:
    open_menu_at("infinity");
    break;
  case Action::ActivateChat:
    open_menu_at("netplay-chat");
    break;
  case Action::RequestGolfControl:
    if (m_golf_requester && NetPlay::IsNetPlayRunning())
      m_golf_requester();
    break;
  case Action::DebugStep:
    debugger([this] { m_debugger.Step(); });
    break;
  case Action::DebugStepOver:
    debugger([this] { m_debugger.StepOver(); });
    break;
  case Action::DebugStepOut:
    debugger([this] { m_debugger.StepOut(); });
    break;
  case Action::DebugSkip:
    debugger([this] { m_debugger.Skip(); });
    break;
  case Action::DebugShowPC:
    debugger([this] { m_debugger.ShowPC(); });
    break;
  case Action::DebugToggleBreakpoint:
  case Action::DebugAddBreakpoint:
    debugger([this] { m_debugger.ToggleBreakpointAtPC(); });
    break;
  case Action::DebugAddMemoryBreakpoint:
    debugger([this] { m_debugger.AddMemoryBreakpoint(); });
    break;
  case Action::OpenAchievements:
    if (!Config::Get(Config::RA_ENABLED))
      OSD::AddMessage("RetroAchievements is turned off");
    else if (!AchievementManager::GetInstance().IsGameLoaded())
      OSD::AddMessage("This game has no achievements");
    else
      open_menu_at("achievements");
    break;
  case Action::GBALoadROM:
  case Action::GBAUnloadROM:
    HandleGBAROMHotkey(action == Action::GBALoadROM);
    break;
  default:
    break;
  }
}

void EmulationSession::Shutdown()
{
  if (!m_active)
    return;

  m_stop_requested = true;
  PromptService::GetInstance().AnswerAll(true);
  Core::Stop(Core::System::GetInstance());
  FinishStop();
}

void EmulationSession::Suspend()
{
  // Pausing waits for the emulation threads, which wait for the answer.
  if (!m_active || m_stop_requested || IsEmulationWaitingForAnswer())
    return;

  auto& system = Core::System::GetInstance();
  if (Core::GetState(system) == Core::State::Running)
  {
    Core::SetState(system, Core::State::Paused);
    m_paused_by_suspend = true;
  }
}

void EmulationSession::FlushSaveData()
{
  // The CPU thread can't pause while it waits for an answer.
  if (!m_active || m_core_stopped || IsEmulationWaitingForAnswer())
    return;

  auto& system = Core::System::GetInstance();
  if (Core::GetState(system) != Core::State::Running &&
      Core::GetState(system) != Core::State::Paused)
  {
    return;
  }
  // With the CPU thread paused, this runs on this thread before returning.
  const Core::CPUThreadGuard guard(system);
  auto& exi = system.GetExpansionInterface();
  for (const ExpansionInterface::Slot slot :
       {ExpansionInterface::Slot::A, ExpansionInterface::Slot::B})
  {
    if (ExpansionInterface::IEXIDevice* const device = exi.GetDevice(slot))
      device->Flush();
  }
}

void EmulationSession::Resume()
{
  if (!std::exchange(m_paused_by_suspend, false) || !m_active || m_overlay != Overlay::None)
    return;

  Core::SetState(Core::System::GetInstance(), Core::State::Running);
}

void EmulationSession::SetFocused(bool focused)
{
  if (!m_active || m_stop_requested || IsEmulationWaitingForAnswer())
    return;

  auto& system = Core::System::GetInstance();
  if (!focused)
  {
    if (!Config::Get(Config::MAIN_PAUSE_ON_FOCUS_LOST) || NetPlay::IsNetPlayRunning() ||
        m_picker_open || Core::GetState(system) != Core::State::Running)
    {
      return;
    }
    Core::SetState(system, Core::State::Paused);
    m_paused_by_focus = true;
    return;
  }

  if (std::exchange(m_paused_by_focus, false) && m_overlay == Overlay::None && !m_paused_by_user &&
      !m_paused_by_suspend)
  {
    Core::SetState(system, Core::State::Running);
  }
}

void EmulationSession::OpenQuickMenu()
{
  auto& system = Core::System::GetInstance();
  HostState::SetControllerInputBlocked(true);
  m_release_barrier = false;
  m_paused_by_menu = Core::GetState(system) == Core::State::Running;
  if (m_paused_by_menu)
    Core::SetState(system, Core::State::Paused);

  m_menu.Open();
  ++m_open_generation;
  m_overlay = Overlay::Menu;
  RebuildMenu(true);
  {
    std::lock_guard lock(m_overlay_mutex);
    m_results.clear();
  }
}

void EmulationSession::PickWhilePaused(bool folder, std::vector<std::wstring> file_types,
                                       std::wstring token,
                                       std::function<void(const std::string&)> picked)
{
  if (!m_services || m_picker_open || m_stop_requested)
    return;

  auto& system = Core::System::GetInstance();
  bool resume = std::exchange(m_paused_by_menu, false);
  if (m_overlay != Overlay::None)
    CloseOverlay();
  if (Core::GetState(system) == Core::State::Running)
  {
    Core::SetState(system, Core::State::Paused);
    resume = true;
  }

  m_picker_open = true;
  auto completed = [this, resume, picked = std::move(picked)](std::string path) {
    m_picker_open = false;
    if (!m_active || m_stop_requested)
      return;
    if (!path.empty())
      picked(path);
    if (resume && m_overlay == Overlay::None && !m_paused_by_user && !m_paused_by_suspend &&
        !m_stop_requested)
    {
      Core::SetState(Core::System::GetInstance(), Core::State::Running);
    }
  };
  if (folder)
    m_services->PickFolder(std::move(token), std::move(completed));
  else
    m_services->PickFile(std::move(file_types), std::move(token), std::move(completed));
}

void EmulationSession::PickDisc()
{
  PickWhilePaused(false, GetBootableFileTypes(), L"dolphin-change-disc",
                  [](const std::string& path) {
                    auto& system = Core::System::GetInstance();
                    system.GetDVDInterface().ChangeDisc(Core::CPUThreadGuard{system}, path);
                  });
}

void EmulationSession::PickStateToLoad()
{
  PickWhilePaused(false, GetStateFileTypes(), L"dolphin-state-file", [](const std::string& path) {
    State::LoadAs(Core::System::GetInstance(), path);
  });
}

void EmulationSession::PickStateToSave()
{
  PickWhilePaused(true, {}, L"dolphin-state-folder", [](const std::string& directory) {
    State::SaveAs(
        Core::System::GetInstance(),
        Recordings::MakeFileName(directory + '/', SConfig::GetInstance().GetGameID(), ".sav"));
  });
}

void EmulationSession::PickGBAFile(QuickMenu::Effect effect)
{
  const size_t port = m_menu.GetGBAPort();
  const std::string name = fmt::format("GBA{}", port + 1);
  switch (effect)
  {
  case QuickMenu::Effect::PickGBAROM:
    PickWhilePaused(
        false, {L".gba", L".agb", L".gb", L".gbc", L".mb", L".rom", L".bin", L".zip", L".7z"},
        L"dolphin-gba-rom", [port](const std::string& path) { GBAScreens::LoadROM(port, path); });
    break;
  case QuickMenu::Effect::PickEReaderCard:
    PickWhilePaused(false, {L".raw"}, L"dolphin-ereader-card",
                    [port](const std::string& path) { GBAScreens::QueueEReaderCard(port, path); });
    break;
  case QuickMenu::Effect::PickGBASaveToImport:
    PickWhilePaused(false, {L".sav"}, L"dolphin-gba-save",
                    [port](const std::string& path) { GBAScreens::ImportSave(port, path); });
    break;
  case QuickMenu::Effect::PickGBASaveFolder:
    PickWhilePaused(true, {}, L"dolphin-gba-save-folder", [port, name](const std::string& folder) {
      GBAScreens::ExportSave(port, Recordings::MakeFileName(folder + '/', name, ".sav"));
    });
    break;
  case QuickMenu::Effect::PickGBAStateToImport:
    PickWhilePaused(
        false,
        {L".ss0", L".ss1", L".ss2", L".ss3", L".ss4", L".ss5", L".ss6", L".ss7", L".ss8", L".ss9"},
        L"dolphin-gba-state",
        [port](const std::string& path) { GBAScreens::ImportState(port, path); });
    break;
  case QuickMenu::Effect::PickGBAStateFolder:
    PickWhilePaused(true, {}, L"dolphin-gba-state-folder", [port, name](const std::string& folder) {
      GBAScreens::ExportState(port, Recordings::MakeFileName(folder + '/', name, ".ss0"));
    });
    break;
  default:
    break;
  }
}

void EmulationSession::PickForDebugger(DebuggerView::PickRequest request)
{
  if (!m_services || m_picker_open || m_stop_requested)
    return;
  m_picker_open = true;
  auto completed = [this, picked = std::move(request.picked)](std::string path) {
    m_picker_open = false;
    if (m_active && !m_stop_requested && !path.empty())
      picked(path);
  };
  if (request.folder)
    m_services->PickFolder(std::move(request.token), std::move(completed));
  else
    m_services->PickFile(std::move(request.file_types), std::move(request.token),
                         std::move(completed));
}

void EmulationSession::PickFileToStart()
{
  PickWhilePaused(false, GetBootableFileTypes(), L"dolphin-open", [this](const std::string& path) {
    m_services->BootFile(path);
    RequestStop();
  });
}

void EmulationSession::OpenDebugger()
{
  HostState::SetControllerInputBlocked(true);
  m_release_barrier = false;
  m_debugger.Open();
  m_debugger_snapshot = m_debugger.BuildSnapshot();
  m_last_debugger_refresh = std::chrono::steady_clock::now();
  m_overlay = Overlay::Debugger;
  ++m_open_generation;
  ++m_focus_generation;
  m_model_dirty = true;
}

void EmulationSession::OpenTasInput(TasView::Target target)
{
  // The game stays paused while the inputs are set, and the panel can advance it a frame at a
  // time.
  auto& system = Core::System::GetInstance();
  if (Core::GetState(system) == Core::State::Running)
    Core::SetState(system, Core::State::Paused);
  m_paused_by_menu = false;
  HostState::SetControllerInputBlocked(true);
  m_release_barrier = false;
  m_tas.Open(target);
  m_tas_snapshot = m_tas.BuildSnapshot();
  m_last_debugger_refresh = std::chrono::steady_clock::now();
  m_overlay = Overlay::Tas;
  ++m_open_generation;
  ++m_focus_generation;
  m_model_dirty = true;
}

void EmulationSession::OpenMapper()
{
  // The game stays paused, as the quick menu left it. The mapping it changes is the game's own.
  m_mapper_game_scope = !InputMappingHost::GetRunningGameId().empty();
  m_overlay = Overlay::Mapper;
  ++m_open_generation;
  ++m_focus_generation;
  std::lock_guard lock(m_overlay_mutex);
  m_results.clear();
}

void EmulationSession::CloseMapper(const OverlayResult& result)
{
  std::string error;
  for (const UwpImGuiFrontend::ControllerProfileDraft& draft : result.mapper_saved)
  {
    const UwpImGuiFrontend::MappingSaveResult saved = m_mapping.SaveProfileDraft(draft);
    if (!saved.succeeded && error.empty())
      error = saved.error;
  }
  if (!result.mapper_saved.empty())
  {
    OSD::AddMessage(!error.empty()      ? error :
                    m_mapper_game_scope ? "Game controller profile saved" :
                                          "Controller settings saved",
                    OSD::Duration::NORMAL);
  }
  m_overlay = Overlay::Menu;
  RebuildMenu(true);
}

void EmulationSession::CloseTasInput()
{
  m_tas.Close();
  m_overlay = Overlay::None;
  m_release_barrier = true;
  auto& system = Core::System::GetInstance();
  if (Core::GetState(system) == Core::State::Paused && !m_paused_by_user && !m_paused_by_suspend)
    Core::SetState(system, Core::State::Running);
}

void EmulationSession::CloseOverlay()
{
  m_overlay = Overlay::None;
  m_release_barrier = true;

  auto& system = Core::System::GetInstance();
  if (std::exchange(m_paused_by_menu, false) && !m_paused_by_suspend)
  {
    Core::SetState(system, Core::State::Running);
    return;
  }

  m_last_present_request = {};
  RequestPresent(false);
}

void EmulationSession::RebuildMenu(bool refocus)
{
  m_menu_model = m_menu.BuildModel();
  m_model_dirty = true;
  if (refocus)
    ++m_focus_generation;
}

void EmulationSession::ApplyMenuEffect(QuickMenu::Effect effect)
{
  switch (effect)
  {
  case QuickMenu::Effect::None:
    RebuildMenu(false);
    break;
  case QuickMenu::Effect::Refocus:
    RebuildMenu(true);
    break;
  case QuickMenu::Effect::Close:
    CloseOverlay();
    break;
  case QuickMenu::Effect::Exit:
    ConfirmStop();
    break;
  case QuickMenu::Effect::OpenDebugger:
    m_paused_by_menu = false;
    OpenDebugger();
    break;
  case QuickMenu::Effect::PickDisc:
    PickDisc();
    break;
  case QuickMenu::Effect::PickStateToLoad:
    PickStateToLoad();
    break;
  case QuickMenu::Effect::PickStateToSave:
    PickStateToSave();
    break;
  case QuickMenu::Effect::PickGBAROM:
  case QuickMenu::Effect::PickEReaderCard:
  case QuickMenu::Effect::PickGBASaveToImport:
  case QuickMenu::Effect::PickGBASaveFolder:
  case QuickMenu::Effect::PickGBAStateToImport:
  case QuickMenu::Effect::PickGBAStateFolder:
    PickGBAFile(effect);
    break;
  case QuickMenu::Effect::OpenTasInput:
    OpenTasInput(m_menu.GetTasTarget());
    break;
  case QuickMenu::Effect::OpenMapper:
    OpenMapper();
    break;
  }
}

void EmulationSession::ApplyResult(const OverlayResult& result)
{
  if (result.answer)
  {
    PromptService::GetInstance().Answer(result.answer->first, result.answer->second);
    return;
  }
  if (m_overlay == Overlay::Mapper)
  {
    if (!result.mapper_error.empty())
      OSD::AddMessage(result.mapper_error, OSD::Duration::NORMAL);
    if (result.mapper_closed)
      CloseMapper(result);
    return;
  }
  if (m_overlay != Overlay::Menu)
    return;

  const QuickMenuResult& menu = result.menu;
  if (menu.closeRequested || menu.choiceCancelled)
    ApplyMenuEffect(m_menu.Back());
  else if (menu.activatedChoice)
    ApplyMenuEffect(m_menu.Choose(*menu.activatedChoice));
  else if (menu.activatedRow)
    ApplyMenuEffect(m_menu.Activate(*menu.activatedRow));
}

void EmulationSession::PublishOverlay(const FrameInput& input,
                                      const std::optional<PromptService::Prompt>& prompt,
                                      bool input_held)
{
  std::lock_guard lock(m_overlay_mutex);
  m_overlay_frame.overlay = m_overlay;
  if (std::exchange(m_model_dirty, false))
  {
    m_overlay_frame.menu = m_menu_model;
    m_overlay_frame.debugger = m_debugger_snapshot;
    m_overlay_frame.tas = m_tas_snapshot;
  }
  m_overlay_frame.prompt = prompt;
  m_overlay_frame.input = input;
  m_overlay_frame.input_held = input_held;
  m_overlay_frame.mapper_game_scope = m_mapper_game_scope;
  m_overlay_frame.focus_generation = m_focus_generation;
  m_overlay_frame.open_generation = m_open_generation;
  m_overlay_frame.theme_index = m_theme_index;
}

void EmulationSession::RequestPresent(bool cpu_blocked)
{
  if (m_stop_requested)
    return;

  auto& system = Core::System::GetInstance();
  if (!cpu_blocked && Core::GetState(system) != Core::State::Paused)
    return;

  const auto now = std::chrono::steady_clock::now();
  if (now - m_last_present_request < PRESENT_INTERVAL || m_present_pending.exchange(true))
    return;

  m_last_present_request = now;
  const auto present = [this] {
    AsyncRequests::GetInstance()->PushEvent([this] {
      if (g_presenter)
        g_presenter->Present();
      m_present_pending = false;
    });
  };
  // A CPU thread that waits for an answer cannot queue the request itself, and while it waits it
  // queues nothing else.
  if (cpu_blocked)
    present();
  else
    Core::RunOnCPUThread(system, present);
}

void EmulationSession::FinishStop()
{
  auto& system = Core::System::GetInstance();
  if (system.GetFifoRecorder().IsRecording())
    system.GetFifoRecorder().StopRecording();
  PromptService::GetInstance().SetEmulationPromptsAllowed(false);
  Core::Shutdown(system);
  m_overlay_hook.reset();
  m_state_hook.reset();
  m_debugger.Close();
  m_tas.Close();
  m_mapper.Close();
  ResetMapperTextures();
  m_mapper_context = nullptr;
  ResetGBAScreens();
  GBAControls::Reset();
  m_overlay = Overlay::None;
  m_prompt_visible = false;
  m_force_stop_deadline.reset();
  HostState::SetControllerInputBlocked(false);
  m_active = false;
  m_finished = true;
}

std::optional<bool> EmulationSession::DrawPrompt(const PromptService::Prompt& prompt,
                                                 const UwpImGuiFrontend::ShellTheme& theme,
                                                 bool focus_default)
{
  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  if (!viewport)
    return std::nullopt;

  ImGui::GetBackgroundDrawList()->AddRectFilled(
      viewport->Pos, {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y},
      IM_COL32(0, 0, 0, 150));
  const float width = std::min(720.0f, viewport->WorkSize.x - 96.0f);
  ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                           viewport->WorkPos.y + viewport->WorkSize.y * 0.5f},
                          ImGuiCond_Always, {0.5f, 0.5f});
  ImGui::SetNextWindowSizeConstraints({width, 0.0f}, {width, viewport->WorkSize.y - 96.0f});
  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {30.0f, 26.0f});
  std::optional<bool> answer;
  if (ImGui::Begin("DolphinPrompt", nullptr, flags))
  {
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);

    UwpImGuiFrontend::SectionTitle(prompt.caption.empty() ? "Dolphin" : prompt.caption.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, theme.textPrimary);
    ImGui::PushTextWrapPos(0.0f);
    const std::string_view text = Translation::Translate(prompt.text);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::Spacing();

    const ImVec2 button{170.0f, 48.0f};
    if (prompt.question)
    {
      if (UwpImGuiFrontend::ShellButton("Yes", button))
        answer = true;
      ImGui::SameLine();
      if (focus_default)
      {
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNavCursorVisible(true);
      }
      if (UwpImGuiFrontend::ShellButton("No", button) ||
          UwpImGuiFrontend::ConsumeWidgetBackPressed())
      {
        answer = false;
      }
    }
    else if (UwpImGuiFrontend::ShellButton("OK", button) ||
             UwpImGuiFrontend::ConsumeWidgetBackPressed())
    {
      answer = true;
    }

    draw->ChannelsSetCurrent(0);
    UwpImGuiFrontend::DrawShellPanelPx(draw, min, {min.x + size.x, min.y + size.y}, theme, 24.0f,
                                       0.97f);
    draw->ChannelsMerge();
  }
  ImGui::End();
  ImGui::PopStyleVar();
  return answer;
}

void EmulationSession::ResetGBAScreens()
{
  m_gba_context = nullptr;
  for (auto& texture : m_gba_textures)
    texture.reset();
  for (auto& previous : m_gba_previous)
    previous.clear();
  m_gba_frames.fill(0);
}

ImTextureData* EmulationSession::UpdateGBATexture(size_t index, bool blending)
{
  u32 width = 0;
  u32 height = 0;
  u64 frame = m_gba_frames[index];
  if (!GBAScreens::CopyScreen(index, &frame, &width, &height, &m_gba_pixels))
    return nullptr;

  std::unique_ptr<ImTextureData>& texture = m_gba_textures[index];
  if (!texture || texture->Width != static_cast<int>(width) ||
      texture->Height != static_cast<int>(height))
  {
    if (texture)
      ImGui::UnregisterUserTexture(texture.get());
    texture = std::make_unique<ImTextureData>();
    texture->Create(ImTextureFormat_RGBA32, static_cast<int>(width), static_cast<int>(height));
    ImGui::RegisterUserTexture(texture.get());
    m_gba_previous[index].clear();
    if (frame == m_gba_frames[index])
    {
      frame = 0;
      GBAScreens::CopyScreen(index, &frame, &width, &height, &m_gba_pixels);
    }
  }

  const size_t count = static_cast<size_t>(width) * height;
  if (frame == m_gba_frames[index] || m_gba_pixels.size() != count)
    return texture.get();

  // Interframe blending averages each frame with the one before.
  u32* const pixels = static_cast<u32*>(texture->GetPixels());
  std::vector<u32>& previous = m_gba_previous[index];
  if (blending && previous.size() == count)
  {
    for (size_t i = 0; i < count; ++i)
      pixels[i] =
          (previous[i] & m_gba_pixels[i]) + (((previous[i] ^ m_gba_pixels[i]) & 0xFEFEFEFEu) >> 1);
  }
  else
  {
    std::memcpy(pixels, m_gba_pixels.data(), count * sizeof(u32));
  }
  previous.swap(m_gba_pixels);

  if (texture->Status == ImTextureStatus_OK)
  {
    const ImTextureRect rect{0, 0, static_cast<unsigned short>(width),
                             static_cast<unsigned short>(height)};
    texture->Updates.resize(0);
    texture->Updates.push_back(rect);
    texture->UpdateRect = rect;
    texture->SetStatus(ImTextureStatus_WantUpdates);
  }
  m_gba_frames[index] = frame;
  return texture.get();
}

void EmulationSession::DrawGBAScreens()
{
  // A new video backend brings a new context, and the old one released its textures.
  ImGuiContext* const context = ImGui::GetCurrentContext();
  if (context != m_gba_context)
  {
    ResetGBAScreens();
    m_gba_context = context;
  }

  const Config::GBALayout layout = Config::Get(Config::UWP_GBA_LAYOUT);
  if (layout == Config::GBALayout::Hidden)
    return;
  const bool show_streamed = Config::Get(Config::UWP_GBA_SHOW_STREAMED);
  const bool shared_blending = Config::Get(Config::UWP_GBA_INTERFRAME_BLENDING);

  struct Screen
  {
    size_t port;
    ImTextureData* texture;
    ImVec2 size;
  };
  std::vector<Screen> screens;
  for (size_t i = 0; i < GBAScreens::MAX_SCREENS; ++i)
  {
    // A screen that a phone shows stays off the TV unless asked otherwise.
    if (!show_streamed && MediaStreamer::IsStreaming(MediaStreamer::GBASource(i)))
      continue;
    const int blending = Config::Get(Config::UWP_GBA_BLENDING[i]);
    if (ImTextureData* const texture =
            UpdateGBATexture(i, blending < 0 ? shared_blending : blending != 0))
    {
      screens.push_back({i, texture, {}});
    }
  }
  if (screens.empty())
    return;

  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  const ImVec2 origin = viewport->WorkPos;
  const ImVec2 area = viewport->WorkSize;
  constexpr float margin = 16.0f;
  constexpr float gap = 12.0f;
  const float count = static_cast<float>(screens.size());
  const bool row = layout == Config::GBALayout::Bottom || layout == Config::GBALayout::Top;
  const bool column = layout == Config::GBALayout::Left || layout == Config::GBALayout::Right;

  // The automatic size is the largest whole scale that fits the space each screen gets.
  ImVec2 room{area.x * 0.25f, area.y * 0.3f};
  if (row)
    room.x = (area.x - margin * 2.0f - gap * (count - 1.0f)) / count;
  else if (column)
    room.y = (area.y - margin * 2.0f - gap * (count - 1.0f)) / count;
  const int shared_scale = std::clamp(Config::Get(Config::UWP_GBA_SCALE), 0, 4);
  float total = gap * (count - 1.0f);
  for (Screen& screen : screens)
  {
    const int own_scale = Config::Get(Config::UWP_GBA_SCALES[screen.port]);
    const int fixed_scale = own_scale < 0 ? shared_scale : std::min(own_scale, 4);
    const float width = static_cast<float>(screen.texture->Width);
    const float height = static_cast<float>(screen.texture->Height);
    const float scale = fixed_scale != 0 ?
                            static_cast<float>(fixed_scale) :
                            std::max(1.0f, std::floor(std::min(room.x / width, room.y / height)));
    screen.size = {width * scale, height * scale};
    total += row ? screen.size.x : screen.size.y;
  }

  const bool frame = Config::Get(Config::UWP_GBA_FRAME);
  const auto draw_screen = [&](const Screen& screen, ImVec2 min) {
    // A screen that is always on top is drawn above the in-game menus.
    ImDrawList* const draw = Config::Get(Config::UWP_GBA_ALWAYS_ON_TOP[screen.port]) ?
                                 ImGui::GetForegroundDrawList() :
                                 ImGui::GetBackgroundDrawList();
    const ImVec2 max{min.x + screen.size.x, min.y + screen.size.y};
    if (frame)
    {
      draw->AddRectFilled({min.x - 3.0f, min.y - 3.0f}, {max.x + 3.0f, max.y + 3.0f},
                          IM_COL32(0, 0, 0, 200), 4.0f);
    }
    draw->AddImage(screen.texture->GetTexRef(), min, max);
    if (GBAControls::GetControllingPlayers(screen.port) != 0)
    {
      draw->AddRect({min.x - 4.0f, min.y - 4.0f}, {max.x + 4.0f, max.y + 4.0f},
                    IM_COL32(64, 170, 255, 255), 4.0f, 0, 4.0f);
    }
  };

  if (layout == Config::GBALayout::Corners)
  {
    // Each port keeps its own corner, in the order of the controller ports.
    for (const Screen& screen : screens)
    {
      const bool right = (screen.port & 1) != 0;
      const bool bottom = screen.port >= 2;
      draw_screen(screen,
                  {right ? origin.x + area.x - margin - screen.size.x : origin.x + margin,
                   bottom ? origin.y + area.y - margin - screen.size.y : origin.y + margin});
    }
    return;
  }

  float cursor = row ? origin.x + (area.x - total) * 0.5f : origin.y + (area.y - total) * 0.5f;
  for (const Screen& screen : screens)
  {
    if (row)
    {
      const float y = layout == Config::GBALayout::Top ? origin.y + margin :
                                                         origin.y + area.y - margin - screen.size.y;
      draw_screen(screen, {cursor, y});
      cursor += screen.size.x + gap;
    }
    else
    {
      const float x = layout == Config::GBALayout::Left ?
                          origin.x + margin :
                          origin.x + area.x - margin - screen.size.x;
      draw_screen(screen, {x, cursor});
      cursor += screen.size.y + gap;
    }
  }
}

void EmulationSession::DrawOverlay()
{
  DrawGBAScreens();
  // Ends a rumble test of the mapper, which can close before the test does.
  m_overlay_mapping.Tick();
  if (m_overlay == Overlay::None && !m_prompt_visible)
    return;

  OverlayFrame frame;
  {
    std::lock_guard lock(m_overlay_mutex);
    frame = m_overlay_frame;
  }
  if (frame.overlay == Overlay::None && !frame.prompt)
    return;

  const UwpImGuiFrontend::ShellTheme& theme = GetTheme(frame.theme_index);
  if (frame.open_generation != m_drawn_open_generation)
  {
    m_drawn_open_generation = frame.open_generation;
    m_overlay_previous_input = frame.input;
  }
  const bool focus_changed = frame.focus_generation != m_drawn_focus_generation;
  if (focus_changed)
  {
    m_drawn_focus_generation = frame.focus_generation;
    UwpImGuiFrontend::BeginWidgetFrame(theme, {}, {}, false);
    UwpImGuiFrontend::EndWidgetFrame();
  }

  const FrameInput input =
      frame.overlay == Overlay::Mapper && !frame.prompt ? FilterMapperInput(frame) : frame.input;
  UwpImGuiFrontend::SubmitImGuiNavigationInput(input, true);
  UwpImGuiFrontend::BeginWidgetFrame(theme, ToWidgetInput(input),
                                     ToWidgetInput(m_overlay_previous_input), true);
  ImFont* const font = GetOverlayFont();
  OverlayResult result;
  result.focus_generation = frame.focus_generation;
  bool report = false;

  if (frame.prompt)
  {
    if (font)
      ImGui::PushFont(font);
    if (const auto answer = DrawPrompt(*frame.prompt, theme, focus_changed))
    {
      result.answer = std::pair{frame.prompt->id, *answer};
      report = true;
    }
    if (font)
      ImGui::PopFont();
  }
  else if (frame.overlay == Overlay::Menu)
  {
    result.menu = UwpImGuiFrontend::DrawQuickMenu(frame.menu, theme, font);
    report = result.menu.activatedRow || result.menu.activatedChoice ||
             result.menu.closeRequested || result.menu.choiceCancelled;
  }
  else if (frame.overlay == Overlay::Debugger)
  {
    DebuggerView::Draw(frame.debugger, theme, font);
  }
  else if (frame.overlay == Overlay::Tas)
  {
    TasView::Draw(frame.tas, theme, font);
  }
  else if (frame.overlay == Overlay::Mapper)
  {
    report = DrawMapper(frame, input, font, &result);
  }
  UwpImGuiFrontend::EndWidgetFrame();
  m_overlay_previous_input = input;

  if (report)
  {
    std::lock_guard lock(m_overlay_mutex);
    m_results.push_back(std::move(result));
  }
}

// The input of a capture stays away from the mapper until it is released.
FrameInput EmulationSession::FilterMapperInput(const OverlayFrame& frame)
{
  if (m_mapper.IsCapturing())
    m_mapper_release_barrier = true;
  else if (m_mapper_release_barrier && !frame.input_held)
    m_mapper_release_barrier = false;
  return m_mapper_release_barrier ? FrameInput{.deltaSeconds = frame.input.deltaSeconds} :
                                    frame.input;
}

FrameInput EmulationSession::MakeMapperInput(const FrameInput& input)
{
  // The video thread draws at its own pace, which the repeat of held directions follows.
  const auto now = std::chrono::steady_clock::now();
  const float delta =
      std::clamp(std::chrono::duration<float>(now - m_mapper_last_draw).count(), 0.0f, 0.1f);
  m_mapper_last_draw = now;

  const FrameInput& previous = m_overlay_previous_input;
  const bool up = input.up || input.leftStickY >= 0.55f;
  const bool down = input.down || input.leftStickY <= -0.55f;
  const bool left = input.left || input.leftStickX <= -0.55f;
  const bool right = input.right || input.leftStickX >= 0.55f;
  return {
      .deltaSeconds = delta,
      .up = m_mapper_repeat[0].Update(up, delta),
      .down = m_mapper_repeat[1].Update(down, delta),
      .left = m_mapper_repeat[2].Update(left, delta),
      .right = m_mapper_repeat[3].Update(right, delta),
      .accept = input.accept && !previous.accept,
      .back = input.back && !previous.back,
      .context = input.context && !previous.context,
      .alternate = input.alternate && !previous.alternate,
      .menu = input.menu && !previous.menu,
      .view = input.view && !previous.view,
      .leftShoulder = input.leftShoulder && !previous.leftShoulder,
      .rightShoulder = input.rightShoulder && !previous.rightShoulder,
      .controllerInputHeld = input.controllerInputHeld,
      .leftStickX = input.leftStickX,
      .leftStickY = input.leftStickY,
      .rightStickX = input.rightStickX,
      .rightStickY = input.rightStickY,
      .pointer = input.pointer,
      .pointerPressed = input.pointerPressed,
  };
}

bool EmulationSession::DrawMapper(const OverlayFrame& frame, const FrameInput& input, ImFont* font,
                                  OverlayResult* result)
{
  // A new video backend brings a new context, and the old one released its textures.
  if (ImGui::GetCurrentContext() != m_mapper_context)
  {
    ResetMapperTextures();
    m_mapper_context = ImGui::GetCurrentContext();
  }
  if (frame.open_generation != m_mapper_open_generation)
  {
    m_mapper_open_generation = frame.open_generation;
    m_mapper_saved.clear();
    m_mapper_release_barrier = false;
    m_mapper_repeat = {};
    m_mapper_last_draw = std::chrono::steady_clock::now();
    // The on-screen keyboard belongs to the UI thread.
    m_mapper.SetTextEntryAvailable(false);
    m_mapper.Open(m_overlay_mapping,
                  frame.mapper_game_scope ? std::optional<std::uint64_t>(RUNNING_GAME_TITLE) :
                                            std::nullopt,
                  0);
  }
  if (!m_mapper.IsOpen())
    return false;

  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  const UwpImGuiFrontend::ShellLayoutContext context =
      UwpImGuiFrontend::MakeShellLayout(viewport->WorkSize, viewport->WorkPos);
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoBackground;
  UwpImGuiFrontend::InputMapperResult mapped;
  if (ImGui::Begin("DolphinControllerMapper", nullptr, flags))
  {
    UwpImGuiFrontend::DrawDimmedBackground();
    if (font)
      ImGui::PushFont(font);
    mapped = m_mapper.Draw(context, ImGui::GetWindowDrawList(), MakeMapperInput(input), font);
    if (font)
      ImGui::PopFont();
  }
  ImGui::End();

  result->mapper_error = std::move(mapped.error);
  if (!m_mapper.IsOpen())
  {
    result->mapper_closed = true;
    result->mapper_saved = std::exchange(m_mapper_saved, {});
  }
  return result->mapper_closed || !result->mapper_error.empty();
}

UwpImGuiFrontend::TextureHandle
EmulationSession::LoadMapperTexture(const std::filesystem::path& path)
{
  const auto [found, inserted] = m_mapper_textures.try_emplace(PathToString(path));
  std::unique_ptr<ImTextureData>& texture = found->second;
  if (inserted)
  {
    // A picture that can't be read stays empty, and the mapper names the controller instead.
    std::string data;
    Common::UniqueBuffer<u8> pixels;
    u32 width = 0;
    u32 height = 0;
    if (!File::ReadFileToString(PathToString(path), data) ||
        !Common::LoadPNG({reinterpret_cast<const u8*>(data.data()), data.size()}, &pixels, &width,
                         &height))
    {
      return {};
    }
    texture = std::make_unique<ImTextureData>();
    texture->Create(ImTextureFormat_RGBA32, static_cast<int>(width), static_cast<int>(height));
    std::memcpy(texture->GetPixels(), pixels.data(), static_cast<size_t>(width) * height * 4);
    ImGui::RegisterUserTexture(texture.get());
  }
  // The renderer creates the texture when it draws the next frame.
  if (!texture || texture->Status != ImTextureStatus_OK)
    return {};
  return {static_cast<std::uintptr_t>(texture->TexID), static_cast<u32>(texture->Width),
          static_cast<u32>(texture->Height)};
}

void EmulationSession::ResetMapperTextures()
{
  // The textures went with the context of the video backend that drew them.
  m_mapper_textures.clear();
  m_overlay_mapping.ReleaseTextures();
}

ImFont* EmulationSession::GetOverlayFont()
{
  ImGuiContext* const context = ImGui::GetCurrentContext();
  if (context == m_overlay_font_context)
    return m_overlay_font;

  m_overlay_font_context = context;
  m_overlay_font = nullptr;
  if (m_font_data.empty())
    return nullptr;

  ImFontConfig config;
  config.FontDataOwnedByAtlas = false;
  const float scale = std::max(ImGui::GetStyle().FontScaleMain, 0.1f);
  m_overlay_font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<u8*>(m_font_data.data()),
                                                              static_cast<int>(m_font_data.size()),
                                                              OVERLAY_FONT_SIZE / scale, &config);
  return m_overlay_font;
}
}  // namespace DolphinUWP
