/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/hid_flags.h"

DEFINE_bool(guide_button, true, "Forward guide button presses to guest.",
            "HID");

// 1099z17559-2: TEST-ONLY input source. Automated runs must never read the
// user's controller, so they use --hid=nop; this makes the nop driver report a
// connected gamepad on slot 0 whose buttons follow a fixed script. It feeds the
// host InputSystem only - everything the guest sees still comes through the
// kernel's XInputd exports. Not a model of any device.
DEFINE_string(hid_test_pad_script, "",
              "TEST-ONLY (nop driver): \"ms:hexbuttons[:hold_ms],...\" - a "
              "scripted gamepad on slot 0. ms is measured from driver setup; "
              "buttons are XInput bits (0400 = Guide, 1000 = A); hold defaults "
              "to 150 ms. Empty = no device (the nop driver's normal behaviour).",
              "HID");
