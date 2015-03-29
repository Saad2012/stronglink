#include <assert.h>
#include "StrongLink.h"
#include "http/HTTPConnection.h"
#include "http/HTTPHeaders.h"

#define READER_COUNT 64
#define QUEUE_SIZE 64 // TODO: Find a way to lower these without sacrificing performance, and perhaps automatically adjust them somehow.

struct SLNPull {
	uint64_t pullID;
	SLNSessionRef session;
	str_t *host;
	str_t *username;
	str_t *password;
	str_t *cookie;
	str_t *query;

	async_mutex_t connlock[1];
	HTTPConnectionRef conn;

	async_mutex_t mutex[1];
	async_cond_t cond[1];
	bool stop;
	count_t tasks;
	SLNSubmissionRef queue[QUEUE_SIZE];
	bool filled[QUEUE_SIZE];
	index_t cur;
	count_t count;
};

static int reconnect(SLNPullRef const pull);
static int import(SLNPullRef const pull, strarg_t const URI, index_t const pos, HTTPConnectionRef *const conn);

SLNPullRef SLNRepoCreatePull(SLNRepoRef const repo, uint64_t const pullID, uint64_t const userID, strarg_t const host, strarg_t const username, strarg_t const password, strarg_t const cookie, strarg_t const query) {
	SLNPullRef pull = calloc(1, sizeof(struct SLNPull));
	if(!pull) return NULL;
	pull->pullID = pullID;
	pull->session = SLNRepoCreateSessionInternal(repo, userID, SLN_RDWR);
	pull->username = strdup(username);
	pull->password = strdup(password);
	pull->cookie = cookie ? strdup(cookie) : NULL;
	pull->host = strdup(host);
	pull->query = strdup(query);
	return pull;
}
void SLNPullFree(SLNPullRef *const pullptr) {
	SLNPullRef pull = *pullptr;
	if(!pull) return;

	SLNPullStop(pull);

	pull->pullID = 0;
	SLNSessionFree(&pull->session);
	FREE(&pull->host);
	FREE(&pull->username);
	FREE(&pull->password);
	FREE(&pull->cookie);
	FREE(&pull->query);

	assert_zeroed(pull, 1);
	FREE(pullptr); pull = NULL;
}

