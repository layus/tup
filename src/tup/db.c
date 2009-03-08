#define _ATFILE_SOURCE
#include "db.h"
#include "db_util.h"
#include "array_size.h"
#include "list.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sqlite3.h>

#define DB_VERSION 1

enum {
	DB_BEGIN,
	DB_COMMIT,
	DB_ROLLBACK,
	DB_SELECT_NODE_BY_FLAGS_1,
	DB_SELECT_NODE_BY_FLAGS_2,
	DB_SELECT_NODE_BY_FLAGS_3,
	DB_SELECT_NODE_DIR,
	DB_SELECT_NODE_DIR_GLOB,
	DB_DELETE_NODE,
	DB_OPEN_TUPID,
	DB_GET_PATH,
	DB_PARENT,
	DB_ADD_CREATE_LIST,
	DB_ADD_MODIFY_LIST,
	DB_ADD_DELETE_LIST,
	DB_IN_CREATE_LIST,
	DB_IN_MODIFY_LIST,
	DB_IN_DELETE_LIST,
	DB_UNFLAG_CREATE,
	DB_UNFLAG_MODIFY,
	DB_UNFLAG_DELETE,
	DB_DELETE_DIR,
	_DB_GET_RECURSE_DIRS,
	DB_GET_DEST_LINKS,
	DB_GET_SRC_LINKS,
	DB_LINK_EXISTS,
	DB_IS_ROOT_NODE,
	DB_DELETE_LINKS,
	DB_OR_DIRCMD_FLAGS,
	DB_SET_CMD_OUTPUT_FLAGS,
	DB_MODIFY_CMDS_BY_OUTPUT,
	DB_MODIFY_CMDS_BY_INPUT,
	DB_SET_DEPENDENT_DIR_FLAGS,
	DB_MODIFY_DELETED_DEPS,
	DB_SELECT_NODE_BY_LINK,
	DB_DELETE_DEPENDENT_DIR_LINKS,
	DB_CONFIG_SET_INT,
	DB_CONFIG_GET_INT,
	DB_CONFIG_SET_INT64,
	DB_CONFIG_GET_INT64,
	DB_CONFIG_SET_STRING,
	DB_CONFIG_GET_STRING,
	DB_SET_VAR,
	DB_GET_VAR,
	DB_GET_VAR_ID,
	DB_GET_VARLEN,
	DB_WRITE_VAR,
	DB_FLAG_DELETED_VAR_DEPENDENT_DIRS,
	DB_VAR_FOREACH,
	DB_ATTACH_TMPDB,
	DB_DETACH_TMPDB,
	DB_FILES_TO_TMPDB,
	DB_UNFLAG_TMPDB,
	DB_GET_ALL_IN_TMPDB,
	_DB_NODE_INSERT,
	_DB_NODE_SELECT,
	_DB_LINK_INSERT,
	DB_NUM_STATEMENTS
};

static sqlite3 *tup_db = NULL;
static sqlite3_stmt *stmts[DB_NUM_STATEMENTS];
static tupid_t node_insert(tupid_t dt, const char *name, int len, int type);
static int node_select(tupid_t dt, const char *name, int len,
		       struct db_node *dbn);

static int link_insert(tupid_t a, tupid_t b);
static int no_sync(void);
static int get_recurse_dirs(tupid_t dt, struct list_head *list);

int tup_db_open(void)
{
	int rc;
	int version;
	int x;

	rc = sqlite3_open_v2(TUP_DB_FILE, &tup_db, SQLITE_OPEN_READWRITE, NULL);
	if(rc != 0) {
		fprintf(stderr, "Unable to open database: %s\n",
			sqlite3_errmsg(tup_db));
	}
	for(x=0; x<ARRAY_SIZE(stmts); x++) {
		stmts[x] = NULL;
	}

	if(tup_db_config_get_int("db_sync") == 0) {
		if(no_sync() < 0)
			return -1;
	}
	version = tup_db_config_get_int("db_version");
	if(version < 0) {
		fprintf(stderr, "Error getting .tup/db version.\n");
		return -1;
	}
	if(version != DB_VERSION) {
		fprintf(stderr, "Error: Database version %i not compatible with %i\n", version, DB_VERSION);
		return -1;
	}

	/* TODO: better to figure out concurrency access issues? Maybe a full
	 * on flock on the db would work?
	 */
	sqlite3_busy_timeout(tup_db, 500);
	return rc;
}

int tup_db_close(void)
{
	return db_close(tup_db, stmts, ARRAY_SIZE(stmts));
}

