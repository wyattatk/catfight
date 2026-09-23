include_guard(GLOBAL)

# catfight's game content (configs, shaders, maps, textures) is authored under
# <source>/catfight and version-controlled there. The engine expects to find it
# in the game directory next to the binaries, so mirror it into the build output
# on every build.
#
# This is a plain directory copy, not a pack step -- loose files in the game
# directory take priority over pk3s and are far easier to iterate on. Packing
# into a .pk3 is a release concern, not a development one.
#
# To refresh assets without a full rebuild:
#   cmake --build <builddir> --target catfight_assets
#
# TODO: this copies the whole tree, so map sources and compiler intermediates
# (.map, .prt, .srf) land in the game directory alongside the .bsp the engine
# actually reads. Harmless during development and useful for debugging a
# compile, but a release package should carry only the .bsp. copy_directory has
# no exclude option, so filtering means either enumerating what to copy or
# moving map sources outside the game directory. Worth doing when there is a
# packaging step to hang it off; not worth the indirection before then.

set(CATFIGHT_ASSET_SOURCE_DIR ${CMAKE_SOURCE_DIR}/${BASEGAME})

# No asset tree is a supported configuration, and it is the one the published
# source tree is in: the GPL covers program source, not artwork, maps or sounds,
# so assets are not published (see netplay/publish-source.ps1).
#
# Warning and then adding the target anyway made the build FAIL at the copy --
# "The system cannot find the path specified" -- after everything had compiled,
# which turned "you have no assets" into "the build is broken". A recipient
# building the published source hit that on the first try.
#
# A binary built without assets runs against an existing installation's game
# directory, which is exactly how a recipient exercises their right to modify
# and rebuild the client they were given.
if(EXISTS ${CATFIGHT_ASSET_SOURCE_DIR})
    add_custom_target(catfight_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CATFIGHT_ASSET_SOURCE_DIR}
            ${CMAKE_BINARY_DIR}/$<CONFIG>/${BASEGAME}
        COMMENT "Copying catfight assets into the game directory"
        VERBATIM)
else()
    message(STATUS
        "No asset directory at ${CATFIGHT_ASSET_SOURCE_DIR}; building binaries "
        "only. Point the result at an existing catfight installation to run it.")
endif()
