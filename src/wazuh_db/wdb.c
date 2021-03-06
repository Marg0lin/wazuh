/*
 * Wazuh SQLite integration
 * Copyright (C) 2016 Wazuh Inc.
 * June 06, 2016.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include "wdb.h"

#define BUSY_SLEEP 1
#define MAX_ATTEMPTS 1000

static const char *SQL_VACUUM = "VACUUM;";
static const char *SQL_INSERT_INFO = "INSERT INTO info (key, value) VALUES (?, ?);";
static const char *SQL_BEGIN = "BEGIN;";
static const char *SQL_COMMIT = "COMMIT;";

sqlite3 *wdb_global = NULL;

/* Open global database. Returns 0 on success or -1 on failure. */
int wdb_open_global() {
    char dir[OS_FLSIZE + 1];

    if (!wdb_global) {
        // Database dir
        snprintf(dir, OS_FLSIZE, "%s%s/%s", isChroot() ? "/" : "", WDB_DIR, WDB_GLOB_NAME);

        // Connect to the database

        if (sqlite3_open_v2(dir, &wdb_global, SQLITE_OPEN_READWRITE, NULL)) {
            mdebug1("Global database not found, creating.");
            sqlite3_close_v2(wdb_global);
            wdb_global = NULL;

            if (wdb_create_global(dir) < 0) {
                wdb_global = NULL;
                return -1;
            }

            // Retry to open

            if (sqlite3_open_v2(dir, &wdb_global, SQLITE_OPEN_READWRITE, NULL)) {
                merror("Can't open SQLite database '%s': %s", dir, sqlite3_errmsg(wdb_global));
                sqlite3_close_v2(wdb_global);
                wdb_global = NULL;
                return -1;
            }
        }
    }

    sqlite3_busy_timeout(wdb_global, BUSY_SLEEP);
    return 0;
}

/* Close global database */
void wdb_close_global() {
    sqlite3_close_v2(wdb_global);
    wdb_global = NULL;
}

/* Open database for agent */
sqlite3* wdb_open_agent(int id_agent, const char *name) {
    char dir[OS_FLSIZE + 1];
    sqlite3 *db;

    snprintf(dir, OS_FLSIZE, "%s%s/agents/%03d-%s.db", isChroot() ? "/" : "", WDB_DIR, id_agent, name);

    if (sqlite3_open_v2(dir, &db, SQLITE_OPEN_READWRITE, NULL)) {
        mdebug1("No SQLite database found for agent '%s', creating.", name);
        sqlite3_close_v2(db);

        if (wdb_create_agent_db(id_agent, name) < 0) {
            merror("Couldn't create SQLite database '%s'", dir);
            sqlite3_close_v2(db);
            return NULL;
        }

        // Retry to open

        if (sqlite3_open_v2(dir, &db, SQLITE_OPEN_READWRITE, NULL)) {
            merror("Can't open SQLite database '%s': %s", dir, sqlite3_errmsg(db));
            sqlite3_close_v2(db);
            return NULL;
        }

    } else
        sqlite3_busy_timeout(db, BUSY_SLEEP);


    return db;
}

/* Get agent name from location string */
char* wdb_agent_loc2name(const char *location) {
    char *name;
    char *end;

    switch (location[0]) {
    case 'r':
    case 's':
        if (!(strncmp(location, "syscheck", 8) && strncmp(location, "rootcheck", 9)))
            return strdup("localhost");
            else
            return NULL;

    case '(':
        name = strdup(location + 1);

        if ((end = strchr(name, ')')))
            *end = '\0';
        else {
            free(name);
            name = NULL;
        }

        return name;

    default:
        return NULL;
    }
}

/* Prepare SQL query with availability waiting */
int wdb_prepare(sqlite3 *db, const char *zSql, int nByte, sqlite3_stmt **stmt, const char **pzTail) {
    int result;
    int attempts;

    for (attempts = 0; (result = sqlite3_prepare_v2(db, zSql, nByte, stmt, pzTail)) == SQLITE_BUSY; attempts++) {
        if (attempts == MAX_ATTEMPTS) {
            mdebug1("Maximum attempts exceeded for sqlite3_prepare_v2()");
            return -1;
        }
    }

    return result;
}