int tup_db_create(int db_sync)
{
	int rc;
	int x;
	const char *sql[] = {
		"create table node (id integer primary key not null, dir integer not null, type integer not null, name varchar(4096))",
		"create table link (from_id integer, to_id integer)",
		"create table var (id integer primary key not null, value varchar(4096))",
		"create table config(lval varchar(256) unique, rval varchar(256))",
		"create table create_list (id integer primary key not null)",
		"create table modify_list (id integer primary key not null)",
		"create table delete_list (id integer primary key not null)",
		"create index node_dir_index on node(dir, name)",
		"create index link_index on link(from_id)",
		"create index link_index2 on link(to_id)",
		"insert into config values('show_progress', 1)",
		"insert into config values('keep_going', 0)",
		"insert into config values('db_sync', 1)",
		"insert into config values('db_version', 0)",
		"insert into config values('autoupdate', 0)",
		"insert into config values('num_jobs', 1)",
		"insert into node values(1, 0, 2, '.')",
		"insert into node values(2, 1, 2, '@')",
	};

	rc = sqlite3_open(TUP_DB_FILE, &tup_db);
	if(rc == 0) {
		printf(".tup repository initialized.\n");
	} else {
		fprintf(stderr, "Unable to create database: %s\n",
			sqlite3_errmsg(tup_db));
	}

	if(db_sync == 0) {
		if(no_sync() < 0)
			return -1;
	}

	for(x=0; x<ARRAY_SIZE(sql); x++) {
		char *errmsg;
		if(sqlite3_exec(tup_db, sql[x], NULL, NULL, &errmsg) != 0) {
			fprintf(stderr, "SQL error: %s\nQuery was: %s",
				errmsg, sql[x]);
			return -1;
		}
	}

	if(db_sync == 0) {
		if(tup_db_config_set_int("db_sync", 0) < 0)
			return -1;
	}
	if(tup_db_config_set_int("db_version", DB_VERSION) < 0)
		return -1;

	return 0;
}

int tup_db_begin(void)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_BEGIN];
	static char s[] = "begin";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	return 0;
}

int tup_db_commit(void)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_COMMIT];
	static char s[] = "commit";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	return 0;
}

int tup_db_rollback(void)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_ROLLBACK];
	static char s[] = "rollback";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	return 0;
}

int tup_db_select(int (*callback)(void *, int, char **, char **),
		  void *arg, const char *sql, ...)
{
	va_list ap;
	int rc;
	char *buf;
	char *errmsg;

	if(!tup_db) {
		fprintf(stderr, "Error: tup_db not opened.\n");
		return -1;
	}

	va_start(ap, sql);
	buf = sqlite3_vmprintf(sql, ap);
	va_end(ap);

	rc = sqlite3_exec(tup_db, buf, callback, arg, &errmsg);
	if(rc != 0) {
		fprintf(stderr, "SQL select error: %s\nQuery was: %s\n",
			errmsg, buf);
		sqlite3_free(errmsg);
	}
	sqlite3_free(buf);
	return rc;
}

tupid_t tup_db_create_node(tupid_t dt, const char *name, int type)
{
	return tup_db_create_node_part(dt, name, -1, type);
}

tupid_t tup_db_create_node_part(tupid_t dt, const char *name, int len, int type)
{
	struct db_node dbn;
	tupid_t tupid;

	if(node_select(dt, name, len, &dbn) < 0) {
		return -1;
	}

	if(dbn.tupid != -1) {
		if(dbn.type != type) {
			fprintf(stderr, "Error: Attempt to insert node '%s' with type %i, which already exists as type %i\n", name, type, dbn.type);
			return -1;
		}
		if(tup_db_unflag_delete(dbn.tupid) < 0)
			return -1;
		return dbn.tupid;
	}

	tupid = node_insert(dt, name, len, type);
	if(tupid < 0)
		return -1;
	return tupid;
}

tupid_t tup_db_create_dup_node(tupid_t dt, const char *name, int type)
{
	tupid_t tupid;
	tupid = node_insert(dt, name, -1, type);
	if(tupid < 0)
		return -1;
	if(tup_db_unflag_modify(tupid) < 0)
		return -1;
	return tupid;
}

tupid_t tup_db_select_node(tupid_t dt, const char *name)
{
	struct db_node dbn;

	if(node_select(dt, name, -1, &dbn) < 0) {
		return -1;
	}

	return dbn.tupid;
}

int tup_db_select_dbn(tupid_t dt, const char *name, struct db_node *dbn)
{
	if(node_select(dt, name, -1, dbn) < 0)
		return -1;

	dbn->dt = dt;
	dbn->name = name;

	return 0;
}

tupid_t tup_db_select_node_part(tupid_t dt, const char *name, int len)
{
	struct db_node dbn;

	if(node_select(dt, name, len, &dbn) < 0) {
		return -1;
	}

	return dbn.tupid;
}

