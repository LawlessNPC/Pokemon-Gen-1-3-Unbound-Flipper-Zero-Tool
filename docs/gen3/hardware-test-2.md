# Gen III hardware test 2: trading with Ruby

Goal: the Flipper trades Pokemon with a genuine Pokemon Ruby in the Cable Club Trade Center, in both directions. The link keeps running while you use the Flipper's Gen III screens, an edit is never lost to a trade, and the GBA never shows a link error in normal use.

You need: Flipper with the KBEmbedded Game Link GPIO module, GBA, GBA link cable, genuine Ruby.

**Build and install.** Run `ufbt` (release build). Copy `dist/pokemon.fap` to `apps/GPIO/` on the SD card with qFlipper, or run `ufbt launch`.

## Notes

- **The log** is `apps_data/pokemon/gen3_link.log`. It is overwritten each time a Trade or Link Test link starts, so after a failed run copy it off the SD card before you start another one. It grows by about 8 KB/s while you are in the Trade Center. Each line carries the link phase as `p0` to `p10`: `p0` waiting for the GBA, `p1`-`p3` linking, `p4` Trade Center room, `p5` closing, `p6` error, `p7` trade menu loading, `p8` trade menu, `p9` trade animation, `p10` the GBA's link save.
- **The trade screen** shows `Gen III Trade` on top, three lines that say what is happening and what to do on the GBA, then `Flipper: <NICKNAME>`, the Pokemon the Flipper offers. `E<n>` at the top right counts checksum errors on the link; it should never appear. `Edit` at the bottom (the OK button) opens the editor; it is hidden while a trade is under way.
- **The link runs behind every Gen III screen** once `Trade` has started it: the editor, its keyboards and lists, the species picker, `.pk3` load and save. `Trade` in the Gen III menu returns to the trade screen. While the link runs, the Gen III menu has no `Link Test`.
- The trade screen's texts are shown on three lines; this checklist writes them on one, except where it quotes the lines.
- On any failure, report the step, what each screen showed (Flipper and GBA, including any `E<n>`), and the log file.

## A. Setup and Link Test regression

1. **Ruby.** Pokedex received; at least 3 Pokemon in the party, one you're happy to trade away; none holding Mail or the ENIGMA BERRY. Save the game.
2. **Cable.** Plug the cable as in test 1 (`docs/gen3/hardware-test-1.md`, step 4), with the same plug in the Flipper.
3. **Link Test.** Gen III → `Link Test`: run test 1 steps 5–8 once. The Flipper now sends its LinkPlayerBlock after the GBA's (D2); the Link Test must still reach `in Trade Center` and leave cleanly.
4. **ENIGMA BERRY fixture.** On a computer, copy `docs/gen3/fixtures/enigma-berry.pk3` from this repository to `apps_data/pokemon/gen3/` on the SD card with qFlipper. It is a TREECKO holding the ENIGMA BERRY, which Ruby can't take in a trade; section H uses it to check the refusal. Don't load it before then.

## B. Start a trade

1. **Start.** Start the app fresh and don't edit the Pokemon first. Gen III → `Trade`: the screen shows `Plug in the cable (or swap its ends).` until the cable reads OK, then `Go to the Cable Club: Pokemon Center 2F, right counter.`
2. **Enter.** On Ruby, go to the Trade Center as in test 1. The Flipper shows `Linking...`, then `In the Trade Center: sit at the table on the GBA.`
3. **OT.** Back to the Gen III menu (the link keeps running) → `Edit Pokemon`: `OT Name` is your Ruby name, and `OT TID`/`OT SID` are yours. The default Bulbasaur took your OT (D7). Back to the Gen III menu, then `Trade` to return to the trade screen.

## C. First trade (GBA → Flipper)

1. **Sit.** Walk onto either seat: the Flipper shows `Opening the trade menu...`. Ruby shows the trade menu with BULBASAUR alone on the right.
2. **Trade.** Choose one of yours → TRADE → YES. The Flipper shows `Confirm the trade on the GBA.`, then `Trading... Watch the GBA.`, then during "SAVING..." `The GBA is saving. Don't unplug or turn off. Back is locked.` Press Back on the Flipper during the save: nothing happens.
3. **After the save** Ruby shows the trade menu again, with your Pokemon on the Flipper side. The Flipper shows `Flipper: <its nickname>` and `Edit it (OK), or pick the next trade on the GBA.` (The Flipper recalculates a received Pokemon's stats. If it did so only after the menu reopened, Ruby goes back to the room once instead and the Flipper asks you to sit again, as in D.3. Note it if that happens.)
4. **Ruby:** BULBASAUR is in your party, with you as OT.

## D. Edit and trade back (Review Focus 1)

1. **Edit.** On the trade screen press OK (`Edit`): the editor opens.
2. **The GBA goes back.** As soon as the editor opens, the GBA leaves the trade menu for the room, with your avatar still on its seat. Change the species to TREECKO, then Back to the trade screen. (If you are back before the GBA reaches the room, the screen first shows `Updating the trade menu: the GBA goes back to the room.`)
3. **Sit again.** The Flipper shows:

   ```
   Sit at the table
   again (step off and
   back on if seated).
   ```

   Hold Up for one step, then Down for one step. If Up is blocked, try Down then Up, and note it. The trade menu opens with TREECKO on the Flipper side.
4. **Trade back.** Trade any of yours for TREECKO. Ruby: TREECKO is in your party with you as OT; it obeys in battle and gets no "boosted" Exp.
5. **Again.** Repeat C and D once without leaving the room.
6. **Edit while the GBA asks.** With the GBA in the trade menu, press TRADE on one of yours and, before the Flipper answers, press OK (`Edit`) on the Flipper. The GBA goes back to the room and no trade starts. On the Flipper, Back to the trade screen without changes. Sit again: the menu opens and trading works.

## E. Background link while you work (Review Focus 5)

With the GBA in the Trade Center (opening the editor sends it from the trade menu back to the room; leave it there), for about 3 minutes on the Flipper:

1. **Typing.** Edit Pokemon → Nickname: type 10 letters slowly, then save.
2. **Sprites.** Open the species picker (the `Pokemon` row) and scroll through 60 species with sprites. Back without picking.
   - **2a. Move and item lists.** Edit Pokemon → Select Moves → Move 1. The letter list shows within about 1 s; pick `S`, and its moves show within about 1 s. Do the same for Held Item (its letter list, then `S`). Move and item names are read from the SD card. Ruby shows no link error. Write down roughly how long each of the four screens took to show:

     | Screen | Time |
     |---|---|
     | Move 1 letter list | |
     | Moves under `S` | |
     | Held Item letter list | |
     | Items under `S` | |
3. **Files.** Back to the Gen III menu → `Save .pk3` → `Load .pk3` (browse to the file and load it) → `Trade`.
4. **Expected:** Ruby never shows a link error, the trade screen's title shows no `E<n>`, and trading still works afterwards (sit again, and trade once).
5. **If Ruby shows a link error** here, write down what you were doing. For the rest of the test use the fallback: walk out of the Trade Center before editing, and back in afterwards. The Flipper waits at `Go to the Cable Club` meanwhile.
