# Online play and Bloodmoon (FractumSeraph/mw3ds)

This fork of [MW3DS](https://github.com/3ds-conspiracy/mw3ds) adds these (and updates itself, and can download its
game data: see [Updating](#updating) and [The game data download](#the-game-data-download)):

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
In first person the werewolf has its own arms and claws (`wolf/skin.1st.nif`, with its hand-to-hand animations), on
its own skeleton: the converter writes it as the second first-person skeleton when the data has Bloodmoon.

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

Offline and online are separate games side by side. **New** and **Load** on the title screen are the offline game,
with its own saves, exactly as without the online mode; nothing touches the network unless the **Online** button is
used (backing out of it leaves the offline game as it was). Online, the server keeps the character, and the game's own
autosave goes to `online_save.json` so it never replaces an offline save. Loading a save from the death screen while
online leaves the online game for the offline one.

A lost connection (no Wi-Fi for a moment, the server restarting) is retried by itself after 5 seconds, then less and
less often, while the game plays on; the Online screen says so and has **Reconnect now**. A platform server signs back
in with its session token, a single world server first resumes the session in place (SessionResume) and otherwise
signs in again with the password, which is kept in memory only while that session lasts.

In the game, the HUD's **Online** button (the Saves button offline) opens three tabs:

- **Players:** who is in this world.
- **Chat:** the conversation, and **Say** to type a line. A line starting with `!` goes to the whole world.
- **Friends:** accept a friend request, **Add friend** by username, **Invite here**, or **Join** a friend whose world
  is open. **Party** opens your own world to your friends.

Chat lines also show as notes on the top screen.

The game's own autosave goes to `sdmc:/3ds/mw3ds/online_save.json` while online, so it never replaces the offline
one. The server keeps the online character.

### What is shared

Every part of openmw-web's protocol (server/PROTOCOL.md) a player's client takes part in, phase by phase:

| What | How |
|---|---|
| The players (M1, M2) | Moves (15 a second), cell changes, looks, equipment, health / magicka / fatigue (with the local gain, `d`), attributes, skills, level and reputation, pack, spells, the Mark, chat (and `!` to the whole world). Character creation done (ChargenComplete), the stored character put back on. Everything picked up is credited for what may be dropped (PlayerItemAcquired). Other players' visible magic (Invisibility, Chameleon) shows on their bodies |
| Death (M2) | Dying online tells the server (PlayerDeath, again every five seconds until it answers); its respawn brings the player back where they fell or at its spot (PlayerResurrect). The death screen waits for that instead of offering a load |
| The input tier (Phase 3, 4) | 30 times a second while the server simulates the world: where the player looks, their swing (the use bit) and their stance, so the server's copy of them fights there. What that copy goes through comes back: its bars (SelfStats), skill uses from blocks and armor hits (SelfSkillUse), diseases and other spells (SelfSpells), magic laid on it (SelfActiveSpells), its items' wear, charge and souls (SelfItemStates). The player's own magic is worn by the copy too (PlayerActiveSpells). Where the server corrects the player's position (0x0103), they are put there |
| The world's people and creatures (M4) | On a server with a sim peer (openmw-web's platform), the peer simulates them and streams them (ActorMoveBatch); the 3DS draws them where it says: standing, walking, running, weapon out, their blows, health, deaths and revivals, the magic that shows on them, disposition and Fight / Flee / Alarm, a door they go through, a scripted line, a finished package, a unique corpse stripped. People the server spawns (summons, leveled creatures, a quest's PlaceAtPC, which the 3DS asks the server for) appear as its own. They have no mind of their own on the 3DS. In a cell nobody holds (a server without a peer), the 3DS runs them itself, as offline |
| Fights (M5) | The player's swings land on the server's copy (above). Their spells on the server's people go to it whole (CombatSpellHit: the spell, or the item it came from, and which effects hit), and on another player in the crosshair to that player (a heal, or harm where the server allows player fights); a cast shows on everyone's screen (CombatCast). Blows and spells sent to the player land with their own armor, reflect and resistances. A witness the server simulates who turns on the player is claimed to fight there. Crimes the server sees come back as bounty, and its guards' arrests open the arrest conversation (PlayerArrest) |
| Objects (M3) | Items taken, doors opened, locks, objects scripts switch on or off or move, script variables of objects (MemberVarUpdate: watched on whoever we talk to and whatever we open), the dead: each cell's state from the server on arrival, and everyone's changes as they happen. Things dropped from the pack lie there for everyone (ObjectSpawnRequest / ObjectPlace), and picking one up is the server's to grant: someone who got there first keeps it |
| Containers (M3) | Chests, corpses and plants hold the same for everyone: the first to open one fixes what is in it, then every take or put is the server's. A merchant's purse is one for everyone too (op "gold") |
| The story (M6) | Journal stages, dialogue topics, globals, faction ranks and reputation, the bounty, the global scripts running (as the server's sharing settings say). Visiting a friend's world, their journal is put on for the visit and ours put back at home |
| Conversations (M6) | One player talks to a person at a time (DialogueLock): someone who is already talking to them keeps them, and the 3DS says who. A conversation that sends someone after the player (AIFollow, AIEscort) or somewhere (AITravel) has the server's copy do it |
| The world (M7) | The server's time and date; a rest or a wait asks the server to move everyone's clock on. Each region's weather is simulated by one player there and shown to the rest. Spells, potions and enchanted items players make get the server's id and travel by it (RecordCreate); the others' are built here (RecordsSync). The places explored outdoors go on everyone's map. A cell the server makes new again is reset here |
| Social (Phase C) | Friends (requests, accept, remove), block, mute, invites, joining a friend's party world, opening ours, sending a guest home (as the owner), who sees us online (PresenceMode, SetAvailability), reporting a player to the server's moderators |

What the server itself does to a player is done too: an operator's give (AdminGive) and teleport (AdminTeleport), a
console line from the world's owner (ConsoleCommand, run as the in-game console runs one), an object a quest still
needs put back (QuestSpawn), an attack it refused (CombatRefused, said on screen), a reset cell's state
(CellSnapshotReplace). The HOME menu tells the server the game is paused (ClientVisibility), so it holds our frames
rather than dropping a link that went quiet.

When the server's copy of one of its people changes clothes or armor (ActorEquip), the 3DS dresses them in it from the
third-person pieces (as a stripped corpse is). The 3DS's own limits: their weapon and torch stay the ones the converter
gave them; a script moving one of the server's people with PositionCell moves our copy only.

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

### Updating

At the title screen, when the console is on Wi-Fi, MW3DS asks GitHub for the latest release of FractumSeraph/mw3ds
(`source/mwmp/selfupdate.cpp`). A newer one puts **Update mp-X.Y.Z** beside **Online** (or press SELECT). Its screen
shows the release notes and three buttons: **Update**, **Not now**, and **Never** (no more checks; delete
`sdmc:/3ds/mw3ds/update_off` to have them back).

Only the program downloads, never anything of Morrowind's. An installed copy takes `mw3ds-program.cia` (the CIA without
data) and installs it through the console's own installer; a copy run from the Homebrew Launcher takes `mw3ds.3dsx` and
swaps it in beside the old one. Each file is checked against the size and SHA-256 GitHub lists for it, and a mismatch
changes nothing. The program-only CIA reads its data from `sdmc:/3ds/mw3ds/data`, so the first update of a CIA that
carries its data inside copies that data out to the SD card first (once; 1.4 GB for the whole island with Bloodmoon,
some minutes; the SD card needs that much free). From then on an update is the program alone. Installed, **Start it**
restarts into the new version. The HOME menu's banner then plays the plain tune (the release can't carry Morrowind's
music).

Data and program go together: the converter writes `data_version.txt` (the format, `kDataVersion` in
`source/components/files/dataversion.hpp`), and a release's `update.json` says which it needs. When a release needs
newer data than the console has, the updater asks the data's server (below) for that format: if it has it, the update
installs and the new program downloads its data when it next starts; if not, the screen says so and installs nothing
(convert again with that release's converter).

*Releasing:* tag the commit `mp-X.Y.Z` and push the tag to the fork. `.github/workflows/release.yml` builds a draft
release with the converter, `mw3ds-program.cia`, `mw3ds.3dsx` and `update.json`; publishing the draft is what consoles
see (GitHub's "latest" skips drafts and pre-releases). Raise `kDataVersion` in the same commit as a converter change the
program needs.

### The game data download

The program-only CIA and the .3dsx carry nothing of Morrowind's. When the console has no data of the program's format
(no data in the CIA, none in `sdmc:/3ds/mw3ds/data`, or older data there), a screen at the start offers to download it
(`source/mwmp/datadownload.cpp`) from `https://morrowind.fractumseraph.net/mw3ds/data-v<format>/index.json`, hosted
beside the browser game's own files. It shows the size and the SD card's free space; **Download** fetches packs of about
32 MB, each checked against its SHA-256 in `index.json` and unpacked into `sdmc:/3ds/mw3ds/data.part` as it streams in.
The packs done are noted there, so a download cut off (Wi-Fi lost, the console shut, **Stop**) carries on from the pack
it was in. With every pack in, `data.part` becomes `sdmc:/3ds/mw3ds/data` (older data there is replaced; saves are in
`sdmc:/3ds/mw3ds/saves`). Without Wi-Fi the screen says so; with older data on the card, **Not now** plays on with it.
Over a 3DS's Wi-Fi the whole island takes some time (the emulator managed about 0.5 MB/s): keep the console open and
charging.

The hosted data is the New 3DS's (textures up to 256 texels); an Old 3DS is told to convert its own on a PC.
`sdmc:/3ds/mw3ds/data_url.txt` points the screen at another `index.json` (a test server's); `data_answer.txt` ("yes"
or "no") answers it for a scripted run.

*Hosting it:* `python tools/release/make_datapacks.py out/data-bloodmoon build/datapacks/data-v1` writes `index.json`
and the packs (`NNN-<sha256>.mwp`; the pack format is in the script). Copy the folder to the server so that
`/mw3ds/data-v1/index.json` is served beside them: the packs may be cached for good (their names change with their
content), `index.json` never. A new data format is a new folder (`data-v2`), so consoles on the older program keep
theirs. The data is Morrowind's, converted: host it only where you may.

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
