// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <windows.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Display.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <UwpImGuiFrontend/UwpImGuiFrontend.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Matrix.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/WindowSystemInfo.h"
#include "Core/AchievementManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "DolphinUWP/CrashHandler.h"
#include "DolphinUWP/DisplayMetrics.h"
#include "DolphinUWP/EmulationSession.h"
#include "DolphinUWP/FrontendHost.h"
#include "DolphinUWP/FrontendRenderer.h"
#include "DolphinUWP/GameLibrary.h"
#include "DolphinUWP/GameTextInput.h"
#include "DolphinUWP/HostState.h"
#include "DolphinUWP/HotkeyScheduler.h"
#include "DolphinUWP/InputMappingHost.h"
#include "DolphinUWP/InputSource.h"
#include "DolphinUWP/LogBuffer.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/SettingsPages.h"
#include "DolphinUWP/SystemPrompts.h"
#include "DolphinUWP/Translation.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/UWP/UWP.h"
#include "UICommon/GameFile.h"
#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"

namespace DolphinUWP
{
namespace
{
namespace Activation = winrt::Windows::ApplicationModel::Activation;
namespace CoreApp = winrt::Windows::ApplicationModel::Core;
namespace GamingInput = winrt::Windows::Gaming::Input;
namespace UI = winrt::Windows::UI::Core;
using winrt::Windows::System::VirtualKey;
using winrt::Windows::UI::ViewManagement::ApplicationView;
using winrt::Windows::UI::ViewManagement::ApplicationViewMode;
using winrt::Windows::UI::ViewManagement::ViewModePreferences;

constexpr auto GAME_LOOP_INTERVAL = std::chrono::milliseconds(4);
constexpr auto BATTERY_POLL_INTERVAL = std::chrono::seconds(2);
constexpr auto CURSOR_HIDE_DELAY = std::chrono::seconds(3);
constexpr auto GAMEPAD_CHECK_INTERVAL = std::chrono::milliseconds(500);

std::mutex s_alert_mutex;
std::deque<std::string> s_alerts;

bool HandleAlert(const char* caption, const char* text, bool yes_no, Common::MsgType style)
{
  const std::string message = caption && *caption ? fmt::format("{}: {}", caption, text) : text;
  if (style == Common::MsgType::Critical || style == Common::MsgType::Warning)
    ERROR_LOG_FMT(COMMON, "{}", message);
  else
    INFO_LOG_FMT(COMMON, "{}", message);

  PromptService& prompts = PromptService::GetInstance();
  if (yes_no && prompts.CanAsk())
    return prompts.Ask(caption ? caption : "", text ? text : "", style, true);

  OSD::AddMessage(message, OSD::Duration::VERY_LONG, OSD::Color::RED);
  std::lock_guard lock(s_alert_mutex);
  s_alerts.push_back(message);
  return true;
}

std::vector<std::string> TakeAlerts()
{
  std::lock_guard lock(s_alert_mutex);
  std::vector<std::string> alerts(s_alerts.begin(), s_alerts.end());
  s_alerts.clear();
  return alerts;
}

void WriteFailure(std::string_view message)
{
  try
  {
    const std::filesystem::path path =
        std::filesystem::path(
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()) /
        "startup-error.log";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(message.data(), static_cast<std::streamsize>(message.size()));
  }
  catch (...)
  {
  }
}

std::optional<UwpImGuiFrontend::TextEditingCommand> ToTextCommand(VirtualKey key)
{
  using UwpImGuiFrontend::TextEditingCommand;
  switch (key)
  {
  case VirtualKey::Back:
    return TextEditingCommand::Backspace;
  case VirtualKey::Delete:
    return TextEditingCommand::Delete;
  case VirtualKey::Left:
    return TextEditingCommand::MoveLeft;
  case VirtualKey::Right:
    return TextEditingCommand::MoveRight;
  case VirtualKey::Home:
    return TextEditingCommand::MoveHome;
  case VirtualKey::End:
    return TextEditingCommand::MoveEnd;
  case VirtualKey::Enter:
    return TextEditingCommand::Submit;
  default:
    return std::nullopt;
  }
}

// Frontends pass desktop Dolphin's command line, such as dolphin.exe -b -e "E:\Games\gc\Game.rvz".
std::string ParseCommandLinePath(std::wstring_view command)
{
  static constexpr std::array<std::wstring_view, 14> VALUE_OPTIONS{
      L"-u", L"--user",      L"-m", L"--movie",         L"-s", L"--save_state",
      L"-C", L"--config",    L"-v", L"--video_backend", L"-a", L"--audio_emulation",
      L"-n", L"--nand_title"};

  std::wistringstream stream{std::wstring(command)};
  std::vector<std::wstring> args;
  for (std::wstring arg; stream >> std::quoted(arg, L'"', L'\0');)
    args.push_back(std::move(arg));

  if (!args.empty())
  {
    std::wstring executable = args.front();
    std::ranges::transform(executable, executable.begin(), std::towlower);
    if (executable.ends_with(L".exe"))
      args.erase(args.begin());
  }

  std::wstring path;
  for (size_t i = 0; i < args.size(); ++i)
  {
    const std::wstring& arg = args[i];
    if (arg.starts_with(L"--exec="))
      return WStringToUTF8(arg.substr(7));
    if (arg.starts_with(L"-"))
    {
      if (std::ranges::find(VALUE_OPTIONS, arg) != VALUE_OPTIONS.end())
        ++i;
      continue;
    }
    if (!path.empty())
      path += L' ';
    path += arg;
  }
  return WStringToUTF8(path);
}

float GetUIScale(u32 width, u32 height)
{
  return std::max(
      0.5f, std::min(static_cast<float>(width) / 1280.0f, static_cast<float>(height) / 720.0f));
}

#pragma warning(push)
// C++/WinRT delegate and activation templates instantiated with the lambdas and classes below.
#pragma warning(disable : 4265 4946)

class App : public winrt::implements<App, CoreApp::IFrameworkView>
{
public:
  void Initialize(const CoreApp::CoreApplicationView& view)
  {
    view.Activated({this, &App::OnActivated});
    CoreApp::CoreApplication::Suspending({this, &App::OnSuspending});
    CoreApp::CoreApplication::Resuming({this, &App::OnResuming});
  }

