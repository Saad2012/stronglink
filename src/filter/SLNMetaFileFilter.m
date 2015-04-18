#include "SLNFilter.h"

@implementation SLNMetaFileFilter
- (void)free {
	db_cursor_close(metafiles); metafiles = NULL;
	[super free];
}

- (SLNFilterType)type {
	return SLNMetaFileFilterType;
}
- (SLNFilter *)unwrap {
	return self;
}
- (void)print:(count_t const)depth {
	indent(depth);
	fprintf(stderr, "(meta)\n");
}
- (size_t)getUserFilter:(str_t *const)data :(size_t const)size :(count_t const)depth {
	assert(0);
	return wr(data, size, "");
}

- (int)prepare:(DB_txn *const)txn {
	if([super prepare:txn] < 0) return -1;
	db_cursor_renew(txn, &metafiles); // SLNMetaFileByID
	return 0;
}

- (void)seek:(int const)dir :(uint64_t const)sortID :(uint64_t const)fileID {
	DB_range range[1];
	DB_val key[1];
	SLNMetaFileByIDRange0(range, curtxn);
	SLNMetaFileByIDKeyPack(key, curtxn, sortID);
	int rc = db_cursor_seekr(metafiles, range, key, NULL, dir);
	if(DB_SUCCESS != rc) return;
	uint64_t actualSortID;
	SLNMetaFileByIDKeyUnpack(key, curtxn, &actualSortID);
	if(sortID != actualSortID) return;
	if(dir > 0 && actualSortID >= fileID) return;
	if(dir < 0 && actualSortID <= fileID) return;
	if(dir) {
		[self step:dir];
	} else if(actualSortID != fileID) {
		db_cursor_clear(metafiles);
	}
}
- (void)current:(int const)dir :(uint64_t *const)sortID :(uint64_t *const)fileID {
	DB_val key[1];
	int rc = db_cursor_current(metafiles, key, NULL);
	if(DB_SUCCESS == rc) {
		uint64_t x;
		SLNMetaFileByIDKeyUnpack(key, curtxn, &x);
		if(sortID) *sortID = x;
		if(fileID) *fileID = x;
	} else {
		if(sortID) *sortID = invalid(dir);
		if(fileID) *fileID = invalid(dir);
	}
}
- (void)step:(int const)dir {
	DB_range range[1];
	SLNMetaFileByIDRange0(range, curtxn);
	int rc = db_cursor_nextr(metafiles, range, NULL, NULL, dir);
	assertf(DB_SUCCESS == rc || DB_NOTFOUND == rc, "Database error %s", db_strerror(rc));
}
- (uint64_t)fullAge:(uint64_t const)fileID {
	return fileID;
}
- (uint64_t)fastAge:(uint64_t const)fileID :(uint64_t const)sortID {
	return fileID;
}
@end

