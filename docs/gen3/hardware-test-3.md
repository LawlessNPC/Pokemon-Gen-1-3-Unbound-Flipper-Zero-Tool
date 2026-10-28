# Gen III hardware test 3: cartridge save access

Goal: the Flipper sends its helper program to a GBA over the link cable (multiboot), reads the cartridge, and backs up its save to the SD card. Later sections cover browsing and editing the save's Pokemon, writing it back, restoring a backup and copying the ROM. A save is never written without a verified backup first, and a write that is cut short (cable pulled, GBA switched off) never leaves the game without a save it loads.

You need: Flipper with the KBEmbedded Game Link GPIO module, GBA (or GBA SP), GBA link cable, a genuine Pokemon Ruby cartridge and a genuine Pokemon FireRed cartridge.

**Build and install.** Run `ufbt` (release build). It builds two apps: `dist/pokemon.fap` (Pokemon Trade Tool) and `dist/pokemon_cart.fap` (Pokemon Cartridge). Copy both into the same folder on the SD card, for example `apps/GPIO/` with qFlipper. Or run `ufbt launch APPID=pokemon_cart`, then `ufbt launch APPID=pokemon`: each installs its app in `apps/GPIO/` and starts it.

## Notes

- **Two apps.** Cartridge is its own app, Pokemon Cartridge, because the Flipper loads an app whole into RAM and the cartridge code doesn't fit beside the trade code. Gen III → `Cartridge` in Pokemon Trade Tool closes that app and opens Pokemon Cartridge. When you leave Pokemon Cartridge, the Flipper returns to its app list. Pokemon Cartridge can also be started from the app list directly.
- **Save the Pokemon you are editing first.** Opening Cartridge closes Pokemon Trade Tool, and with it the Pokemon in its editor. Use `Save .pk3` first if you want to keep it.
- **Assets.** After the first Cartridge start, `apps_assets/pokemon_cart/gen3_helper.mb` and `apps_assets/pokemon_cart/gen3_bulk.bin` exist on the SD card. The Flipper unpacks them from `pokemon_cart.fap`.
- **Pins.** Set the pins in Pokemon Trade Tool's Pinout screen. Cartridge copies them at every start; it has no Pinout screen of its own.
- **The log** is `apps_data/pokemon/gen3_cart.log`. It is created again on every Cartridge start (Start on the instructions), so after a failed run copy it off the SD card **before you open Cartridge again**. Keep the log from every failed run.
- **Backups** are in `apps_data/pokemon/gen3/backups/`, named `<game code>_<TID>_<date>-<time>.sav`, for example `AXVE_12345_20261027-093015.sav`. Each is the whole 128 KB flash chip, as emulators and PKHeX load it. Cartridge never deletes one.
- **The instructions screen** (`Cartridge`) reads: `GBA off, cart in.` / `Small plug: Flipper.` / `Hold START+SELECT` / `and power on.`, with `Back` and `Start`.
- **The progress screen** shows what is happening on top, two lines under it, a bar, and at the bottom `Back: stop` (Back stops the step), `Do not power off` (while the save is being written), or nothing.
- **The log's `mb` lines** record each helper upload: `restarts` (failures counted) and `all` (every restart, counted or not), `cc` and `rr` (the GBA's two handshake bytes), `palette_busy` (how often the GBA answered "busy" to the palette word before it went on, at about 1 ms each, against a 4 s limit) and `final_us` (microseconds from the Flipper's first `0x0065` to the GBA's `0x0075`, against a 5 s limit). A.3 asks for the last two.
- On any failure, report the step, what the Flipper and the GBA showed, and the log file.

## A. Connect (Ruby)