  void SetWindow(const UI::CoreWindow& window)
  {
    m_window = window;
    if (IsXbox())
    {
      winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().SetDesiredBoundsMode(
          winrt::Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
    }

    UpdateFullscreenState();
    window.SizeChanged([this](auto&&, auto&&) { OnSizeChanged(); });
    winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().DpiChanged(
        [this](auto&&, auto&&) { OnSizeChanged(); });
    window.VisibilityChanged(
        [this](auto&&, const UI::VisibilityChangedEventArgs& args) { m_visible = args.Visible(); });
    window.Activated([this](auto&&, const UI::WindowActivatedEventArgs& args) {
      const bool active =
          args.WindowActivationState() != UI::CoreWindowActivationState::Deactivated;
      HostState::SetRendererFocus(active);
      m_window_active = active;
      if (m_modal_prompts == 0)
        m_session.SetFocused(active);
      if (!active)
      {
        ciface::UWP::ResetKeyStates();
        ciface::UWP::ResetMouseState();
        m_input.Reset();
      }
    });
    window.Closed([this](auto&&, auto&&) {
      m_closed = true;
      (void)ReturnToFrontend();
    });
    UI::SystemNavigationManager::GetForCurrentView().BackRequested(
        [this](auto&&, const UI::BackRequestedEventArgs& args) {
          args.Handled(true);
          m_input.RequestBack();
        });
    window.KeyDown([this](auto&&, const UI::KeyEventArgs& args) { OnKey(args, true); });
    window.KeyUp([this](auto&&, const UI::KeyEventArgs& args) { OnKey(args, false); });
    window.PointerMoved([this](auto&&, const UI::PointerEventArgs& args) { OnPointer(args); });
    window.PointerPressed([this](auto&&, const UI::PointerEventArgs& args) {
      OnPointer(args);
      // Clicking the game locks the cursor again.
      if (m_session.IsActive())
        m_cursor_unlocked = false;
    });
    window.PointerReleased([this](auto&&, const UI::PointerEventArgs& args) { OnPointer(args); });
    window.PointerWheelChanged([this](auto&&, const UI::PointerEventArgs& args) {
      OnPointer(args);
      const int wheel = args.CurrentPoint().Properties().MouseWheelDelta();
      m_input.AddWheel(static_cast<float>(wheel) / 120.0f);
      ciface::UWP::AddMouseMovement(0.0f, 0.0f, static_cast<float>(wheel));
    });
    window.PointerExited([this](auto&&, auto&&) {
      m_input.SetPointer(std::nullopt, false);
      ciface::UWP::SetMouseButtons(0);
    });
    try
    {
      m_mouse_moved = winrt::Windows::Devices::Input::MouseDevice::GetForCurrentView().MouseMoved(
          winrt::auto_revoke,
          [this](auto&&, const winrt::Windows::Devices::Input::MouseEventArgs& args) {
            OnMouseMoved(args);
          });
    }
    catch (const winrt::hresult_error& error)
    {
      ERROR_LOG_FMT(COMMON, "Mouse movement is unavailable: {}", winrt::to_string(error.message()));
    }
  }

  void Load(const winrt::hstring&) {}

  void Run()
  {
    InstallCrashHandler();
    PromptService::GetInstance().SetUIThread(std::this_thread::get_id());
    PromptService::GetInstance().SetModalHandler(
        [this](const PromptService::Prompt& prompt) { return AskModal(prompt); });
    try
    {
      InitializeDolphin();
      InitializeShell();
      RunLoop();
    }
    catch (const winrt::hresult_error& error)
    {
      WriteFailure(winrt::to_string(error.message()));
    }
    catch (const std::exception& error)
    {
      WriteFailure(error.what());
    }
    Shutdown();
  }

  void Uninitialize() {}

private:
  void InitializeDolphin()
  {
    UICommon::SetUserDirectory({});
    UICommon::CreateDirectories();
    const bool first_run = !File::Exists(File::GetUserPath(F_DOLPHINCONFIG_IDX));
    m_apply_default_mappings = !File::Exists(File::GetUserPath(F_GCPADCONFIG_IDX));

    Common::RegisterMsgAlertHandler(HandleAlert);
    UICommon::Init();
    FrontendHost::ApplyStoragePaths();
    m_storage_paths_callback = Config::AddConfigChangedCallback(&FrontendHost::ApplyStoragePaths);
    Translation::Apply();
    AchievementManager::GetInstance().Init(nullptr);
    LogBuffer::Install();
    // Only a new install defaults to Development files.
    if (first_run)
      FrontendHost::ApplyDefaultStorage();
    if (!ResourcePack::Init())
      ERROR_LOG_FMT(COMMON, "Some resource packs could not be loaded");
    if (first_run && IsXbox())
      Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("D3D12"));
    if (IsXbox() && Config::GetIsoPaths().empty())
    {
      Config::SetIsoPaths(GameLibrary::GetSystemFolders(GameLibrary::XBOX_GAMES_ROOT));
      Config::Save();
    }

    UICommon::InitControllers(MakeWindowSystemInfo());
    InputMappingHost::AddMissingGBAMappings();
    m_hotkeys.Start();
    m_dolphin_initialized = true;
  }

