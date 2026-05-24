# mod-bg-auto-queue

AzerothCore module that periodically auto-queues eligible online players into
battlegrounds, grouped by level bracket, to help battlegrounds reach the
critical mass needed to pop. Players are opted in by default and can opt out
with an in-game command.

## How it works

On a configurable interval the module runs a **queue pass**:

1. It gathers every eligible online player and groups them by PvP level bracket
   (10-19, 20-29, …, 70-79). Brackets are never mixed.
2. For each populated bracket it selects one battleground:
   - **Live-battleground reinforcement (priority).** If a battleground of any
     normal type is already forming or in progress for that bracket and has
     free slots, players are queued into it so they reinforce the live match.
     This is not limited to the configured pool.
   - **Otherwise, a random pick from the configured pool.** With a single
     eligible candidate it is used as-is. With several, candidates whose
     minimum players per team cannot be met by the bracket's available count
     are dropped and one of the rest is chosen at random. If none meet the
     threshold, the smallest battleground (typically Warsong Gulch) is chosen
     anyway so players are still queued.
3. Every player in the bracket is solo-queued (no premade groups) into the
   chosen battleground.

`WarningLeadTime` seconds before each pass, a generic warning is broadcast to
the players who would be queued, reminding them how to opt out or back in.

## Behaviour & interactions

- **Per-bracket matching never mixes brackets**; each bracket gets its own
  battleground.
- **Solo queue only** — players are queued individually, never as a premade.
- **Re-queue on decline.** Declining or ignoring the queue popup carries no
  penalty (no Deserter debuff — that only applies after entering and leaving a
  battleground). The player is simply considered again on the next pass.
- **Reload resets the timer** and re-applies `InitialDelay`. A player who logs
  in between the warning and the pass is still queued, just without a warning.
- **Deserter tracking noise.** If `Battleground.TrackDeserters.Enable` is enabled,
  players who decline auto-queue invites may appear in the deserter tracking
  table. This is informational only, not a player-facing penalty.
- **Queue announcer.** If `Battleground.QueueAnnouncer.Enable` is on in
  immediate (non-timed) mode, a mass pass emits one world announcement per
  player. Consider `Battleground.QueueAnnouncer.Timed` /
  `…PlayerOnly` to avoid spam.

A player is **eligible** when they are online and in the world, not opted out,
within the configured level range, not in a dungeon/raid, not already in a
battleground, not already in a battleground/arena queue, not a deserter, not
using the LFG system, not a Death Knight still locked to Ebon Hold, and (when
`SkipGameMasters` is on) not a game master.

## Operational notes

Things to be aware of when running this on a live server:

- **A pass queues everyone in one tick (burst).** All eligible players are
  queued during a single world update, and a `OnPlayerJoinBG` script hook fires
  **once per queued player**. On a high-population server this is a noticeable
  burst: any other module that listens on `OnPlayerJoinBG` (announcers, reward
  systems, statistics) will fire en masse, and the core BG queue announcer in
  immediate mode emits one line per player (see "Queue announcer" above —
  prefer its timed / player-only mode). Spreading the burst across multiple
  ticks is intentionally not implemented; size your `Interval` accordingly.
- **`.reload config` restarts the timer.** Every config reload resets the
  interval and re-applies `InitialDelay`. If you reload more frequently than
  `Interval`, the next automatic pass is pushed back each time and may never
  fire — use `.bgevents run` to trigger one on demand, or avoid reloading
  right before a pass is due.
- **Opt-out is stored per character via the core PlayerSettings system**
  (`source = "mod-bg-auto-queue"`, index 0; `1` = opted out). The core loads it
  on login, saves it on logout, and deletes it when the character is deleted —
  the module keeps no table of its own. **This requires the core config
  `EnablePlayerSettings = 1` for the opt-out to persist across logins.** With it
  `0` (the core default), `.bgevents off` still works but only for the current
  session, and a warning is logged at startup.

## Installation

1. Clone this folder into `modules/mod-bg-auto-queue/` of your AzerothCore source.
2. Re-run CMake and rebuild the worldserver.
3. Copy `mod-bg-auto-queue.conf.dist` to `mod-bg-auto-queue.conf` in your
   worldserver's configuration directory and adjust as needed.
4. For per-character opt-out to persist across logins, set the core config
   `EnablePlayerSettings = 1` (see the dependency note in the conf). The module
   creates no database tables of its own.

## Configuration

All options are documented in `conf/mod-bg-auto-queue.conf.dist`:

- `BgAutoQueue.Enable` — enable the automatic periodic pass (calling `.bgevents run` works even when disabled).
- `BgAutoQueue.Level.Min` / `BgAutoQueue.Level.Max` — eligible level range.
- `BgAutoQueue.Pool` — CSV of `battleground_template` IDs to pick from.
- `BgAutoQueue.Interval` — minutes between passes (`0` disables the schedule).
- `BgAutoQueue.InitialDelay` — seconds before the first pass after startup/reload.
- `BgAutoQueue.WarningLeadTime` — seconds before a pass to broadcast the warning.
- `BgAutoQueue.BroadcastMessage` — the warning text (empty disables it).
- `BgAutoQueue.CrossFaction` — how the available count is judged against a BG's minimum players per team.
- `BgAutoQueue.SkipGameMasters` — skip GMs in the warning and the queueing.

## Commands

- `.bgevents on` — opt the current character back into battleground events.
- `.bgevents off` — opt the current character out (future passes only; does not dequeue an existing queue).
- `.bgevents` — *(no argument)* show the opt-in state and the time until the next scheduled pass.
- `.bgevents run` — *(GM, console-capable)* run a queue pass immediately, even when the automatic schedule is disabled. Does not reset the periodic timer.
