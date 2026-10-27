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

## Results

| Step | Pass / fail | What each screen showed (Flipper, GBA) |
|---|---|---|
| A.1 Cartridge opens | | |
| A.2 `Waiting for GBA`, power-on lines after 3 s | | |
| A.3 Connect to the hub (`Sending helper` took ___ s, `Backing up save` ___ s) | | |
| A.4 Without the keys, then with them | | |
| A.5 `Cable: plug in/swap ends` | | |
| A.6 Back while sending | | |
| A.7 Data missing, then unpacked again | | |
| B.1 Cart info (flash ____, save ____, trainer ____) | | |
| B.2 Backup file, 131,072 bytes | | |
| B.3 Backup in mGBA or PKHeX (optional) | | |
