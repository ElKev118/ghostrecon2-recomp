/**
 * Ghost Recon 2 port - the keyboard and mouse layout (port_controls.h).
 *
 * The defaults follow the game's own controller screen (the loading
 * screen's diagram) on a PC shooter's keys: WASD to move, the mouse to look,
 * left button to fire, right button for the scope (the right stick click),
 * F for the action, Shift / Ctrl to stand up / go down, Q / E to lean, M for
 * the map, R to reload. Arrows, Enter and Backspace also drive the menus.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_controls.h"
#include "mkb_input.h"

const PortControlRow port_control_rows[] = {
    { MKB_MOVE_UP,    "Move forward" },
    { MKB_MOVE_DOWN,  "Move back" },
    { MKB_MOVE_LEFT,  "Move left" },
    { MKB_MOVE_RIGHT, "Move right" },
    { MKB_RTRIGGER,   "Fire (RT)" },
    { MKB_RTHUMB,     "Zoom (right stick click)" },
    { MKB_LTHUMB,     "Action (left stick click)" },
    { MKB_A,          "Reload / confirm (A)" },
    { MKB_B,          "Inventory / back (B)" },
    { MKB_X,          "Vision (X)" },
    { MKB_Y,          "Order / lock range (Y)" },
    { MKB_LTRIGGER,   "Reference map (LT)" },
    { MKB_WHITE,      "Fire mode / designate target (White)" },
    { MKB_BLACK,      "Regroup team / camera (Black)" },
    { MKB_DPAD_UP,    "Stance up (D-pad up)" },
    { MKB_DPAD_DOWN,  "Stance down (D-pad down)" },
    { MKB_DPAD_LEFT,  "Lean left (D-pad left)" },
    { MKB_DPAD_RIGHT, "Lean right (D-pad right)" },
    { MKB_START,      "Start (pause menu)" },
    { MKB_BACK,       "Scoreboard (Back)" },
    { MKB_LOOK_UP,    "Look up (keys)" },
    { MKB_LOOK_DOWN,  "Look down (keys)" },
    { MKB_LOOK_LEFT,  "Look left (keys)" },
    { MKB_LOOK_RIGHT, "Look right (keys)" },
};
const int port_control_row_count = sizeof port_control_rows / sizeof port_control_rows[0];

void port_controls_defaults(void)
{
    static const int k_def[][3] = {          /* action, first key, second key */
        { MKB_MOVE_UP,    'W', 0 },
        { MKB_MOVE_DOWN,  'S', 0 },
        { MKB_MOVE_LEFT,  'A', 0 },
        { MKB_MOVE_RIGHT, 'D', 0 },
        { MKB_RTRIGGER,   MKB_MOUSE_LEFT, 0 },
        { MKB_RTHUMB,     MKB_MOUSE_RIGHT, 0 },
        { MKB_LTHUMB,     'F', 0 },
        { MKB_A,          'R', VK_RETURN },
        { MKB_B,          'I', VK_BACK },
        { MKB_X,          'N', 0 },
        { MKB_Y,          'C', 0 },
        { MKB_LTRIGGER,   'M', 0 },
        { MKB_WHITE,      'B', MKB_MOUSE_MIDDLE },
        { MKB_BLACK,      'G', 0 },
        { MKB_DPAD_UP,    VK_LSHIFT, VK_UP },
        { MKB_DPAD_DOWN,  VK_LCONTROL, VK_DOWN },
        { MKB_DPAD_LEFT,  'Q', VK_LEFT },
        { MKB_DPAD_RIGHT, 'E', VK_RIGHT },
        { MKB_START,      VK_ESCAPE, 0 },
        { MKB_BACK,       VK_TAB, 0 },
    };
    int a, s, i;

    for (a = 0; a < MKB_ACTION_COUNT; a++)
        for (s = 0; s < MKB_SLOTS; s++)
            mkb_bind(a, s, 0);
    for (i = 0; i < (int)(sizeof k_def / sizeof k_def[0]); i++) {
        mkb_bind(k_def[i][0], 0, k_def[i][1]);
        mkb_bind(k_def[i][0], 1, k_def[i][2]);
    }
    mkb_set_sensitivity(1.5);
    mkb_set_invert_y(0);
}

void port_controls_load(const char *ini_path, int use_ini)
{
    char buf[64];
    int a;

    port_controls_defaults();
    if (!use_ini || !GetPrivateProfileStringA("Controls", "Version", "", buf, sizeof buf, ini_path))
        return;                                 /* no section yet: the defaults */
    GetPrivateProfileStringA("Controls", "MouseSensitivity", "", buf, sizeof buf, ini_path);
    if (buf[0])
        mkb_set_sensitivity(atof(buf));
    mkb_set_invert_y(GetPrivateProfileIntA("Controls", "InvertMouseY", 0, ini_path));
    for (a = 0; a < MKB_ACTION_COUNT; a++) {
        char *end;
        long c0, c1 = 0;
        if (!GetPrivateProfileStringA("Controls", mkb_action_key(a), "", buf, sizeof buf, ini_path))
            continue;                           /* not in the file: keep the default */
        c0 = strtol(buf, &end, 0);
        if (*end == ',')
            c1 = strtol(end + 1, NULL, 0);
        mkb_bind(a, 0, 0);
        mkb_bind(a, 1, 0);
        mkb_bind(a, 0, (int)c0);
        mkb_bind(a, 1, (int)c1);
    }
}

int port_controls_save(const char *ini_path)
{
    char b[32];
    int a, ok = 1;

    ok &= WritePrivateProfileStringA("Controls", "Version", "1", ini_path) != 0;
    snprintf(b, sizeof b, "%.2f", mkb_sensitivity());
    ok &= WritePrivateProfileStringA("Controls", "MouseSensitivity", b, ini_path) != 0;
    ok &= WritePrivateProfileStringA("Controls", "InvertMouseY", mkb_invert_y() ? "1" : "0",
                                     ini_path) != 0;
    for (a = 0; a < MKB_ACTION_COUNT; a++) {
        /* Codes, not names: a key name depends on the keyboard layout. */
        snprintf(b, sizeof b, "0x%X,0x%X", mkb_binding(a, 0), mkb_binding(a, 1));
        ok &= WritePrivateProfileStringA("Controls", mkb_action_key(a), b, ini_path) != 0;
    }
    return ok;
}
