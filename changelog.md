# Changelog - Patch Notes

## Unreleased
**Gen III link test (experimental)**
- Add `Gen III  (GBA non-JPN)` menu with a Link Test: the Flipper joins Pokemon Ruby's Cable Club Trade Center as a second player and logs the link to `apps_data/pokemon/gen3_link.log`
- See `docs/gen3/hardware-test-1.md`

**Gen III Pokemon editor and .pk3 load/save (experimental)**
- Add `Edit Pokemon` to the Gen III menu: species, nickname, level, EV/IV preset, nature, ability, shininess, gender, Unown form, moves, held item, Pokerus, ball and OT
- Add `Load .pk3` and `Save .pk3` for PKHeX `.pk3` files in `apps_data/pokemon/gen3/`
- `files/gen3_bulk.bin` (species, move and item names, and learnsets) ships as an app asset; if the editor says `Data missing`, delete `apps_assets/pokemon` on the SD card and reopen
- See `docs/gen3/editor-test.md`

**Gen III Cable Club trade with Ruby/Sapphire (experimental)**
- Add `Trade` to the Gen III menu: the Flipper sits at the Cable Club Trade Center table with Pokemon Ruby and trades the Pokemon from `Edit Pokemon`; the one it gets back can be edited and traded back without leaving the room, and the link keeps running behind every Gen III screen
- The GBA player becomes the OT of the default Pokemon and of each species picked after linking; `OT: use <NAME>` in the editor gives any Pokemon the player's OT
- The Flipper refuses Eggs, Bad Eggs and Mail holders, and doesn't offer Mail, the ENIGMA BERRY or items Ruby lacks; Back is locked while the GBA saves, and `End trade` makes the GBA walk out of the Trade Center by itself
- `National Dex: Off` / `On` in the Gen III menu (off at every start) lets a trade unlock the GBA's National Dex, for good on that save
- See `docs/gen3/hardware-test-2.md`

## Version 2.3
- Fix potential bug in copying incoming trade data to the live working struct

## Version 2.2
**New Features**
- Update to gblink v0.63 which includes saving/loading of pin configurations for the EXT link interface

## Version 2.1
**New Features**
- Add ability to reset trade state without affecting current Pokemon being configured

**Refactor**
- Move files, consolidate files where appropriate, change scene management to be more in-line with Flipper paradigms

## Version 2.0
**Bug Fixes**  
- Older MALVEKE style pinout no longer breaks OK button, MALVEKE users now able to modify traded Pokemon

**New Features**  
- Generation II support (Gold, Silver, Crystal)  
  - Ability to configure name, level, held item, moveset, EV/IV, shininess, gender, pokerus, Unown form, OT ID/name
- Custom Sprite Art for all Pokemon, all sprites are the full 56x56 px size  
- Better state synchronization during trades, able to request canceling a trade
- Exit confirmation to prevent accidentally losing configuration  
- Add main menu to select generation and pinout  
- Adjust Trade and Select views to show full sprite, with proper transparency

**Refactor**
- Sprites moved to file on SD card  
- Create accessors for generically modifying Pokemon struct data  
