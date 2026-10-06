/**
 * @file N64Game.h
 * @brief Which game this package is. Everything else in Source/ is com.recomp.n64's shared code
 *        (Packages/com.recomp.n64/Source/Game), made for {{TITLE}} by these names.
 *
 * Made from com.recomp.n64's game template.
 */
#pragma once

#define N64_GAME_PLAYER {{NAME}}Player
#define N64_GAME_ID "{{ID}}"
#define N64_GAME_PACKAGE "{{PACKAGE}}"
#define N64_GAME_TITLE "{{TITLE}}"
// The ROM's name in the project (Assets/Recomp/Rom/...): Recomp/game.json's rom.file
#define N64_GAME_ROM_FILE "{{ID}}.{{REGION}}.z64"
// The entry point of a packaged (statically linked) build: PolyphasePlugin_GetDesc_<package id>
#define N64_GAME_PLUGIN_ENTRY {{ENTRY}}

// A controller layout of the game's own (the default puts the buttons where they sit on an N64
// pad; see Game/N64GamePlayerImpl.h): define this and, in {{NAME}}Player.cpp before the
// implementation, `namespace N64GamePlayerDetail { void N64GameMapGamepad(int port, PortPad& pad) {...} }`.
// #define N64_GAME_CUSTOM_PAD_MAPPING 1

// Changes whenever the game library is rebuilt, so the addon relinks.
#include "Generated/{{NAME}}LibStamp.h"