  void InitializeShell()
  {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    const OutputSize size = GetWindowPixelSize(m_window);
    LoadFonts(io, GetUIScale(size.width, size.height));
    if (!m_renderer.Initialize(m_window, size.width, size.height))
      throw std::runtime_error("The Direct3D 11 interface renderer could not be created");

    UwpImGuiFrontend::InitializeNativeTextInput({
        .contextName = "Dolphin",
        .setInputCapture = [this](bool active) { m_text_capture = active; },
        .log = [this](UwpImGuiFrontend::LogLevel level,
                      std::string_view message) { m_frontend_host.Log(level, message); },
    });

    m_mapping_host.SetTextureCallbacks(
        m_frontend_host.ResourceRoot(), StringToPath(File::GetSysDirectory() + RESOURCES_DIR),
        [this](const std::filesystem::path& path) { return m_renderer.LoadTexture(path); },
        [this](UwpImGuiFrontend::TextureHandle texture) { m_renderer.ReleaseTexture(texture); });

    m_library.Initialize(m_frontend_host.StateRoot() / "metadata");
    m_frontend_host.Initialize(
        {
            .load_texture =
                [this](const std::filesystem::path& path) { return m_renderer.LoadTexture(path); },
            .create_texture =
                [this](const u8* rgba, u32 width, u32 height) {
                  return m_renderer.CreateTexture(rgba, width, height);
                },
            .release_texture =
                [this](UwpImGuiFrontend::TextureHandle texture) {
                  m_renderer.ReleaseTexture(texture);
                },
        },
        m_text_font, m_shell_font);

    m_session.SetOverlayFont(m_font_data);
    m_session.SetNotify(
        [this](std::string message) { m_frontend_host.Notify(std::move(message)); });
    m_session.SetChatSender(
        [this](const std::string& message) { m_frontend_host.GetNetPlay().SendChat(message); });
    m_session.SetNetPlayStopper([this] { m_frontend_host.GetNetPlay().RequestStopGame(); });
    m_session.SetGolfRequester([this] { m_frontend_host.GetNetPlay().RequestGolfControl(); });
    m_session.SetServices(&m_frontend_host.GetServices());
    m_session.SetMapperResources(
        m_frontend_host.ResourceRoot(), StringToPath(File::GetSysDirectory() + RESOURCES_DIR),
        [this] { return m_frontend_host.GetDsuServers(); },
        [this] { return m_frontend_host.GetStorageChoices("controller-profiles"); });
    m_shell_initialized = true;
    m_last_frame = std::chrono::steady_clock::now();
  }

  void LoadFonts(ImGuiIO& io, float scale)
  {
    const std::filesystem::path path = m_frontend_host.ResourceRoot() / "fonts" / "Lato-Medium.ttf";
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
      throw std::runtime_error("The packaged interface font is missing");
    const std::streamsize size = input.tellg();
    if (size <= 0)
      throw std::runtime_error("The packaged interface font is empty");
    m_font_data.resize(static_cast<size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!input.read(reinterpret_cast<char*>(m_font_data.data()), size))
      throw std::runtime_error("The packaged interface font could not be read");

    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 2;
    config.OversampleV = 2;
    m_text_font =
        io.Fonts->AddFontFromMemoryTTF(m_font_data.data(), static_cast<int>(m_font_data.size()),
                                       std::max(10.0f, std::round(18.0f * scale)), &config);
    m_shell_font =
        io.Fonts->AddFontFromMemoryTTF(m_font_data.data(), static_cast<int>(m_font_data.size()),
                                       std::max(10.0f, std::round(24.0f * scale)), &config);
    if (!m_text_font || !m_shell_font)
      throw std::runtime_error("The packaged interface font could not be loaded");
    io.FontDefault = m_text_font;
  }

  void RunLoop()
  {
    const auto dispatcher = m_window.Dispatcher();
    auto& system = Core::System::GetInstance();
    while (!m_closed)
    {
      dispatcher.ProcessEvents(UI::CoreProcessEventsOption::ProcessAllIfPresent);
      if (std::exchange(m_resize_pending, false))
        OnSizeChanged();
      m_system_prompts.Update();
      UpdateCursorLock();
      if (ciface::UWP::ConsumeCursorCenterRequest())
        CenterCursor();
      Core::HostDispatchJobs(system);
      UpdateGameWindow();
      if (m_shell_initialized)
        m_frontend_host.Tick();
      AssignGamepads();
      m_mapping_host.Tick();

      UpdateDisplayRequest();
      if (m_session.IsActive())
      {
        m_hotkeys.SetEnabled(!m_session.IsOverlayActive() && m_visible);
        DispatchHotkeys();
        UpdateGame();
        continue;
      }
      m_hotkeys.SetEnabled(m_shell_initialized && m_activated && m_visible && !m_text_capture &&
                           !m_frontend_host.IsCapturingInput() &&
                           !m_frontend_host.GetServices().picker->IsOpen());
      DispatchLibraryHotkeys();

      if (m_exit_requested)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        continue;
      }

      if (m_session.ConsumeFinished())
      {
        RestoreShell();
        continue;
      }

      for (std::string& alert : TakeAlerts())
        m_frontend_host.Notify(std::move(alert));

      if (m_pending_boot_path)
      {
        const std::string path = *std::exchange(m_pending_boot_path, std::nullopt);
        StartGame(path, {}, std::nullopt);
        continue;
      }

      if (auto request = m_frontend_host.TakePendingBoot())
      {
        StartBoot(std::move(*request));
        continue;
      }

      if (!m_activated || !m_visible)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        continue;
      }

