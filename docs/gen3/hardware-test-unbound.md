# Gen III hardware test: Pokemon Unbound on an EZ-Flash Omega (read-only)

Goal: Pokemon Cartridge connects to an EZ-Flash Omega Definitive Edition over the link cable, finds the game the Omega started last and its save on the Omega's microSD card, checks that the game file is Pokemon Unbound 2.1.1.1, backs the save up to the Flipper, and lists the party and the 24 boxes. This version only reads: nothing is written to the Omega's card.

You need: Flipper with the KBEmbedded Game Link GPIO module, GBA (or GBA SP), a Game Boy Color link cable (or a GBA link cable if the module's SD line is connected), an EZ-Flash Omega Definitive Edition with Pokemon Unbound on its microSD card, and a genuine Ruby cartridge for G.

**Build and install.** `ufbt build APPID=pokemon_cart`, then `ufbt launch APPID=pokemon_cart`.

## Notes

- **The DE's save.** While you play, the Omega Definitive Edition keeps the save in the cart, and copies it to its microSD card the next time you switch on normally (a saving screen, or `Save sav?` if its auto-save setting is off). A START+SELECT power-on skips that, so after playing, **switch on normally once and let the Omega save, then switch off**, and only then connect. Cartridge checks this itself (A).
- **Which game.** Cartridge reads the game the Omega started last, from `SAVER/Recently play.txt`, and its save, `SAVER/<game name>.sav`.
- **The log** is `apps_data/pokemon/gen3_cart.log`. Its Omega lines, each only for what that Connect got to: `omega flag <word>` (`0011` while the DE has a save to copy; write down what it shows in B), `omega mount ...`, `omega recent '<game file>'`, `omega save '<save file>' size 131072`, `omega rom '<game file>' size <n> clus <n>`, `omega sha <hash> cached <0|1> match <0|1>`, `backup <name>`, `ub slots ...` and `ub trainer <name> <TID>`. Close Cartridge before copying it off (the USB CLI can't read it while Cartridge runs).
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

1. Choose `Cart info`: `EZ-Flash Omega`, then `Unbound 2.1.1.1: yes`, `Save: A #<n>` or `Save: B #<n>`, your trainer name and TID (compare the in-game trainer card), `Read-only for now`, `Backup <MMDD-HHMMSS>`.
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

## M. Memory

1. During B's `Backing up save`, run `free` in the qFlipper CLI (or `ufbt cli`) and write down `Free heap size`. Under 4,000: report it.

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
| M Free heap while backing up: ______ bytes | | |

## If it fails

Report the step, what the Flipper and the GBA showed, and `apps_data/pokemon/gen3_cart.log`, copied off after you closed Cartridge.

| Symptom | Likely cause | Try |
|---|---|---|
| `Omega hasn't saved` every time | the DE still has a save to copy: its auto-save is off and `Save sav?` was answered B, or it was switched off before the copy ended | switch on normally, answer A at `Save sav?`, wait for the menu, switch off |
| `Play Unbound on the Omega first` | `Recently play.txt` names another game, Unbound's save isn't `SAVER/<same name>.sav` of 131,072 bytes, or that save isn't Unbound's (another game's, or Unbound never saved) | start Unbound from the Omega's menu once and save, then connect again; report the log's `omega recent` and `omega save` lines |
| `The helper reported an error` right after `Finding the game` | the card couldn't be read (`omega mount` missing from the log) | report the log |
| `Version: not 2.1.1.1` | another Unbound version, or a patched copy | report the `omega sha` hash |
| A box shows other Pokemon than the game | the save's layout differs from 2.1.1.1's | report the box, and the backup file |
