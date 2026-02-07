#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>

#define GS 8       /* grid size */
#define CELL 7     /* pixels per grid cell (6 filled + 1 gap) */
#define GX 1       /* grid x origin */
#define GY 3       /* grid y origin */
#define PX 62      /* right panel x */
#define PCELL 3    /* preview cell pixels (reduced to fit tall pieces) */
#define HAND 3     /* pieces in hand */
#define NTYPES 19  /* piece type count */
#define MAX_DIM 5  /* max piece width or height */
#define PREV_Y0 20 /* preview column start y */
#define PREV_GAP 3 /* gap between preview pieces */

typedef enum {
  PhaseSelect,
  PhasePlace,
  PhaseOver,
} Phase;

typedef struct {
  uint8_t w, h;
  uint8_t rows[MAX_DIM]; /* bitmask per row, LSB = left column */
} PieceDef;

static const PieceDef pdefs[NTYPES] = {
    /* singles & horizontal lines */
    {1, 1, {0x01}}, /*  0: 1x1  */
    {2, 1, {0x03}}, /*  1: 2h   */
    {3, 1, {0x07}}, /*  2: 3h   */
    {4, 1, {0x0F}}, /*  3: 4h   */
    {5, 1, {0x1F}}, /*  4: 5h   */
    /* vertical lines */
    {1, 2, {0x01, 0x01}},                   /*  5: 2v   */
    {1, 3, {0x01, 0x01, 0x01}},             /*  6: 3v   */
    {1, 4, {0x01, 0x01, 0x01, 0x01}},       /*  7: 4v   */
    {1, 5, {0x01, 0x01, 0x01, 0x01, 0x01}}, /*  8: 5v   */
    /* squares */
    {2, 2, {0x03, 0x03}},       /*  9: 2x2  */
    {3, 3, {0x07, 0x07, 0x07}}, /* 10: 3x3  */
    /* small L-shapes (2x2 corner) */
    {2, 2, {0x03, 0x01}}, /* 11: L-bl */
    {2, 2, {0x03, 0x02}}, /* 12: L-br */
    {2, 2, {0x01, 0x03}}, /* 13: L-tl */
    {2, 2, {0x02, 0x03}}, /* 14: L-tr */
    /* big L-shapes (3x3 corner) */
    {3, 3, {0x07, 0x01, 0x01}}, /* 15: bigL */
    {3, 3, {0x07, 0x04, 0x04}}, /* 16: bigJ */
    {3, 3, {0x01, 0x01, 0x07}}, /* 17: bigL2 */
    {3, 3, {0x04, 0x04, 0x07}}, /* 18: bigJ2 */
};

/* ── Sound sequences ─────────────────────────────────────────────── */

static const NotificationSequence seq_move = {
    &message_force_speaker_volume_setting_1f,
    &message_note_c7,
    &message_delay_10,
    &message_sound_off,
    NULL,
};

static const NotificationSequence seq_place = {
    &message_force_speaker_volume_setting_1f,
    &message_note_e6,
    &message_delay_50,
    &message_sound_off,
    NULL,
};

