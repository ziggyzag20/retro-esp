# PixelDeck Script API 2

API 2 lets a cartridge describe a new game from its own rules and events. It is an interpreted game language, not a choice among Snake, Pong, Breakout, and Invaders. A downloaded cart remains data: it cannot run native C/C++ or machine code.

## Limits on the FNK0104N

- Complete cartridge file, including its manifest: up to 24 KiB.
- Compiled commands: up to 768, with at most 8 nested `if` / `repeat` blocks.
- Repeats: up to 64 iterations each; an event is capped at 2,048 executed commands per call.
- Rectangle entities: 96, addressed as IDs 0 through 95.
- Integer variables: 64, with names up to 15 characters.
- Text labels: 8, addressed as IDs 0 through 7.
- Update cadence: `tick_ms=20` through `tick_ms=250`.
- One command per line; a line can contain up to 220 characters.

The play field is 448 by 184 pixels. Its origin is the top left. Shapes and labels use play-field coordinates. The current renderer provides colored rectangles with rounded corners and text labels; it does not yet load bitmap art or play sound effects.

## Cartridge file

The manifest goes first. `--SCRIPT--` begins the program. Event blocks use `on|event` and close with `end`.

```text
api=2
id=my-first-game
title=MY FIRST GAME
description=An original game made from script commands.
type=SCRIPT
tick_ms=48
accent=56E5FF
--SCRIPT--
on|start
set|score|0
rect|0|80|70|20|20|56E5FF|8
end
on|update
move|0|1|0
end
```

The `id` must match the catalog entry and use letters, numbers, hyphens, or underscores. `type=SCRIPT` and `api=2` are required. Lines beginning with `#` are comments. The pipe character separates fields and cannot be used inside a text value. Older template carts remain supported.

The language has no unbounded loops or jumps. `repeat|count` / `endrepeat` provides bounded loops, while the 2,048-command event budget keeps each frame responsive. A rejected API 2 cart reports its source line to Serial Monitor when it is installed or launched; the console also shows the line for an install or launch error.

For the store, add a catalog row with `SCRIPT` as its type and publish the cartridge as `games/<id>.cart`:

```text
my-first-game|MY FIRST GAME|SCRIPT|8|56E5FF|An original game made with API 2.
```

## Events and input

Every game needs `start` and `update` handlers. `start` runs on launch and restart. `update` runs at the chosen `tick_ms` cadence. Input handlers run when a control is pressed, released, or clicked.

```text
on|start
  # create the starting scene
end
on|update
  if|left
    move|0|-3|0
  endif
end
on|left_down
  # runs once as the left control is pressed
end
on|left_up
  # runs once as the left control is released
end
on|a
  # A was clicked
end
on|b
  # B was clicked
end
```

Available event names: `start`, `update` (or `tick`), `left_down`, `left_up`, `up_down`, `up_up`, `down_down`, `down_up`, `right_down`, `right_up`, `a`, and `b`.

The update handler can test held controls as values: `left`, `right`, `up`, `down`, `button_a`, and `button_b`. `touch` reports a press inside the game field; `touch_x` and `touch_y` are field-relative coordinates. `screen_x` and `screen_y` return display coordinates. `time` is elapsed seconds since the run began.

## State, conditions, and random values

Variables are signed integers clamped to ±1,000,000,000. A `$` prefix is optional when reading one.

```text
set|player_x|200
add|score|10
sub|lives|1
mul|bonus|2
div|bonus|2
mod|bonus|3
random|spawn_x|0|420
repeat|3
  add|bonus|10
endrepeat

if|$score|>=|100
  set|level|2
else
  set|level|1
endif
```

Conditions support `==`, `=`, `!=`, `>`, `<`, `>=`, `<=`, and the aliases `eq`, `ne`, `gt`, `lt`, `ge`, `le`. A one-field condition such as `if|left` tests whether its value is nonzero. `random` chooses from the minimum inclusive to the maximum exclusive.

Built-in state names are `score`, `best`, `lives`, `level`, and `time`. `best` is read-only. Setting `score`, `lives`, or `level` updates the corresponding game state. The console saves a new high score when the run ends.

## Drawing and entity commands

`rect|id|x|y|width|height|color|radius` creates or updates a rectangle. Colors are six-digit RGB hex, with or without `#`.

```text
rect|0|$player_x|150|20|20|56E5FF|6
move|0|-4|0
position|0|210|150
velocity|1|0|3|wrap
physics
collide|hit|0|1
if|hit
  sub|lives|1
endif
```

`move` applies a delta; `position` sets an absolute location. `velocity` stores per-update movement. Its optional final argument `wrap` wraps an entity around the field; without it, the entity bounces at the field edges. `physics` advances every moving entity, or `physics|id` advances one. Call it from `update` when needed.

Other entity commands:

- `collide|variable|first_id|second_id` sets the variable to 1 when two visible rectangles overlap, otherwise 0.
- `getx|variable|id`, `gety|variable|id`, `getvx|variable|id`, and `getvy|variable|id` read one entity property into a variable.
- `exists|variable|id` checks whether an entity is active.
- `hide|id`, `show|id`, and `remove|id` change an entity's visibility or active state.
- `tween|id|target_x|target_y|duration_ms` animates an entity to a position.

## Text and run state

```text
label|0|10|4|300|FFFFFF|SCORE $score   LIVES $lives
text|0|SCORE $score   LIVES $lives
clearlabel|0
```

Labels update variable substitutions when set. `over|message` and `win|message` end the run, show the message, and save the high score. A or B restarts after the game has ended.

## Complete example

`store/games/orbit-courier.cart` is a complete API 2 game. It includes custom movement, three independent hazards, collision handling, lives, score, escalating levels, and A/B actions. It is listed in `store/catalog.txt` as a downloadable game.
