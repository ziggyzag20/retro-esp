# PixelDeck public store files

Copy catalog.txt and the games/ directory into the root of a public GitHub repository. In the console Settings page, set the raw repository root URL to:

https://raw.githubusercontent.com/<owner>/<repository>/<branch>/

The firmware requests catalog.txt, then games/<id>.cart. A catalog type of SNAKE, PONG, BREAKOUT, or INVADERS installs a legacy built-in-engine cart. Use SCRIPT with `api=2` to publish a game with custom event handlers and rules. See ../API_REFERENCE.md and games/orbit-courier.cart for the full format and a working example.