static void reader(SLNPullRef const pull) {
	HTTPConnectionRef conn = NULL;
	int rc;

	for(;;) {
		if(pull->stop) goto stop;

		str_t URI[URI_MAX];

		async_mutex_lock(pull->connlock);

		rc = HTTPConnectionReadBodyLine(pull->conn, URI, sizeof(URI));
		if(rc < 0) {
			for(;;) {
				if(reconnect(pull) >= 0) break;
				if(pull->stop) break;
				async_sleep(1000 * 5);
			}
			async_mutex_unlock(pull->connlock);
			continue;
		}

		async_mutex_lock(pull->mutex);
		while(pull->count + 1 > QUEUE_SIZE) {
			async_cond_wait(pull->cond, pull->mutex);
			if(pull->stop) {
				async_mutex_unlock(pull->mutex);
				async_mutex_unlock(pull->connlock);
				goto stop;
			}
		}
		index_t pos = (pull->cur + pull->count) % QUEUE_SIZE;
		pull->count += 1;
		async_mutex_unlock(pull->mutex);

		async_mutex_unlock(pull->connlock);

		for(;;) {
			if(import(pull, URI, pos, &conn) >= 0) break;
			if(pull->stop) goto stop;
			async_sleep(1000 * 5);
		}

	}

stop:
	HTTPConnectionFree(&conn);
	async_mutex_lock(pull->mutex);
	assertf(pull->stop, "Reader ended early");
	assert(pull->tasks > 0);
	pull->tasks--;
	async_cond_broadcast(pull->cond);
	async_mutex_unlock(pull->mutex);
}
static void writer(SLNPullRef const pull) {
	SLNSubmissionRef queue[QUEUE_SIZE];
	count_t count = 0;
	count_t skipped = 0;
	double time = uv_now(loop) / 1000.0;
	for(;;) {
		if(pull->stop) goto stop;

		async_mutex_lock(pull->mutex);
		while(0 == count || (count < QUEUE_SIZE && pull->count > 0)) {
			index_t const pos = pull->cur;
			while(!pull->filled[pos]) {
				async_cond_wait(pull->cond, pull->mutex);
				if(pull->stop) {
					async_mutex_unlock(pull->mutex);
					goto stop;
				}
				if(!count) time = uv_now(loop) / 1000.0;
			}
			assert(pull->filled[pos]);
			// Skip any bubbles in the queue.
			if(pull->queue[pos]) queue[count++] = pull->queue[pos];
			else skipped++;
			pull->queue[pos] = NULL;
			pull->filled[pos] = false;
			pull->cur = (pull->cur + 1) % QUEUE_SIZE;
			pull->count--;
			async_cond_broadcast(pull->cond);
		}
		async_mutex_unlock(pull->mutex);
		assert(count <= QUEUE_SIZE);

		for(;;) {
			int rc = SLNSubmissionBatchStore(queue, count);
			if(rc >= 0) break;
			fprintf(stderr, "Submission error %s / %s (%d)\n", uv_strerror(rc), db_strerror(rc), rc);
			async_sleep(1000 * 5);
		}
		for(index_t i = 0; i < count; ++i) {
			SLNSubmissionFree(&queue[i]);
		}

		double const now = uv_now(loop) / 1000.0;
		fprintf(stderr, "Pulled %f files per second\n", count / (now - time));
		time = now;
		count = 0;
		skipped = 0;

	}

stop:
	for(index_t i = 0; i < count; ++i) {
		SLNSubmissionFree(&queue[i]);
	}
	assert_zeroed(queue, QUEUE_SIZE);

	async_mutex_lock(pull->mutex);
	assertf(pull->stop, "Writer ended early");
	assert(pull->tasks > 0);
	pull->tasks--;
	async_cond_broadcast(pull->cond);
	async_mutex_unlock(pull->mutex);
}
int SLNPullStart(SLNPullRef const pull) {
	if(!pull) return 0;
	assert(0 == pull->tasks);
	async_mutex_init(pull->connlock, 0);
	async_mutex_init(pull->mutex, 0);
	async_cond_init(pull->cond, 0);
	for(index_t i = 0; i < READER_COUNT; ++i) {
		pull->tasks++;
		async_spawn(STACK_DEFAULT, (void (*)())reader, pull);
	}
	pull->tasks++;
	async_spawn(STACK_DEFAULT, (void (*)())writer, pull);
	// TODO: It'd be even better to have one writer shared between all pulls...

	return 0;
}
void SLNPullStop(SLNPullRef const pull) {
	if(!pull) return;
	if(!pull->connlock) return;

	async_mutex_lock(pull->mutex);
	pull->stop = true;
	async_cond_broadcast(pull->cond);
	while(pull->tasks > 0) {
		async_cond_wait(pull->cond, pull->mutex);
	}
	async_mutex_unlock(pull->mutex);

	async_mutex_destroy(pull->connlock);
	HTTPConnectionFree(&pull->conn);

	async_mutex_destroy(pull->mutex);
	async_cond_destroy(pull->cond);
	pull->stop = false;

	for(index_t i = 0; i < QUEUE_SIZE; ++i) {
		SLNSubmissionFree(&pull->queue[i]);
		pull->filled[i] = false;
	}
	pull->cur = 0;
	pull->count = 0;
}

static int auth(SLNPullRef const pull);

