#ifndef RSUITE_PROGRESS_H
#define RSUITE_PROGRESS_H

// ProgView is one drawn row: base
// name, last phase text, live flag,
// plus 1-based phase index and total
// for known files; idx displays as
// idx+1, maxing at total, live rows
// never show N/N. Zero total means
// unknown. Frac num plus den show
// sub-phase spot when den is past
// zero; fr_idx is the frac phase.
// Done marks a just done row with
// dkind 0 pass, 1 fail, 2 skip.
typedef struct {
    const char *base;
    const char *phase;
    int live;
    int ph_idx;
    int ph_total;
    long fr_num;
    long fr_den;
    long fr_idx;
    int done;
    int dkind;
} ProgView;

// Draws rows plus tail, dirty only:
// skips all output when live set,
// phase texts, frac values, and tail
// count match the prior draw; Prev
// stays when skipping. No head line,
// no counts, no bars, no percent, no
// elapsed, no spinners. Known live
// rows show phase plus 1-based
// stage [idx+1/total] plus frac
// num/den when den is past zero;
// unknown live rows show phase only
// plus frac when set. Done rows show
// once as ok green, not ok red, skip
// dim. Mute prints text plus newline
// only, never CR.
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