1. **Open Cartridge.** In Pokemon Trade Tool, Gen III → `Cartridge`. Pokemon Trade Tool closes, and after about 1 s Pokemon Cartridge opens with the instructions. While a trade runs, `Cartridge` isn't in the Gen III menu (`End trade` shows instead); end the trade first. If `pokemon_cart.fap` isn't next to `pokemon.fap`, an `App missing` dialog says where to copy it, and Pokemon Trade Tool stays open.
2. **Waiting.** With the GBA off and the Ruby cartridge in it, put the cable's small plug in the Flipper and the other in the GBA. Press `Start`. The Flipper shows `Waiting for GBA`. After 3 s its lines show `Hold START+SELECT` / `and power on the GBA`.
3. **Connect.** Hold START+SELECT on the GBA and switch it on; keep holding until the Nintendo logo is gone. The Flipper shows `Sending helper` with a bar (about 3–5 s), and the GBA screen turns green. Then the Flipper shows `Reading cart`, then `Backing up save` with `about 70 s` and a bar, then `Checking backup` and `Reading save`, and then the hub, headed `POKEMON RUBY`, with `Cart info`.
4. **Without the keys.** Back on the hub closes Cartridge. Switch the GBA off, open Cartridge again, press `Start`, and switch the GBA on without holding START+SELECT. The game starts, and the Flipper keeps waiting (`Waiting for GBA`, with the power-on lines). Switch the GBA off, then on again holding START+SELECT: it connects as in step 3.
5. **Wrong plug.** On the instructions, press `Start` with the cable's other plug in the Flipper (or no cable): the Flipper shows `Cable: plug in/swap ends` under `Waiting for GBA`. Swap the plugs: it carries on waiting for the GBA.
6. **Back while sending.** Start a Connect, and press Back while `Sending helper` shows. The Flipper shows `Stopping`, then the instructions. Back again closes Cartridge, and the Flipper returns to its app list. Switch the GBA off before the next Connect.
7. **Data missing.** On a computer, replace `apps_assets/pokemon_cart/gen3_bulk.bin` with an empty file, and open Cartridge: `Data missing` shows (`Gen III data missing.` / `Delete apps_assets/` / `pokemon_cart on the` / `SD card, then reopen.`), and OK closes the app. Then delete the folder `apps_assets/pokemon_cart` and open Cartridge again: the instructions show and a Connect works, because the Flipper unpacked the assets again.

## B. Cart info and backup