int tup_db_select_node_by_flags(int (*callback)(void *, struct db_node *),
				void *arg, int flags)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt;
	static char s1[] = "select id, dir, name, type from node where id in (select * from create_list)";
	static char s2[] = "select id, dir, name, type from node where id in (select * from modify_list)";
	static char s3[] = "select id, dir, name, type from node where id in (select * from delete_list)";
	char *sql;
	int sqlsize;

	if(flags == TUP_FLAGS_CREATE) {
		stmt = &stmts[DB_SELECT_NODE_BY_FLAGS_1];
		sql = s1;
		sqlsize = sizeof(s1);
	} else if(flags == TUP_FLAGS_MODIFY) {
		stmt = &stmts[DB_SELECT_NODE_BY_FLAGS_2];
		sql = s2;
		sqlsize = sizeof(s2);
	} else if(flags == TUP_FLAGS_DELETE) {
		stmt = &stmts[DB_SELECT_NODE_BY_FLAGS_3];
		sql = s3;
		sqlsize = sizeof(s3);
	} else {
		fprintf(stderr, "Error: tup_db_select_node_by_flags() must specify exactly one of TUP_FLAGS_CREATE/MODIFY/DELETE\n");
		return -1;
	}

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, sql, sqlsize, stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), sql);
			return -1;
		}
	}

	while(1) {
		struct db_node dbn;

		dbrc = sqlite3_step(*stmt);
		if(dbrc == SQLITE_DONE) {
			rc = 0;
			goto out_reset;
		}
		if(dbrc != SQLITE_ROW) {
			fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
			rc = -1;
			goto out_reset;
		}

		dbn.tupid = sqlite3_column_int64(*stmt, 0);
		dbn.dt = sqlite3_column_int64(*stmt, 1);
		dbn.name = (const char *)sqlite3_column_text(*stmt, 2);
		dbn.type = sqlite3_column_int(*stmt, 3);

		if((rc = callback(arg, &dbn)) < 0) {
			goto out_reset;
		}
	}

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_select_node_dir(int (*callback)(void *, struct db_node *), void *arg,
			   tupid_t dt)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_SELECT_NODE_DIR];
	static char s[] = "select id, name, type from node where dir=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, dt) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	while(1) {
		struct db_node dbn;

		dbrc = sqlite3_step(*stmt);
		if(dbrc == SQLITE_DONE) {
			rc = 0;
			goto out_reset;
		}
		if(dbrc != SQLITE_ROW) {
			fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
			rc = -1;
			goto out_reset;
		}

		dbn.tupid = sqlite3_column_int64(*stmt, 0);
		dbn.dt = dt;
		dbn.name = (const char *)sqlite3_column_text(*stmt, 1);
		dbn.type = sqlite3_column_int(*stmt, 2);

		if(callback(arg, &dbn) < 0) {
			rc = -1;
			goto out_reset;
		}
	}

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_select_node_dir_glob(int (*callback)(void *, struct db_node *),
				void *arg, tupid_t dt, const char *glob)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_SELECT_NODE_DIR_GLOB];
	static char s[] = "select id, name, type from node where dir=? and type=? and name glob ?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, dt) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int(*stmt, 2, TUP_NODE_FILE) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_text(*stmt, 3, glob, -1, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	while(1) {
		struct db_node dbn;

		dbrc = sqlite3_step(*stmt);
		if(dbrc == SQLITE_DONE) {
			rc = 0;
			goto out_reset;
		}
		if(dbrc != SQLITE_ROW) {
			fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
			rc = -1;
			goto out_reset;
		}

		dbn.tupid = sqlite3_column_int64(*stmt, 0);
		dbn.dt = dt;
		dbn.name = (const char *)sqlite3_column_text(*stmt, 1);
		dbn.type = sqlite3_column_int(*stmt, 2);

		if(callback(arg, &dbn) < 0) {
			rc = -1;
			goto out_reset;
		}
	}

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_set_flags_by_name(tupid_t dt, const char *name, int flags)
{
	struct db_node dbn;

	if(node_select(dt, name, -1, &dbn) < 0)
		return -1;
	if(dbn.tupid == -1)
		return -1;

	if(tup_db_set_flags_by_id(dbn.tupid, flags) < 0)
		return -1;
	return 0;
}

int tup_db_set_flags_by_id(tupid_t tupid, int flags)
{
	if(flags & TUP_FLAGS_CREATE) {
		if(tup_db_add_create_list(tupid) < 0)
			return -1;
	} else {
		if(tup_db_unflag_create(tupid) < 0)
			return -1;
	}
	if(flags & TUP_FLAGS_MODIFY) {
		if(tup_db_add_modify_list(tupid) < 0)
			return -1;
	} else {
		if(tup_db_unflag_modify(tupid) < 0)
			return -1;
	}
	if(flags & TUP_FLAGS_DELETE) {
		if(tup_db_add_delete_list(tupid) < 0)
			return -1;
	} else {
		if(tup_db_unflag_delete(tupid) < 0)
			return -1;
	}
	return 0;
}