static const NotificationSequence seq_clear = {
    &message_force_speaker_volume_setting_1f,
    &message_note_c6,
    &message_delay_50,
    &message_note_e6,
    &message_delay_50,
    &message_note_g6,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

static const NotificationSequence seq_invalid = {
    &message_force_speaker_volume_setting_1f,
    &message_note_c5,
    &message_delay_25,
    &message_sound_off,
    &message_delay_25,
    &message_note_c5,
    &message_delay_25,
    &message_sound_off,
    NULL,
};

static const NotificationSequence seq_gameover = {
    &message_force_speaker_volume_setting_1f,
    &message_note_g5,
    &message_delay_100,
    &message_note_e5,
    &message_delay_100,
    &message_note_c5,
    &message_delay_250,
    &message_sound_off,
    NULL,
};

/* ── Game state ───────────────────────────────────────────────────── */

typedef struct {
  uint8_t grid[GS];   /* row bitmasks (8 rows, 8 bits each) */
  uint8_t hand[HAND]; /* piece-type index per slot, 0xFF = used */
  int8_t sel;         /* selected hand slot (0-2) */
  int8_t cx, cy;      /* ghost cursor on grid */
  Phase phase;
  uint32_t score;
  FuriMutex *mutex;
} Game;

static inline bool piece_bit(const PieceDef *p, uint8_t x, uint8_t y) {
  return (p->rows[y] >> x) & 1;
}

static bool can_place(const Game *g, const PieceDef *p, int8_t ox, int8_t oy) {
  if (ox < 0 || oy < 0 || ox + p->w > GS || oy + p->h > GS)
    return false;
  for (uint8_t r = 0; r < p->h; r++) {
    if (g->grid[oy + r] & (p->rows[r] << ox))
      return false;
  }
  return true;
}

static uint8_t count_bits(uint8_t v) {
  uint8_t c = 0;
  while (v) {
    c += v & 1;
    v >>= 1;
  }
  return c;
}

static void place_piece(Game *g, const PieceDef *p, int8_t ox, int8_t oy) {
  uint8_t cells = 0;
  for (uint8_t r = 0; r < p->h; r++) {
    g->grid[oy + r] |= (p->rows[r] << ox);
    cells += count_bits(p->rows[r]);
  }
  g->score += cells;
}

static bool clear_lines(Game *g) {
  uint8_t cleared = 0;
  uint8_t full_rows = 0;
  uint8_t full_cols = 0;

  for (uint8_t y = 0; y < GS; y++) {
    if (g->grid[y] == 0xFF) {
      full_rows |= (1 << y);
      cleared++;
    }
  }

  for (uint8_t x = 0; x < GS; x++) {
    bool full = true;
    for (uint8_t y = 0; y < GS; y++) {
      if (!(g->grid[y] & (1 << x))) {
        full = false;
        break;
      }
    }
    if (full) {
      full_cols |= (1 << x);
      cleared++;
    }
  }

  for (uint8_t y = 0; y < GS; y++) {
    if (full_rows & (1 << y))
      g->grid[y] = 0;
  }

  if (full_cols) {
    for (uint8_t y = 0; y < GS; y++) {
      g->grid[y] &= ~full_cols;
    }
  }

  if (cleared > 0) {
    g->score += (uint32_t)10 * cleared * cleared;
    return true;
  }
  return false;
}

/* Score a piece type by its best placement on the current board.
   Higher = more useful (completes lines, fills near-full rows/cols). */
static uint8_t score_piece_type(const Game *g, uint8_t type) {
  const PieceDef *p = &pdefs[type];
  uint8_t best = 0;
  for (int8_t y = 0; y <= GS - p->h; y++) {
    for (int8_t x = 0; x <= GS - p->w; x++) {
      if (!can_place(g, p, x, y))
        continue;
      /* simulate placement */
      uint8_t temp[GS];
      memcpy(temp, g->grid, GS);
      for (uint8_t r = 0; r < p->h; r++)
        temp[y + r] |= (p->rows[r] << x);
      /* count completed lines */
      uint8_t lines = 0;
      for (uint8_t row = 0; row < GS; row++)
        if (temp[row] == 0xFF)
          lines++;
      for (uint8_t col = 0; col < GS; col++) {
        bool full = true;
        for (uint8_t row = 0; row < GS; row++) {
          if (!(temp[row] & (1 << col))) {
            full = false;
            break;
          }
        }
        if (full)
          lines++;
      }
      /* count near-full rows/cols (6+ of 8 filled) */
      uint8_t near = 0;
      for (uint8_t row = 0; row < GS; row++)
        if (count_bits(temp[row]) >= 6 && temp[row] != 0xFF)
          near++;
      for (uint8_t col = 0; col < GS; col++) {
        uint8_t cnt = 0;
        for (uint8_t row = 0; row < GS; row++)
          if (temp[row] & (1 << col))
            cnt++;
        if (cnt >= 6 && cnt < 8)
          near++;
      }
      uint8_t s = lines * 10 + near;
      if (s > best)
        best = s;
    }
  }
  return best;
}

static void new_hand(Game *g) {
  /* score every piece type against current board */
  uint16_t weights[NTYPES];
  uint16_t total = 0;
  for (uint8_t t = 0; t < NTYPES; t++) {
    uint8_t s = score_piece_type(g, t);
    weights[t] = 3 + s; /* base weight 3 keeps randomness, score adds bias */
    total += weights[t];
  }
  /* weighted random selection per hand slot */
  for (uint8_t i = 0; i < HAND; i++) {
    uint16_t r = rand() % total;
    uint16_t cumul = 0;
    g->hand[i] = 0; /* fallback */
    for (uint8_t t = 0; t < NTYPES; t++) {
      cumul += weights[t];
      if (r < cumul) {
        g->hand[i] = t;
        break;
      }
    }
  }
}

static bool any_move_possible(const Game *g) {
  for (uint8_t i = 0; i < HAND; i++) {
    if (g->hand[i] == 0xFF)
      continue;
    const PieceDef *p = &pdefs[g->hand[i]];
    for (int8_t y = 0; y <= GS - p->h; y++) {
      for (int8_t x = 0; x <= GS - p->w; x++) {
        if (can_place(g, p, x, y))
          return true;
      }
    }
  }
  return false;
}

static bool hand_empty(const Game *g) {
  for (uint8_t i = 0; i < HAND; i++) {
    if (g->hand[i] != 0xFF)
      return false;
  }
  return true;
}

static void game_init(Game *g) {
  memset(g->grid, 0, GS);
  g->score = 0;
  g->phase = PhaseSelect;
  g->sel = 0;
  g->cx = 3;
  g->cy = 3;
  new_hand(g);
}

static void sel_first_available(Game *g) {
  for (uint8_t i = 0; i < HAND; i++) {
    if (g->hand[i] != 0xFF) {
      g->sel = i;
      return;
    }
  }
}

/* ── Drawing ──────────────────────────────────────────────────────── */

static void draw_callback(Canvas *canvas, void *ctx) {
  Game *g = ctx;
  furi_mutex_acquire(g->mutex, FuriWaitForever);
  canvas_clear(canvas);
  canvas_set_color(canvas, ColorBlack);

  /* grid border */
  canvas_draw_frame(canvas, GX - 1, GY - 1, GS * CELL + 2, GS * CELL + 2);

  /* grid cells */
  for (uint8_t y = 0; y < GS; y++) {
    for (uint8_t x = 0; x < GS; x++) {
      uint8_t px = GX + x * CELL;
      uint8_t py = GY + y * CELL;
      if (g->grid[y] & (1 << x)) {
        canvas_draw_box(canvas, px, py, CELL - 1, CELL - 1);
      } else {
        canvas_draw_dot(canvas, px + 2, py + 2);
      }
    }
  }

  /* ghost piece (XOR so it's visible over both empty and filled cells) */
  if (g->phase == PhasePlace && g->hand[g->sel] != 0xFF) {
    const PieceDef *p = &pdefs[g->hand[g->sel]];
    bool valid = can_place(g, p, g->cx, g->cy);

    canvas_set_color(canvas, ColorXOR);
    for (uint8_t r = 0; r < p->h; r++) {
      for (uint8_t c = 0; c < p->w; c++) {
        if (!piece_bit(p, c, r))
          continue;
        int8_t gx = g->cx + (int8_t)c;
        int8_t gy = g->cy + (int8_t)r;
        /* clip to grid bounds */
        if (gx < 0 || gx >= GS || gy < 0 || gy >= GS)
          continue;
        uint8_t px = GX + gx * CELL;
        uint8_t py = GY + gy * CELL;
        if (valid) {
          /* solid XOR box: inverts whatever is underneath */
          canvas_draw_box(canvas, px, py, CELL - 1, CELL - 1);
        } else {
          /* just an outline for invalid positions */
          canvas_draw_frame(canvas, px, py, CELL - 1, CELL - 1);
        }
      }
    }
    /* clear-preview: highlight rows/cols that would vanish */
    if (valid) {
      uint8_t temp[GS];
      memcpy(temp, g->grid, GS);
      for (uint8_t r = 0; r < p->h; r++)
        temp[g->cy + r] |= (p->rows[r] << g->cx);
      uint8_t clr_rows = 0, clr_cols = 0;
      for (uint8_t yy = 0; yy < GS; yy++)
        if (temp[yy] == 0xFF)
          clr_rows |= (1 << yy);
      for (uint8_t xx = 0; xx < GS; xx++) {
        bool full = true;
        for (uint8_t yy = 0; yy < GS; yy++) {
          if (!(temp[yy] & (1 << xx))) {
            full = false;
            break;
          }
        }
        if (full)
          clr_cols |= (1 << xx);
      }
      if (clr_rows || clr_cols) {
        /* checkerboard XOR over cells in clearing lines */
        for (uint8_t yy = 0; yy < GS; yy++) {
          for (uint8_t xx = 0; xx < GS; xx++) {
            if (!((clr_rows & (1 << yy)) || (clr_cols & (1 << xx))))
              continue;
            uint8_t px = GX + xx * CELL;
            uint8_t py = GY + yy * CELL;
            for (uint8_t dy = 0; dy < CELL - 1; dy++)
              for (uint8_t dx = 0; dx < CELL - 1; dx++)
                if ((dx + dy) & 1)
                  canvas_draw_dot(canvas, px + dx, py + dy);
          }
        }
      }
    }
    canvas_set_color(canvas, ColorBlack);
  }

  /* score */
  canvas_set_font(canvas, FontSecondary);
  canvas_draw_str(canvas, PX, 7, "SCORE");
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)g->score);
  canvas_draw_str(canvas, PX, 17, buf);

  /* piece preview column (dynamically positioned) */
  uint8_t cur_y = PREV_Y0;
  for (uint8_t i = 0; i < HAND; i++) {
    if (g->hand[i] == 0xFF)
      continue;
    const PieceDef *p = &pdefs[g->hand[i]];
    uint8_t bx = PX + 4;

    /* selection highlight */
    if (g->phase == PhaseSelect && i == g->sel) {
      canvas_draw_frame(canvas, bx - 2, cur_y - 2, p->w * PCELL + 3,
                        p->h * PCELL + 3);
    }

    /* draw piece preview */
    for (uint8_t r = 0; r < p->h; r++) {
      for (uint8_t c = 0; c < p->w; c++) {
        if (piece_bit(p, c, r)) {
          canvas_draw_box(canvas, bx + c * PCELL, cur_y + r * PCELL, PCELL - 1,
                          PCELL - 1);
        }
      }
    }

    cur_y += p->h * PCELL + PREV_GAP;
  }

  /* game over overlay */
  if (g->phase == PhaseOver) {
    for (uint8_t y = 14; y < 50; y++)
      for (uint8_t x = 22; x < 106; x++)
        if ((x + y) & 1)
          canvas_draw_dot(canvas, x, y);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 26, 20, 76, 24);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, 26, 20, 76, 24);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 34, 33, "GAME OVER");
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "Score: %lu", (unsigned long)g->score);
    canvas_draw_str(canvas, 34, 42, buf);
  }

  furi_mutex_release(g->mutex);
}

