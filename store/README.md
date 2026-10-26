# PixelDeck public store files

Copy `catalog.txt` and the `games/` directory into the root of a public GitHub repository. In console Settings, set the raw repository root URL to:

`https://raw.githubusercontent.com/<owner>/<repository>/<branch>/`

The firmware requests `catalog.txt`, then downloads `games/<id>.cart`. A catalog type of SNAKE, PONG, BREAKOUT, or INVADERS installs a legacy built-in-engine cart. Use SCRIPT with `api=3` for original event-driven games. API 2 carts remain compatible. See [API_REFERENCE.md](../API_REFERENCE.md) and `games/orbit-courier.cart` for a working example.

Declare media in the cart manifest, for example `asset.player=player.bmp` or `asset.music=theme.wav`. Publish each file at `games/<id>/assets/<filename>`. The store installs the cart and its media to the SD card. JPEG and uncompressed 24-bit BMP images and the documented audio formats are supported; use lowercase audio extensions. Each cartridge may declare up to 8 assets, with a 512 KiB file limit and 2 MiB total download limit.
