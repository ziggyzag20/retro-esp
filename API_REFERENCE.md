# Retro-ESP Script API 3

API 3 is a data-only, event-driven game language for Retro-ESP on the FNK0104N. It lets a cartridge define its own rules, controls, entities, interface, bitmap art, and sound. The console interprets the commands; downloaded carts never execute C/C++, scripts, or machine code. API 2 carts remain accepted.

## Device limits

- Cartridge manifest and script: 96 KiB maximum.
- Compiled commands: 4,096 total; control-flow nesting: 16 levels.
- Repeat count: 0 to 128; one event may execute up to 8,192 commands per call.
- Entities: IDs 0 through 159.
- Integer variables: 128, names up to 15 characters.
- Text labels: IDs 0 through 15.
- One command per line; each line can contain up to 384 characters.
- Tick interval: 20 to 250 ms.
- Media: up to 8 declared assets, each up to 512 KiB, with a 2 MiB total download cap.
- A bitmap is decoded to at most 65,536 pixels and at most 512 pixels on either side. Larger images are reduced automatically.

The play field is 448 by 184 pixels. Its origin is the top left. Shapes, sprites, labels, and touch coordinates use play-field coordinates.

## Cartridge and asset layout

Put manifest fields first and start the program with `--SCRIPT--`. Asset names use letters, numbers, hyphens, and underscores. Filenames are simple basenames; folders and path separators are not allowed.

```text
api=3
id=star-runner
title=STAR RUNNER
description=A custom game with sprites, sound, and original rules.
type=SCRIPT
tick_ms=40
accent=56E5FF
asset.player=player.bmp
asset.music=theme.wav
asset.hit=hit.wav
--SCRIPT--
on|start
  # create the scene
end
```

Publish the cartridge at `games/<id>.cart`. Publish its media at `games/<id>/assets/<filename>`. The store downloads the cart and all declared media together, then saves them under `/ARCADE/GAMES/<id>/` on the SD card. Use the same filename and letter case in the manifest and repository. Legacy template carts remain supported.

Accepted bitmap formats are JPEG (`.jpg`, `.jpeg`) and uncompressed 24-bit Windows BMP (`.bmp`). They are opaque RGB images; transparency and PNG are not supported. Supported audio extensions are `.mp3`, `.wav`, `.aac`, `.flac`, `.m4a`, `.ogg`, `.oga`, and `.opus`; write audio extensions in lowercase. Small 44.1 kHz mono or stereo sounds work well on the FNK0104N.

## Events and input

Every game needs `start` and `update` handlers. `start` runs when the game starts or restarts. `update` runs at `tick_ms`. Direction handlers run on press and release; A and B handlers run when those buttons are clicked.

```text
on|start
  set|score|0
end
on|update
  if|left
    move|0|-3|0
  endif
end
on|left_down
  # one-time press action
end
on|left_up
  # release action
end
on|a
  # A button action
end
```

Events: `start`, `update` (or `tick`), `left_down`, `left_up`, `up_down`, `up_up`, `down_down`, `down_up`, `right_down`, `right_up`, `a`, and `b`.

The update handler can read `left`, `right`, `up`, `down`, `button_a`, and `button_b`. Touch polling is available through `touch`, `touch_x`, and `touch_y`. `screen_x` and `screen_y` return display coordinates. `time` is elapsed seconds since launch.

## State, conditions, and random values

Variables are signed integers clamped to ±1,000,000,000. A `$` prefix is optional when reading a variable.

```text
set|score|0
add|score|10
sub|lives|1
mul|bonus|2
div|bonus|2
mod|bonus|3
random|spawn_x|0|420

if|$score|>=|100
  set|level|2
else
  set|level|1
endif
repeat|3
  add|bonus|10
endrepeat
```

Conditions support `==`, `=`, `!=`, `>`, `<`, `>=`, `<=`, and `eq`, `ne`, `gt`, `lt`, `ge`, `le`. A one-field condition such as `if|left` tests whether its value is nonzero. `random` chooses from the minimum inclusive to the maximum exclusive. Built-in values are `score`, `best`, `lives`, `level`, and `time`; `best` is read-only. Setting `score`, `lives`, or `level` updates the matching game state.

## Drawing and entities

```text
background|080D18
rect|0|80|70|24|18|56E5FF|5
circle|1|150|70|18|F356B8
image|2|player|210|90|100
move|0|-4|0
position|0|210|90
velocity|1|0|3|wrap
physics
alpha|0|190
collide|hit|0|1
```

- `rect|id|x|y|width|height|color|radius` creates or updates a rounded rectangle.
- `circle|id|x|y|diameter|color` creates a circle.
- `image|id|asset|x|y|scale_percent` creates or updates a bitmap sprite. Scale is clamped to 25–400 percent.
- `background|color` sets the play-field background. Colors are six-digit RGB hex, with or without `#`.
- `move|id|dx|dy` and `position|id|x|y` update an entity.
- `velocity|id|vx|vy[|wrap]` stores per-update movement. Without `wrap`, entities bounce at the play-field edges.
- `physics` advances all moving entities; `physics|id` advances one entity.
- `alpha|id|0_to_255` changes entity opacity.
- `collide|variable|first_id|second_id` sets a variable to 1 when visible entities overlap, otherwise 0.
- `getx`, `gety`, `getvx`, `getvy`, and `exists` read an entity property. Example: `getx|player_x|0`.
- `hide|id`, `show|id`, and `remove|id` change entity visibility or active state.
- `tween|id|target_x|target_y|duration_ms` animates an entity to a position, up to 60 seconds.

## Text and sound

```text
label|0|8|4|300|FFFFFF|SCORE $score    LIVES $lives
text|0|SCORE $score    LIVES $lives
clearlabel|0
music|music|loop
sfx|hit|12
audio_volume|9
audio_stop
```

- `label|id|x|y|width|color|text` creates a label. `text|id|text` replaces its contents. Variable substitutions such as `$score` are expanded. `clearlabel|id` clears it.
- `music|asset[|loop]` plays a declared audio asset. Looping is the default; `music|asset|once` plays once.
- `sfx|asset|volume` plays a declared sound effect at volume 0–21. A playing loop resumes after the effect ends. The device has one audio stream, so a sound effect temporarily interrupts background music rather than mixing with it.
- `audio_volume|volume` sets the game volume from 0–21. `audio_stop` stops playback and forgets the current background track.
- `over|message` and `win|message` end the run and save a new high score. A or B restarts after the game ends.

## Complete sample

`store/games/orbit-courier.cart` is a fully custom game using a BMP sprite, a looping WAV theme, collision effects, custom movement, hazards, lives, score, and increasing difficulty. Its media files are beside the cart in `store/games/orbit-courier/assets/`.

The language is deliberately bounded so downloaded games remain responsive and safe on the FNK0104N. For larger worlds, update the rules over multiple events and reuse entity IDs instead of creating unbounded objects.
