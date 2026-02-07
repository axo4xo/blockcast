# Block Cast for Flipper Zero

A Block Blast-inspired puzzle game for the Flipper Zero. Place pieces on an 8x8 grid and clear full rows and columns to score points.

## How to Play

You receive 3 random pieces each round. Place all 3 on the 8x8 grid before receiving new ones. Any completely filled row or column is cleared automatically. The game ends when no remaining piece can fit on the board.

### Pieces

19 piece types: single blocks, horizontal/vertical lines (2-5), 2x2 and 3x3 squares, small L-shapes, and large L-shapes.

### Scoring

- **Placement:** 1 point per cell in the placed piece
- **Line clears:** 10 x (lines cleared)^2 — clearing multiple lines at once gives a combo bonus

## Controls

| Phase | Button | Action |
| :--- | :--- | :--- |
| **Selection** | Left / Right | Highlight a piece in your hand |
| **Selection** | OK | Pick up the highlighted piece |
| **Selection** | Back | Exit app |
| **Placement** | D-Pad | Move the ghost piece on the grid |
| **Placement** | OK | Confirm placement (if valid) |
| **Placement** | Back | Cancel and return to selection |
| **Game Over** | OK | New game |
| **Game Over** | Back | Exit app |

A solid outlined ghost means valid placement. Corner dots mean the position overlaps existing blocks.

## Building

Requires [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) (Flipper Zero build tool).

```bash
# Build and launch over USB
ufbt launch

# Build .fap only
ufbt
```

The compiled `.fap` file will be in `dist/`. Copy it to `SD Card/apps/Games/` on your Flipper Zero.

## Project Structure

```
blockcast.c        — game source (single file)
application.fam    — Flipper Zero app manifest
```

## License

MIT
