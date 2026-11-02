/**
 * Ghost Recon 2 port - the keyboard and mouse layout: this game's defaults,
 * the names the menu shows, and the [Controls] section of gr2.ini. The
 * table itself lives in the toolkit (mkb_input.h), which reads it on every
 * poll, so a change in the menu works at once.
 */
#ifndef GR2_PORT_CONTROLS_H
#define GR2_PORT_CONTROLS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int         action;     /* MKB_* */
    const char *label;      /* what it does in this game */
} PortControlRow;

/* In the order the menu lists them. */
extern const PortControlRow port_control_rows[];
extern const int            port_control_row_count;

/* This game's layout into the toolkit's table (and the default mouse feel). */
void port_controls_defaults(void);
/* [Controls] from the ini when it has the section, else the defaults. */
void port_controls_load(const char *ini_path, int use_ini);
/* Write [Controls]. Returns 1 on success. */
int  port_controls_save(const char *ini_path);

#ifdef __cplusplus
}
#endif

#endif
