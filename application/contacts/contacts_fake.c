// Made-up contacts for UI work without an SD card.

#include <stdio.h>
#include <string.h>

#include "contacts.h"
#include "contacts_priv.h"

int contacts_fake_friends(int count)
{
    static const char *const names[] = {
        "Aurora",  "Björn",   "Chiyo",    "Dmitri", "Élodie", "Finn",     "Günther", "Hana",
        "Ingrid",  "Jørgen",  "Kasimir",  "Lumi",   "Mårten", "Nadežda", "Oskar",   "Paweł",
        "Quentin", "Ráðhild", "Søren",    "Tomás",
    };
    const int n = (int)(sizeof(names) / sizeof(names[0]));
    contacts_friend_count = count < n ? count : n;
    contacts_friends_fake = true;
    for (int i = 0; i < contacts_friend_count; i++)
    {
        snprintf(contacts_friend_uuids[i], CONTACT_UUID_LEN, "%016X", i);
        contacts_copy_utf8(contacts_friend_names[i], CONTACT_NAME_LEN, names[i]);
    }
    return contacts_friend_count;
}

void contacts_fake_friend(int index, contact_t *out)
{
    contacts_fake(out);
    contacts_copy_utf8(out->name, sizeof(out->name), contacts_friend_names[index]);
    memcpy(out->uuid, contacts_friend_uuids[index], CONTACT_UUID_LEN);
    // A different avatar per friend: shift the bands, every third has none.
    for (size_t i = 0; i < CONTACT_AVATAR_LEN_MAX; i++)
    {
        out->avatar[i] = (uint8_t)(((out->avatar[i] & 3) + index) & 3) * 0x55;
    }
    if (index % 3 == 2)
    {
        out->avatar_res = 0;
    }
}

void contacts_fake(contact_t *out)
{
    memset(out, 0, sizeof(*out));
    contacts_copy_utf8(out->name, sizeof(out->name), "Test Fox");
    contacts_copy_utf8(out->species, sizeof(out->species), "Red fox");
    contacts_copy_utf8(out->from, sizeof(out->from), "Malmö, Sweden");
    contacts_copy_utf8(out->uuid, sizeof(out->uuid), "0000000000000000");
    contacts_copy_utf8(out->links[0].platform, sizeof(out->links[0].platform), "telegram");
    contacts_copy_utf8(out->links[0].url, sizeof(out->links[0].url), "https://t.me/example");
    out->link_count = 1;

    // Diagonal bands through the four gray levels.
    out->avatar_res = CONTACT_AVATAR_RES_MAX;
    out->avatar_mode = 1;
    for (int page = 0; page < CONTACT_AVATAR_RES_MAX / 4; page++)
    {
        for (int x = 0; x < CONTACT_AVATAR_RES_MAX; x++)
        {
            uint8_t gray = ((x + page * 4) / 32) & 3;
            out->avatar[page * CONTACT_AVATAR_RES_MAX + x] = gray * 0x55;
        }
    }
}
