// Copyright 2014-2015 Ben Trask
// MIT licensed (see LICENSE for details)

#include <async/http/HTTP.h>
#include <async/http/HTTPServer.h>
#include <async/http/MultipartForm.h>
#include <async/http/QueryString.h>
#include "../StrongLink.h"
#include "Template.h"

typedef struct Blog* BlogRef;

#define PENDING_MAX 4

struct Blog {
	SLNRepoRef repo;

	str_t *dir;
	str_t *cacheDir;

	TemplateRef header;
	TemplateRef footer;
	TemplateRef entry_start;
	TemplateRef entry_end;
	TemplateRef preview;
	TemplateRef empty;
	TemplateRef compose;
	TemplateRef upload;
	TemplateRef login;
	TemplateRef notfound;
	TemplateRef noresults;

	async_mutex_t pending_mutex[1];
	async_cond_t pending_cond[1];
	strarg_t pending[PENDING_MAX];
};

BlogRef BlogCreate(SLNRepoRef const repo);
void BlogFree(BlogRef *const blogptr);
int BlogDispatch(BlogRef const blog, SLNSessionRef const session, HTTPConnectionRef const conn, HTTPMethod const method, strarg_t const URI, HTTPHeadersRef const headers);

int BlogConvert(BlogRef const blog,
                SLNSessionRef const session,
                strarg_t const html,
                SLNSubmissionRef *const meta,
                strarg_t const URI,
                SLNFileInfo const *const src);
int BlogGeneric(BlogRef const blog,
                SLNSessionRef const session,
                strarg_t const htmlpath,
                strarg_t const URI,
                SLNFileInfo const *const src);

// TODO: Get rid of this stuff, or refactor it.
typedef struct {
	BlogRef blog;
	SLNSessionRef session;
	strarg_t fileURI;
} preview_state;
extern TemplateArgCBs const preview_cbs;