int tup_db_delete_node(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_DELETE_NODE];
	static char s[] = "delete from node where id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_delete_dir(tupid_t dt)
{
	LIST_HEAD(subdir_list);
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_DELETE_DIR];
	static char s[] = "insert or replace into delete_list select id from node where dir=?";

	if(tup_db_set_flags_by_id(dt, TUP_FLAGS_DELETE) < 0)
		return -1;
	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int(*stmt, 1, dt) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(get_recurse_dirs(dt, &subdir_list) < 0)
		return -1;
	while(!list_empty(&subdir_list)) {
		struct id_entry *ide = list_entry(subdir_list.next,
						  struct id_entry, list);
		tup_db_delete_dir(ide->tupid);
		list_del(&ide->list);
		free(ide);
	}

	return 0;
}

int tup_db_open_tupid(tupid_t tupid)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_OPEN_TUPID];
	static char s[] = "select dir, name from node where id=?";
	tupid_t parent;
	char *path;
	int fd;

	if(tupid == 0) {
		fprintf(stderr, "Error: Trying to tup_db_open_tupid(0)\n");
		return -1;
	}
	if(tupid == 1) {
		return open(".", O_RDONLY);
	}
	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		rc = -ENOENT;
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		rc = -1;
		goto out_reset;
	}

	parent = sqlite3_column_int64(*stmt, 0);
	path = strdup((const char *)sqlite3_column_text(*stmt, 1));
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	fd = tup_db_open_tupid(parent);
	if(fd < 0)
		return fd;

	rc = openat(fd, path, O_RDONLY);
	if(rc < 0) {
		if(errno == ENOENT)
			rc = -ENOENT;
		else
			perror(path);
	}
	close(fd);
	free(path);

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_get_path(tupid_t tupid, char *path, int size)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_GET_PATH];
	static char s[] = "select dir, name from node where id=?";
	tupid_t parent;
	char *name;
	int len = 0;
	int namelen;

	if(tupid == 0) {
		fprintf(stderr, "Error: Trying to tup_db_get_path(0)\n");
		return -1;
	}
	if(tupid == 1) {
		if(size > 2) {
			strcpy(path, ".");
			return 1;
		} else {
			fprintf(stderr, "tup_db_get_path() - Out of space for path of ID %lli\n", tupid);
			return -1;
		}
	}
	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		rc = -ENOENT;
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		rc = -1;
		goto out_reset;
	}

	parent = sqlite3_column_int64(*stmt, 0);
	name = strdup((const char *)sqlite3_column_text(*stmt, 1));
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	/* Include space for prefixed '/' */
	namelen = strlen(name) + 1;
	len = tup_db_get_path(parent, path, size - namelen);
	path[len] = '/';
	/* Here we use namelen (which has +1) to get the nul */
	memcpy(path+len+1, name, namelen);
	rc = len + namelen;
	free(name);

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

tupid_t tup_db_parent(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_PARENT];
	static char s[] = "select dir from node where id=?";
	tupid_t parent;

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(rc == SQLITE_DONE) {
		parent = -1;
		goto out_reset;
	}
	if(rc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		parent = -1;
		goto out_reset;
	}

	parent = sqlite3_column_int64(*stmt, 0);

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return parent;
}

int tup_db_get_node_flags(tupid_t tupid)
{
	int rc;
	int flags = 0;

	rc = tup_db_in_create_list(tupid);
	if(rc < 0)
		return -1;
	if(rc == 1)
		flags |= TUP_FLAGS_CREATE;

	rc = tup_db_in_modify_list(tupid);
	if(rc < 0)
		return -1;
	if(rc == 1)
		flags |= TUP_FLAGS_MODIFY;

	rc = tup_db_in_delete_list(tupid);
	if(rc < 0)
		return -1;
	if(rc == 1)
		flags |= TUP_FLAGS_DELETE;
	return flags;
}

int tup_db_add_create_list(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_ADD_CREATE_LIST];
	static char s[] = "insert or replace into create_list values(?)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_add_modify_list(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_ADD_MODIFY_LIST];
	static char s[] = "insert or replace into modify_list values(?)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_add_delete_list(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_ADD_DELETE_LIST];
	static char s[] = "insert or replace into delete_list values(?)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_in_create_list(tupid_t tupid)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_IN_CREATE_LIST];
	static char s[] = "select id from create_list where id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		rc = 0;
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		rc = -1;
		goto out_reset;
	}

	rc = 1;

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_in_modify_list(tupid_t tupid)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_IN_MODIFY_LIST];
	static char s[] = "select id from modify_list where id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		rc = 0;
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		rc = -1;
		goto out_reset;
	}

	rc = 1;

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_in_delete_list(tupid_t tupid)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_IN_DELETE_LIST];
	static char s[] = "select id from delete_list where id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		rc = 0;
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		rc = -1;
		goto out_reset;
	}

	rc = 1;

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_unflag_create(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_UNFLAG_CREATE];
	static char s[] = "delete from create_list where id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_unflag_modify(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_UNFLAG_MODIFY];
	static char s[] = "delete from modify_list where id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_unflag_delete(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_UNFLAG_DELETE];
	static char s[] = "delete from delete_list where id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