      DrawShellFrame();
      if (const auto item = m_frontend_host.TakePendingLaunch())
        LaunchItem(*item);
    }
  }

  void DrawShellFrame()
  {
    const auto now = std::chrono::steady_clock::now();
    const float delta =
        std::clamp(std::chrono::duration<float>(now - m_last_frame).count(), 1.0f / 1000.0f, 0.1f);
    m_last_frame = now;

    UwpImGuiFrontend::FrameInput input = m_input.Read(delta);
    if (m_frontend_host.IsCapturingInput())
      m_capture_release_barrier = true;
    else if (m_capture_release_barrier && !m_input.IsAnyInputHeld())
      m_capture_release_barrier = false;
    if (m_capture_release_barrier)
      input = {.deltaSeconds = delta};
    UwpImGuiFrontend::SubmitImGuiNavigationInput(input, true);
    ImGuiIO& io = ImGui::GetIO();
    if (input.pointer)
      io.AddMousePosEvent(input.pointer->x, input.pointer->y);
    else
      io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    io.AddMouseButtonEvent(0, input.pointerPressed);
    if (const float wheel = m_input.TakeWheel(); wheel != 0.0f)
      io.AddMouseWheelEvent(0.0f, wheel);

    m_renderer.BeginFrame(delta);
    m_frontend_host.DrawFrame(input, GetControllerStatus(), GetControllerBattery());
    m_renderer.EndFrame();
  }

  void UpdateGame()
  {
    const auto now = std::chrono::steady_clock::now();
    const float delta =
        std::clamp(std::chrono::duration<float>(now - m_last_frame).count(), 1.0f / 1000.0f, 0.1f);
    m_last_frame = now;

    const UwpImGuiFrontend::FrameInput input = m_input.Read(delta);
    const bool quick_menu = m_input.ConsumeQuickMenuRequest();
    m_session.Update(input, quick_menu, m_input.IsAnyInputHeld());
    std::this_thread::sleep_for(GAME_LOOP_INTERVAL);
  }

  // A question raised by the UI thread itself, which cannot draw a frame until it is answered.
  std::optional<bool> AskModal(const PromptService::Prompt& prompt)
  {
    if (!m_window || m_closed)
      return std::nullopt;

    SetCursorLocked(false);
    HostState::SetControllerInputBlocked(true);
    ++m_modal_prompts;
    const std::optional<bool> answer =
        m_system_prompts.AskModal(m_window.Dispatcher(), prompt, [this] { return m_closed; });
    --m_modal_prompts;

    m_input.Reset();
    m_capture_release_barrier = true;
    m_session.HoldInputUntilReleased();
    return answer;
  }

  void DispatchHotkeys()
  {
    for (const HotkeyScheduler::Request& request : m_hotkeys.TakeRequests())
    {
      if (request.action == HotkeyScheduler::Action::ToggleFullscreen)
        ToggleFullscreen();
      else if (request.action == HotkeyScheduler::Action::UnlockCursor)
        m_cursor_unlocked = true;
      else
        m_session.HandleHotkey(request.action, request.argument);
    }
  }

  void DispatchLibraryHotkeys()
  {
    using Action = HotkeyScheduler::Action;
    for (const HotkeyScheduler::Request& request : m_hotkeys.TakeRequests())
    {
      if (request.action == Action::OpenFile)
        m_frontend_host.PickFileToStart();
      else if (request.action == Action::RefreshLibrary)
        m_frontend_host.RefreshLibrary();
      else if (request.action == Action::PlayRecording)
        m_frontend_host.PickMovieToPlay();
    }
  }

  // A hidden cursor stays in place and only reports movement, so hiding it stands in for a lock.
  void UpdateCursorLock()
  {
    const bool in_game = !IsXbox() && m_session.IsActive() && m_window_active && m_visible &&
                         m_modal_prompts == 0 && !m_session.IsOverlayActive();
    bool hidden = false;
    switch (Config::Get(Config::MAIN_SHOW_CURSOR))
    {
    case Config::ShowCursor::Never:
      hidden = in_game;
      break;
    case Config::ShowCursor::OnMovement:
      hidden = in_game && std::chrono::steady_clock::now() - m_last_mouse_move > CURSOR_HIDE_DELAY;
      break;
    default:
      break;
    }
    SetCursorLocked(hidden ||
                    (in_game && Config::Get(Config::MAIN_LOCK_CURSOR) && !m_cursor_unlocked));
  }

  void UpdateGameWindow()
  {
    if (HostState::ConsumeTitleChanged())
    {
      m_session.OnTitleChanged();
      UpdateWindowTitle();
    }
    if (const auto size = HostState::ConsumeWindowSizeRequest())
      ResizeToGame(size->first, size->second);
  }

  void UpdateWindowTitle()
  {
    if (IsXbox())
      return;
    std::string title;
    if (m_session.IsActive() && Config::Get(Config::MAIN_SHOW_ACTIVE_TITLE))
      title = SConfig::GetInstance().GetTitleDescription();
    try
    {
      ApplicationView::GetForCurrentView().Title(winrt::to_hstring(title));
    }
    catch (const winrt::hresult_error& error)
    {
      ERROR_LOG_FMT(COMMON, "The window title could not be set: {}",
                    winrt::to_string(error.message()));
    }
  }

