# Gen III editor test

Goal: the Gen III Pokemon editor on the Flipper shows and changes a Pokemon correctly, and Gen I and Gen II still work. No GBA is needed for this test.

**Build and install.** Run `ufbt` (release build). Copy `dist/pokemon.fap` to `apps/GPIO/` on the SD card with qFlipper, or run `ufbt launch`.

## Notes

- The editor's Pokemon is created the first time the Gen III menu opens and is kept until the app exits. Leaving the editor with Back does not reset it.
- Nature, Shiny, Gender, Ability and Unown Form are all stored in the Pokemon's PID. Changing one of them picks a new PID that keeps the others, so the other rows must not change.
- A Pokemon loaded from a file keeps its current HP until the first edit. Any edit then heals it to full HP. That is by design.
- On the Flipper keyboard, the first character typed is a capital, and `_` in that position becomes a space. To test an unsupported character, type `_` with a short press after at least one letter.

## A. Editor basics

1. **Gen III menu.** Main menu → `Gen III  (GBA non-JPN)` shows `Edit Pokemon` and `Link Test`. `Link Test` still opens the Link Test screen, and Back from it returns to this menu.
2. **Defaults.** `Edit Pokemon` shows, in this order: Nickname `BULBASAUR`, Level `5`, `Random IV, Zero EV`, a nature, Ability `OVERGROW`, Shiny `No`, Gender `Male` or `Female`, Ball `POKE BALL`, OT Name `FLIPPER`, OT TID `22222`, OT SID `00000`. The menu has no header, and the values line up in one column (except Unown Form, which only shows for Unown).
3. **Nickname.** Set it to `Leafy` → the row shows `Leafy`. Clear the field and save it empty → the row shows `BULBASAUR`. Type `A_B` → the keyboard shows `Unsupported character!` and stays open.
4. **Level.** `0` and `101` are refused with `Level must be 1-100!`, and so is an empty field. `50` is accepted and the row shows `50`.
5. **Nature.** The list shows 25 natures, with the current one highlighted, e.g. `HARDY`, `LONELY +Atk -Def`, `ADAMANT +Atk -SpA`. Choose `ADAMANT +Atk -SpA` → the row shows `ADAMANT`, and the Gender and Shiny rows are unchanged.
6. **Shiny and OT ID.** Set Shiny to `Shiny` → the row shows `Yes`. Then set OT TID to `12345` → the TID row shows `12345` and Shiny is still `Yes`. OT TID and OT SID refuse `65536` with `ID must be 0-65535!`.
7. **Ability.** On Bulbasaur, pressing OK on the Ability row does nothing (one ability).
8. **Kept on Back.** Press Back to the Gen III menu, then open `Edit Pokemon` again → the same values as before, and the last row you used is highlighted.
9. **No regression.** Back to the main menu. `Gen I` and `Gen II` each open and show their own editors as before.

## Results

| Step | Pass / fail | What the Flipper showed |
|---|---|---|
| A1 Gen III menu | | |
| A2 Defaults | | |
| A3 Nickname | | |
| A4 Level | | |
| A5 Nature | | |
| A6 Shiny and OT ID | | |
| A7 Ability | | |
| A8 Kept on Back | | |
| A9 Gen I / Gen II | | |