static int get_recurse_dirs(tupid_t dt, struct list_head *list)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[_DB_GET_RECURSE_DIRS];
	static char s[] = "select id from node where dir=? and type=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, dt) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int(*stmt, 2, TUP_NODE_DIR) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	while(1) {
		struct id_entry *ide;

		dbrc = sqlite3_step(*stmt);
		if(dbrc == SQLITE_DONE) {
			rc = 0;
			goto out_reset;
		}
		if(dbrc != SQLITE_ROW) {
			fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
			rc = -1;
			goto out_reset;
		}

		ide = malloc(sizeof *ide);
		if(ide == NULL) {
			perror("malloc");
			return -1;
		}
		ide->tupid = sqlite3_column_int64(*stmt, 0);
		list_add(&ide->list, list);
	}

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_create_link(tupid_t a, tupid_t b)
{
	if(tup_db_link_exists(a, b) == 0)
		return 0;
	if(link_insert(a, b) < 0)
		return -1;
	return 0;
}

int tup_db_create_unique_link(tupid_t a, tupid_t b)
{
	int rc;
	if(tup_db_link_exists(a, b) == 0)
		return 0;
	rc = tup_db_is_root_node(b);
	if(rc < 0)
		return -1;
	if(rc == 0) {
		fprintf(stderr, "Error: Unable to create a unique link from %lli to %lli because the destination has other incoming links.\n", a, b);
		return -1;
	}
	if(link_insert(a, b) < 0)
		return -1;
	return 0;
}

