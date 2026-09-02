#
# catfight's three game modules: qagame (server), cgame (client), ui.
#
# This replaces upstream's basegame.cmake, which built Quake 3's game code. The
# module *names* are still cgame/qagame/ui because the engine hardcodes them
# when it calls VM_Create(); the code behind them is catfight's own.
#
# Layout:
#   code/cf_shared   compiled into all three -- gameplay definitions and the
#                    movement code, which the server and the client must run
#                    identically for prediction to work
#   code/cf_game     the server game: authority over the world
#   code/cf_cgame    the client game: draws the world and predicts the player
#   code/cf_ui       menus and the screen when not in a level
#
# The engine-side ABI headers (game/g_public.h, cgame/cg_public.h,
# ui/ui_public.h) are still upstream's and are included by relative path. They
# describe the engine's interface, not Quake 3's gameplay, so there is nothing
# to replace in them.
#

if(NOT BUILD_GAME_LIBRARIES AND NOT BUILD_GAME_QVMS)
    return()
endif()

if(BUILD_GAME_QVMS)
    message(FATAL_ERROR
        "BUILD_GAME_QVMS is not supported yet.\n"
        "catfight's modules link against the C library, which a QVM does not "
        "have -- upstream solves this with game/bg_lib.c, and catfight has no "
        "equivalent yet. Configure with -DBUILD_GAME_QVMS=OFF.")
endif()

include(utils/set_output_dirs)

set(CF_SHARED_SOURCES
    ${SOURCE_DIR}/cf_shared/cf_pmove.c
    ${SOURCE_DIR}/cf_shared/cf_entstate.c
    ${SOURCE_DIR}/cf_shared/cf_weapons.c
    ${SOURCE_DIR}/qcommon/q_math.c
    ${SOURCE_DIR}/qcommon/q_shared.c
)

set(CF_GAME_SOURCES
    ${SOURCE_DIR}/cf_game/g_main.c
    ${SOURCE_DIR}/cf_game/g_client.c
    ${SOURCE_DIR}/cf_game/g_combat.c
    ${SOURCE_DIR}/cf_game/g_team.c
${SOURCE_DIR}/cf_game/g_bot.c
    ${SOURCE_DIR}/cf_game/g_round.c
    ${SOURCE_DIR}/cf_game/g_spawn.c
    ${SOURCE_DIR}/cf_game/g_ticket.c
    ${SOURCE_DIR}/cf_game/g_weapon.c
    ${SOURCE_DIR}/cf_game/g_utils.c
    ${SOURCE_DIR}/cf_game/g_syscalls.c
)

set(CF_CGAME_SOURCES
    ${SOURCE_DIR}/cf_cgame/cg_main.c
    ${SOURCE_DIR}/cf_cgame/cg_view.c
    ${SOURCE_DIR}/cf_cgame/cg_draw.c
    ${SOURCE_DIR}/cf_cgame/cg_scoreboard.c
    ${SOURCE_DIR}/cf_cgame/cg_event.c
    ${SOURCE_DIR}/cf_cgame/cg_ents.c
    ${SOURCE_DIR}/cf_cgame/cg_predict.c
    ${SOURCE_DIR}/cf_cgame/cg_snapshot.c
    ${SOURCE_DIR}/cf_cgame/cg_servercmds.c
    ${SOURCE_DIR}/cf_cgame/cg_syscalls.c
)

set(CF_UI_SOURCES
    ${SOURCE_DIR}/cf_ui/ui_main.c
    ${SOURCE_DIR}/cf_ui/ui_syscalls.c
)

# The modules include q_shared.h, which reads BASEGAME and the standalone flag.
# Without this they would compile against "baseq3" while the engine uses
# "catfight" -- a mismatch that shows up as the game looking in the wrong
# directory, with no error to explain it.
if(BUILD_STANDALONE)
    list(APPEND CF_MODULE_DEFINITIONS STANDALONE)
endif()

function(cf_add_module TARGET_BASENAME DEFINE)
    set(TARGET ${TARGET_BASENAME}_${BASEGAME})

    add_library(                ${TARGET} SHARED ${ARGN} ${CF_SHARED_SOURCES})
    target_compile_definitions( ${TARGET} PRIVATE ${DEFINE} ${CF_MODULE_DEFINITIONS})
    target_link_libraries(      ${TARGET} PRIVATE ${COMMON_LIBRARIES})
    set_target_properties(      ${TARGET} PROPERTIES OUTPUT_NAME ${TARGET_BASENAME})
    set_output_dirs(            ${TARGET} SUBDIRECTORY ${BASEGAME})
endfunction()

# qagame is built only if its source is present.
#
# It is absent in exactly one situation: the public source tree, which exists to
# satisfy the GPL for the binaries catfight actually distributes -- the engine,
# cgame and ui. qagame.dll is NOT distributed (see netplay/package-client.ps1
# for the reasoning), so its source carries no obligation and is not published.
#
# The tree still has to CONFIGURE AND BUILD without it. "Corresponding source"
# that does not compile is not much of an offer, and a recipient must be able to
# build and modify the parts they were given. Everything here except this one
# target does exactly that.
if(EXISTS ${SOURCE_DIR}/cf_game/g_main.c)
    cf_add_module(${GAME_MODULE} QAGAME ${CF_GAME_SOURCES})
else()
    message(STATUS "cf_game/ is absent -- building the client only, without qagame")
endif()

cf_add_module(${CGAME_MODULE} CGAME  ${CF_CGAME_SOURCES})
cf_add_module(${UI_MODULE}    UI     ${CF_UI_SOURCES})
