# Gen III hardware test: Pokemon Unbound on an EZ-Flash Omega

Goal: Pokemon Cartridge connects to an EZ-Flash Omega Definitive Edition over the link cable, finds the game the Omega started last and its save on the Omega's microSD card, checks that the game file is Pokemon Unbound 2.1.1.1, backs the save up to the Flipper, lists the party and the 24 boxes, edits Pokemon, and writes the changes back to the save on the Omega's card (a verified write into the save slot the game doesn't load). A restore puts a backup back the same way.

**Run order.** Do H first: it connects, makes one small edit and writes it. **If H fails, stop the test** and report it (the last `omega program` line in the log says how far the write got; see "If it fails"). Then A to G (part 1: they still apply; A needs a save pending, so do it whenever you have played), then I, J, K, L, R, P and M.

**Before you start.** Back up the Omega's microSD card (copy `SAVER/` to a computer) if you can. The write only touches the open save file's own blocks, but the backup costs a minute.

You need: Flipper with the KBEmbedded Game Link GPIO module, GBA (or GBA SP), a Game Boy Color link cable (or a GBA link cable if the module's SD line is connected), an EZ-Flash Omega Definitive Edition with Pokemon Unbound on its microSD card, and a genuine Ruby cartridge for G.

**Build and install.** `ufbt build APPID=pokemon_cart`, then `ufbt launch APPID=pokemon_cart`.

## Notes

- **The DE's save.** While you play, the Omega Definitive Edition keeps the save in the cart, and copies it to its microSD card the next time you switch on normally (a saving screen, or `Save sav?` if its auto-save setting is off). A START+SELECT power-on skips that, so after playing, **switch on normally once and let the Omega save, then switch off**, and only then connect. Cartridge checks this itself (A).
- **Which game.** Cartridge reads the game the Omega started last, from `SAVER/Recently play.txt`, and its save, `SAVER/<game name>.sav`.
- **The log** is `apps_data/pokemon/gen3_cart.log`. Its Omega lines, each only for what that Connect got to: `omega flag <word>` (`0011` while the DE has a save to copy; write down what it shows in B), `omega mount ...`, `omega recent '<game file>'`, `omega save '<save file>' size 131072`, `omega rom '<game file>' size <n> clus <n>`, `omega sha <hash> cached <0|1> match <0|1>`, `backup <name>`, `ub slots ...`, `ub trainer <name> <TID>`, and (P) `omega pics ok front <hex> pal <hex>` or `omega pics off <reason>`, and `job picture <species number> end <result>`. Close Cartridge before copying it off (the USB CLI can't read it while Cartridge runs).
- **Write log lines.** `omega write changes <n> dirty <mask>` when you choose `Write`, then `omega program 1/15` ... `omega program 15/15` (each once its sector is written: the write test, then the 14 sectors of the slot the game doesn't load; the total is 16 or 17 if the edit changed the boxes in sectors 30 and 31), then `omega disarm Ok`, the new backup's lines, and `job write end Ok`. Anything but `Ok` there is a failure (see the table). A restore logs `omega restore <file>` instead of `omega write changes`, then the same `omega program`, `omega disarm` and backup lines, ending `job restore end Ok`.
- **Backups** are in `apps_data/pokemon/gen3/backups/`, named `UNBOUND_<TID>_<date>-<time>.sav`: the whole 128 KB save as the Omega's card holds it.

## A. Pending save

1. Play Unbound and save in the game. Switch the GBA off.
2. Open Pokemon Cartridge, press `Start`, then switch the GBA on holding START+SELECT until the logo is gone.
3. The Flipper shows `Sending helper`, then `Connect failed` with `Omega hasn't saved` / `to its card. Switch` / `on normally once.`. The log has `omega flag 0011`. Nothing was backed up.

## B. Connect

1. Switch the GBA on normally, let the Omega save to its card and show its menu, then switch it off.
2. Press `Retry` (or `Start`), then switch the GBA on holding START+SELECT.
3. The Flipper shows `Sending helper`, `Finding the game`, `Checking game file` with a bar (the first time only; write down how long it took), then `Backing up save`, `Checking backup` and the hub, headed `POKEMON UNBOUND`.
4. The log has `omega flag <word>` (not `0011`: write it down), `omega recent '/ROMS/...gba'`, `omega save ... size 131072`, and `omega sha <hash> cached 0 match 1`. Write the hash down below.

## C. Version

1. Choose `Cart info`: `EZ-Flash Omega`, then `Unbound 2.1.1.1: yes`, `Save: A #<n>` or `Save: B #<n>`, your trainer name and TID (compare the in-game trainer card), `Writing on`, `Backup <MMDD-HHMMSS>`.
2. If it says `Version: not 2.1.1.1` (the hub is headed `EZ-FLASH OMEGA` then), write down the log's `omega sha` hash and report it: browsing still works, and the next version decides how to check your copy.

## D. Backup

1. On the SD card, `apps_data/pokemon/gen3/backups/UNBOUND_<TID>_<date>-<time>.sav` is 131,072 bytes. Write down how long `Backing up save` took.
2. `Back up again` makes a second file.

## E. Browse

1. `Party and boxes`: `Party (<n>/6)`, then `BOX 1` to `BOX 24`.
2. The party lists your Pokemon as `01 <SPECIES> Lv<level>`, matching the game.
3. Open three boxes, one with a Pokemon from after Gen 3 in it: each row matches the game's species and level; eggs show `EGG`, empty slots `-- empty --`.

## F. Second connect

1. Back on the hub closes Cartridge. Switch the GBA off, open Cartridge, and connect again as in B.2.
2. No `Checking game file` this time: the log has `cached 1`.

## G. Regression

1. With a genuine Ruby cartridge, Connect reaches the hub `POKEMON RUBY` and backs up as in `docs/gen3/hardware-test-3.md` A.3. Nothing in the log says `omega`.

## H. Write test (run this first)

1. Connect as in B.1-3, until the hub (headed `POKEMON UNBOUND`) shows `Party and boxes`, `Back up again`, `Restore backup` and `Cart info`. In `Cart info`, write down the time on the `Backup` line (`Backup <MMDD-HHMMSS>`): this backup holds your save from before any edit, and R puts it back.
2. `Party and boxes`, then one of BOX 1 to 19 that has a Pokemon (not an egg). Write down its slot, species and level. Open it, choose `Level`, enter a different level (for example 5 more, 1-100), press `Back` until the box list: its row shows the new level followed by `*`.
3. Back on the hub, the row reads `Write changes (1)` and `Discard changes` appears, and `Back up again` is gone.
4. Choose `Write changes (1)`: the question `Write changes?` says `1 change. The save` / `is backed up. Keep` / `the GBA on.`. Choose `Write`.
5. The Flipper shows `Preparing` with a bar (it checks the card still holds the backup's save), then `Writing` with a bar for a while, then `Verifying`, `Saving backup`, then `Written` with `Written and verified.` / `Start Unbound from` / `the Omega menu.`. Write down how long `Writing` took. (If it says `Written` with `Written, but boxes 20-22 may be wrong: Restore.` instead, see the table; it means you edited a box in 20-22; H must use BOX 1 to 19.)
6. The log has `omega write changes 1 dirty ...`, then `omega program 1/15` through `omega program 15/15`, then `omega disarm Ok` and the new backup's lines, then `job write end Ok`. Write down whether all 15 `omega program` lines are there.
7. Back on the hub the edit is gone from the list of changes (`Write changes` is gone) and the backup in `Cart info` is a new one (`Backup <MMDD-HHMMSS>`, a later time than step 1).
8. Switch the GBA off.
9. Switch the GBA on normally (not START+SELECT) and, before starting any game, look at the Omega's menu: it lists your games and folders as before, and if you open `SAVER` its `.sav` files are there with the same names. If anything is missing or garbled, start nothing, switch off, and report it.

## I. In the game

1. From the Omega's menu (H.9), start Unbound.
2. Load your save: the Pokemon from H has the new level (check its summary), and the rest of the party and boxes are as before.
3. Save in the game, switch the GBA off and on normally again, start Unbound: the Pokemon is still there at that level. (The Omega copies the new save to its card at that power-on: let it.)

## J. Species, moves, item, nature, shiny, ability

Connect again as in B (the game saved, so the backup needs redoing), then edit one party Pokemon (not an egg), one thing at a time or all together, and write once:

1. `Pokemon`: another species. `Level` stays; the stats follow the new species.
2. `Move 1` to `Move 4`: change one or two (a move it can't normally learn is fine).
3. `Held item`: pick an item (or `None`).
4. `Nature`: another nature. `Ability`: the other slot, if the species has one. `Shiny`: `Yes`.
5. `IVs`: `All 31`. `EVs`: set Atk to 252, say.
6. Back to the hub, `Write changes (1)` (a Pokemon edited several ways counts once), `Write`, wait for `Written`.
7. Switch on normally, start Unbound, and check the Pokemon: the species, moves (it uses them in a battle), item, nature (the summary's stat colours), ability, shiny sparkle/colour, IVs/EVs (the judge, if you have one), and that its stats match its level. Write down anything off.

## K. New Pokemon

1. Connect again as in B. `Party and boxes`, a box, an empty slot (`-- empty --`) in BOX 1 to 19, not 20-22 (those get L's riskier write): the menu `BOX <n> slot <k>` offers `New Pokemon`.
2. Choose it, pick an original species (Gen 1-3, for example `Charmander`), set its level in the editor, press `Back`, and answer `Put <species> Lv<level>` / `in BOX <n>` / `slot <k>?` with `Put` (the right button; `Cancel` keeps the slot empty). The row now reads `<species> Lv<level>*`.
3. In another empty slot, do the same with a newer species (Gen 4 or later, for example `Garchomp`).
4. Back on the hub, `Write changes (2)`, `Write`, wait for `Written`.
5. Switch on normally, start Unbound, open both boxes: both Pokemon are there with the right species and level. Withdraw one to the party and check its summary and moves; the game doesn't freeze or call it a Bad Egg.

## L. Boxes 20-22

Boxes 20, 21 and 22 are stored in the two sectors (30 and 31) that both save slots share, so they get a different, riskier write. Test them with a box you can lose (take a backup first).

1. Connect again as in B. `Party and boxes`, `BOX 21`: edit one Pokemon's level (or put a new one in an empty slot).
2. `Write changes (1)`, `Write`. The log has `omega program` lines up to 16/16 (or 17/17 if you edited boxes in both sectors), then `job write end Ok`; the screen says `Written and verified.` If it says `Written, but boxes 20-22 may be wrong: Restore.`, write that down, then press `Restore` on that result screen (the Flipper first backs up the card's save, `Backing up save`, then lists the backups) and pick the one from before the write, second from the top. If you left the result screen, press `Back up again` first, then the hub's `Restore backup`.
3. Switch on normally, start Unbound and check BOX 21: the Pokemon is changed, and the other boxes 20-22 Pokemon are as before.

## R. Restore

R rolls the save back to before H, then puts the current one back (R.4): pick each backup by its time, not by its place in the list. The folder may hold older backups (part 1's tests among them), and the list's top rows are the newest.

1. Connect as in B. In `Cart info`, write down the time on the `Backup` line: this Connect's backup holds the save as it is now (I.3's progress, J, K and L's edits), which R.4 puts back. Choose `Restore backup`. The list shows backups newest first as `<MMDD-HHMMSS> <TID>` (at most 20). Pick the row whose time is the one you wrote down in H.1 (if it isn't listed, stop and report it). The Flipper shows `Checking file`, then the question `Restore backup?`; choose `Restore`.
2. `Preparing`, `Writing`, `Verifying`, `Saving backup`, then `Restored` with `Restored, verified.` / `Start Unbound from` / `the Omega menu.`.
3. Switch the GBA off, then on normally and start Unbound: the game loads the save from before H (the level from H is back to what it was). Don't save in the game.
4. Put the current save back: switch the GBA off, connect again as in B, choose `Restore backup` and pick the row with the time you wrote down in R.1. It is the third from the top: this Connect's backup, then the one R.2's restore made, then R.1's. `Restore`, wait for `Restored`. Switch off, then on normally and start Unbound: the game loads the save from before R, with I.3's progress and J, K and L's edits.

## M. Memory

1. During B's `Backing up save`, and again during H's `Writing`, run `free` in the qFlipper CLI (or `ufbt cli`) and write down `Free heap size`. Under 4,000: report it.
2. Over the whole test, write down whether Cartridge ever crashed (the Flipper restarted or showed an error screen) or froze (no answer to the buttons), and on which screen. Either: report it with the log.

## P. Pictures

Pictures of the original species (up to Deoxys) are in the app. The newer ones are read from your own Unbound file on the Omega's card the first time they are shown, and kept on the Flipper in `apps_data/pokemon/gen3/unbound_pics`. This only works while Cartridge is connected to the Omega and the game file is Unbound 2.1.1.1 (`Cart info`); otherwise a newer species shows `?`. Nothing is written to the Omega's card.

1. Connect as in B, until the hub (headed `POKEMON UNBOUND`). `Party and boxes`, a box, a Pokemon (not an egg), then the row `Pokemon: <name>`: the letters. Choose `B`. The picker opens on the Pokemon's own species if it is on that page, else on the page's first name, which for B is `BAD EGG` (`1/62`, shown as `?`: it has no shipped picture, so this first view starts a fetch and puts a `job picture` line in the log). Step to `BULBASAUR` (`54/62`; Left wraps round the end, so press Left 9 times). Its picture shows at once, since it is in the app. (Or start from a Bulbasaur in the boxes: it opens on it.) Left and Right step through the page's species.
2. Back to the letters, choose `T`. It opens on `TALONFLAME` (`1/58`) unless the Pokemon is on that page. Step to `TURTWIG` (`48/58`: Left 11 times, wrapping, or Right 47 times). It shows a box with `?` first, then its picture within about 3 seconds of `TURTWIG` coming up. Write down how long it took.
3. Step on and back to `TURTWIG`: its picture shows at once, with no `?`.
4. Back to the letters. Long letters are split into pages: choose `SAB-SHI` (54 names, 35 of them newer species). Press Right ten times quickly. The Flipper keeps answering every button press at once, and the species you stop on gets its picture. Only one fetch runs and one waits (the newest replaces the waiting one), so species you flew past may stay `?`: step back to them one at a time and each fills in.
5. Back out to the letters and choose `SAB-SHI` and `T` again: the species whose pictures had appeared (`TURTWIG` among them) show them at once. Press `OK` on one: the editor carries on as before and the species of the Pokemon you opened changes (or, in a `New Pokemon` slot, the editor opens on it). Back out of the editor without writing, or choose `Discard changes` on the hub afterwards.
6. Close Cartridge and copy `gen3_cart.log` off. It has `omega pics ok front <hex> pal <hex>` once, and a `job picture <species number> end Ok` line for each newer picture you waited for. It has no `omega pics off`.
7. M, but with the free heap taken in the picker while a picture loads (step 2 or 4).

## Results

| Step | Pass / fail | What each screen showed (Flipper, GBA) |
|---|---|---|
| A Pending save refused, log `omega flag 0011` | | |
| B Connect to the hub (`Checking game file` took ___ s; `omega flag` ____; `omega sha` ________) | | |
| C Cart info, `Unbound 2.1.1.1: yes` | | |
| D Backup file 131,072 bytes (`Backing up save` took ___ s) | | |
| E Party and 3 boxes match the game | | |
| F Second connect: `cached 1`, no hash | | |
| G Ruby connects as before | | |
| H Write test: level changed, 15 `omega program` lines, `job write end Ok` (`Writing` took ___ s); H.1's `Backup` time ____; the Omega's menu as before (H.9) | | |
| I In the game: new level there, still there after a game save and power cycle | | |
| J Species, moves, item, nature, ability, shiny, IVs, EVs correct in the game; stats match level | | |
| K Two new Pokemon (original, newer) in the game; one withdrawn to the party | | |
| L Box 21 edit written and correct in the game (screen said ________) | | |
| R Restore of H.1's backup: the game loads the save from before H; R.4 puts R.1's backup (time ____) back and the game loads it | | |
| P Pictures: Bulbasaur at once; TURTWIG `?` then its picture in ___ s; cached ones at once; ten fast steps stayed responsive and the species stopped on got its picture; log `omega pics ok`, no `omega pics off` | | |
| M Free heap while backing up: ______ bytes; while writing: ______ bytes; crashes or freezes: ______; free heap in the picker while a picture loaded: ______ bytes | | |

## If it fails

Report the step, what the Flipper and the GBA showed, and `apps_data/pokemon/gen3_cart.log`, copied off after you closed Cartridge.

| Symptom | Likely cause | Try |
|---|---|---|
| `Omega hasn't saved` every time | the DE still has a save to copy: its auto-save is off and `Save sav?` was answered B, or it was switched off before the copy ended | switch on normally, answer A at `Save sav?`, wait for the menu, switch off |
| `Play Unbound on the Omega first` | `Recently play.txt` names another game, Unbound's save isn't `SAVER/<same name>.sav` of 131,072 bytes, or that save isn't Unbound's (another game's, or Unbound never saved) | start Unbound from the Omega's menu once and save, then connect again; report the log's `omega recent` and `omega save` lines |
| `The helper reported an error` right after `Finding the game` | the card couldn't be read (`omega mount` missing from the log) | report the log |
| `Version: not 2.1.1.1` | another Unbound version, or a patched copy | report the `omega sha` hash |
| A box shows other Pokemon than the game | the save's layout differs from 2.1.1.1's | report the box, and the backup file |
| `Save once in Unbound first, then connect again.` | an in-game save was interrupted and left the other slot half written | start Unbound normally, save once in the game, switch off, switch on normally once more and let the Omega save, then connect again |
| `Write failed` (or `Restore failed`) with `Flash write failed. The cart still loads its last good save.`, `Check failed: the data read back differs.` or `SD card error.` (the log's `job write end` or `job restore end` says `FlashError`, `VerifyFailed` or `SdError`) | the card didn't take a block or gave back other bytes, or the Flipper's own SD card failed. The log's last `omega program k/n` line (n is 15, or 16-17 with boxes 20-22) says how far it got. No such line: the write test failed; it only rewrites, in the slot the game doesn't load, the bytes the card already holds, so the game's save is as it was. k from 1 to 14: the slot the game doesn't load is partly written; the game still loads the old save, but the card no longer matches the backup, so `Write changes` again would only say `The cart changed since the backup.` and drop the edits. k 15 or more: the new save is whole and the game loads it, but the Verify step found a sector other than written | report the log. No `omega program` line: try again (`Write changes`, or the restore) with the card seated (reinsert the microSD in the Omega); if it then says `The cart changed since the backup.`, see that row. k from 1 to 14: `Discard changes` (after a write), then `Back up again` (if the hub doesn't show it, close Cartridge and Connect again, which backs up), then redo the edits. k 15 or more: `Back up again`, then check the Pokemon in the game |
| `Write failed` or `Restore failed` with `Not allowed for this cartridge or save.` | Cartridge refused before sending anything; the log's `omega refused <why>` line says why. `slot parity`: the save's layout isn't the one the game itself writes (slot A's save counter is even, slot B's odd, in every save the game makes), so Cartridge won't write next to it. `no working copy`: the edits' copy on the Flipper's SD card went bad. Nothing was written | report it with `gen3_cart.log` (and, for `slot parity`, the backup file). For `no working copy`: `Discard changes`, then redo the edits |
| `Restore failed` with `Not a valid backup for this game.` | the file's save didn't come through whole: a section of it, copied into the edits' working copy on the Flipper's SD card, no longer had its own checksum. Nothing was written | report it with the log and the backup file; try the restore again |
| `Write failed` with `The cart changed since the backup. Back up again.` (the log's `job write end Changed`) | you played and saved in the game without connecting again, so the card's save differs from the backup. Nothing was written, and the edits and the backup are dropped | the hub offers `Back up again`; do that, then redo the edits |
| `Written, but boxes 20-22 may be wrong: Restore.` | the write reached the new slot and the game will load it, but boxes 20-22 (sectors 30/31, shared by both slots) didn't read back as written | press `Restore` on the result screen (the Flipper backs up the card's save first, then lists backups), pick the one from before the write (second from the top, rows read `<MMDD-HHMMSS> <TID>`), `Restore`, then check the boxes in the game. If you left the result screen: `Back up again`, then the hub's `Restore backup` |
| `Written and verified, but no new backup. Back up again first.` | the write is done and checked, but the link dropped before the new backup was saved | `Back up again` if the hub shows it; if not, close Cartridge (Back on the hub), reopen it and Connect again, which makes a fresh backup, before editing again |
| `Link lost.` during the write | the cable or the GBA's power dropped. The game still loads a whole save (the old one unless the write got to its end) | `Retry` (switch the GBA on holding START+SELECT), check the hub and the game, report the log |
| `Writing off` in `Cart info` | the game file isn't 2.1.1.1, or no current backup | `Back up again`; for a version, see `Version: not 2.1.1.1` |
| `omega pics off front table pointer`, `palette table pointer`, `front pointer`, `palette pointer`, `front size`, `front tag` or `palette tag` in the log, and every newer species shows `?` | your Unbound file's picture tables aren't where DPE puts them (the file's header pointers at 0x128 and 0x130 don't lead to tables that check out), so Cartridge doesn't read any picture from it | nothing to fix: the original species still show their pictures. Report `gen3_cart.log` |
| A newer species shows `?` and stays so, with `job picture <species number> end <result>` other than `Ok` in the log | that species' entry in your file didn't check out, or the link or the card failed for that read. Nothing was kept, so the next view tries again | step away and back; if it repeats, report the species and the log line. `Link lost` or a `Refused` result: connect again |
| A newer species shows `?` and the log has no `job picture` line for it | not connected to the Omega, or the game file isn't Unbound 2.1.1.1 (`Cart info`) | connect as in B |
| A picture looks wrong (another Pokemon, a smear) | the converted picture kept in the cache is bad | report the species. Then, on the Flipper, open Archive, the Browser tab, `apps_data`, `pokemon`, `gen3`, and delete the whole `unbound_pics` folder (safe: the pictures are fetched again as you open them). A single picture is the file `<species number>.fxb` in it, but the number is hard to tell, so delete the folder |
| The Flipper froze or restarted while you browsed pictures | memory | report it under M with the screen and the log |
