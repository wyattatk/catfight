set(PROJECT_NAME catfight)
set(PROJECT_VERSION 0.1.0)

set(SERVER_NAME catfightded)
set(CLIENT_NAME catfight)

set(BASEGAME catfight)

# These three must stay as-is: the engine hardcodes these module names when it
# calls VM_Create() (see sv_game.c, cl_cgame.c, cl_ui.c). They name the modules,
# not the game.
set(CGAME_MODULE cgame)
set(GAME_MODULE qagame)
set(UI_MODULE ui)

# Both are GENERATED from misc/icon/ by mapping/gen_icon.ps1, along with the
# SDL window icon in code/sdl/sdl_icon.h. Edit the mark, re-run that script,
# rebuild -- never edit these three by hand, or they drift apart.
set(WINDOWS_ICON_PATH ${CMAKE_SOURCE_DIR}/misc/windows/catfight.ico)

set(MACOS_ICON_PATH ${CMAKE_SOURCE_DIR}/misc/macos/catfight.icns)
set(MACOS_BUNDLE_ID dev.catfight.${CLIENT_NAME})

set(COPYRIGHT "Catfight. Built on ioquake3, which is licensed under the GPL v2.")

set(CONTACT_EMAIL "wyattatk@gmail.com")
set(PROTOCOL_HANDLER_SCHEME catfight)