/* Execute statement with availability waiting */
int wdb_step(sqlite3_stmt *stmt) {
    int result;
    int attempts;

    for (attempts = 0; (result = sqlite3_step(stmt)) == SQLITE_BUSY; attempts++) {
        if (attempts == MAX_ATTEMPTS) {
            mdebug1("Maximum attempts exceeded for sqlite3_step()");
            return -1;
        }
    }

    return result;
}

/* Begin transaction */
int wdb_begin(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    int result;

    if (wdb_prepare(db, SQL_BEGIN, -1, &stmt, NULL)) {
        mdebug1("SQLite: %s", sqlite3_errmsg(db));
        return -1;
    }

    result = wdb_step(stmt) != SQLITE_DONE;
    sqlite3_finalize(stmt);
    return result;
}

/* Commit transaction */
int wdb_commit(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    int result;

    if (wdb_prepare(db, SQL_COMMIT, -1, &stmt, NULL)) {
        mdebug1("SQLite: %s", sqlite3_errmsg(db));
        return -1;
    }

    result = wdb_step(stmt) != SQLITE_DONE;
    sqlite3_finalize(stmt);
    return result;
}

/* Create global database */
int wdb_create_global(const char *path) {
    char max_agents[16];
    snprintf(max_agents, 15, "%d", MAX_AGENTS);

    if (wdb_create_file(path, schema_global_sql) < 0)
        return -1;
    else if (wdb_insert_info("max_agents", max_agents) < 0)
        return -1;
    else if (wdb_insert_info("openssl_support", "yes") < 0)
        return -1;
    else
        return 0;
}

/* Create profile database */
int wdb_create_profile(const char *path) {
    return wdb_create_file(path, schema_agents_sql);
}

/* Create new database file from SQL script */
int wdb_create_file(const char *path, const char *source) {
    const char *ROOT = "root";
    const char *sql;
    const char *tail;
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int result;
    uid_t uid;
    gid_t gid;

    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL)) {
        mdebug1("Couldn't create SQLite database '%s': %s", path, sqlite3_errmsg(db));
        sqlite3_close_v2(db);
        return -1;
    }

    for (sql = source; sql && *sql; sql = tail) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, &tail) != SQLITE_OK) {
            mdebug1("Preparing statement: %s", sqlite3_errmsg(db));
            sqlite3_close_v2(db);
            return -1;
        }

        result = sqlite3_step(stmt);

        switch (result) {
        case SQLITE_MISUSE:
        case SQLITE_ROW:
        case SQLITE_DONE:
            break;
        default:
            mdebug1("Stepping statement: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            sqlite3_close_v2(db);
            return -1;

        }

        sqlite3_finalize(stmt);
    }

    sqlite3_close_v2(db);

    uid = Privsep_GetUser(ROOT);
    gid = Privsep_GetGroup(GROUPGLOBAL);

    if (uid == (uid_t) - 1 || gid == (gid_t) - 1) {
        merror(USER_ERROR, ROOT, GROUPGLOBAL);
        return -1;
    }

    if (chown(path, uid, gid) < 0) {
        merror(CHOWN_ERROR, path, errno, strerror(errno));
        return -1;
    }

    if (chmod(path, 0660) < 0) {
        merror(CHMOD_ERROR, path, errno, strerror(errno));
        return -1;
    }

    return 0;
}

/* Rebuild database. Returns 0 on success or -1 on error. */
int wdb_vacuum(sqlite3 *db) {
    sqlite3_stmt *stmt;
    int result;

    if (!wdb_prepare(db, SQL_VACUUM, -1, &stmt, NULL)) {
        result = wdb_step(stmt) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(stmt);
    } else {
        mdebug1("SQLite: %s", sqlite3_errmsg(db));
        result = -1;
    }

    sqlite3_close_v2(db);
    return result;
}

/* Insert key-value pair into info table */
int wdb_insert_info(const char *key, const char *value) {
    int result = 0;
    sqlite3_stmt *stmt;

    if (wdb_open_global() < 0)
        return -1;

    if (wdb_prepare(wdb_global, SQL_INSERT_INFO, -1, &stmt, NULL)) {
        mdebug1("SQLite: %s", sqlite3_errmsg(wdb_global));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, key, -1, NULL);
    sqlite3_bind_text(stmt, 2, value, -1, NULL);

    result = wdb_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    wdb_close_global();
    return result;
}
