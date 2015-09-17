// Copyright 2014-2015 Ben Trask
// MIT licensed (see LICENSE for details)

#include "SLNFilter.h"

// TODO: Large amounts of redundancy here.
// Might just be the nature of the beast though.

@implementation SLNURIFilter
- (void)free {
	curtxn = NULL;
	FREE(&URI);
	db_cursor_close(files); files = NULL;
	db_cursor_close(age); age = NULL;
	[super free];
}

- (SLNFilterType)type {
	return SLNURIFilterType;
}
- (strarg_t)stringArg:(size_t const)i {
	if(0 == i) return URI;
	return NULL;
}
- (int)addStringArg:(strarg_t const)str :(size_t const)len {
	if(!URI) {
		URI = strndup(str, len);
		if(!URI) return DB_ENOMEM;
		return 0;
	}
	return DB_EINVAL;
}
- (void)printSexp:(FILE *const)file :(size_t const)depth {
	indent(file, depth);
	fprintf(file, "(uri \"%s\")\n", URI);
}
- (void)printUser:(FILE *const)file :(size_t const)depth {
	fprintf(file, "%s", URI);
}

- (int)prepare:(DB_txn *const)txn {
	int rc = [super prepare:txn];
	if(rc < 0) return rc;
	db_cursor_renew(txn, &files); // SLNURIAndFileID
	db_cursor_renew(txn, &age); // SLNURIAndFileID
	curtxn = txn;
	return 0;
}
- (void)seek:(int const)dir :(uint64_t const)sortID :(uint64_t const)fileID {
	uint64_t x = sortID;
	if(valid(x) && dir > 0 && fileID > sortID) x++;
	if(valid(x) && dir < 0 && fileID < sortID) x--;

	DB_range range[1];
	DB_val key[1];
	SLNURIAndFileIDRange1(range, curtxn, URI);
	SLNURIAndFileIDKeyPack(key, curtxn, URI, x);
	int rc = db_cursor_seekr(files, range, key, NULL, dir);
	db_assertf(rc >= 0 || DB_NOTFOUND == rc, "Database error %s", sln_strerror(rc));

	// TODO: Skip files without any meta-files. The content of the
	// meta-file doesn't matter.
}
- (void)current:(int const)dir :(uint64_t *const)sortID :(uint64_t *const)fileID {
	DB_val key[1];
	int rc = db_cursor_current(files, key, NULL);
	if(rc >= 0) {
		strarg_t u;
		uint64_t x;
		SLNURIAndFileIDKeyUnpack(key, curtxn, &u, &x);
		if(sortID) *sortID = x;
		if(fileID) *fileID = x;
	} else {
		if(sortID) *sortID = invalid(dir);
		if(fileID) *fileID = invalid(dir);
	}
}
- (void)step:(int const)dir {
	DB_range range[1];
	SLNURIAndFileIDRange1(range, curtxn, URI);
	int rc = db_cursor_nextr(files, range, NULL, NULL, dir);
	db_assertf(rc >= 0 || DB_NOTFOUND == rc, "Database error %s", sln_strerror(rc));

	// TODO: Skip files without meta-files.
}
- (SLNAgeRange)fullAge:(uint64_t const)fileID {
	DB_val key[1];
	SLNURIAndFileIDKeyPack(key, curtxn, URI, fileID);
	int rc = db_cursor_seek(age, key, NULL, 0);
	if(DB_NOTFOUND == rc) return (SLNAgeRange){UINT64_MAX, UINT64_MAX};
	db_assertf(rc >= 0, "Database error %s", sln_strerror(rc));
	return (SLNAgeRange){fileID, UINT64_MAX};
}
- (uint64_t)fastAge:(uint64_t const)fileID :(uint64_t const)sortID {
	return [self fullAge:fileID].min;
}
@end

@implementation SLNTargetURIFilter
- (void)free {
	curtxn = NULL;
	FREE(&targetURI);
	db_cursor_close(metafiles); metafiles = NULL;
	db_cursor_close(age); age = NULL;
	[super free];
}

- (SLNFilterType)type {
	return SLNTargetURIFilterType;
}
- (strarg_t)stringArg:(size_t const)i {
	if(0 == i) return targetURI;
	return NULL;
}
- (int)addStringArg:(strarg_t const)str :(size_t const)len {
	if(!targetURI) {
		targetURI = strndup(str, len);
		if(!targetURI) return DB_ENOMEM;
		return 0;
	}
	return DB_EINVAL;
}
- (void)printSexp:(FILE *const)file :(size_t const)depth {
	indent(file, depth);
	fprintf(file, "(target \"%s\")\n", targetURI);
}
- (void)printUser:(FILE *const)file :(size_t const)depth {
	fprintf(file, "target=%s", targetURI);
}

