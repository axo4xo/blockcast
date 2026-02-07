#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdlib.h>
#include <string.h>

#define GS 8      /* grid size */
#define CELL 7    /* pixels per grid cell (6 filled + 1 gap) */
#define GX 1      /* grid x origin */
#define GY 3      /* grid y origin */
#define PX 62     /* right panel x */
#define PCELL 4   /* preview cell pixels */
#define HAND 3    /* pieces in hand */
#define NTYPES 19 /* piece type count */
#define MAX_DIM 5 /* max piece width or height */

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

static void clear_lines(Game *g) {
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
  }
}

static void new_hand(Game *g) {
  for (uint8_t i = 0; i < HAND; i++) {
    g->hand[i] = rand() % NTYPES;
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


static void draw_callback(Canvas *canvas, void *ctx) {
  Game *g = ctx;
  furi_mutex_acquire(g->mutex, FuriWaitForever);
  canvas_clear(canvas);
  canvas_set_color(canvas, ColorBlack);

  canvas_draw_frame(canvas, GX - 1, GY - 1, GS * CELL + 2, GS * CELL + 2);

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

  if (g->phase == PhasePlace && g->hand[g->sel] != 0xFF) {
    const PieceDef *p = &pdefs[g->hand[g->sel]];
    bool valid = can_place(g, p, g->cx, g->cy);

    for (uint8_t r = 0; r < p->h; r++) {
      for (uint8_t c = 0; c < p->w; c++) {
        if (!piece_bit(p, c, r))
          continue;
        uint8_t px = GX + (g->cx + c) * CELL;
        uint8_t py = GY + (g->cy + r) * CELL;
        if (valid) {
          canvas_draw_frame(canvas, px, py, CELL - 1, CELL - 1);
          for (uint8_t dy = 1; dy < CELL - 2; dy += 2)
            for (uint8_t dx = 1; dx < CELL - 2; dx += 2)
              canvas_draw_dot(canvas, px + dx, py + dy);
        } else {
          canvas_draw_dot(canvas, px, py);
          canvas_draw_dot(canvas, px + CELL - 2, py);
          canvas_draw_dot(canvas, px, py + CELL - 2);
          canvas_draw_dot(canvas, px + CELL - 2, py + CELL - 2);
        }
      }
    }
  }

  canvas_set_font(canvas, FontSecondary);
  canvas_draw_str(canvas, PX, 7, "SCORE");
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)g->score);
  canvas_draw_str(canvas, PX, 17, buf);

  static const uint8_t prev_y[HAND] = {22, 38, 54};
  for (uint8_t i = 0; i < HAND; i++) {
    if (g->hand[i] == 0xFF)
      continue;
    const PieceDef *p = &pdefs[g->hand[i]];
    uint8_t bx = PX + 6;
    uint8_t by = prev_y[i];

    if (g->phase == PhaseSelect && i == g->sel) {
      canvas_draw_frame(canvas, bx - 2, by - 2, p->w * PCELL + 3,
                        p->h * PCELL + 3);
    }

    for (uint8_t r = 0; r < p->h; r++) {
      for (uint8_t c = 0; c < p->w; c++) {
        if (piece_bit(p, c, r)) {
          canvas_draw_box(canvas, bx + c * PCELL, by + r * PCELL, PCELL - 1,
                          PCELL - 1);
        }
      }
    }
  }

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


static void input_callback(InputEvent *event, void *ctx) {
  FuriMessageQueue *q = ctx;
  furi_message_queue_put(q, event, FuriWaitForever);
}


int32_t blockcast_app(void *p) {
  UNUSED(p);
  srand(DWT->CYCCNT);

  Game *g = malloc(sizeof(Game));
  g->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
  game_init(g);

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
      case InputKeyLeft:
        for (uint8_t i = 1; i < HAND; i++) {
          int8_t idx = (g->sel - i + HAND) % HAND;
          if (g->hand[idx] != 0xFF) {
            g->sel = idx;
            break;
          }
        }
        break;
      case InputKeyRight:
        for (uint8_t i = 1; i < HAND; i++) {
          int8_t idx = (g->sel + i) % HAND;
          if (g->hand[idx] != 0xFF) {
            g->sel = idx;
            break;
          }
        }
        break;
      case InputKeyOk:
        if (g->hand[g->sel] != 0xFF) {
          const PieceDef *pc = &pdefs[g->hand[g->sel]];
          g->cx = (GS - pc->w) / 2;
          g->cy = (GS - pc->h) / 2;
          g->phase = PhasePlace;
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
          clear_lines(g);

          if (hand_empty(g))
            new_hand(g);

          if (!any_move_possible(g)) {
            g->phase = PhaseOver;
          } else {
            g->phase = PhaseSelect;
            sel_first_available(g);
          }
        }
        break;
      case InputKeyBack:
        g->phase = PhaseSelect;
        break;
      default:
        break;
      }
      break;
    }

    case PhaseOver:
      if (ev.key == InputKeyOk) {
        game_init(g);
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
  view_port_free(vp);
  furi_message_queue_free(queue);
  furi_mutex_free(g->mutex);
  free(g);

  return 0;
}
