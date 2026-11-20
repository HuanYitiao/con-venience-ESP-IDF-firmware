#pragma once

#include <stdbool.h>

#include "contacts.h"
#include "esp_err.h"

// The setup portal on the soft AP, as the old firmware's wifi_config:
//   /              settings index (in flash, so it works with a bad card)
//   /profile       the profile generator, /web/profile-generator.html on SD
//   /jszip.min.js  next to it, /web/jszip.min.js on SD
//   /profile.json, /avatar.bin   the own profile, for editing it
//   /upload        POST multipart: "profile" and "avatar" -> own profile
//   /settime       GET a page that POSTs the phone's time back -> RTC
// HTTP handlers run in the HTTP server task.

// AP up and the routes in place. Takes a moment (the AP password needs the
// radio on).
esp_err_t portal_start(void);

void portal_stop(void);

// True once an upload saved the own profile; uuid (may be NULL) gets its id.
bool portal_saved(char uuid[CONTACT_UUID_LEN]);