static int reconnect(SLNPullRef const pull) {
	int rc;
	HTTPConnectionFree(&pull->conn);

	if(!pull->cookie) {
		rc = auth(pull);
		if(rc < 0) return rc;
	}

	rc = HTTPConnectionCreateOutgoing(pull->host, &pull->conn);
	if(rc < 0) {
		fprintf(stderr, "Pull couldn't connect to %s (%s)\n", pull->host, uv_strerror(rc));
		return rc;
	}
	HTTPConnectionWriteRequest(pull->conn, HTTP_GET, "/efs/query?count=all", pull->host);
	// TODO: Pagination...
	// TODO: More careful error handling.
	// TODO: POST an actual query, GET is just a hack.
	assert(pull->cookie);
	HTTPConnectionWriteHeader(pull->conn, "Cookie", pull->cookie);
	HTTPConnectionBeginBody(pull->conn);
	rc = HTTPConnectionEnd(pull->conn);
	if(rc < 0) {
		fprintf(stderr, "Pull couldn't connect to %s (%s)\n", pull->host, uv_strerror(rc));
		return rc;
	}
	int const status = HTTPConnectionReadResponseStatus(pull->conn);
	if(status < 0) {
		fprintf(stderr, "Pull connection error %s\n", uv_strerror(status));
		return status;
	}
	if(403 == status) {
		fprintf(stderr, "Pull connection authentication failed\n");
		FREE(&pull->cookie);
		return UV_EACCES;
	}
	if(status < 200 || status >= 300) {
		fprintf(stderr, "Pull connection error %d\n", status);
		return UV_EPROTO;
	}

	HTTPHeadersRef headers = HTTPHeadersCreateFromConnection(pull->conn);
	HTTPHeadersFree(&headers); // TODO
/*	rc = HTTPConnectionReadHeaders(pull->conn, NULL, NULL, 0);
	if(rc < 0) {
		fprintf(stderr, "Pull connection error %s\n", uv_strerror(rc));
		return rc;
	}*/

	return 0;
}

static int auth(SLNPullRef const pull) {
	if(!pull) return 0;
	FREE(&pull->cookie);

	HTTPConnectionRef conn = NULL;
	int rc = 0;
	rc = rc < 0 ? rc : HTTPConnectionCreateOutgoing(pull->host, &conn);
	rc = rc < 0 ? rc : HTTPConnectionWriteRequest(conn, HTTP_POST, "/efs/auth", pull->host);
	rc = rc < 0 ? rc : HTTPConnectionWriteContentLength(conn, 0);
	rc = rc < 0 ? rc : HTTPConnectionBeginBody(conn);
	// TODO: Send credentials.
	rc = rc < 0 ? rc : HTTPConnectionEnd(conn);
	if(rc < 0) {
		fprintf(stderr, "Pull authentication error %s\n", uv_strerror(rc));
		HTTPConnectionFree(&conn);
		return rc;
	}

	int const status = HTTPConnectionReadResponseStatus(conn);
	if(status < 0) {
		fprintf(stderr, "Pull authentication status %d\n", status);
		HTTPConnectionFree(&conn);
		return status;
	}

	HTTPHeadersRef headers = HTTPHeadersCreateFromConnection(conn);
	assert(headers); // TODO
	strarg_t const cookie = HTTPHeadersGet(headers, "set-cookie");
	assert(cookie);

	HTTPConnectionFree(&conn);

	if(!prefix("s=", cookie)) {
		HTTPHeadersFree(&headers);
		return -1;
	}

	strarg_t x = cookie+2;
	while('\0' != *x && ';' != *x) x++;

	FREE(&pull->cookie);
	pull->cookie = strndup(cookie, x-cookie);
	if(!pull->cookie) {
		HTTPHeadersFree(&headers);
		return UV_ENOMEM;
	}
	fprintf(stderr, "Cookie for %s: %s\n", pull->host, pull->cookie);
	// TODO: Update database?

	HTTPHeadersFree(&headers);
	return 0;
}


