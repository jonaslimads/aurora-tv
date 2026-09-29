Claro. O repositório que você instalou na LG C5 é:

[GuiDev1994/aurora-tv — GitHub](https://github.com/GuiDev1994/aurora-tv?utm_source=chatgpt.com)

E o `repo.json` para o Homebrew Channel, embora você não precise dele no seu setup via Dev Manager:

[Aurora TV repo.json](https://raw.githubusercontent.com/GuiDev1994/aurora-tv/main/repo.json?utm_source=chatgpt.com)

Para entregar a outro agente/Claude Code/Codex trabalhando diretamente no repo, eu usaria este handoff:

# Aurora TV — Gamepad Input UX Fix

Repository:
https://github.com/GuiDev1994/aurora-tv

Target hardware:
- LG C5 / webOS
- DualShock 4 connected directly to the TV
- Aurora streaming from an Apollo host
- Gamepad itself works correctly in games.

## Current observed behavior

The controller works normally in streamed games.

Aurora's overlay can also be opened successfully using the controller.

However, Virtual Mouse / on-screen keyboard handling has several UX/input problems.

### 1. Virtual Mouse toggle

Virtual Mouse can be enabled/disabled through the Aurora overlay.

However, the documented/expected gamepad shortcut:

    L1 + R3

does not toggle Virtual Mouse.

When Virtual Mouse is configured to start enabled, it simply remains enabled until changed through the overlay.

Investigate whether this shortcut still exists in the current implementation and whether the documentation and implementation have diverged.

### 2. Keyboard shortcut

While Virtual Mouse is active, pressing:

    Triangle

directly opens the webOS/Aurora on-screen keyboard.

This currently works.

However, there is no obvious controller-only way to close the keyboard without affecting the streamed game.

The overlay cannot be opened while the keyboard is active.

### 3. Circle leaks to the streamed game

Pressing Circle on the DualShock 4 closes the on-screen keyboard.

However, the same Circle/B input is also forwarded to the streaming host/game.

This causes real problems.

Example:

    Game asks for player name
        ↓
    Open Aurora keyboard
        ↓
    Enter player name
        ↓
    Press Circle to dismiss keyboard
        ↓
    Keyboard closes
        ↓
    Circle/B is also received by the game
        ↓
    Game interprets it as Cancel/Back

This makes some text-entry screens difficult or impossible to use correctly from the controller.

## Desired behavior

Please inspect the current input implementation first rather than assuming the documentation matches the code.

Trace:

- webOS gamepad events
- DS4 button mapping
- streaming/gamepad forwarding
- Virtual Mouse input handling
- on-screen keyboard handling
- overlay input handling
- event consumption / propagation

### Desired shortcuts

Prefer:

    L1 + R3
        Toggle Virtual Mouse

    R1 + R3
        Toggle on-screen keyboard

These shortcut button events must be consumed by Aurora and must NOT be forwarded to the streaming host.

The shortcuts should work both for enabling and disabling their respective modes.

### Keyboard dismissal

When the on-screen keyboard is currently open:

    Circle / B
        → close keyboard
        → consume the input locally
        → DO NOT forward B/Circle to the streaming host

This is important.

The button used to dismiss Aurora's own UI must not simultaneously perform an action inside the streamed game.

### Triangle behavior

Currently Triangle opens the keyboard while Virtual Mouse is active.

Investigate whether this is intentional/currently documented.

Ideally, with `R1 + R3` available as a keyboard toggle, avoid unnecessarily stealing Triangle from the streamed game.

If retaining Triangle-to-keyboard is desirable for compatibility, make sure its interception is limited to the intended Virtual Mouse/UI state and does not interfere with normal gameplay.

## Important implementation requirement

Do not fix this by merely reacting after the button has already been sent to the host.

The relevant hotkey/dismissal event should be intercepted and consumed before normal gamepad forwarding.

Conceptually:

    receive controller event
        ↓
    Aurora UI/hotkey handler
        ↓
    if consumed:
        perform Aurora action
        return
        ↓
    otherwise:
        forward to streaming host

For keyboard dismissal, behavior should effectively be:

    if keyboard_is_open && button == B:
        close_keyboard()
        consume_event()
        return

Use the actual architecture/APIs present in this repository rather than implementing this pseudocode literally.

## Documentation discrepancy

Also inspect the repository documentation.

There appears to be a discrepancy between current documentation/pages regarding controller shortcuts, including references to:

    LB + RS → Virtual Mouse
    RB + RS → Keyboard

Compare the actual implementation with README/description documentation and update documentation if necessary.

## Deliverables

1. Identify the exact source files/functions responsible for these behaviors.
2. Explain why `L1 + R3` currently does not toggle Virtual Mouse on the tested setup.
3. Explain why Circle/B used to dismiss the keyboard leaks through to the streamed game.
4. Implement the smallest clean fix.
5. Preserve existing overlay functionality.
6. Preserve normal DS4/gamepad behavior during gameplay.
7. Build the webOS ARM `.ipk`.
8. Report the generated `.ipk` path/name so it can be installed through webOS Dev Manager.
9. Summarize changed files and behavior.

Do not make unrelated refactors.

Esse handoff deixa uma coisa importante aberta para o agente: **primeiro verificar o fonte de verdade**, em vez de assumir que nossa interpretação do README é a implementação atual. Isso é especialmente importante porque já vimos comportamento na sua C5 que não bate perfeitamente com a documentação.
