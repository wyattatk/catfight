catfight engine -- source
=========================

This is the complete corresponding source for the GPL-licensed binaries
distributed with catfight: the engine (catfight.exe), the renderers
(renderer_opengl1.dll, renderer_opengl2.dll), and the client game modules
(cgame.dll, ui.dll).

The engine is derived from ioquake3 (https://ioquake3.org), which is licensed
under the GNU General Public License version 2. See COPYING.txt.

Building
--------

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DBUILD_STANDALONE=ON -DBUILD_GAME_QVMS=OFF -DBUILD_MISSIONPACK=OFF
    cmake --build build

What is not here, and why
-------------------------

* code/cf_game -- the server-side game module (qagame). It is not distributed
  with the game: catfight is played on dedicated servers we run, and qagame.dll
  is not part of any download. Nothing obliges its publication and it is not
  published.

* The matchmaker, the server agent, and the Steam helper. The first two are
  server software that is never distributed. The Steam helper is a separate
  program that links the proprietary Steamworks SDK and communicates with the
  game only over a text protocol on a pipe; it shares no source with the engine
  and is not part of this work.

* Game assets. The GPL covers program source, not artwork, maps or sounds.

This tree builds a working client. It will not build qagame, and the build is
configured to skip it when absent.