int tup_db_get_dest_links(tupid_t from_id, struct list_head *head)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_GET_DEST_LINKS];
	static char s[] = "select to_id from link where from_id=? and to_id not in (select id from delete_list where id=to_id)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, from_id) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	while(1) {
		struct id_entry *ide;

		ide = malloc(sizeof *ide);
		if(!ide) {
			perror("malloc");
			rc = -1;
			goto out_reset;
		}
		dbrc = sqlite3_step(*stmt);
		if(dbrc == SQLITE_DONE) {
			rc = 0;
			goto out_reset;
		}
		if(dbrc != SQLITE_ROW) {
			fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
			rc = -1;
			goto out_reset;
		}

		ide->tupid = sqlite3_column_int64(*stmt, 0);
		list_add(&ide->list, head);
	}

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_get_src_links(tupid_t to_id, struct list_head *head)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_GET_SRC_LINKS];
	static char s[] = "select from_id from link where to_id=? and from_id not in (select id from delete_list where id=from_id)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, to_id) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	while(1) {
		struct id_entry *ide;

		ide = malloc(sizeof *ide);
		if(!ide) {
			perror("malloc");
			rc = -1;
			goto out_reset;
		}
		dbrc = sqlite3_step(*stmt);
		if(dbrc == SQLITE_DONE) {
			rc = 0;
			goto out_reset;
		}
		if(dbrc != SQLITE_ROW) {
			fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
			rc = -1;
			goto out_reset;
		}

		ide->tupid = sqlite3_column_int64(*stmt, 0);
		list_add(&ide->list, head);
	}

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_link_exists(tupid_t a, tupid_t b)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_LINK_EXISTS];
	static char s[] = "select to_id from link where from_id=? and to_id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, a) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int64(*stmt, 2, b) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(rc == SQLITE_DONE) {
		return -1;
	}
	if(rc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_is_root_node(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_IS_ROOT_NODE];
	static char s[] = "select from_id from link, node where to_id=? and from_id=id and not from_id in (select id from delete_list where id=from_id)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(rc == SQLITE_DONE) {
		return 1;
	}
	if(rc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_delete_links(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_DELETE_LINKS];
	static char s[] = "delete from link where from_id=? or to_id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int64(*stmt, 2, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_or_dircmd_flags(tupid_t parent, int flags, int type)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_OR_DIRCMD_FLAGS];
	static char s[] = "insert or replace into delete_list select id from node where dir=? and type=?";

	if(flags != TUP_FLAGS_DELETE) {
		fprintf(stderr, "Error: tup_db_or_dircmd_flags() now only works with DELETE.\n");
		return -1;
	}
	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, parent) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int(*stmt, 2, type) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_set_cmd_output_flags(tupid_t parent, int flags)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_SET_CMD_OUTPUT_FLAGS];
	static char s[] = "insert or replace into delete_list select to_id from link where from_id in (select id from node where dir=? and type=?)";

	if(flags != TUP_FLAGS_DELETE) {
		fprintf(stderr, "Error: tup_db_set_cmd_output_flags() now only works with DELETE.\n");
		return -1;
	}
	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, parent) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int(*stmt, 2, TUP_NODE_CMD) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_modify_cmds_by_output(tupid_t output)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_MODIFY_CMDS_BY_OUTPUT];
	static char s[] = "insert or replace into modify_list select from_id from link where to_id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, output) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_modify_cmds_by_input(tupid_t input)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_MODIFY_CMDS_BY_INPUT];
	static char s[] = "insert or replace into modify_list select to_id from link, node where from_id=? and to_id=id and type=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, input) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int(*stmt, 2, TUP_NODE_CMD) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_set_dependent_dir_flags(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_SET_DEPENDENT_DIR_FLAGS];
	static char s[] = "insert or replace into create_list select to_id from link, node where from_id=? and to_id=id and type=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int(*stmt, 2, TUP_NODE_DIR) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_modify_deleted_deps(void)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_MODIFY_DELETED_DEPS];
	static char s[] = "insert or replace into modify_list select to_id from link where from_id in (select id from delete_list)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_select_node_by_link(int (*callback)(void *, struct db_node *),
			       void *arg, tupid_t tupid)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_SELECT_NODE_BY_LINK];
	static char s[] = "select id, dir, name, type from node where id in (select to_id from link where from_id=?)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	while(1) {
		struct db_node dbn;

		dbrc = sqlite3_step(*stmt);
		if(dbrc == SQLITE_DONE) {
			rc = 0;
			goto out_reset;
		}
		if(dbrc != SQLITE_ROW) {
			fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
			rc = -1;
			goto out_reset;
		}

		dbn.tupid = sqlite3_column_int64(*stmt, 0);
		dbn.dt = sqlite3_column_int64(*stmt, 1);
		dbn.name = (const char *)sqlite3_column_text(*stmt, 2);
		dbn.type = sqlite3_column_int(*stmt, 3);

		if(callback(arg, &dbn) < 0) {
			rc = -1;
			goto out_reset;
		}
	}

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_delete_dependent_dir_links(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_DELETE_DEPENDENT_DIR_LINKS];
	static char s[] = "delete from link where to_id=? and from_id in (select from_id from link, node where to_id=? and from_id=id)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int64(*stmt, 2, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_config_set_int(const char *lval, int x)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_CONFIG_SET_INT];
	static char s[] = "insert or replace into config values(?, ?)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_text(*stmt, 1, lval, -1, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int(*stmt, 2, x) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_config_get_int(const char *lval)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_CONFIG_GET_INT];
	static char s[] = "select rval from config where lval=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_text(*stmt, 1, lval, -1, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		rc = -1;
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		rc = -1;
		goto out_reset;
	}

	rc = sqlite3_column_int(*stmt, 0);

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_config_set_int64(const char *lval, sqlite3_int64 x)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_CONFIG_SET_INT64];
	static char s[] = "insert or replace into config values(?, ?)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_text(*stmt, 1, lval, -1, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int64(*stmt, 2, x) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

sqlite3_int64 tup_db_config_get_int64(const char *lval)
{
	sqlite3_int64 rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_CONFIG_GET_INT64];
	static char s[] = "select rval from config where lval=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_text(*stmt, 1, lval, -1, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		rc = -1;
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		rc = -1;
		goto out_reset;
	}

	rc = sqlite3_column_int64(*stmt, 0);

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_config_set_string(const char *lval, const char *rval)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_CONFIG_SET_STRING];
	static char s[] = "insert or replace into config values(?, ?)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_text(*stmt, 1, lval, -1, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_text(*stmt, 2, rval, -1, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_config_get_string(char **res, const char *lval, const char *def)
{
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_CONFIG_GET_STRING];
	static char s[] = "select rval from config where lval=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_text(*stmt, 1, lval, -1, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		*res = strdup(def);
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		*res = NULL;
		goto out_reset;
	}

	*res = strdup((const char *)sqlite3_column_text(*stmt, 0));

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(*res)
		return 0;
	return -1;
}

int tup_db_set_var(tupid_t tupid, const char *value)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_SET_VAR];
	static char s[] = "insert or replace into var values(?, ?)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_text(*stmt, 2, value, -1, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

tupid_t tup_db_get_var(const char *var, int varlen, char **dest)
{
	int dbrc;
	int len;
	tupid_t tupid = -1;
	const char *value;
	sqlite3_stmt **stmt = &stmts[DB_GET_VAR];
	static char s[] = "select var.id, value, length(value) from var, node where node.dir=? and node.name=? and node.id=var.id and node.id not in (select node.id from delete_list)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int(*stmt, 1, VAR_DT) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_text(*stmt, 2, var, varlen, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		fprintf(stderr,"Error: Variable '%.*s' not found in .tup/db.\n",
			varlen, var);
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		goto out_reset;
	}

	len = sqlite3_column_int(*stmt, 2);
	if(len < 0) {
		goto out_reset;
	}
	value = (const char *)sqlite3_column_text(*stmt, 1);
	if(!value) {
		goto out_reset;
	}
	memcpy(*dest, value, len);
	*dest += len;

	tupid = sqlite3_column_int64(*stmt, 0);

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return tupid;
}

