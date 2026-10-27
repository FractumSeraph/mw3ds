# Online play and Bloodmoon (FractumSeraph/mw3ds)

This fork of [MW3DS](https://github.com/3ds-conspiracy/mw3ds) adds two things:

- **Bloodmoon.** The converter takes the add-on and the engine runs the script functions its quests need.
- **An online mode.** The 3DS plays as a player on an [openmw-web](https://github.com/FractumSeraph/openmw-web)
  multiplayer server, beside people playing in the browser.

## Bloodmoon

Convert with Tribunal and Bloodmoon, in the Game of the Year load order openmw-web servers use:

```sh
python tools/build/build_cia.py --bloodmoon
```

That writes the data to `out/data-bloodmoon` and the CIA to `build/mw3ds-bloodmoon.cia`. `--bloodmoon` implies
`--tribunal`. The converter's own option is `tools/convert/level.py --bloodmoon` (or `MW3DS_BLOODMOON=1`).

What the engine runs for Bloodmoon (`source/mwmechanics/werewolf.cpp` and the script extensions):

| Script functions | What they do |
|---|---|
| BecomeWerewolf, UndoWerewolf, IsWerewolf | The player changes form. The beast's attributes and skills come from the `fWerewolf*` settings and its health from `fWereWolfHealth`. The werewolf robe goes on and the rest comes off. Inventory, magic and casting are refused. Witnesses make the player a known werewolf, and add a bounty when they report it. |
| GetWerewolfKills, SetWerewolfAcrobatics | Kills in the beast's form; its Acrobatics once Bloodmoon grants it |
| Get / Set / Mod + 24 magic effects | ModResistFire, ModResistParalysis and the rest: a lasting modifier under the effect, saved |
| GetPCInJail, GetPCTraveling | True just after jail or a guide's travel |
| GetWeaponType | The weapon in hand's type, -1 none, -2 a lockpick, -3 a probe |
| GetScale, SetScale, ModScale | An actor drawn at the new size (0.5 to 2) |
| TurnMoonRed, TurnMoonWhite | Secunda in Morrowind.ini's `[Moons] Script Color` |

The werewolf is drawn as Bloodmoon draws its werewolf NPCs: the beast-legged skeleton under the werewolf robe, with
the werewolf head and hair. That covers the player in third person and other online players who are werewolves.
**Not yet:** the beast's first-person claws (the first-person view shows bare hands).

## The online mode

### Playing

1. Make an account on the server's website (for FractumSeraph's server, morrowind.fractumseraph.net). The 3DS signs
   in with the account's name and password, so the account needs a password.
2. On the title screen, tap **Online** (or press Y).
3. Check the server address. `https://morrowind.fractumseraph.net` is the default. Enter the account name and sign in.
   The password is asked each time and is never written to the SD card.
4. If the server asks, choose a username (and give an email).
5. Choose a character, or **New** to make one. A new one goes through Morrowind's own character creation. A
   returning one is loaded as the server keeps it: looks, stats, pack, equipment, spells, quests, topics, factions,
   bounty and where they stood.

In the game, the HUD's **Online** button (the Saves button offline) opens three tabs:

- **Players:** who is in this world.
- **Chat:** the conversation, and **Say** to type a line. A line starting with `!` goes to the whole world.
- **Friends:** accept a friend request, **Add friend** by username, **Invite here**, or **Join** a friend whose world
  is open. **Party** opens your own world to your friends.

Chat lines also show as notes on the top screen.

The game's own autosave goes to `sdmc:/3ds/mw3ds/online_save.json` while online, so it never replaces the offline
one. The server keeps the online character.

### What is shared, and what is not yet

| What | How |
|---|---|
| The players | Moves (15 a second), cell changes, looks, equipment, health / magicka / fatigue, attributes, skills, level, pack, spells, chat, friends, invites, party worlds. Character creation done (ChargenComplete), the stored character put back on |
| The world's people and creatures | On a server with a sim peer (openmw-web's platform), the peer simulates them and streams them; the 3DS draws them where it says: standing, walking, running, weapon out, their blows, their health, deaths and revivals. They have no mind of their own on the 3DS. In a cell nobody holds (a server without a peer), the 3DS runs them itself, as offline |
| Fights | The player's swings go to the server's copy of the player (the input tier's use bit), so blows land there, armor and all, and the server's copy takes the world's blows: its health, magicka and fatigue come back (SelfStats). The player's spells on the server's people go to it whole (CombatSpellHit: the spell, or the item it came from, and the effects that hit), and their owner applies them, reflect and resistances included; the server only takes spells the character knows. Crimes the server sees come back as bounty |
| Objects | Items taken, doors opened, locks, objects scripts switch on or off, the dead: each cell's state from the server on arrival, and everyone's changes as they happen. Things dropped from the pack lie there for everyone (ObjectSpawnRequest / ObjectPlace), and picking one up is the server's to grant: someone who got there first keeps it |
| Containers | Chests, corpses and plants hold the same for everyone: the first to open one fixes what is in it, then every take or put is the server's |
| The story | Journal stages, dialogue topics, globals, faction ranks and reputation, the bounty (as the server's sharing settings say) |
| The clock | The server's time and date; a rest or a wait asks the server to move everyone's clock on |

**Not yet:** merchants' purses, a conversation's lock on an NPC
(DialogueLock), followers and escorts, the weather, and the werewolf's first-person claws.

Each player sees the others walk and run where they are, dressed as they are.

### How it talks to the server

The protocol is openmw-web's `server/PROTOCOL.md` (omw-mp.2, proto 3). The client is in `source/mwmp`:

| File | What it is |
|---|---|
| `netsocket.cpp` | TCP without blocking. On the console, TLS by devkitPro's Mbed TLS with Let's Encrypt's roots (`tlsroots.hpp`); on the PC test build, Winsock without TLS |
| `websocket.cpp` | The WebSocket and HTTP/1.1 clients |
| `lser.cpp` | OpenMW's Lua serialization, which every event body is |
| `online.cpp` | Signing in, the session, the binary frames, the events |
| `mpgame.cpp` | The game side: the record put on, the state sent, the other players and the world's people drawn, objects, containers, the story and the clock, the Online screen |

A platform server (its gateway, an `https://` address) is reached the way openmw-web's launcher reaches it:

1. `POST /auth/password` returns a login ticket and a session token.
2. `GET /auth/profile`, then `POST /auth/profile` if the server needs a username.
3. `GET /auth/characters`, or `POST /auth/characters` for a new one.
4. `POST /worlds` with the character opens its private world.
5. The client dials `wss://host/w/<world>` and joins with the ticket (`SessionLoginTicket`).

A single world server (a `ws://` or `wss://` address) takes the password in `SessionLoginRequest` instead.

The 3DS says it does not simulate actors, so the server's simulation keeps the NPCs. The world's objects and people are
addressed by their reference numbers (OpenMW's RefNum: the content file and the index), which the converter keeps in
each cell's file (`"rn"`). Data converted before that gets them with `python tools/convert/refnums.py` (the same
`MW3DS_OUT`, `MW3DS_TRIBUNAL` and `MW3DS_BLOODMOON` as its conversion), in a minute, without converting again. Its content list is
`builtin.omwscripts`, the data's masters (`morrowind.esm`, `tribunal.esm`, `bloodmoon.esm`) and `mp.omwscripts`, as a
browser's is. A server that runs other files says which; the client retries with that list only when every file in it
is one this data has.

### Building

Without MSYS2, everything builds in a Linux image (`tools/build/docker`):

```sh
sh tools/build/docker/run.sh python3 tools/build/make.py
```

```sh
sh tools/build/docker/run.sh python3 tools/convert/level.py --tribunal --bloodmoon --world
```

```sh
sh tools/build/docker/run.sh python3 tools/build/build_cia.py --bloodmoon --skip-convert
```

The first builds `mw3ds.3dsx`. The second converts the data. The third packs both into `build/mw3ds-bloodmoon.cia`
(the image has makerom, built from Project_CTR's tagged source). Set `MW3DS_MORROWIND_DIR` in `.env`, and
`MW3DS_DOCKER_FLAGS="-e MW3DS_OUT=out/data-bloodmoon"` to choose where the data goes.

### Testing

Test against a local openmw-mp server. Its `dist/testhost.mjs` runs a world without the simulation peer, and its
gateway runs those worlds with `--server-entry dist/testhost.mjs`.

`sdmc:/3ds/mw3ds/online_test.json` signs a scripted run in without the screens:

```json
{"server": "ws://127.0.0.1:18081", "account": "tester3ds", "password": "...", "character": "first",
 "say": "hello", "join": "Tester2", "probe": "https://morrowind.fractumseraph.net/auth/providers"}
```

- `character`: `first`, `new` or a character id.
- `say`: a line said once in the world.
- `join`: a friend's username, whose request is accepted and whose world is joined.
- `probe`: a public address fetched first, to check the console's TLS without signing in anywhere.

A server with no sim peer runs every cell's people on the clients. To try the world's stream without one, the local
test server's `mp-simpeer.ts` (in FractumSeraph's test copy of the server) logs in as the peer with the server's
`[server] password` and streams a few of Seyda Neen's people round the first player it sees.

Use only a local test server's test accounts here. The PC test build (`tools/test/native`) talks `ws://` and `http://`;
`MW3DS_ONLINE_REALTIME=1` holds it to real time so a scripted run meets other players.
