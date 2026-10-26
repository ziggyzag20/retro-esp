# Retro-ESP: Pocket Games Device

The Retro-ESP is a handheld touchscreen retro game device that you can use anywhere, online or offline. With an open-source games library, you can create and install any game using the API from an official source or your own.

# How to use on your own projects:
<ins>You will need:</ins>

- The Freenove FNK0104N display
- Micro-SD card (Any size will do, I'd go for 4GB so you have both plenty of storage storage and something cheap)
- Rechargable 3.7V Li-ion battery (optional, needed for portable use)

<ins>Uploading the code:</ins>
1. Take the .ino file from the repo and open it with the Arduino IDE. You can use the ESP-IDE if wanted however I have not used it myself so I will not provide help if you cannot get it working.
2. Ensure you have the following things set in the *Tools* menu:
   - Board: ESP32S3 Dev Module
   - USB CDC enabled on boot: Enabled
   - CPU Frequency: 240MHz (WiFi)
   - Flash mode: QIO 80MHz
   - Flash size: 16MB (128Mb)
   - Partition scheme: 16M Flash (3MB APP/9.9MB FATFS)
   - PSRAM: OPI PSRAM
  
     Anything else should be left as is.
3. Upload the code. If any errors present themselves, recheck everything and report as a bug if necessary.
4. Before booting, copy the library to the SD card and insert it. If you insert after booting, restart the microcontroller by pressing the RESET button or simply unplug your power source.
5. Test for any issues. If issues occur, check everything again and report as a bug if necessary.

## Controls

The on-screen arrow buttons move. A and B are available to custom games. II pauses and resumes.

<ins>Installing games:</ins>
To install any games, you can simply use the game store function to download any games over Wi-Fi to the SD card. Playing them is as simple as opening them.

Should any issues arise, you should report it as a bug on the branch for that game. Please do not contact me for support if I did not make the game.

# Making your own games
Making your own games is unfortunately not as simple as using python or the IDE.

To make your own games, you will need to use the API found in the project. Follow the examples for help if needed.
When making a game, you will need to upload it to a repo and set that repo in your settings. However, you can upload multiple games into one repo, as this essentially acts as a store. Use the official repo as a template and review it thoroughly.

## GitHub game store

1. Create a public GitHub repository.
2. Copy the contents of this project's `store/` folder and `API_REFERENCE.md` to the repository root, keeping `catalog.txt` and the `games/` folder structure.
3. On the device, open **Settings**, enter Wi-Fi details, and set the raw repository URL to `https://raw.githubusercontent.com/<owner>/<repository>/main/` (or your chosen branch).
4. Save and connect to Wi-Fi. Open **Game Store** and choose **Refresh**.

The console downloads `catalog.txt`, then `games/<id>.cart`. Assets declared in the cartridge manifest are downloaded from `games/<id>/assets/<filename>`. The store only accepts `raw.githubusercontent.com` URLs and public repositories. HTTPS traffic is encrypted, though the current client does not validate the remote certificate.

Cartridges are checked before the SD card cart is replaced. Each game can declare up to 8 assets, with a 512 KiB per-file limit and 2 MiB total asset limit. Images are JPEG or uncompressed 24-bit BMP; audio is MP3, WAV, AAC, FLAC, M4A, OGG/OGA, or OPUS. Files are stored in `/ARCADE/GAMES/<id>/` on the card.