static int import(SLNPullRef const pull, strarg_t const URI, index_t const pos, HTTPConnectionRef *const conn) {
	if(!pull) return 0;
	if(!pull->cookie) goto fail;

	// TODO: Even if there's nothing to do, we have to enqueue something to fill up our reserved slots. I guess it's better than doing a lot of work inside the connection lock, but there's got to be a better way.
	SLNSubmissionRef sub = NULL;
	HTTPHeadersRef headers = NULL;

	if(!URI) goto enqueue;

	str_t algo[SLN_ALGO_SIZE];
	str_t hash[SLN_HASH_SIZE];
	if(SLNParseURI(URI, algo, hash) < 0) goto enqueue;

	if(SLNSessionGetFileInfo(pull->session, URI, NULL) >= 0) goto enqueue;

	// TODO: We're logging out of order when we do it like this...
//	fprintf(stderr, "Pulling %s\n", URI);

	int rc = 0;
	if(!*conn) {
		rc = HTTPConnectionCreateOutgoing(pull->host, conn);
		if(rc < 0) {
			fprintf(stderr, "Pull import connection error %s\n", uv_strerror(rc));
			goto fail;
		}
	}

	str_t *path = aasprintf("/efs/file/%s/%s", algo, hash);
	if(!path) {
		fprintf(stderr, "Pull aasprintf error\n");
		goto fail;
	}
	rc = 0;
	rc = rc < 0 ? rc : HTTPConnectionWriteRequest(*conn, HTTP_GET, path, pull->host);
	FREE(&path);

	assert(pull->cookie);
	rc = rc < 0 ? rc : HTTPConnectionWriteHeader(*conn, "Cookie", pull->cookie);
	rc = rc < 0 ? rc : HTTPConnectionBeginBody(*conn);
	rc = rc < 0 ? rc : HTTPConnectionEnd(*conn);
	if(rc < 0) {
		fprintf(stderr, "Pull import request error %s\n", uv_strerror(rc));
		goto fail;
	}
	int const status = HTTPConnectionReadResponseStatus(*conn);
	if(status < 0) {
		fprintf(stderr, "Pull import response error %s\n", uv_strerror(status));
		goto fail;
	}
	if(status < 200 || status >= 300) {
		fprintf(stderr, "Pull import status error %d\n", status);
		goto fail;
	}

	headers = HTTPHeadersCreateFromConnection(*conn);
	assert(headers); // TODO
/*	if(rc < 0) {
		fprintf(stderr, "Pull import headers error %s\n", uv_strerror(rc));
		goto fail;
	}*/
	strarg_t const type = HTTPHeadersGet(headers, "content-type");

	sub = SLNSubmissionCreate(pull->session, type);
	if(!sub) {
		fprintf(stderr, "Pull submission error\n");
		goto fail;
	}
	for(;;) {
		if(pull->stop) goto fail;
		uv_buf_t buf[1] = {};
		rc = HTTPConnectionReadBody(*conn, buf);
		if(rc < 0) {
			fprintf(stderr, "Pull download error %s\n", uv_strerror(rc));
			goto fail;
		}
		if(0 == buf->len) break;
		rc = SLNSubmissionWrite(sub, (byte_t *)buf->base, buf->len);
		if(rc < 0) {
			fprintf(stderr, "Pull write error\n");
			goto fail;
		}
	}
	rc = SLNSubmissionEnd(sub);
	if(rc < 0) {
		fprintf(stderr, "Pull submission error %s\n", uv_strerror(rc));
		goto fail;
	}

enqueue:
	HTTPHeadersFree(&headers);
	async_mutex_lock(pull->mutex);
	pull->queue[pos] = sub; sub = NULL;
	pull->filled[pos] = true;
	async_cond_broadcast(pull->cond);
	async_mutex_unlock(pull->mutex);
	return 0;

fail:
	HTTPHeadersFree(&headers);
	SLNSubmissionFree(&sub);
	HTTPConnectionFree(conn);
	return -1;
}
