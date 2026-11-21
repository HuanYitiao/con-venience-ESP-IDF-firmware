#pragma once

// The old firmware's main loop housekeeping, in its own task: idle
// shutdown, the battery check, USB power, and a card going into the slot
// on the "No SD Card" page. It decides and posts fsm events; the fsm does
// the rest in its own task.

// After fsm_start().
void supervisor_start(void);

// A button press, from any task: restarts the idle timer.
void supervisor_activity(void);