int tup_db_get_var_id(tupid_t tupid, char **dest)
{
	int rc = -1;
	int dbrc;
	int len;
	const char *value;
	sqlite3_stmt **stmt = &stmts[DB_GET_VAR_ID];
	static char s[] = "select value, length(value) from var where var.id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		fprintf(stderr,"Error: Variable id %lli not found in .tup/db.\n", tupid);
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		goto out_reset;
	}

	len = sqlite3_column_int(*stmt, 1);
	if(len < 0) {
		goto out_reset;
	}
	value = (const char *)sqlite3_column_text(*stmt, 0);
	if(!value) {
		goto out_reset;
	}
	*dest = malloc(len + 1);
	if(!*dest) {
		perror("malloc");
		goto out_reset;
	}
	memcpy(*dest, value, len);
	(*dest)[len] = 0;
	rc = 0;

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_get_varlen(const char *var, int varlen)
{
	int rc = -1;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_GET_VARLEN];
	static char s[] = "select length(value) from var, node where node.dir=? and node.name=? and node.id=var.id and node.id not in (select node.id from delete_list)";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int(*stmt, 1, VAR_DT) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_text(*stmt, 2, var, varlen, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		fprintf(stderr,"Error: Variable '%.*s' not found in .tup/db.\n",
			varlen, var);
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		goto out_reset;
	}

	rc = sqlite3_column_int(*stmt, 0);

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

tupid_t tup_db_write_var(const char *var, int varlen, int fd)
{
	int dbrc;
	int len;
	const char *value;
	sqlite3_stmt **stmt = &stmts[DB_WRITE_VAR];
	static char s[] = "select var.id, value, length(value) from var, node where node.dir=? and node.name=? and node.id=var.id and node.id not in (select node.id from delete_list)";
	tupid_t tupid = -1;

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, VAR_DT) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_text(*stmt, 2, var, varlen, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		fprintf(stderr,"Error: Variable '%.*s' not found in .tup/db.\n",
			varlen, var);
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		goto out_reset;
	}

	len = sqlite3_column_int(*stmt, 2);
	if(len < 0) {
		goto out_reset;
	}
	value = (const char *)sqlite3_column_text(*stmt, 1);
	if(!value) {
		goto out_reset;
	}

	if(write(fd, value, len) == len)
		tupid = sqlite3_column_int64(*stmt, 0);

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return tupid;
}

int tup_db_flag_deleted_var_dependent_dirs(void)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_FLAG_DELETED_VAR_DEPENDENT_DIRS];
	static char s[] = "insert or replace into create_list select id from node where id in (select to_id from link where from_id in (select delete_list.id from delete_list inner join node on delete_list.id=node.id where node.type=?)) and type=?";
	/* This should move directories into the create list that are dependent
	 * on variables that are deleted. There's probably an easier way to do
	 * it, since it takes much longer to say it in SQL that it does in
	 * English.
	 */

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int(*stmt, 1, TUP_NODE_VAR) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int(*stmt, 2, TUP_NODE_DIR) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_var_foreach(int (*callback)(void *, const char *var, const char *value), void *arg)
{
	int rc = -1;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_VAR_FOREACH];
	static char s[] = "select name, value from var, node where node.dir=? and node.id=var.id";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int(*stmt, 1, VAR_DT) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	while(1) {
		const char *var;
		const char *value;

		dbrc = sqlite3_step(*stmt);
		if(dbrc == SQLITE_DONE) {
			rc = 0;
			goto out_reset;
		}
		if(dbrc != SQLITE_ROW) {
			fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
			rc = -1;
			goto out_reset;
		}

		var = (const char *)sqlite3_column_text(*stmt, 0);
		value = (const char *)sqlite3_column_text(*stmt, 1);

		if((rc = callback(arg, var, value)) < 0) {
			goto out_reset;
		}
	}

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

int tup_db_attach_tmpdb(void)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_ATTACH_TMPDB];
	static char s[] = "attach ':memory:' as tmpdb";
	char *errmsg;

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(sqlite3_exec(tup_db, "create table tmpdb.tmp_list (id integer primary key not null)", NULL, NULL, &errmsg) != 0) {
		fprintf(stderr, "SQL error creating tmp_list table: %s\n",
			errmsg);
		return -1;
	}
	return 0;
}

int tup_db_detach_tmpdb(void)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_DETACH_TMPDB];
	static char s[] = "detach tmpdb";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	return 0;
}

