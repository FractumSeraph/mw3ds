# Online play (FractumSeraph/mw3ds, FractumMorrowind)

This fork of [MW3DS](https://github.com/3ds-conspiracy/mw3ds) adds an online mode: the 3DS plays as a player on an
[openmw-web](https://github.com/FractumSeraph/openmw-web) multiplayer server, beside people playing in the browser. It
also updates itself and downloads its game data: see [Updating](#updating) and
[The game data download](#the-game-data-download).

## Bloodmoon

Bloodmoon is MW3DS's own now ([spec/bloodmoon.md](../spec/bloodmoon.md); the fork's earlier version gave way to it in
the merge of 2026-11-01). openmw-web servers play the Game of the Year load order, so convert with both add-ons:

```sh
python tools/build/build_cia.py --tribunal --bloodmoon
```

That writes the data to `out/data-tribunal-bloodmoon` (`--old3ds` too: `out/o3ds-tribunal-bloodmoon`). Another
online player in the werewolf's form is drawn with the werewolf's own model, as the player is.

## The online mode

### Playing

1. On the title screen, tap **Online** (or press Y).
2. Check the server address. `https://morrowind.fractumseraph.net` is the default. Enter the account name and sign in.
   The password is asked each time and is never written to the SD card.

   No account yet? **New account** (or Y) makes one there: an account name, the server's invite code (filled in for
   morrowind.fractumseraph.net: `fractumseraph.net`) and a password of 12 or more characters, asked twice. It is
   openmw-web's password sign-up (`POST /auth/password/register`), which the server's owner turns on
   (`[login].allowPasswordSignup`, with registration open); without it the 3DS says so, and the account is made on
   the server's website instead. The same account signs in there.
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

When the server's copy of one of its people changes clothes, armor or weapon (ActorEquip), the 3DS dresses and arms
them in it from the third-person pieces (as a stripped corpse is dressed); their torch stays the converter's. A script
here moving one of the server's people (PositionCell, Position) moves our copy at once and asks the cell's holder to
move theirs (the position claim, at most 5 a minute).

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

At the title screen, when the console is on Wi-Fi, FractumMorrowind asks GitHub for the latest release of FractumSeraph/mw3ds
(`source/mwmp/selfupdate.cpp`). A newer one puts **Update mp-X.Y.Z** beside **Online** (or press SELECT). Its screen
shows the release notes and three buttons: **Update**, **Not now**, and **Never** (no more checks; delete
`sdmc:/3ds/mw3ds/update_off` to have them back).

Only the program downloads, never anything of Morrowind's. An installed copy takes `FractumMorrowind.cia` (the CIA
without data) and installs it through the console's own installer; a copy run from the Homebrew Launcher takes `FractumMorrowind.3dsx` and
swaps it in beside the old one. Each file is checked against the size and SHA-256 GitHub lists for it, and a mismatch
changes nothing. The program-only CIA reads its data from `sdmc:/3ds/mw3ds/data`, so the first update of a CIA that
carries its data inside copies that data out to the SD card first (once; 1.4 GB for the whole island with Bloodmoon,
some minutes; the SD card needs that much free). From then on an update is the program alone. Installed, **Start it**
restarts into the new version.

Data and program go together: the converter writes `data_version.txt` (the format, `kDataVersion` in
`source/components/files/dataversion.hpp`), and a release's `update.json` says which it needs. When a release needs
newer data than the console has, the updater asks the data's server (below) for that format: if it has it, the update
installs and the new program downloads its data when it next starts; if not, the screen says so and installs nothing
(convert again with that release's converter).

*Releasing:* tag the commit `mp-X.Y.Z` and push the tag to the fork. `.github/workflows/release.yml` builds a draft
release with the converter, `FractumMorrowind.cia`, `FractumMorrowind.3dsx` and `update.json`; publishing the draft is what consoles
see (GitHub's "latest" skips drafts and pre-releases). Raise `kDataVersion` in the same commit as a converter change the
program needs.

### The game data download

The program-only CIA and the .3dsx carry nothing of Morrowind's. When the console has no data of the program's format
for its console (no data in the CIA, none in `sdmc:/3ds/mw3ds/data`, older data there, or on an Old 3DS the New 3DS's
data), a screen at the start offers to download it (`source/mwmp/datadownload.cpp`) from the seedbox that hosts the
Halo maps (GitHub's space stays free of it): `https://halomaps.fractumseraph.net/mw3ds/data-v<format>/index.json` on a
New 3DS, `data-v<format>-o3ds/` on an Old 3DS (its textures quarter-size, up to 128 texels). It shows the size and the
SD card's free space; **Download** fetches packs of about 32 MB, each checked against its SHA-256 in `index.json` and
unpacked into `sdmc:/3ds/mw3ds/data.part` as it streams in. The packs done are noted there, so a download cut off
(Wi-Fi lost, the console shut, **Stop**) carries on from the pack it was in. A pack is tried three times; after that
the screen says the download stopped and tries again by itself every 15 s, 20 times (or at **Try again**), carrying on
where it stopped without asking again (never starting one the player didn't). With every pack in, `data.part` becomes
`sdmc:/3ds/mw3ds/data` (older data there is replaced; saves are in `sdmc:/3ds/mw3ds/saves`). Without Wi-Fi the screen
says so; with older data on the card, **Not now** plays on with it. Over a 3DS's Wi-Fi the whole island takes some time
(the emulator managed about 0.5 MB/s): keep the console open and charging.

**From a PC instead:** each set is on the seedbox as one zip too, `FractumMorrowind-data-v<format>.zip` (New 3DS) and
`FractumMorrowind-data-v<format>-o3ds.zip` (Old 3DS) in `https://halomaps.fractumseraph.net/mw3ds/`, each with its
`.sha256` beside it. Stored, holding `3ds/mw3ds/data`: unzipped to the SD card's root with a card reader, it is found at
the start (`data_version.txt` says its format) and nothing downloads. Format 1's (`data-v1`, its zip) stays for
consoles still on a program of that format.

`sdmc:/3ds/mw3ds/data_url.txt` points the screen at another `index.json` (a test server's; its `data-v<n>[-o3ds]` is
made this program's format and console's); `data_answer.txt` ("yes" or "no") answers it for a scripted run.

*Hosting it:* convert both sets (`build_cia.py --tribunal --bloodmoon --mods`, then with `--old3ds` too), then

```sh
python tools/release/make_datapacks.py out/data-tribunal-bloodmoon build/datapacks/data-v2 --zip build/datapacks/FractumMorrowind-data-v2.zip
python tools/release/make_datapacks.py out/o3ds-tribunal-bloodmoon build/datapacks/data-v2-o3ds --zip build/datapacks/FractumMorrowind-data-v2-o3ds.zip
```

write each set's `index.json` and packs (`NNN-<sha256>.mwp`; the pack format is in the script) and its zip. Copy each
folder to the server so that `/mw3ds/data-v2/index.json` is served beside its packs (on the seedbox:
`~/www/halomaps.fractumseraph.net/public_html/mw3ds/`, the packs first and `index.json` last, so no console sees an index
whose packs aren't there yet), and the zips with their `.sha256` in `/mw3ds/`. The packs may be cached for good (their
names change with their content), `index.json` never. A new data format is a new folder, so consoles on the older
program keep theirs.

### Building

Without MSYS2, everything builds in a Linux image (`tools/build/docker`):

```sh
sh tools/build/docker/run.sh python3 tools/build/make.py
```

```sh
sh tools/build/docker/run.sh python3 tools/build/build_cia.py --tribunal --bloodmoon --mods
```

The first builds `mw3ds.3dsx`. The second converts the data with both add-ons and the optimization mods (about 80
minutes the first time; `--old3ds` for the Old 3DS's set) and packs it with the program into
`build/mw3ds-tribunal-bloodmoon.cia` (the image has makerom, built from Project_CTR's tagged source; `--skip-convert`
packs data already converted). Set `MW3DS_MORROWIND_DIR` in `.env`.

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
- `invite`: the account is made first (New account's sign-up) with this invite code; a gateway's address only.

A server with no sim peer runs every cell's people on the clients. To try the world's stream without one, the local
test server's `mp-simpeer.ts` (in FractumSeraph's test copy of the server) logs in as the peer with the server's
`[server] password` and streams a few of Seyda Neen's people round the first player it sees.

Use only a local test server's test accounts here. The PC test build (`tools/test/native`) talks `ws://` and `http://`;
`MW3DS_ONLINE_REALTIME=1` holds it to real time so a scripted run meets other players.
