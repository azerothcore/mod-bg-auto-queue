# mod-bg-auto-queue

AzerothCore module that automatically queues players into a battleground on
login. Players are opted in by default and can opt out with an in-game command.

## Features

- Automatically enqueues eligible players into a battleground on login.
- Configurable level range that gates the auto-join behavior.
- Configurable default battleground: Random Battleground, Warsong Gulch, or
  Arathi Basin. Falls back to Warsong Gulch when the chosen battleground is not
  available for the player's level.
- Per-character opt-out persisted in the characters database.
- `.bgevents` command to enable, disable, or inspect the auto-join state.

## Installation

1. Clone this folder into `modules/mod-bg-auto-queue/` of your AzerothCore source.
2. Re-run CMake and rebuild the worldserver.
3. Copy `mod-bg-auto-queue.conf.dist` to `mod-bg-auto-queue.conf` in your worldserver's
   configuration directory and adjust as needed.
4. The module installs its characters-database table automatically through the
   AzerothCore SQL updater on first run.

## Configuration

See `conf/mod-bg-auto-queue.conf.dist` for the full list of options.

## Commands

- `.bgevents on` — enable auto-join for the current character (default state).
- `.bgevents off` — disable auto-join for the current character.
- `.bgevents status` — show the current auto-join state.