int tup_db_files_to_tmpdb(void)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_FILES_TO_TMPDB];
	static char s[] = "insert into tmpdb.tmp_list select id from node where type=? or type=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int(*stmt, 1, TUP_NODE_FILE) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int(*stmt, 2, TUP_NODE_DIR) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_unflag_tmpdb(tupid_t tupid)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[DB_UNFLAG_TMPDB];
	static char s[] = "delete from tmpdb.tmp_list where id=?";

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, tupid) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

int tup_db_get_all_in_tmpdb(struct list_head *list)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[DB_GET_ALL_IN_TMPDB];
	static char s[] = "select node.id, dir, type from tmpdb.tmp_list, node where tmp_list.id = node.id";
	struct del_entry *de;

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	while(1) {
		dbrc = sqlite3_step(*stmt);
		if(dbrc == SQLITE_DONE) {
			rc = 0;
			goto out_reset;
		}
		if(dbrc != SQLITE_ROW) {
			fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
			rc = -1;
			goto out_reset;
		}

		de = malloc(sizeof *de);
		if(de == NULL) {
			perror("malloc");
			return -1;
		}
		de->tupid = sqlite3_column_int64(*stmt, 0);
		de->dt = sqlite3_column_int64(*stmt, 1);
		de->type = sqlite3_column_int(*stmt, 2);
		list_add(&de->list, list);
	}

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	return rc;
}

static tupid_t node_insert(tupid_t dt, const char *name, int len, int type)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[_DB_NODE_INSERT];
	static char s[] = "insert into node(dir, type, name) values(?, ?, ?)";
	tupid_t tupid;

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, dt) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int(*stmt, 2, type) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_text(*stmt, 3, name, len, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	tupid = sqlite3_last_insert_rowid(tup_db);
	switch(type) {
		case TUP_NODE_DIR:
			if(tup_db_add_create_list(tupid) < 0)
				return -1;
			break;
		case TUP_NODE_FILE:
		case TUP_NODE_CMD:
			if(tup_db_add_modify_list(tupid) < 0)
				return -1;
			break;
		case TUP_NODE_VAR:
			if(tup_db_add_create_list(tupid) < 0)
				return -1;
			if(tup_db_add_modify_list(tupid) < 0)
				return -1;
			break;
	}

	return tupid;
}

static int node_select(tupid_t dt, const char *name, int len,
		       struct db_node *dbn)
{
	int rc;
	int dbrc;
	sqlite3_stmt **stmt = &stmts[_DB_NODE_SELECT];
	static char s[] = "select id, type from node where dir=? and name=?";

	dbn->tupid = -1;
	dbn->dt = -1;
	dbn->name = NULL;
	dbn->type = 0;

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, dt) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_text(*stmt, 2, name, len, SQLITE_STATIC) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	dbrc = sqlite3_step(*stmt);
	if(dbrc == SQLITE_DONE) {
		rc = 0;
		goto out_reset;
	}
	if(dbrc != SQLITE_ROW) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		rc = -1;
		goto out_reset;
	}

	rc = 0;
	dbn->tupid = sqlite3_column_int64(*stmt, 0);
	dbn->type = sqlite3_column_int64(*stmt, 1);

out_reset:
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return rc;
}

static int link_insert(tupid_t a, tupid_t b)
{
	int rc;
	sqlite3_stmt **stmt = &stmts[_DB_LINK_INSERT];
	static char s[] = "insert into link(from_id, to_id) values(?, ?)";

	if(a == b) {
		fprintf(stderr, "Error: Attempt made to link a node to itself (%lli)\n", a);
		return -1;
	}

	if(!*stmt) {
		if(sqlite3_prepare_v2(tup_db, s, sizeof(s), stmt, NULL) != 0) {
			fprintf(stderr, "SQL Error: %s\nStatement was: %s\n",
				sqlite3_errmsg(tup_db), s);
			return -1;
		}
	}

	if(sqlite3_bind_int64(*stmt, 1, a) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}
	if(sqlite3_bind_int64(*stmt, 2, b) != 0) {
		fprintf(stderr, "SQL bind error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	rc = sqlite3_step(*stmt);
	if(sqlite3_reset(*stmt) != 0) {
		fprintf(stderr, "SQL reset error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	if(rc != SQLITE_DONE) {
		fprintf(stderr, "SQL step error: %s\n", sqlite3_errmsg(tup_db));
		return -1;
	}

	return 0;
}

static int no_sync(void)
{
	char *errmsg;
	char sql[] = "PRAGMA synchronous=OFF";

	if(sqlite3_exec(tup_db, sql, NULL, NULL, &errmsg) != 0) {
		fprintf(stderr, "SQL error: %s\nQuery was: %s",
			errmsg, sql);
		return -1;
	}
	return 0;
}