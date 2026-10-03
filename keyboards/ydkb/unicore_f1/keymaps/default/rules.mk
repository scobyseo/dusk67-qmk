# Dusk67 — VIA-native keymap
#
# Plain VIA protocol 13 (quantum/via.h). No Vial dependency at all, unlike
# the vial-qmk fork where quantum/via.c refuses to compile without
# VIAL_ENABLE.
#
# Note: the .vil export had BL_TOGG bound on layer 0. This board has no
# backlight (only the single caps indicator LED on B15, driven from the VIA
# layout options in led.c), so BL_TOGG is left unbound — see keymap.c.
#
# BACKLIGHT_ENABLE is deliberately NOT set: the stock QMK backlight driver
# needs a BACKLIGHT_PIN and would drive an unrelated pad.
VIA_ENABLE = yes

