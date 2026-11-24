#include "legacy-migration.h"
#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const unsigned classes[] = {6, 7};
enum { LEGACY = 1, WRAPPED = 2 };

/* These constants are the trusted source for this public test only. HKwr
 * cannot establish whether the decoded content is authentic by itself. */
static void fixture(unsigned cls, uint8_t plain[32], uint8_t legacy[40])
{
    memset(legacy, 0, 40);
    memcpy(legacy, "HKwr", 4);
    legacy[4] = (uint8_t)cls;
    for (unsigned i = 0; i < 32; ++i) {
        plain[i] = (uint8_t)(17 * i + cls);
        legacy[8 + i] = plain[i] ^ (uint8_t)(0x5a + 37 * i);
    }
}

static int execute(sqlite3 *db, const char *sql)
{
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : EIO;
}

static int open_fixture(sqlite3 **db, const char *path, int mode)
{
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != geteuid() || (st.st_mode & 07777) != 0600) return EACCES;
    return sqlite3_open_v2(path, db, mode | SQLITE_OPEN_NOFOLLOW, NULL) == SQLITE_OK ? 0 : EIO;
}

int legacy_fixture_seed(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    if (close(fd)) return errno;

    sqlite3 *db = NULL;
    sqlite3_stmt *insert = NULL;
    int err = open_fixture(&db, path, SQLITE_OPEN_READWRITE);
    if (err) goto done;
    err = execute(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; BEGIN IMMEDIATE;"
                      "CREATE TABLE fixture(class INTEGER PRIMARY KEY CHECK(class IN (6,7)),"
                      " format INTEGER NOT NULL CHECK(format IN (1,2)), value BLOB NOT NULL)");
    if (err) goto done;
    if (sqlite3_prepare_v2(db, "INSERT INTO fixture VALUES (?1,1,?2)", -1, &insert, NULL) != SQLITE_OK) {
        err = EIO;
        goto done;
    }
    for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        uint8_t plain[32], legacy[40];
        fixture(classes[i], plain, legacy);
        if (sqlite3_bind_int(insert, 1, (int)classes[i]) != SQLITE_OK ||
            sqlite3_bind_blob(insert, 2, legacy, sizeof(legacy), SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(insert) != SQLITE_DONE || sqlite3_reset(insert) != SQLITE_OK) {
            err = EIO;
            goto done;
        }
    }
    err = execute(db, "COMMIT");
done:
    sqlite3_finalize(insert);
    if (db && sqlite3_close(db) != SQLITE_OK && !err) err = EIO;
    return err;
}

/* Both rows belong to the same transaction. A row already in the current
 * format is verified directly, independently of whether another row is old. */
static int verify_rows(sqlite3 *db, struct item_keys *keys, int expected_format,
                       sqlite3_stmt *update, void (*after_update)(void *), void *arg)
{
    sqlite3_stmt *read = NULL;
    if (sqlite3_prepare_v2(db, "SELECT format,value FROM fixture WHERE class=?1", -1, &read, NULL) != SQLITE_OK)
        return EIO;
    int err = 0;
    for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        unsigned cls = classes[i];
        uint8_t plain[32], legacy[40], wrapped[48], decoded[32];
        size_t wrapped_size = sizeof(wrapped), decoded_size = sizeof(decoded);
        fixture(cls, plain, legacy);
        if (sqlite3_bind_int(read, 1, (int)cls) != SQLITE_OK || sqlite3_step(read) != SQLITE_ROW ||
            sqlite3_column_type(read, 1) != SQLITE_BLOB) { err = EBADMSG; break; }
        int format = sqlite3_column_int(read, 0);
        int size = sqlite3_column_bytes(read, 1);
        const void *value = sqlite3_column_blob(read, 1);
        if (!value || (expected_format && format != expected_format)) { err = EBADMSG; break; }
        if (format == LEGACY) {
            if (size != sizeof(legacy) || memcmp(value, legacy, sizeof(legacy))) { err = EBADMSG; break; }
            if (!update) {
                if (sqlite3_reset(read) != SQLITE_OK) { err = EIO; break; }
                continue;
            }
            err = ik_import_fixture(keys, cls, value, (size_t)size, wrapped, &wrapped_size);
            if (err) break;
            value = wrapped;
            size = (int)wrapped_size;
        } else if (format != WRAPPED || size != sizeof(wrapped)) { err = EBADMSG; break; }

        err = ik_unwrap(keys, cls, value, (size_t)size, decoded, &decoded_size);
        if (err) break;
        if (decoded_size != sizeof(plain) || memcmp(decoded, plain, sizeof(plain))) { err = EBADMSG; break; }
        if (sqlite3_reset(read) != SQLITE_OK) { err = EIO; break; }
        if (format == WRAPPED) continue;
        if (sqlite3_bind_int(update, 1, (int)cls) != SQLITE_OK ||
            sqlite3_bind_blob(update, 2, wrapped, (int)wrapped_size, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(update) != SQLITE_DONE || sqlite3_changes(db) != 1 || sqlite3_reset(update) != SQLITE_OK) {
            err = EIO;
            break;
        }
        if (after_update) {
            if (sqlite3_db_cacheflush(db) != SQLITE_OK) { err = EIO; break; }
            after_update(arg);
        }
    }
    sqlite3_finalize(read);
    return err;
}

int legacy_fixture_verify(const char *path, struct item_keys *keys, int migrated)
{
    sqlite3 *db = NULL;
    /* A writable connection lets SQLite recover an interrupted rollback journal. */
    int err = open_fixture(&db, path, SQLITE_OPEN_READWRITE);
    if (!err) err = verify_rows(db, keys, migrated ? WRAPPED : LEGACY, NULL, NULL, NULL);
    if (db && sqlite3_close(db) != SQLITE_OK && !err) err = EIO;
    return err;
}

int legacy_fixture_migrate(const char *path, struct item_keys *keys,
                          void (*after_update)(void *), void *arg)
{
    if (!keys || !keys->a_ready || !keys->c_ready) return EACCES;
    sqlite3 *db = NULL;
    sqlite3_stmt *update = NULL;
    int err = open_fixture(&db, path, SQLITE_OPEN_READWRITE);
    if (err) goto done;
    err = execute(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
    if (err) goto done;
    if (sqlite3_prepare_v2(db, "UPDATE fixture SET format=2,value=?2 WHERE class=?1 AND format=1", -1,
                           &update, NULL) != SQLITE_OK) err = EIO;
    if (!err) err = verify_rows(db, keys, 0, update, after_update, arg);
    sqlite3_finalize(update);
    if (!err) err = execute(db, "COMMIT");
    if (err) (void)execute(db, "ROLLBACK");
done:
    if (db && sqlite3_close(db) != SQLITE_OK && !err) err = EIO;
    return err;
}
