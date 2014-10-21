#include "../deps/liblmdb/lmdb.h"
#include "common.h"

// Makes beginning a transaction slightly clearer.
#define MDB_RDWR 0

#define DB_VARINT_MAX 9

#define DB_VAL(name, cols) \
	byte_t __buf_##name[DB_VARINT_MAX * cols]; \
	MDB_val name[1] = {{ 0, __buf_##name }};

typedef struct {
	MDB_dbi main;
} DB_schema;

typedef uint64_t dbid_t;
enum {
	DBSchema = 0,
	DBStringByID = 1,
	DBStringIDByValue = 2,
	DBStringIDByHash = 3,
};

uint64_t db_column(MDB_val const *const val, index_t const col);
strarg_t db_column_text(MDB_txn *const txn, DB_schema const *const schema, MDB_val const *const val, index_t const col);

void db_bind(MDB_val *const val, uint64_t const item);

uint64_t db_last_id(MDB_txn *txn, MDB_dbi dbi);

uint64_t db_string_id(MDB_txn *const txn, DB_schema const *const schema, strarg_t const str);
uint64_t db_string_id_len(MDB_txn *const txn, DB_schema const *const schema, strarg_t const str, size_t const len, bool_t const nulterm);

int db_cursor(MDB_txn *const txn, MDB_dbi const dbi, MDB_cursor **const cur);
int db_cursor_get(MDB_cursor *const cur, MDB_val *const key, MDB_val *const val, MDB_cursor_op const op);






int db_cursor_seek(MDB_cursor *const cursor, MDB_val *const key, MDB_val *const data, int const dir);
int db_cursor_first(MDB_cursor *const cursor, MDB_val *const key, MDB_val *const data, int const dir);
int db_cursor_next(MDB_cursor *const cursor, MDB_val *const key, MDB_val *const data, int const dir);

typedef struct {
	MDB_val *min;
	MDB_val *max;
} DB_range;

int db_cursor_seekr(MDB_cursor *const cursor, DB_range const *const range, MDB_val *const key, MDB_val *const data, int const dir);
int db_cursor_firstr(MDB_cursor *const cursor, DB_range const *const range, MDB_val *const key, MDB_val *const data, int const dir);
int db_cursor_nextr(MDB_cursor *const cursor, DB_range const *const range, MDB_val *const key, MDB_val *const data, int const dir);