1. **Cart info.** Connect as in A.3, then choose `Cart info`. Its header is `AXVE v1` (or `v0`: your cart's version), and its lines are:
   - `Flash 09C2 Macronix` or `Flash 1362 Sanyo`;
   - `Save: A #<n>` or `Save: B #<n>`: the half of the save chip the game loads, and its save counter;
   - your trainer name and TID, for example `BRENDAN 12345`; compare the in-game trainer card;
   - `Writing on`;
   - `Backup <MMDD-HHMMSS>`: the backup's date and time.

   Back returns to the hub.
2. **The backup file.** In `apps_data/pokemon/gen3/backups/` there is `AXVE_<tid>_<date>-<time>.sav`, 131,072 bytes, whose date and time match Cart info's last line.
3. **Optional:** it loads in mGBA (as the save of a Ruby ROM) or in PKHeX and shows your party.

**The hub's items** from here on, each only when it applies: `Party and boxes`; `Write changes (<n>)` (n Pokemon changed and not yet written) or, when the cart can't be written, `Writing off: <why>`, which opens Cart info; `Discard changes`; `Reconnect`; `Back up again`; `Restore backup` (a cart that can be written, with a current backup, even if its save is empty or damaged); `Copy ROM`; `Cart info`. `Restore backup` and `Copy ROM` hide while `Reconnect` shows.

## C. Browse (Ruby)

1. **Party.** Choose `Party and boxes`. The first row is `Party (<n>/6)`, n being your party's size. Choose it: one row per Pokemon, `01 <SPECIES> Lv<level>` and so on, with the levels the game shows, and `-- empty --` after the last one when the party has fewer than six.
2. **Boxes.** Back. The next 14 rows are your box names as the game shows them (`BOX 1`, ... or the names you gave them).
3. **A box.** Choose a box holding Pokemon. It lists its 30 slots, each row starting with the slot's number: `01 <SPECIES> Lv<level>`, `05 EGG` for an egg, and `07 -- empty --` for an empty slot. The header is the box name.
4. **Timing.** Time how long the first box screen takes to open after you choose it, and write it down below. It reads about 60 small pieces of the SD card.

## D. Edit and write (Ruby)

1. **Open.** Open party slot 1. The editor shows its species, level, nickname and OT as the game does.
2. **Edit.** Set the level to 50, then Back. The row shows `*` after it, and the hub shows `Write changes (1)` and `Discard changes`.
3. **Write.** Choose `Write changes (1)`. `Write changes?` asks `1 change. The save` / `is backed up. Keep` / `the GBA on.`; choose `Write`. The Flipper shows `Writing` with `Do not power off` (about 10 s), then `Verifying` and `Saving backup`, then `Written`: `Written and verified.` / `Power-cycle the GBA` / `to play.`
4. **Play.** Switch the GBA off and on without the keys, and choose Continue. There is no "save corrupt" message, and party slot 1 is Lv 50. Save in the game, switch off and on, and load again: still fine.
5. **Reconnect.** Back on the hub closes Cartridge (nothing is waiting to be written). Open Cartridge again, then connect as in A.3. The hub shows the save as it is now, and the connect made a new `.sav` in `backups/`.
6. **Link drop.** Edit any Pokemon, choose `Write changes`, then `Write`, and pull the cable while `Writing` shows. The Flipper shows `Write failed` with `Link lost. ...` and `Retry`. Plug the cable back in and press `Retry`: the Flipper reconnects (`Reconnected`). Choose `Write changes` again: `Written and verified.` The game loads the edit, with no corrupt-save message. If the GBA lost power instead, the game may say `The save file is corrupt. The previous save file will be loaded.` and loads the old save. That is expected; Retry the write.
7. **Mail.** Give a party Pokemon a Mail in the game and save, then connect again. Open that Pokemon: `Can't edit` / `Take the Mail off` / `in the game first.` shows, OK returns to the list, and nothing is marked `*`.
8. **Unchanged.** Open any Pokemon and Back without changing anything: its row has no `*`, and the hub has no `Write changes`.

## E. Put Pokemon (Ruby)

**First,** since closing Cartridge or saving in the game drops the edits not yet written: in the game, leave fewer than six Pokemon in the party (put one in a PC box) and save. Close Cartridge (Back on the hub), and in Pokemon Trade Tool make a Pokemon in the Gen III editor and `Save .pk3`; it goes to `apps_data/pokemon/gen3/`. Then open Cartridge again and connect as in A.3.

1. **New.** In box 2's first empty slot, choose `New Pokemon`, set the species to TREECKO and the level to 10, then Back. `Put TREECKO Lv10` asks `in <box name>` / `slot <k>?`; choose `Put`. The slot shows `TREECKO Lv10*`.
2. **Load .pk3.** In another empty box slot choose `Load .pk3` and pick the file saved before E.1. If the load fixed something in it, `Loaded` says what first; OK. The editor shows it; Back, then `Put`.
3. **Party.** In the party's `-- empty --` row (the party has fewer than six), choose `New Pokemon`, then Back and `Put`. The party is one longer.
4. **Write.** Write the changes as in D.3. In the game the boxes show the new Pokemon, and the party has the new member. Those made with `New Pokemon` have your name and ID as OT and obey you in battle; the `.pk3` one keeps the OT it had.
5. **Discard.** Change a Pokemon, then choose `Discard changes`: `Discard changes?` / `1 change.`; choose `Discard`. `Discarded` shows, the `*` is gone, and the hub has no `Write changes`.
6. **Cancel and Back.** In an empty slot choose `New Pokemon`, then Back: on `Put ...?` press Back to return to the editor, Back again, then `Cancel`. The slot stays `-- empty --`.

## F. Restore (Ruby)

The backup to restore is the `.sav` that A.3's connect made, from before D and E: the oldest `AXVE_...` file in `backups/`.

1. **Pick.** Choose `Restore backup`. The file list opens in `backups/`; pick that `.sav`. The Flipper shows `Checking file` with a bar, then `Restore backup?`: `Cart: <name> <TID>` / `File: <name> <TID>` / `Hall of Fame is kept.`, both trainers. Back on the file list, or `Cancel` on the question, returns to the hub.
2. **Restore.** Choose `Restore backup` and pick the file again, then `Restore` (about 40 s). The Flipper shows `Writing` with `Do not power off`, then `Verifying` and `Saving backup`; Back does nothing meanwhile. Then `Restored`: `Restored and` / `verified. Power-cycle` / `the GBA to play.`
3. **Play.** Switch the GBA off and on without the keys, and choose Continue. The game loads the save as it was at A: the party levels from before D, and none of E's Pokemon.
4. **A save that was corrupt.** Restoring onto a cart whose save was corrupt: until you save once in the game, it may say `The save file is corrupt…` at each start. Save in the game once.
5. **An edit not written.** Connect again, change a Pokemon, then choose `Restore backup` and pick any `.sav`: the question has a fourth line, `1 change is lost.` Choose `Cancel`: the hub still shows `Write changes (1)`. Discard the change.

## G. Copy ROM (Ruby)

1. **Ask.** Choose `Copy ROM`: `Copy ROM?` / `16 MB, about` / `2 h 30 min.` / `Keep both powered.`, with `Cancel` and `Copy`.
2. **Copy, then stop.** Choose `Copy`. The Flipper shows `Copying ROM`, the percent under it, the bar, and after about 10 s `<h> h <m> min left`; every two or three minutes `Checking ROM` shows for a moment. After 1 min, press Back: `Stop copying?` / `You can resume` / `later.`, with `Keep going` and `Stop`. `Keep going` returns to the copy, which went on meanwhile. Press Back again and choose `Stop`: `Stopping`, then `ROM copy stopped`: `Copy ROM again` / `to resume.`
3. **Resume.** Choose `Copy ROM` again: `Resume copy?` / `<pct>% done`, with `Restart` and `Resume`. Choose `Resume`: the percent goes on from there, not from 0. (`Restart` would copy from 0 %.)
4. **Optional: finish** (about 2.5 h). `ROM copied`: `Verified.` / `roms/AXVE_v<n>.gba`. On the SD card, `apps_data/pokemon/gen3/roms/AXVE_v<n>.gba` is 16,777,216 bytes, and on a computer its `shasum` matches your revision's SHA-1 in `docs/gen3/multiboot-and-save.md` §3.6.

## Results

| Step | Pass / fail | What each screen showed (Flipper, GBA) |
|---|---|---|
| A.1 Cartridge opens | | |
| A.2 `Waiting for GBA`, power-on lines after 3 s | | |
| A.3 Connect to the hub (`Sending helper` took ___ s, `Backing up save` ___ s; the log's `mb done` line: `palette_busy` ___, `final_us` ___) | | |
| A.4 Without the keys, then with them | | |
| A.5 `Cable: plug in/swap ends` | | |
| A.6 Back while sending | | |
| A.7 Data missing, then unpacked again | | |
| B.1 Cart info (flash ____, save ____, trainer ____) | | |
| B.2 Backup file, 131,072 bytes | | |
| B.3 Backup in mGBA or PKHeX (optional) | | |
| C.1 Party with the right levels | | |
| C.2 Box names | | |
| C.3 A box's slots, `-- empty --` | | |
| C.4 First box screen opened in ___ s | | |
| D.1 Party slot 1 in the editor | | |
| D.2 Level 50: `*`, `Write changes (1)` | | |
| D.3 `Written and verified` (`Writing` took ___ s) | | |
| D.4 The game loads Lv 50, no corrupt-save message | | |
| D.5 Reconnect: new backup | | |
| D.6 Link drop, Retry, written | | |
| D.7 Mail: `Can't edit` | | |
| D.8 Unchanged: no `*` | | |
| E.1 New TREECKO Lv10 put | | |
| E.2 `.pk3` loaded and put | | |
| E.3 New party member put | | |
| E.4 Written: in the game, OT yours, obeys | | |
| E.5 Discard | | |
| E.6 Put: Back to the editor, Cancel | | |
| F.1 `Restore backup?` shows both trainers | | |
| F.2 `Restored and verified` (took ___ s) | | |
| F.3 The game loads the save from A | | |
| F.4 After a corrupt save: one save in the game | | |
| F.5 `1 change is lost.`; Cancel keeps it | | |
| G.1 `16 MB, about 2 h 30 min` | | |
| G.2 Time left shown; Keep going; Stop: `ROM copy stopped` | | |
| G.3 `Resume copy?` (___ % done); the percent goes on | | |
| G.4 ROM copied, size and SHA-1 match (optional) | | |