  // Resizes the window so that the game's output fills it without scaling.
  void ResizeToGame(int width, int height)
  {
    if (IsXbox() || !m_session.IsActive() || !Config::Get(Config::MAIN_RENDER_WINDOW_AUTOSIZE))
      return;
    try
    {
      const ApplicationView view = ApplicationView::GetForCurrentView();
      if (view.IsFullScreenMode() || view.ViewMode() != ApplicationViewMode::Default)
        return;
      const float scale = std::max(GetPixelScale(), 0.1f);
      view.TryResizeView({static_cast<float>(width) / scale, static_cast<float>(height) / scale});
    }
    catch (const winrt::hresult_error& error)
    {
      ERROR_LOG_FMT(COMMON, "The window could not be resized: {}",
                    winrt::to_string(error.message()));
    }
  }

  void SetCursorLocked(bool locked)
  {
    if (locked == m_cursor_locked)
      return;
    m_cursor_locked = locked;
    try
    {
      if (locked)
      {
        m_window.PointerCursor(nullptr);
        return;
      }
      m_window.PointerCursor(UI::CoreCursor(UI::CoreCursorType::Arrow, 0));
      // The cursor reappears where the game's pointer was.
      if (m_window_active)
        MoveCursorTo(m_cursor_position);
    }
    catch (const winrt::hresult_error& error)
    {
      ERROR_LOG_FMT(COMMON, "The mouse cursor could not be changed: {}",
                    winrt::to_string(error.message()));
    }
  }

  // Moves the system cursor to a position that ranges from -1 to 1 across the window.
  void MoveCursorTo(Common::Vec2 position)
  {
    const winrt::Windows::Foundation::Rect bounds = m_window.Bounds();
    m_window.PointerPosition({bounds.X + (position.x + 1.0f) * 0.5f * bounds.Width,
                              bounds.Y + (position.y + 1.0f) * 0.5f * bounds.Height});
  }

  void CenterCursor()
  {
    m_cursor_position = {};
    ciface::UWP::SetCursorPosition(0.0f, 0.0f);
    if (m_cursor_locked || !m_window_active)
      return;
    try
    {
      MoveCursorTo(m_cursor_position);
    }
    catch (const winrt::hresult_error& error)
    {
      ERROR_LOG_FMT(COMMON, "The mouse cursor could not be moved: {}",
                    winrt::to_string(error.message()));
    }
  }

  // Xbox apps always fill the screen. On PC, the window state stands in for exclusive fullscreen.
  void ToggleFullscreen()
  {
    if (IsXbox())
      return;
    const ApplicationView view = ApplicationView::GetForCurrentView();
    if (view.ViewMode() == ApplicationViewMode::CompactOverlay)
    {
      // The window that stays on top goes back to a normal window first.
      m_restore_view_mode = false;
      view.TryEnterViewModeAsync(ApplicationViewMode::Default);
      return;
    }
    if (view.IsFullScreenMode())
      view.ExitFullScreenMode();
    else
      view.TryEnterFullScreenMode();
    UpdateFullscreenState();
  }

  static void UpdateFullscreenState()
  {
    HostState::SetFullscreen(IsXbox() || ApplicationView::GetForCurrentView().IsFullScreenMode());
  }

  // A UWP window only stays on top in compact overlay mode, which takes the place of fullscreen.
  void EnterGameWindowMode()
  {
    if (IsXbox())
      return;
    try
    {
      const ApplicationView view = ApplicationView::GetForCurrentView();
      if (Config::Get(Config::MAIN_KEEP_WINDOW_ON_TOP) &&
          view.IsViewModeSupported(ApplicationViewMode::CompactOverlay))
      {
        if (view.IsFullScreenMode())
          view.ExitFullScreenMode();
        const ViewModePreferences preferences =
            ViewModePreferences::CreateDefault(ApplicationViewMode::CompactOverlay);
        preferences.CustomSize({640.0f, 480.0f});
        view.TryEnterViewModeAsync(ApplicationViewMode::CompactOverlay, preferences);
        m_restore_view_mode = true;
        UpdateFullscreenState();
        return;
      }
    }
    catch (const winrt::hresult_error& error)
    {
      ERROR_LOG_FMT(COMMON, "The window could not stay on top: {}",
                    winrt::to_string(error.message()));
    }
    EnterGameFullscreen();
  }

  void LeaveGameWindowMode()
  {
    if (std::exchange(m_restore_view_mode, false))
    {
      try
      {
        ApplicationView::GetForCurrentView().TryEnterViewModeAsync(ApplicationViewMode::Default);
      }
      catch (const winrt::hresult_error& error)
      {
        ERROR_LOG_FMT(COMMON, "The window could not leave the top: {}",
                      winrt::to_string(error.message()));
      }
    }
    LeaveGameFullscreen();
    UpdateWindowTitle();
  }

  void EnterGameFullscreen()
  {
    if (IsXbox() || !Config::Get(Config::MAIN_FULLSCREEN))
      return;
    const ApplicationView view = ApplicationView::GetForCurrentView();
    if (!view.IsFullScreenMode() && view.TryEnterFullScreenMode())
      m_restore_windowed = true;
    UpdateFullscreenState();
  }

  void LeaveGameFullscreen()
  {
    if (!std::exchange(m_restore_windowed, false))
      return;
    ApplicationView::GetForCurrentView().ExitFullScreenMode();
    UpdateFullscreenState();
  }

