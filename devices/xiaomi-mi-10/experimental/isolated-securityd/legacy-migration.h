#ifndef UMI_LEGACY_MIGRATION_H
#define UMI_LEGACY_MIGRATION_H

#include "../native-keybag-transaction/item-keys.h"

/* Independent synthetic SQLite fixture. The schema is local to this test and
 * separate from Apple's persistent SecItem database. */
int legacy_fixture_seed(const char *path);
int legacy_fixture_verify(const char *path, struct item_keys *keys, int migrated);
int legacy_fixture_migrate(const char *path, struct item_keys *keys,
                          void (*after_update)(void *), void *arg);

#endif