- (int)prepare:(DB_txn *const)txn {
	int rc = [super prepare:txn];
	if(rc < 0) return rc;
	db_cursor_renew(txn, &metafiles); // SLNTargetURIAndMetaFileID
	db_cursor_renew(txn, &age); // SLNTargetURIAndMetaFileID
	curtxn = txn;
	return 0;
}
- (void)seek:(int const)dir :(uint64_t const)sortID :(uint64_t const)fileID {
	// TODO: Copy and paste from SLNURIFilter.
	uint64_t x = sortID;
	if(valid(x) && dir > 0 && fileID > sortID) x++;
	if(valid(x) && dir < 0 && fileID < sortID) x--;

	DB_range range[1];
	DB_val key[1];
	SLNTargetURIAndMetaFileIDRange1(range, curtxn, targetURI);
	SLNTargetURIAndMetaFileIDKeyPack(key, curtxn, targetURI, x);
	int rc = db_cursor_seekr(metafiles, range, key, NULL, dir);
	db_assertf(rc >= 0 || DB_NOTFOUND == rc, "Database error %s", sln_strerror(rc));
}
- (void)current:(int const)dir :(uint64_t *const)sortID :(uint64_t *const)fileID {
	DB_val key[1];
	int rc = db_cursor_current(metafiles, key, NULL);
	if(rc >= 0) {
		strarg_t URI = NULL;
		uint64_t x = 0;
		SLNTargetURIAndMetaFileIDKeyUnpack(key, curtxn, &URI, &x);
		assert(0 == strcmp(URI, targetURI));
		if(sortID) *sortID = x;
		if(fileID) *fileID = x;
	} else {
		if(sortID) *sortID = invalid(dir);
		if(fileID) *fileID = invalid(dir);
	}
}
- (void)step:(int const)dir {
	DB_range range[1];
	SLNTargetURIAndMetaFileIDRange1(range, curtxn, targetURI);
	int rc = db_cursor_nextr(metafiles, range, NULL, NULL, dir);
	db_assertf(rc >= 0 || DB_NOTFOUND == rc, "Database error %s", sln_strerror(rc));
}
- (SLNAgeRange)fullAge:(uint64_t const)fileID {
	DB_val key[1];
	SLNTargetURIAndMetaFileIDKeyPack(key, curtxn, targetURI, fileID);
	int rc = db_cursor_seek(age, key, NULL, 0);
	if(DB_NOTFOUND == rc) return (SLNAgeRange){UINT64_MAX, UINT64_MAX};
	db_assertf(rc >= 0, "Database error %s", sln_strerror(rc));
	return (SLNAgeRange){fileID, UINT64_MAX};
}
- (uint64_t)fastAge:(uint64_t const)fileID :(uint64_t const)sortID {
	return [self fullAge:fileID].min;
}
@end

@implementation SLNAllFilter
- (void)free {
	db_cursor_close(files); files = NULL;
	[super free];
}

- (SLNFilterType)type {
	return SLNAllFilterType;
}
- (void)printSexp:(FILE *const)file :(size_t const)depth {
	indent(file, depth);
	fprintf(file, "(all)\n");
}
- (void)printUser:(FILE *const)file :(size_t const)depth {
	fprintf(file, "*");
}

- (int)prepare:(DB_txn *const)txn {
	int rc = [super prepare:txn];
	if(rc < 0) return rc;
	db_cursor_renew(txn, &files); // SLNFileByID
	return 0;
}
- (void)seek:(int const)dir :(uint64_t const)sortID :(uint64_t const)fileID {
	// TODO: Copy and paste from SLNURIFilter.
	uint64_t x = sortID;
	if(valid(x) && dir > 0 && fileID > sortID) x++;
	if(valid(x) && dir < 0 && fileID < sortID) x--;

	DB_range range[1];
	DB_val key[1];
	SLNFileByIDRange0(range, curtxn);
	SLNFileByIDKeyPack(key, curtxn, x);
	int rc = db_cursor_seekr(files, range, key, NULL, dir);
	db_assertf(rc >= 0 || DB_NOTFOUND == rc, "Database error %s", sln_strerror(rc));
}
- (void)current:(int const)dir :(uint64_t *const)sortID :(uint64_t *const)fileID {
	DB_val key[1];
	int rc = db_cursor_current(files, key, NULL);
	if(rc >= 0) {
		dbid_t const table = db_read_uint64(key);
		assert(SLNFileByID == table);
		uint64_t const x = db_read_uint64(key);
		if(sortID) *sortID = x;
		if(fileID) *fileID = x;
	} else {
		if(sortID) *sortID = invalid(dir);
		if(fileID) *fileID = invalid(dir);
	}
}
- (void)step:(int const)dir {
	DB_range range[1];
	SLNFileByIDRange0(range, curtxn);
	int rc = db_cursor_nextr(files, range, NULL, NULL, dir);
	db_assertf(rc >= 0 || DB_NOTFOUND == rc, "Database error %s", sln_strerror(rc));
}
- (SLNAgeRange)fullAge:(uint64_t const)fileID {
	return (SLNAgeRange){fileID, UINT64_MAX};
}
- (uint64_t)fastAge:(uint64_t const)fileID :(uint64_t const)sortID {
	return [self fullAge:fileID].min;
}
@end