  // Keeps the display on while a game runs.
  void UpdateDisplayRequest()
  {
    const bool active = m_session.IsActive() && Config::Get(Config::MAIN_DISABLE_SCREENSAVER) &&
                        Core::GetState(Core::System::GetInstance()) == Core::State::Running;
    if (active == m_display_requested)
      return;
    try
    {
      if (!m_display_request)
        m_display_request = winrt::Windows::System::Display::DisplayRequest();
      if (active)
        m_display_request.RequestActive();
      else
        m_display_request.RequestRelease();
      m_display_requested = active;
    }
    catch (const winrt::hresult_error& error)
    {
      ERROR_LOG_FMT(COMMON, "Display request failed: {}", winrt::to_string(error.message()));
      m_display_requested = active;
    }
  }

  void LaunchItem(UwpImGuiFrontend::ItemId item)
  {
    const auto game = m_library.FindGame(item);
    const auto entry = m_library.FindItem(item);
    if (!game || !entry)
    {
      m_frontend_host.Notify("The selected game is no longer available");
      return;
    }
    StartGame(game->GetFilePath(), entry->name, item);
  }

  void StartGame(const std::string& path, std::string title,
                 std::optional<UwpImGuiFrontend::ItemId> item)
  {
    if (!item)
    {
      if (const auto entry = m_library.FindItemByPath(path))
      {
        item = entry->id;
        if (title.empty())
          title = entry->name;
      }
    }
    if (title.empty())
      title = PathToString(StringToPath(path).stem());
    StartBoot({BootParameters::GenerateFromFile(path), std::move(title), item});
  }

  void StartBoot(BootRequest request)
  {
    // A game that a frontend started returns to that frontend.
    m_session.SetExitLabel(m_launch_on_exit.empty() ? "Exit to Library" : "Exit");
    if (!request.parameters)
    {
      AbortMovie();
      m_frontend_host.Notify(fmt::format("Unable to start {}", request.title));
      return;
    }

    m_frontend_host.ReleaseGraphicsResources();
    m_renderer.Shutdown();
    m_shell_context = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(nullptr);

    m_session.SetThemeIndex(m_frontend_host.GetThemeIndex());
    m_input.Reset();
    m_cursor_unlocked = false;
    m_last_mouse_move = std::chrono::steady_clock::now();
    EnterGameWindowMode();
    if (!m_session.Start(std::move(request.parameters), request.title, MakeWindowSystemInfo()))
    {
      AbortMovie();
      RestoreShell();
      m_frontend_host.Notify(fmt::format("Unable to start {}", request.title));
      return;
    }
    m_frontend_host.SetRunningContent(request.item);
  }

  static void AbortMovie()
  {
    auto& movie = Core::System::GetInstance().GetMovie();
    if (movie.IsMovieActive())
      movie.EndPlayInput(false);
  }

  void RestoreShell()
  {
    LeaveGameWindowMode();
    ImGui::SetCurrentContext(m_shell_context);
    m_shell_context = nullptr;
    m_frontend_host.SetRunningContent(std::nullopt);
    m_input.Reset();

    if (const auto frontend = ReturnToFrontend())
    {
      frontend.Completed([](auto&&, auto&&) { CoreApp::CoreApplication::Exit(); });
      return;
    }

    const OutputSize size = GetWindowPixelSize(m_window);
    if (!m_renderer.Initialize(m_window, size.width, size.height))
    {
      WriteFailure("The Direct3D 11 interface renderer could not be recreated");
      m_closed = true;
      return;
    }
    m_last_frame = std::chrono::steady_clock::now();
  }

  // A gamepad that connects plays as the player with its number.
  void AssignGamepads()
  {
    const auto now = std::chrono::steady_clock::now();
    if (now < m_next_gamepad_check)
      return;
    m_next_gamepad_check = now + GAMEPAD_CHECK_INTERVAL;

    constexpr std::string_view prefix = "WGInput/";
    std::array<std::string, 4> devices;
    for (const std::string& device : g_controller_interface.GetAllDeviceStrings())
    {
      size_t number = 0;
      if (!device.starts_with(prefix) ||
          std::from_chars(device.data() + prefix.size(), device.data() + device.size(), number)
                  .ec != std::errc{} ||
          number >= devices.size() || !devices[number].empty())
      {
        continue;
      }
      devices[number] = device;
    }

    for (size_t player = 0; player < devices.size(); ++player)
    {
      const bool connected = !devices[player].empty();
      if (connected && !m_gamepad_connected[player])
      {
        if (player == 0 && std::exchange(m_apply_default_mappings, false))
          InputMappingHost::ApplyDefaultGamepadMappings(0, devices[0]);
        InputMappingHost::AssignGamepad(static_cast<int>(player), devices[player]);
      }
      else if (!connected && m_gamepad_connected[player] && player > 0)
      {
        InputMappingHost::DisconnectGamepad(static_cast<int>(player));
      }
      m_gamepad_connected[player] = connected;
    }
  }

  WindowSystemInfo MakeWindowSystemInfo() const
  {
    const OutputSize output = GetOutputSize(m_window);
    WindowSystemInfo wsi;
    wsi.type = WindowSystemType::WinRT;
    wsi.render_window = winrt::get_abi(m_window);
    wsi.render_surface = winrt::get_abi(m_window);
    wsi.render_surface_width = static_cast<int>(output.width);
    wsi.render_surface_height = static_cast<int>(output.height);
    wsi.render_surface_scale = std::clamp(static_cast<float>(output.height) / 1080.0f, 1.0f, 2.0f);
    return wsi;
  }

  std::string GetControllerStatus() const
  {
    try
    {
      const u32 count = GamingInput::Gamepad::Gamepads().Size();
      if (count == 0)
        return "No controller";
      return count == 1 ? "1 controller" : fmt::format("{} controllers", count);
    }
    catch (const winrt::hresult_error&)
    {
      return "No controller";
    }
  }

