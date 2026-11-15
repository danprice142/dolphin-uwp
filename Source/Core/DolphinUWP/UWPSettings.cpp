// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/UWPSettings.h"

namespace Config
{
// Violet.
const Info<int> UWP_THEME{{System::Main, "UWP", "Theme"}, 5};
const Info<bool> UWP_SHOW_ACTION_HINTS{{System::Main, "UWP", "ShowActionHints"}, true};
const Info<bool> UWP_ANIMATE_LAUNCHES{{System::Main, "UWP", "AnimateLaunches"}, true};
const Info<int> UWP_STATE_SLOT{{System::Main, "UWP", "StateSlot"}, 1};
const Info<std::string> UWP_FAVORITES{{System::Main, "UWP", "Favorites"}, ""};
const Info<std::string> UWP_GC_PATH{{System::Main, "UWP", "GCPath"}, ""};
const Info<std::string> UWP_CONTROLLER_PROFILES_PATH{
    {System::Main, "UWP", "ControllerProfilesPath"}, ""};
const Info<std::string> UWP_GAME_SETTINGS_PATH{{System::Main, "UWP", "GameSettingsPath"}, ""};
const Info<std::string> UWP_CACHE_PATH{{System::Main, "UWP", "CachePath"}, ""};
const Info<std::string> UWP_GRAPHICS_MOD_SETTINGS_PATH{
    {System::Main, "UWP", "GraphicsModSettingsPath"}, ""};
const Info<int> UWP_LIBRARY_PLATFORM{{System::Main, "UWP", "LibraryPlatform"}, 0};
const Info<int> UWP_LIBRARY_SORT{{System::Main, "UWP", "LibrarySort"}, 0};

const Info<bool> UWP_SCREENSCRAPER_ENABLED{{System::Main, "UWP", "ScreenScraperEnabled"}, false};
const Info<std::string> UWP_SCREENSCRAPER_MEDIA_DIRECTORY{
    {System::Main, "UWP", "ScreenScraperMediaDirectory"}, ""};
const Info<int> UWP_SCREENSCRAPER_LIST_ARTWORK{{System::Main, "UWP", "ScreenScraperListArtwork"},
                                               0};
const Info<int> UWP_SCREENSCRAPER_DETAIL_ARTWORK{
    {System::Main, "UWP", "ScreenScraperDetailArtwork"}, 0};
const Info<int> UWP_SCREENSCRAPER_BACKGROUND{{System::Main, "UWP", "ScreenScraperBackground"}, 0};
const Info<int> UWP_SCREENSCRAPER_REGION{{System::Main, "UWP", "ScreenScraperRegion"}, 0};
const Info<int> UWP_SCREENSCRAPER_DOWNLOADS{{System::Main, "UWP", "ScreenScraperDownloads"}, 0x37};
const Info<bool> UWP_SCREENSCRAPER_OVERWRITE{{System::Main, "UWP", "ScreenScraperOverwrite"},
                                             false};

const Info<std::string> UWP_DSU_SERVERS{{System::Main, "UWP", "DSUServers"}, ""};
const Info<GBALayout> UWP_GBA_LAYOUT{{System::Main, "UWP", "GBALayout"}, GBALayout::Bottom};
const Info<int> UWP_GBA_SCALE{{System::Main, "UWP", "GBAScale"}, 0};
const Info<bool> UWP_GBA_FRAME{{System::Main, "UWP", "GBAFrame"}, true};
const Info<bool> UWP_GBA_INTERFRAME_BLENDING{{System::Main, "UWP", "GBAInterframeBlending"}, false};
const Info<bool> UWP_GBA_SHOW_STREAMED{{System::Main, "UWP", "GBAShowStreamed"}, false};
const std::array<Info<int>, 4> UWP_GBA_SCALES{
    Info<int>{{System::Main, "UWP", "GBA1Scale"}, -1},
    Info<int>{{System::Main, "UWP", "GBA2Scale"}, -1},
    Info<int>{{System::Main, "UWP", "GBA3Scale"}, -1},
    Info<int>{{System::Main, "UWP", "GBA4Scale"}, -1},
};
const std::array<Info<int>, 4> UWP_GBA_BLENDING{
    Info<int>{{System::Main, "UWP", "GBA1InterframeBlending"}, -1},
    Info<int>{{System::Main, "UWP", "GBA2InterframeBlending"}, -1},
    Info<int>{{System::Main, "UWP", "GBA3InterframeBlending"}, -1},
    Info<int>{{System::Main, "UWP", "GBA4InterframeBlending"}, -1},
};
const std::array<Info<bool>, 4> UWP_GBA_ALWAYS_ON_TOP{
    Info<bool>{{System::Main, "UWP", "GBA1AlwaysOnTop"}, false},
    Info<bool>{{System::Main, "UWP", "GBA2AlwaysOnTop"}, false},
    Info<bool>{{System::Main, "UWP", "GBA3AlwaysOnTop"}, false},
    Info<bool>{{System::Main, "UWP", "GBA4AlwaysOnTop"}, false},
};
const Info<bool> UWP_STREAMING_ENABLED{{System::Main, "UWP", "StreamingEnabled"}, false};
const Info<std::string> UWP_STREAMING_URL{{System::Main, "UWP", "StreamingURL"},
                                          "udp://127.0.0.1:5000"};
}  // namespace Config
