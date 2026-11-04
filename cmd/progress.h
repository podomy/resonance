#ifndef RSUITE_PROGRESS_H
#define RSUITE_PROGRESS_H

// ProgView is one drawn row: base
// name, last phase text, live flag,
// plus 0-based phase index and total
// for known files; idx maxes at
// total-1, live rows never show N/N.
// Zero total means unknown.
typedef struct {
    const char *base;
    const char *phase;
    int live;
    int ph_idx;
    int ph_total;
} ProgView;

// Draws rows plus tail: no head line,
// no counts, no bars, no percent, no
// elapsed, no spinners. Known rows
// show phase plus 0-based [idx/total];
// unknown rows show phase only.
void prog_draw(ProgView *vs, int n);

// True when the progress view may
// run: tty stdout and no explicit
// --tap or --json flag given.
int prog_want(int tap_flag,
    int json_flag);

// True when ANSI colors may print:
// TERM not dumb and NO_COLOR unset.
// Any set value disables color.
int prog_color(void);

#endif
