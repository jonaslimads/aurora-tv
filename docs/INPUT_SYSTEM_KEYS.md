# System keys: the TV remote vs. the stream

Aurora can either leave the TV's system keys with webOS, or claim them for the stream.
On webOS the claim is not app-local — it changes what the compositor delivers to the TV
itself, so getting it wrong makes the television uncontrollable while Aurora runs. This
document records what each knob does and why all of them are opt-in.

## The three capture mechanisms

| Mechanism | Where | Effect when enabled |
|---|---|---|
| `SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_HOME` (+ `_RIBBON=false`) | `src/app/app.c`, at init | webOS hands Home/Win to the app for the whole app session. The remote's Home button no longer opens the launcher. |
| `SDL_SetWindowGrab` + `SDL_SetWindowKeyboardGrab` | `src/app/ui/streaming/streaming.controller.c`, at stream start | The webOS SDL port implements this as a Wayland `keyboard-shortcuts-inhibit`: the compositor stops consuming its own shortcuts while streaming. Home, Back and the input-source ribbon reach the game instead of the TV. |
| `EVIOCGRAB` on USB keyboards | `src/app/platform/webos/keyboard_evdev.c`, at stream start | Only real USB keyboard nodes (`KEY_A` + `KEY_LEFTCTRL`, name not `LGE`/`M-RCU`/`Builtin`/`gpio`) are taken. Gives the stream F1–F12/Insert. The Magic Remote is never touched. |

`KEYS_BACK` and `KEYS_EXIT` are always enabled: Back is how the app exits a stream, and
without the exit policy the app cannot be closed at all.

## Configuration

**Settings → Input → "Capture system keys"** (`syskey_capture`, ini key
`[input] syskey_capture`), **off by default on every platform**, including webOS.

- Off (default): the remote keeps controlling the TV. USB keyboards still deliver
  F1–F12/Insert to the stream through the evdev claim.
- On: Win/Meta reach the host, Home is delivered to the app, and the stream inhibits the
  TV's keyboard shortcuts. The remote drives only the game until the stream ends and the
  app is restarted.

The access-policy hints are latched when the window is created, so changing the setting
needs an app restart.

## Regression record (v1.3.0, fixed in 1.4.1)

Upstream v1.3.0 made two of these mechanisms unconditional on webOS:

- `config->syskey_capture = true` in the webOS default block (`app_settings.c`), turning
  an opt-in into the default;
- `SDL_SetWindowGrab(...TRUE)` + `SDL_SetWindowKeyboardGrab(...TRUE)` executed for
  `TARGET_WEBOS` without consulting the setting, with the comment "on webOS always grab
  while streaming".

Result on a real TV (webOS 6.12.44 kernel, Magic Remote + Bluetooth DualShock 4): the
remote's system keys went into the game and the television was unusable while Aurora ran,
even with `syskey_capture = false` persisted in `conf/moonlight.ini`. The webOS keyboard
claim in `keyboard_evdev.c` was ruled out by reading the capability bits of all
`/dev/input/event*` nodes on the TV — none of them matches the USB-keyboard rule, and
`EVIOCGRAB` probes showed the LGE remote nodes free while Aurora ran.

Do not make either mechanism unconditional again. If a feature needs F-keys, use the
evdev keyboard claim; it does not take anything away from the TV.