/* ── Input ────────────────────────────────────────────────────────── */

static void input_callback(InputEvent *event, void *ctx) {
  FuriMessageQueue *q = ctx;
  furi_message_queue_put(q, event, FuriWaitForever);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int32_t blockcast_app(void *p) {
  UNUSED(p);
  srand(DWT->CYCCNT);

  Game *g = malloc(sizeof(Game));
  g->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
  game_init(g);

  NotificationApp *notif = furi_record_open(RECORD_NOTIFICATION);

  FuriMessageQueue *queue = furi_message_queue_alloc(8, sizeof(InputEvent));

  ViewPort *vp = view_port_alloc();
  view_port_draw_callback_set(vp, draw_callback, g);
  view_port_input_callback_set(vp, input_callback, queue);

  Gui *gui = furi_record_open(RECORD_GUI);
  gui_add_view_port(gui, vp, GuiLayerFullscreen);

  bool running = true;
  InputEvent ev;

  while (running) {
    if (furi_message_queue_get(queue, &ev, 100) != FuriStatusOk)
      continue;
    if (ev.type != InputTypePress && ev.type != InputTypeRepeat)
      continue;

    furi_mutex_acquire(g->mutex, FuriWaitForever);

    switch (g->phase) {
    case PhaseSelect:
      switch (ev.key) {
      case InputKeyUp:
        for (uint8_t i = 1; i < HAND; i++) {
          int8_t idx = (g->sel - i + HAND) % HAND;
          if (g->hand[idx] != 0xFF) {
            g->sel = idx;
            break;
          }
        }
        notification_message(notif, &seq_move);
        break;
      case InputKeyDown:
        for (uint8_t i = 1; i < HAND; i++) {
          int8_t idx = (g->sel + i) % HAND;
          if (g->hand[idx] != 0xFF) {
            g->sel = idx;
            break;
          }
        }
        notification_message(notif, &seq_move);
        break;
      case InputKeyOk:
        if (g->hand[g->sel] != 0xFF) {
          const PieceDef *pc = &pdefs[g->hand[g->sel]];
          g->cx = (GS - pc->w) / 2;
          g->cy = (GS - pc->h) / 2;
          g->phase = PhasePlace;
          notification_message(notif, &seq_move);
        }
        break;
      case InputKeyBack:
        running = false;
        break;
      default:
        break;
      }
      break;

    case PhasePlace: {
      const PieceDef *pc = &pdefs[g->hand[g->sel]];
      switch (ev.key) {
      case InputKeyUp:
        if (g->cy > 0)
          g->cy--;
        break;
      case InputKeyDown:
        if (g->cy < GS - pc->h)
          g->cy++;
        break;
      case InputKeyLeft:
        if (g->cx > 0)
          g->cx--;
        break;
      case InputKeyRight:
        if (g->cx < GS - pc->w)
          g->cx++;
        break;
      case InputKeyOk:
        if (can_place(g, pc, g->cx, g->cy)) {
          place_piece(g, pc, g->cx, g->cy);
          g->hand[g->sel] = 0xFF;
          bool cleared = clear_lines(g);

          if (cleared) {
            notification_message(notif, &seq_clear);
          } else {
            notification_message(notif, &seq_place);
          }

          if (hand_empty(g))
            new_hand(g);

          if (!any_move_possible(g)) {
            g->phase = PhaseOver;
            notification_message(notif, &seq_gameover);
          } else {
            g->phase = PhaseSelect;
            sel_first_available(g);
          }
        } else {
          notification_message(notif, &seq_invalid);
        }
        break;
      case InputKeyBack:
        g->phase = PhaseSelect;
        notification_message(notif, &seq_move);
        break;
      default:
        break;
      }
      break;
    }

    case PhaseOver:
      if (ev.key == InputKeyOk) {
        game_init(g);
        notification_message(notif, &seq_move);
      } else if (ev.key == InputKeyBack) {
        running = false;
      }
      break;
    }

    furi_mutex_release(g->mutex);
    view_port_update(vp);
  }

  gui_remove_view_port(gui, vp);
  furi_record_close(RECORD_GUI);
  furi_record_close(RECORD_NOTIFICATION);
  view_port_free(vp);
  furi_message_queue_free(queue);
  furi_mutex_free(g->mutex);
  free(g);

  return 0;
}