  std::optional<float> GetControllerBattery()
  {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_battery_poll < BATTERY_POLL_INTERVAL)
      return m_battery_level;
    m_last_battery_poll = now;
    m_battery_level.reset();

    try
    {
      const auto gamepads = GamingInput::Gamepad::Gamepads();
      if (gamepads.Size() == 0)
        return m_battery_level;
      const auto report = gamepads.GetAt(0).TryGetBatteryReport();
      if (!report)
        return m_battery_level;
      const auto remaining = report.RemainingCapacityInMilliwattHours();
      const auto full = report.FullChargeCapacityInMilliwattHours();
      if (remaining && full && full.Value() > 0)
      {
        m_battery_level = std::clamp(
            static_cast<float>(remaining.Value()) / static_cast<float>(full.Value()), 0.0f, 1.0f);
      }
    }
    catch (const winrt::hresult_error&)
    {
    }
    return m_battery_level;
  }

  void OnKey(const UI::KeyEventArgs& args, bool pressed)
  {
    const VirtualKey key = args.VirtualKey();
    // Typing in an in-game overlay doesn't reach the game. Releases still do, so no key stays
    // held.
    if (pressed && m_session.IsActive() && GameTextInput::IsActive())
    {
      if (GameTextInput::HandleKey(key))
        args.Handled(true);
      return;
    }
    if (pressed && m_text_capture && !m_session.IsActive())
    {
      if (const auto command = ToTextCommand(key))
      {
        const bool shift = (m_window.GetKeyState(VirtualKey::Shift) &
                            UI::CoreVirtualKeyStates::Down) == UI::CoreVirtualKeyStates::Down;
        UwpImGuiFrontend::HandleNativeTextEditingCommand(*command, shift);
        args.Handled(true);
        return;
      }
      if (key != VirtualKey::Escape && key != VirtualKey::Tab)
        return;
    }

    m_input.SetKey(key, pressed);
    ciface::UWP::SetKeyState(static_cast<u32>(key), pressed);
  }

  void OnPointer(const UI::PointerEventArgs& args)
  {
    m_last_mouse_move = std::chrono::steady_clock::now();
    const auto point = args.CurrentPoint();
    const float scale = GetPixelScale();
    m_input.SetPointer(
        UwpImGuiFrontend::Vec2{point.Position().X * scale, point.Position().Y * scale},
        point.Properties().IsLeftButtonPressed());

    const auto properties = point.Properties();
    ciface::UWP::SetMouseButtons((properties.IsLeftButtonPressed() ? 1u : 0u) |
                                 (properties.IsRightButtonPressed() ? 2u : 0u) |
                                 (properties.IsMiddleButtonPressed() ? 4u : 0u) |
                                 (properties.IsXButton1Pressed() ? 8u : 0u) |
                                 (properties.IsXButton2Pressed() ? 16u : 0u));
    if (m_cursor_locked)
      return;
    const winrt::Windows::Foundation::Rect bounds = m_window.Bounds();
    m_cursor_position = {point.Position().X / std::max(bounds.Width, 1.0f) * 2.0f - 1.0f,
                         point.Position().Y / std::max(bounds.Height, 1.0f) * 2.0f - 1.0f};
    ciface::UWP::SetCursorPosition(m_cursor_position.x, m_cursor_position.y);
  }

  // While the cursor is locked, its movement moves the pointer that the game sees.
  void OnMouseMoved(const winrt::Windows::Devices::Input::MouseEventArgs& args)
  {
    const winrt::Windows::Devices::Input::MouseDelta delta = args.MouseDelta();
    if (delta.X != 0 || delta.Y != 0)
      m_last_mouse_move = std::chrono::steady_clock::now();
    ciface::UWP::AddMouseMovement(static_cast<float>(delta.X), static_cast<float>(delta.Y), 0.0f);
    if (!m_cursor_locked)
      return;
    const OutputSize size = GetWindowPixelSize(m_window);
    m_cursor_position.x = std::clamp(m_cursor_position.x + static_cast<float>(delta.X) * 2.0f /
                                                               std::max(size.width, 1u),
                                     -1.0f, 1.0f);
    m_cursor_position.y = std::clamp(m_cursor_position.y + static_cast<float>(delta.Y) * 2.0f /
                                                               std::max(size.height, 1u),
                                     -1.0f, 1.0f);
    ciface::UWP::SetCursorPosition(m_cursor_position.x, m_cursor_position.y);
  }

  void OnSizeChanged()
  {
    // A dialog raised in the middle of a frame defers resizing until the frame is over.
    if (m_modal_prompts > 0)
    {
      m_resize_pending = true;
      return;
    }

    UpdateFullscreenState();
    if (m_session.IsActive())
    {
      if (!IsXbox() && Core::IsRunning(Core::System::GetInstance()) && g_presenter)
        g_presenter->ResizeSurface();
      return;
    }
    if (m_renderer.IsInitialized())
    {
      const OutputSize size = GetWindowPixelSize(m_window);
      m_renderer.Resize(size.width, size.height);
    }
  }

  void OnActivated(const CoreApp::CoreApplicationView&, const Activation::IActivatedEventArgs& args)
  {
    if (args.Kind() == Activation::ActivationKind::Protocol)
    {
      const auto protocol = args.as<Activation::ProtocolActivatedEventArgs>();
      const auto query = protocol.Uri().QueryParsed();
      for (uint32_t i = 0; i < query.Size(); ++i)
      {
        const auto entry = query.GetAt(i);
        if (entry.Name() == L"cmd")
        {
          const std::string path = ParseCommandLinePath(entry.Value());
          if (!path.empty() && !m_session.IsActive())
            m_pending_boot_path = path;
        }
        else if (entry.Name() == L"launchOnExit")
        {
          m_launch_on_exit = entry.Value();
        }
      }
    }
    m_window.Activate();
    m_activated = true;
  }

  // Opens the frontend that started Dolphin, once. Returns nothing when there is none.
  winrt::Windows::Foundation::IAsyncOperation<bool> ReturnToFrontend()
  {
    if (m_launch_on_exit.empty())
      return nullptr;
    try
    {
      auto launch = winrt::Windows::System::Launcher::LaunchUriAsync(
          winrt::Windows::Foundation::Uri(std::exchange(m_launch_on_exit, {})));
      m_exit_requested = true;
      return launch;
    }
    catch (const winrt::hresult_error&)
    {
      return nullptr;
    }
  }

  void OnSuspending(const winrt::Windows::Foundation::IInspectable&,
                    const winrt::Windows::ApplicationModel::SuspendingEventArgs& args)
  {
    const auto deferral = args.SuspendingOperation().GetDeferral();
    // The system only suspends a visible app to close it, as Quit in the guide does. Going Home
    // hides the app first.
    const bool closing = m_visible;
    const auto frontend = closing ? ReturnToFrontend() : nullptr;
    if (closing)
    {
      m_session.Shutdown();
    }
    else if (m_modal_prompts == 0)
    {
      m_session.Suspend();
      // The app can be closed while it is suspended.
      m_session.FlushSaveData();
    }
    if (m_dolphin_initialized)
    {
      m_settings_pages.Flush();
      Config::Save();
    }

    // The system closes the app once the suspension completes, so the frontend opens first.
    if (frontend)
      frontend.Completed([deferral](auto&&, auto&&) { deferral.Complete(); });
    else
      deferral.Complete();
  }

  void OnResuming(const winrt::Windows::Foundation::IInspectable&,
                  const winrt::Windows::Foundation::IInspectable&)
  {
    m_last_frame = std::chrono::steady_clock::now();
    m_session.Resume();
  }

  void Shutdown()
  {
    PromptService::GetInstance().Shutdown();
    m_session.Shutdown();
    UpdateDisplayRequest();
    if (m_shell_context)
      ImGui::SetCurrentContext(m_shell_context);

    if (m_shell_initialized)
    {
      m_frontend_host.Shutdown();
      m_mapping_host.ReleaseTextures();
      UwpImGuiFrontend::ShutdownNativeTextInput();
      m_shell_initialized = false;
    }
    m_library.Shutdown();
    m_renderer.Shutdown();
    if (ImGui::GetCurrentContext())
      ImGui::DestroyContext();

    if (m_dolphin_initialized)
    {
      m_hotkeys.Stop();
      AchievementManager::GetInstance().Shutdown();
      Config::Save();
      UICommon::ShutdownControllers();
      if (m_storage_paths_callback)
        Config::RemoveConfigChangedCallback(*m_storage_paths_callback);
      UICommon::Shutdown();
      m_dolphin_initialized = false;
    }
  }

  UI::CoreWindow m_window{nullptr};
  FrontendRenderer m_renderer;
  InputSource m_input;
  GameLibrary m_library;
  InputMappingHost m_mapping_host;
  HotkeyScheduler m_hotkeys;
  SystemPrompts m_system_prompts;
  winrt::Windows::Devices::Input::MouseDevice::MouseMoved_revoker m_mouse_moved;
  Common::Vec2 m_cursor_position;
  std::chrono::steady_clock::time_point m_last_mouse_move;
  bool m_cursor_locked = false;
  bool m_cursor_unlocked = false;
  bool m_window_active = false;
  int m_modal_prompts = 0;
  bool m_resize_pending = false;
  bool m_capture_release_barrier = false;
  SettingsPages m_settings_pages;
  FrontendHost m_frontend_host{m_library, m_mapping_host, m_settings_pages};
  EmulationSession m_session;

  std::vector<u8> m_font_data;
  ImFont* m_text_font = nullptr;
  ImFont* m_shell_font = nullptr;
  ImGuiContext* m_shell_context = nullptr;

  std::optional<std::string> m_pending_boot_path;
  winrt::hstring m_launch_on_exit;
  std::chrono::steady_clock::time_point m_last_frame;
  std::chrono::steady_clock::time_point m_last_battery_poll;
  std::optional<float> m_battery_level;
  winrt::Windows::System::Display::DisplayRequest m_display_request{nullptr};
  bool m_display_requested = false;
  bool m_restore_windowed = false;
  bool m_restore_view_mode = false;
  bool m_text_capture = false;
  bool m_exit_requested = false;
  bool m_apply_default_mappings = false;
  std::array<bool, 4> m_gamepad_connected{};
  std::chrono::steady_clock::time_point m_next_gamepad_check{};
  std::optional<Config::ConfigChangedCallbackID> m_storage_paths_callback;
  bool m_dolphin_initialized = false;
  bool m_shell_initialized = false;
  bool m_activated = false;
  bool m_visible = true;
  bool m_closed = false;
};

class AppSource : public winrt::implements<AppSource, CoreApp::IFrameworkViewSource>
{
public:
  CoreApp::IFrameworkView CreateView() { return winrt::make<App>(); }
};
}  // namespace
}  // namespace DolphinUWP

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
  try
  {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
  }
  catch (const winrt::hresult_error& error)
  {
    if (static_cast<HRESULT>(error.code()) != RPC_E_CHANGED_MODE)
      return 1;
  }

  winrt::Windows::ApplicationModel::Core::CoreApplication::Run(
      winrt::make<DolphinUWP::AppSource>());
  return 0;
}

#pragma warning(pop)
