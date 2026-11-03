<img width="100%" src="https://capsule-render.vercel.app/api?type=waving&color=0:EE1515,100:3B4CCA&height=200&section=header&text=Pokemon%20Gen%201-3%20%2B%20Unbound&fontSize=42&fontColor=FFFFFF&fontAlignY=36&desc=for%20Flipper%20Zero&descSize=20&descAlignY=58" alt="Pokemon Gen 1-3 + Unbound for Flipper Zero"/>

<p align="center">
  <a href="https://github.com/DenverCoder1/readme-typing-svg"><img src="https://readme-typing-svg.demolab.com?font=Montserrat&weight=600&size=22&duration=3500&pause=800&color=3B4CCA&center=true&vCenter=true&width=560&lines=Trade+Pokemon+with+your+Flipper+Zero;Change+any+Pokemon+you+like;Add+Pokemon+to+your+Unbound+save;No+computer+needed" alt="Trade Pokemon, change them, and edit your Unbound save"/></a>
</p>

<p align="center">
  <a href="https://github.com/LawlessNPC/Pokemon-Gen-1-3-Unbound-Flipper-Zero-Tool/releases/latest"><img src="https://img.shields.io/github/v/release/LawlessNPC/Pokemon-Gen-1-3-Unbound-Flipper-Zero-Tool?style=for-the-badge&label=Download&color=EE1515" alt="Download"/></a>
  <img src="https://img.shields.io/badge/Flipper%20Zero-FF8200?style=for-the-badge" alt="Flipper Zero"/>
  <img src="https://img.shields.io/badge/Game%20Boy%20%7C%20GBA-3B4CCA?style=for-the-badge" alt="Game Boy and Game Boy Advance"/>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-FFCB05?style=for-the-badge" alt="MIT License"/></a>
</p>

## ✨ What it does

- 🔁 **Trade** Pokemon between your Flipper Zero and your Game Boy or Game Boy Advance.
- ✏️ **Customize** any Pokemon on the Flipper (species, level, moves, nickname, shiny and more), then trade it back.
- 🎮 **Edit your save** on a Game Boy Advance over the link cable: change your Pokemon or add new ones. Works with **Pokemon Unbound** on an EZ-Flash Omega.
- 💾 **Stay safe**: your save is backed up to the Flipper before anything changes.

| Game | Trade | Edit the save |
| --- | :---: | :---: |
| Red · Blue · Yellow · Gold · Silver · Crystal | ✅ | |
| Ruby · Sapphire · FireRed · LeafGreen | ✅ ¹ | ✅ |
| Pokemon Unbound on an EZ-Flash Omega Definitive Edition | | ✅ |

<sub>¹ Needs a GBA link cable and a link module with its SD line connected. Game Boy Advance trading is new and not yet tested on real hardware.</sub>

## 🧰 What you need

- A **Flipper Zero**
- A **link module** for it: the [Flipper GB Link](https://www.tindie.com/products/kbembedded/game-link-gpio-module-for-flipper-zero-game-boy/) or a [MALVEKE](https://www.tindie.com/products/efuentealba/malveke-game-boy-tools-for-flipper-zero/) board
- A **Game Boy Color link cable**. It works for Game Boy trades and for editing Game Boy Advance saves.

## 📥 Install

1. Download **`pokemon.fap`** and **`pokemon_cart.fap`** from the [latest release](https://github.com/LawlessNPC/Pokemon-Gen-1-3-Unbound-Flipper-Zero-Tool/releases/latest).
2. Plug your Flipper into your computer and open [qFlipper](https://docs.flipper.net/qflipper).
3. Copy both files into **SD Card › apps › GPIO**.

On the Flipper they are under **Apps › GPIO**: **Pokemon Trade Tool** and **Pokemon Cartridge**.

## 🎮 Pokemon Unbound: change and add Pokemon

> [!IMPORTANT]
> After playing, save in Unbound. Then turn the GBA **on normally once** and let the EZ-Flash copy your save to its card, and turn it off again.

1. Connect the Flipper and the GBA with the link cable. Keep the GBA off.
2. On the Flipper, open **Pokemon Cartridge** and press **Start**.
3. Hold **START + SELECT** on the GBA and turn it on. Keep holding until the logo is gone.
4. Wait about a minute and a half. When the menu says **POKEMON UNBOUND**, you're in.
5. Choose **Party and boxes**:
   - **Change a Pokemon:** open it and pick what to change. For a new species, pick a letter and scroll through the pictures.
   - **Add a Pokemon:** open an empty box slot and choose **New Pokemon**.
6. Choose **Write changes**, then **Write**.
7. Turn the GBA off and on and start Unbound. Your Pokemon are there! 🎉

It works the same way for a Ruby, Sapphire, FireRed or LeafGreen cartridge.

## 🔁 Trade with a Game Boy or GBA

1. Open **Pokemon Trade Tool** and choose your game's generation.
2. Pick the Pokemon to send, change it if you like, then choose **Trade**.
3. Connect the link cable. In the game, go upstairs in a Pokemon Center and talk to the trade counter.
4. Sit at the table and trade. You can change the Pokemon you get back and trade it back again.

## 🆘 Help

<details>
<summary><b>The menu says "Bad cartridge header" instead of POKEMON UNBOUND</b></summary>
<br>
The GBA couldn't read the EZ-Flash. Turn the GBA off, wait about 10 seconds, and connect again.
</details>

<details>
<summary><b>It says "Omega hasn't saved to its card"</b></summary>
<br>
Turn the GBA on normally once, let the EZ-Flash save, turn it off, and connect again.
</details>

<details>
<summary><b>It says "Not enough memory"</b></summary>
<br>
Close Pokemon Cartridge, restart the Flipper (<b>Settings › Power › Reboot</b>) and open it again.
</details>

<details>
<summary><b>The Flipper froze while it was plugged into my computer</b></summary>
<br>
Close Pokemon Cartridge before you plug the Flipper into a computer. To restart a frozen Flipper, hold <b>Left + Back</b> for 5 seconds.
</details>

<details>
<summary><b>Where are my backups?</b></summary>
<br>
On the Flipper's SD card in <b>apps_data › pokemon › gen3 › backups</b>. <b>Restore backup</b> in Pokemon Cartridge puts one back.
</details>

## 📖 Want more?

The [full guide](docs/guide.md) covers every option, trading with the Game Boy Advance, cartridge details, building the apps and wiring your own cable. What's new is in the [changelog](changelog.md).

## ❤️ Credits

Built on the Pokemon Trade Tool by Esteban Fuentealba, KBEmbedded and its contributors (MIT License). Pokemon Unbound support uses data from [PUSE](https://github.com/Zannael/PUSE) (MIT License), and the EZ-Flash Omega's card access follows EZ-Flash's [Omega Definitive Edition kernel](https://github.com/ez-flash/omega-de-kernel).

<img width="100%" src="https://capsule-render.vercel.app/api?type=waving&color=0:3B4CCA,100:EE1515&height=120&section=footer" alt=""/>
