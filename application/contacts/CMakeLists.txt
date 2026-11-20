idf_component_register(SRCS "contacts.c" "contacts_store.c" "contacts_pack.c" "contacts_fake.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES sdcard esp_hw_support)
