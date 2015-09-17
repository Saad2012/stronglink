// Copyright 2014-2015 Ben Trask
// MIT licensed (see LICENSE for details)

#include <assert.h>
#include "HTTPConnection.h"
#include "status.h"
#include "../util/strext.h"

#define BUFFER_SIZE (1024 * 8)

enum {
	HTTPMessageIncomplete = 1 << 0,
};

static http_parser_settings const settings;

struct HTTPConnection {
	SocketRef socket;
	http_parser parser[1];
	HTTPEvent type;
	uv_buf_t out[1];
	unsigned flags;
};

int HTTPConnectionCreateIncoming(uv_stream_t *const ssocket, unsigned const flags, HTTPConnectionRef *const out) {
	return HTTPConnectionCreateIncomingSecure(ssocket, NULL, flags, out);
}
int HTTPConnectionCreateIncomingSecure(uv_stream_t *const ssocket, struct tls *const ssecure, unsigned const flags, HTTPConnectionRef *const out) {
	HTTPConnectionRef conn = calloc(1, sizeof(struct HTTPConnection));
	if(!conn) return UV_ENOMEM;
	int rc = SocketAccept(ssocket, ssecure, &conn->socket);
	if(rc < 0) goto cleanup;
	http_parser_init(conn->parser, HTTP_REQUEST);
	conn->parser->data = conn;
	*out = conn; conn = NULL;
cleanup:
	HTTPConnectionFree(&conn);
	return rc;
}

int HTTPConnectionCreateOutgoing(strarg_t const domain, unsigned const flags, HTTPConnectionRef *const out) {
	assert(0);
	return UV_ENOSYS;
/*	str_t host[1023+1];
	str_t service[15+1];
	host[0] = '\0';
	service[0] = '\0';
	int matched = sscanf(domain, "%1023[^:]:%15[0-9]", host, service);
	if(matched < 1) return UV_EINVAL;
	if('\0' == host[0]) return UV_EINVAL;

	static struct addrinfo const hints = {
		.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG | AI_NUMERICSERV,
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = 0, // ???
	};
	struct addrinfo *info = NULL;
	HTTPConnectionRef conn = NULL;
	int rc;

	rc = async_getaddrinfo(host, service[0] ? service : "80", &hints, &info);
	if(rc < 0) goto cleanup;

	conn = calloc(1, sizeof(struct HTTPConnection));
	if(!conn) rc = UV_ENOMEM;
	if(rc < 0) goto cleanup;

	rc = UV_EADDRNOTAVAIL;
	for(struct addrinfo *each = info; each; each = each->ai_next) {
		rc = uv_tcp_init(async_loop, conn->stream);
		if(rc < 0) break;

		rc = async_tcp_connect(conn->stream, each->ai_addr);
		if(rc >= 0) break;

		async_close((uv_handle_t *)conn->stream);
	}
	if(rc < 0) goto cleanup;

	http_parser_init(conn->parser, HTTP_RESPONSE);
	conn->parser->data = conn;
	*out = conn; conn = NULL;

cleanup:
	uv_freeaddrinfo(info); info = NULL;
	HTTPConnectionFree(&conn);
	return rc;*/
}
void HTTPConnectionFree(HTTPConnectionRef *const connptr) {
	HTTPConnectionRef conn = *connptr;
	if(!conn) return;

	SocketFree(&conn->socket);

	// http_parser does not need to be freed, closed or destroyed.
	memset(conn->parser, 0, sizeof(*conn->parser));

	conn->type = HTTPNothing;
	*conn->out = uv_buf_init(NULL, 0);

	conn->flags = 0;

	assert_zeroed(conn, 1);
	FREE(connptr); conn = NULL;
}

int HTTPConnectionStatus(HTTPConnectionRef const conn) {
	if(!conn) return UV_EINVAL;
	int rc = HTTP_PARSER_ERRNO(conn->parser);
	if(HPE_INVALID_EOF_STATE == rc) return UV_ECONNABORTED;
	if(HPE_OK != rc && HPE_PAUSED != rc) return UV_UNKNOWN;
	rc = SocketStatus(conn->socket);
	if(rc < 0) return rc;
	return 0;
}
int HTTPConnectionPeek(HTTPConnectionRef const conn, HTTPEvent *const type, uv_buf_t *const buf) {
	if(!conn) return UV_EINVAL;
	if(!type) return UV_EINVAL;
	if(!buf) return UV_EINVAL;

	// Repeat previous errors.
	int rc = HTTPConnectionStatus(conn);
	if(rc < 0) return rc;

	while(HTTPNothing == conn->type) {
		uv_buf_t raw[1];
		rc = SocketPeek(conn->socket, raw);
		if(UV_EAGAIN == rc) continue;
		if(UV_EOF == rc && (HTTPMessageIncomplete & conn->flags)) {
			rc = 0;
			*raw = uv_buf_init(NULL, 0);
		}
		if(rc < 0) return rc;

		http_parser_pause(conn->parser, 0);
		size_t len = http_parser_execute(conn->parser, &settings, raw->base, raw->len);
		rc = HTTP_PARSER_ERRNO(conn->parser);

		// HACK: http_parser returns 1 when the input length is 0 (EOF).
		if(len > raw->len) len = raw->len;

		SocketPop(conn->socket, len);

		if(HPE_INVALID_EOF_STATE == rc) return UV_ECONNABORTED;
		if(HPE_OK != rc && HPE_PAUSED != rc) {
			// TODO: We should convert HPE_* and return them
			// instead of logging and returning UV_UNKNOWN.
			alogf("HTTP parse error: %s (%d)\n",
				http_errno_name(rc),
				HTTP_PARSER_ERRNO_LINE(conn->parser));
//			alogf("%s (%lu)\n", strndup(raw->base, raw->len), raw->len);
			return UV_UNKNOWN;
		}
	}
	assertf(HTTPNothing != conn->type, "HTTPConnectionPeek must return an event");
	*type = conn->type;
	*buf = *conn->out;
	return 0;
}
void HTTPConnectionPop(HTTPConnectionRef const conn, size_t const len) {
	if(!conn) return;
	assert(len <= conn->out->len);
	conn->out->base += len;
	conn->out->len -= len;
	if(conn->out->len) return;
	conn->type = HTTPNothing;
	conn->out->base = NULL;
}


ssize_t HTTPConnectionReadRequest(HTTPConnectionRef const conn, HTTPMethod *const method, str_t *const out, size_t const max) {
	if(!conn) return UV_EINVAL;
	if(!max) return UV_EINVAL;
	uv_buf_t buf[1];
	int rc;
	HTTPEvent type;
	size_t len = 0;
	for(;;) {
		// TODO
		// Use unref because we shouldn't block the server
		// on a request that may never arrive.
//		uv_unref((uv_handle_t *)conn->stream);
		rc = HTTPConnectionPeek(conn, &type, buf);
//		uv_ref((uv_handle_t *)conn->stream);
		if(rc < 0) return rc;
		if(HTTPHeaderField == type || HTTPHeadersComplete == type) break;
		HTTPConnectionPop(conn, buf->len);
		if(HTTPMessageBegin == type) continue;
		if(HTTPURL != type) {
			assertf(0, "Unexpected HTTP event %d", type);
			return UV_UNKNOWN;
		}
		if(len+buf->len+1 > max) return UV_EMSGSIZE;
		memcpy(out+len, buf->base, buf->len);
		len += buf->len;
		out[len] = '\0';
	}
	*method = conn->parser->method;
	return (ssize_t)len;
}
int HTTPConnectionReadResponseStatus(HTTPConnectionRef const conn) {
	if(!conn) return UV_EINVAL;
	uv_buf_t buf[1];
	int rc;
	HTTPEvent type;
	for(;;) {
		rc = HTTPConnectionPeek(conn, &type, buf);
		if(rc < 0) return rc;
		if(HTTPHeaderField == type || HTTPHeadersComplete == type) break;
		if(HTTPMessageBegin != type) {
			assertf(0, "Unexpected HTTP event %d", type);
			return UV_UNKNOWN;
		}
		HTTPConnectionPop(conn, buf->len);
	}
	return conn->parser->status_code;
}

ssize_t HTTPConnectionReadHeaderField(HTTPConnectionRef const conn, str_t out[], size_t const max) {
	if(!conn) return UV_EINVAL;
	uv_buf_t buf[1];
	int rc;
	HTTPEvent type;
	size_t len = 0;
	if(max > 0) out[0] = '\0';
	for(;;) {
		rc = HTTPConnectionPeek(conn, &type, buf);
		if(rc < 0) return rc;
		if(HTTPHeaderValue == type) break;
		if(HTTPHeadersComplete == type) break;
		HTTPConnectionPop(conn, buf->len);
		if(HTTPHeaderField != type) {
			assertf(0, "Unexpected HTTP event %d", type);
			return UV_UNKNOWN;
		}
		if(len+buf->len+1 > max) return UV_EMSGSIZE;
		memcpy(out+len, buf->base, buf->len);
		len += buf->len;
		out[len] = '\0';
	}
	return (ssize_t)len;
}
ssize_t HTTPConnectionReadHeaderValue(HTTPConnectionRef const conn, str_t out[], size_t const max) {
	if(!conn) return UV_EINVAL;
	uv_buf_t buf[1];
	int rc;
	HTTPEvent type;
	size_t len = 0;
	if(max > 0) out[0] = '\0';
	for(;;) {
		rc = HTTPConnectionPeek(conn, &type, buf);
		if(rc < 0) return rc;
		if(HTTPHeaderField == type) break;
		if(HTTPHeadersComplete == type) break;
		HTTPConnectionPop(conn, buf->len);
		if(HTTPHeaderValue != type) {
			assertf(0, "Unexpected HTTP event %d", type);
			return UV_UNKNOWN;
		}
		if(len+buf->len+1 > max) return UV_EMSGSIZE;
		memcpy(out+len, buf->base, buf->len);
		len += buf->len;
		out[len] = '\0';
	}
	return (ssize_t)len;
}
int HTTPConnectionReadBody(HTTPConnectionRef const conn, uv_buf_t *const buf) {
	if(!conn) return UV_EINVAL;
	HTTPEvent type;
	int rc = HTTPConnectionPeek(conn, &type, buf);
	if(rc < 0) return rc;
	if(HTTPBody != type && HTTPMessageEnd != type) {
		assertf(0, "Unexpected HTTP event %d", type);
		return UV_UNKNOWN;
	}
	HTTPConnectionPop(conn, buf->len);
	return 0;
}
int HTTPConnectionReadBodyLine(HTTPConnectionRef const conn, str_t out[], size_t const max) {
	if(!conn) return UV_EINVAL;
	if(!max) return UV_EINVAL;
	uv_buf_t buf[1];
	int rc;
	size_t i;
	HTTPEvent type;
	out[0] = '\0';
	for(;;) {
		rc = HTTPConnectionPeek(conn, &type, buf);
		if(rc < 0) return rc;
		if(HTTPMessageEnd == type) {
			if(out[0]) return 0;
			HTTPConnectionPop(conn, buf->len);
			return UV_EOF;
		}
		if(HTTPBody != type) {
			assertf(0, "Unexpected HTTP event %d", type);
			return UV_UNKNOWN;
		}
		for(i = 0; i < buf->len; ++i) {
			if('\r' == buf->base[i]) break;
			if('\n' == buf->base[i]) break;
		}
		append_buf_to_string(out, max, buf->base, i);
		HTTPConnectionPop(conn, i);
		if(i < buf->len) break;
	}

	rc = HTTPConnectionPeek(conn, &type, buf);
	if(rc < 0) return rc;
	if(HTTPMessageEnd == type) {
		if(out[0]) return 0;
		HTTPConnectionPop(conn, i);
		return UV_EOF;
	}
	if(HTTPBody != type) return UV_UNKNOWN;
	if('\r' == buf->base[0]) HTTPConnectionPop(conn, 1);

	rc = HTTPConnectionPeek(conn, &type, buf);
	if(rc < 0) return rc;
	if(HTTPMessageEnd == type) {
		if(out[0]) return 0;
		HTTPConnectionPop(conn, i);
		return UV_EOF;
	}
	if(HTTPBody != type) return UV_UNKNOWN;
	if('\n' == buf->base[0]) HTTPConnectionPop(conn, 1);

	return 0;
}
ssize_t HTTPConnectionReadBodyStatic(HTTPConnectionRef const conn, byte_t *const out, size_t const max) {
	if(!conn) return UV_EINVAL;
	ssize_t len = 0;
	for(;;) {
		uv_buf_t buf[1];
		int rc = HTTPConnectionReadBody(conn, buf);
		if(rc < 0) return rc;
		if(!buf->len) break;
		if(len+buf->len >= max) return UV_EMSGSIZE;
		memcpy(out, buf->base, buf->len);
		len += buf->len;
	}
	return len;
}
int HTTPConnectionDrainMessage(HTTPConnectionRef const conn) {
	if(!conn) return 0;

	// It's critical that we track and report persistent errors here so
	// that the server knows to close the connection. Failure to do so
	// can cause an endless loop as we keep failing to process the same
	// request.
	int rc = HTTPConnectionStatus(conn);
	if(rc < 0) return rc;

	if(!(HTTPMessageIncomplete & conn->flags)) return 0;

	uv_buf_t buf[1];
	HTTPEvent type;
	for(;;) {
		rc = HTTPConnectionPeek(conn, &type, buf);
		if(rc < 0) return rc;
		if(HTTPMessageBegin == type) {
			assertf(0, "HTTPConnectionDrainMessage shouldn't start a new message");
			return UV_UNKNOWN;
		}
		HTTPConnectionPop(conn, buf->len);
		if(HTTPMessageEnd == type) break;
	}
	return 0;
}


int HTTPConnectionWrite(HTTPConnectionRef const conn, byte_t const *const buf, size_t const len) {
	if(!conn) return 0;
	uv_buf_t info = uv_buf_init((char *)buf, len);
	return SocketWrite(conn->socket, &info);
}
int HTTPConnectionWritev(HTTPConnectionRef const conn, uv_buf_t parts[], unsigned int const count) {
	if(!conn) return 0;
	for(size_t i = 0; i < count; i++) {
		int rc = SocketWrite(conn->socket, &parts[i]);
		if(rc < 0) return rc;
	}
	return 0;
}
int HTTPConnectionWriteRequest(HTTPConnectionRef const conn, HTTPMethod const method, strarg_t const requestURI, strarg_t const host) {
	if(!conn) return 0;
	strarg_t methodstr = http_method_str(method);
	uv_buf_t parts[] = {
		uv_buf_init((char *)methodstr, strlen(methodstr)),
		uv_buf_init((char *)STR_LEN(" ")),
		uv_buf_init((char *)requestURI, strlen(requestURI)),
		uv_buf_init((char *)STR_LEN(" HTTP/1.1\r\n")),
		uv_buf_init((char *)STR_LEN("Host: ")),
		uv_buf_init((char *)host, strlen(host)),
		uv_buf_init((char *)STR_LEN("\r\n")),
	};
	return HTTPConnectionWritev(conn, parts, numberof(parts));
}

int HTTPConnectionWriteResponse(HTTPConnectionRef const conn, uint16_t const status, strarg_t const message) {
	assertf(status >= 100 && status < 600, "Invalid HTTP status %d", (int)status);
	if(!conn) return 0;

	str_t status_str[4+1];
	int status_len = snprintf(status_str, sizeof(status_str), "%d", status);
	assert(3 == status_len);

	uv_buf_t parts[] = {
		uv_buf_init((char *)STR_LEN("HTTP/1.1 ")),
		uv_buf_init(status_str, status_len),
		uv_buf_init((char *)STR_LEN(" ")),
		uv_buf_init((char *)message, strlen(message)),
		uv_buf_init((char *)STR_LEN("\r\n")),
	};
	return HTTPConnectionWritev(conn, parts, numberof(parts));
}
int HTTPConnectionWriteHeader(HTTPConnectionRef const conn, strarg_t const field, strarg_t const value) {
	assert(field);
	assert(value);
	if(!conn) return 0;
	uv_buf_t parts[] = {
		uv_buf_init((char *)field, strlen(field)),
		uv_buf_init((char *)STR_LEN(": ")),
		uv_buf_init((char *)value, strlen(value)),
		uv_buf_init((char *)STR_LEN("\r\n")),
	};
	return HTTPConnectionWritev(conn, parts, numberof(parts));
}
int HTTPConnectionWriteContentLength(HTTPConnectionRef const conn, uint64_t const length) {
	if(!conn) return 0;
	str_t str[16];
	int const len = snprintf(str, sizeof(str), "%llu", (unsigned long long)length);
	uv_buf_t parts[] = {
		uv_buf_init((char *)STR_LEN("Content-Length: ")),
		uv_buf_init(str, len),
		uv_buf_init((char *)STR_LEN("\r\n")),
	};
	return HTTPConnectionWritev(conn, parts, numberof(parts));
}
int HTTPConnectionWriteSetCookie(HTTPConnectionRef const conn, strarg_t const cookie, strarg_t const path, uint64_t const maxage) {
	assert(cookie);
	assert(path);
	if(!conn) return 0;
	str_t maxage_str[16];
	int const maxage_len = snprintf(maxage_str, sizeof(maxage_str), "%llu", (unsigned long long)maxage);
	uv_buf_t parts[] = {
		uv_buf_init((char *)STR_LEN("Set-Cookie: ")),
		uv_buf_init((char *)cookie, strlen(cookie)),
		uv_buf_init((char *)STR_LEN("; Path=")),
		uv_buf_init((char *)path, strlen(path)),
		uv_buf_init((char *)STR_LEN("; Max-Age=")),
		uv_buf_init(maxage_str, maxage_len),
		uv_buf_init((char *)STR_LEN("; HttpOnly")),
		SocketIsSecure(conn->socket) ?
			uv_buf_init((char *)STR_LEN("; Secure" "\r\n")) :
			uv_buf_init((char *)STR_LEN("\r\n")),
	};
	return HTTPConnectionWritev(conn, parts, numberof(parts));
}
int HTTPConnectionBeginBody(HTTPConnectionRef const conn) {
	if(!conn) return 0;
	if(SocketIsSecure(conn->socket)) {
		return HTTPConnectionWrite(conn, (byte_t *)STR_LEN(
			"Strict-Transport-Security: max-age=31536000; includeSubDomains; preload\r\n"
			"Connection: keep-alive\r\n"
			"\r\n"));
	} else {
		return HTTPConnectionWrite(conn, (byte_t *)STR_LEN(
			"Connection: keep-alive\r\n"
			"\r\n"));
	}
}
int HTTPConnectionWriteFile(HTTPConnectionRef const conn, uv_file const file) {
	// TODO: This function should support lengths and offsets.
	if(!conn) return 0;
	char *buf = malloc(BUFFER_SIZE);
	if(!buf) return UV_ENOMEM;
	uv_buf_t const info = uv_buf_init(buf, BUFFER_SIZE);
	int rc;
	for(;;) {
		ssize_t const len = rc = async_fs_readall_simple(file, &info);
		if(0 == len) break;
		if(rc < 0) break;
		uv_buf_t write = uv_buf_init(buf, len);
		rc = SocketWrite(conn->socket, &write);
		if(rc < 0) break;
	}
	FREE(&buf);
	return rc;
}
int HTTPConnectionWriteChunkLength(HTTPConnectionRef const conn, uint64_t const length) {
	if(!conn) return 0;
	str_t str[16];
	int const slen = snprintf(str, sizeof(str), "%llx\r\n", (unsigned long long)length);
	if(slen < 0) return UV_UNKNOWN;
	return HTTPConnectionWrite(conn, (byte_t const *)str, slen);
}
int HTTPConnectionWriteChunkv(HTTPConnectionRef const conn, uv_buf_t parts[], unsigned int const count) {
	if(!conn) return 0;
	uint64_t total = 0;
	for(size_t i = 0; i < count; i++) total += parts[i].len;
	if(total <= 0) return 0;
	int rc = 0;
	rc = rc < 0 ? rc : HTTPConnectionWriteChunkLength(conn, total);
	rc = rc < 0 ? rc : HTTPConnectionWritev(conn, parts, count);
	rc = rc < 0 ? rc : HTTPConnectionWrite(conn, (byte_t const *)STR_LEN("\r\n"));
	return rc;
}
int HTTPConnectionWriteChunkFile(HTTPConnectionRef const conn, strarg_t const path) {
	bool worker = false;
	uv_file file = -1;
	byte_t *buf = NULL;
	int rc;

	async_pool_enter(NULL); worker = true;
	rc = async_fs_open(path, O_RDONLY, 0000);
	if(rc < 0) goto cleanup;
	file = rc;

	buf = malloc(BUFFER_SIZE);
	if(!buf) rc = UV_ENOMEM;
	if(rc < 0) goto cleanup;

	uv_buf_t chunk = uv_buf_init((char *)buf, BUFFER_SIZE);
	ssize_t len = async_fs_readall_simple(file, &chunk);
	if(len < 0) rc = len;
	if(rc < 0) goto cleanup;

	// Fast path for small files.
	if(len < BUFFER_SIZE) {
		str_t pfx[16];
		int const pfxlen = snprintf(pfx, sizeof(pfx), "%llx\r\n", (unsigned long long)len);
		if(pfxlen < 0) rc = UV_UNKNOWN;
		if(rc < 0) goto cleanup;

		uv_buf_t parts[] = {
			uv_buf_init(pfx, pfxlen),
			uv_buf_init((char *)buf, len),
			uv_buf_init((char *)STR_LEN("\r\n")),
		};
		async_fs_close(file); file = -1;
		async_pool_leave(NULL); worker = false;
		rc = HTTPConnectionWritev(conn, parts, numberof(parts));
		goto cleanup;
	}

	uv_fs_t req[1];
	rc = async_fs_fstat(file, req);
	if(rc < 0) goto cleanup;
	if(0 == req->statbuf.st_size) goto cleanup;

	async_pool_leave(NULL); worker = false;

	// TODO: HACK, WriteFile continues from where we left off
	rc = rc < 0 ? rc : HTTPConnectionWriteChunkLength(conn, req->statbuf.st_size);
	rc = rc < 0 ? rc : HTTPConnectionWritev(conn, &chunk, 1);
	rc = rc < 0 ? rc : HTTPConnectionWriteFile(conn, file);
	rc = rc < 0 ? rc : HTTPConnectionWrite(conn, (byte_t const *)STR_LEN("\r\n"));

cleanup:
	FREE(&buf);
	if(file >= 0) { async_fs_close(file); file = -1; }
	if(worker) { async_pool_leave(NULL); worker = false; }
	assert(file < 0);
	assert(!worker);
	return rc;
}
int HTTPConnectionWriteChunkEnd(HTTPConnectionRef const conn) {
	if(!conn) return 0;
	return HTTPConnectionWrite(conn, (byte_t const *)STR_LEN("0\r\n\r\n"));
}
int HTTPConnectionEnd(HTTPConnectionRef const conn) {
	if(!conn) return 0;
	int rc = HTTPConnectionFlush(conn);
	if(rc < 0) return rc;
	// We assume keep-alive is enabled.
	return 0;
}
int HTTPConnectionFlush(HTTPConnectionRef const conn) {
	if(!conn) return 0;
	return SocketFlush(conn->socket, false);
}

int HTTPConnectionSendMessage(HTTPConnectionRef const conn, uint16_t const status, strarg_t const str) {
	if(!conn) return 0;
	size_t const len = strlen(str);
	int rc = 0;
	rc = rc < 0 ? rc : HTTPConnectionWriteResponse(conn, status, str);
	rc = rc < 0 ? rc : HTTPConnectionWriteHeader(conn, "Content-Type", "text/plain; charset=utf-8");
	rc = rc < 0 ? rc : HTTPConnectionWriteContentLength(conn, len+1);
	rc = rc < 0 ? rc : HTTPConnectionBeginBody(conn);
	// TODO: Check how HEAD responses should look.
	if(HTTP_HEAD != conn->parser->method) { // TODO: Expose method? What?
		rc = rc < 0 ? rc : HTTPConnectionWrite(conn, (byte_t const *)str, len);
		rc = rc < 0 ? rc : HTTPConnectionWrite(conn, (byte_t const *)STR_LEN("\n"));
	}
	rc = rc < 0 ? rc : HTTPConnectionEnd(conn);
//	if(status >= 400) alogf("%s: %d %s\n", HTTPConnectionGetRequestURI(conn), (int)status, str);
	return rc;
}
int HTTPConnectionSendStatus(HTTPConnectionRef const conn, uint16_t const status) {
	strarg_t const str = statusstr(status);
	return HTTPConnectionSendMessage(conn, status, str);
}
int HTTPConnectionSendRedirect(HTTPConnectionRef const conn, uint16_t const status, strarg_t const location) {
	int rc = 0;
	strarg_t const str = statusstr(status);
	rc = rc < 0 ? rc : HTTPConnectionWriteResponse(conn, status, str);
	rc = rc < 0 ? rc : HTTPConnectionWriteHeader(conn, "Location", location);
	rc = rc < 0 ? rc : HTTPConnectionWriteContentLength(conn, 0);
	rc = rc < 0 ? rc : HTTPConnectionBeginBody(conn);
	rc = rc < 0 ? rc : HTTPConnectionEnd(conn);
	return rc;
}
int HTTPConnectionSendFile(HTTPConnectionRef const conn, strarg_t const path, strarg_t const type, int64_t size) {
	int rc = async_fs_open(path, O_RDONLY, 0000);
	if(UV_ENOENT == rc) return HTTPConnectionSendStatus(conn, 404);
	if(rc < 0) return HTTPConnectionSendStatus(conn, 400); // TODO: Error conversion.

	uv_file file = rc; rc = 0;
	if(size < 0) {
		uv_fs_t req[1];
		rc = async_fs_fstat(file, req);
		if(rc < 0) {
			rc = HTTPConnectionSendStatus(conn, 400);
			goto cleanup;
		}
		if(S_ISDIR(req->statbuf.st_mode)) {
			rc = UV_EISDIR;
			goto cleanup;
		}
		if(!S_ISREG(req->statbuf.st_mode)) {
			rc = HTTPConnectionSendStatus(conn, 403);
			goto cleanup;
		}
		size = req->statbuf.st_size;
	}
	rc = rc < 0 ? rc : HTTPConnectionWriteResponse(conn, 200, "OK");
	rc = rc < 0 ? rc : HTTPConnectionWriteContentLength(conn, size);
	rc = rc < 0 ? rc : HTTPConnectionWriteHeader(conn, "Cache-Control", "max-age=604800, public"); // TODO: Just cache all static files for one week, for now.
	if(type) rc = rc < 0 ? rc : HTTPConnectionWriteHeader(conn, "Content-Type", type);
	rc = rc < 0 ? rc : HTTPConnectionBeginBody(conn);
	rc = rc < 0 ? rc : HTTPConnectionWriteFile(conn, file);
	rc = rc < 0 ? rc : HTTPConnectionEnd(conn);

cleanup:
	async_fs_close(file); file = -1;
	return rc;
}


static int on_message_begin(http_parser *const parser) {
	HTTPConnectionRef const conn = parser->data;
	assert(!(HTTPMessageIncomplete & conn->flags));
	conn->type = HTTPMessageBegin;
	*conn->out = uv_buf_init(NULL, 0);
	conn->flags |= HTTPMessageIncomplete;
	http_parser_pause(parser, 1);
	return 0;
}
static int on_url(http_parser *const parser, char const *const at, size_t const len) {
	HTTPConnectionRef const conn = parser->data;
	conn->type = HTTPURL;
	*conn->out = uv_buf_init((char *)at, len);
	http_parser_pause(parser, 1);
	return 0;
}
static int on_header_field(http_parser *const parser, char const *const at, size_t const len) {
	HTTPConnectionRef const conn = parser->data;
	conn->type = HTTPHeaderField;
	*conn->out = uv_buf_init((char *)at, len);
	http_parser_pause(parser, 1);
	return 0;
}
static int on_header_value(http_parser *const parser, char const *const at, size_t const len) {
	HTTPConnectionRef const conn = parser->data;
	conn->type = HTTPHeaderValue;
	*conn->out = uv_buf_init((char *)at, len);
	http_parser_pause(parser, 1);
	return 0;
}
static int on_headers_complete(http_parser *const parser) {
	HTTPConnectionRef const conn = parser->data;
	conn->type = HTTPHeadersComplete;
	*conn->out = uv_buf_init(NULL, 0);
	http_parser_pause(parser, 1);
	return 0;
}
static int on_body(http_parser *const parser, char const *const at, size_t const len) {
	HTTPConnectionRef const conn = parser->data;
	conn->type = HTTPBody;
	*conn->out = uv_buf_init((char *)at, len);
	http_parser_pause(parser, 1);
	return 0;
}
static int on_message_complete(http_parser *const parser) {
	HTTPConnectionRef const conn = parser->data;
	assert(HTTPMessageIncomplete & conn->flags);
	conn->type = HTTPMessageEnd;
	*conn->out = uv_buf_init(NULL, 0);
	conn->flags &= ~HTTPMessageIncomplete;
	http_parser_pause(parser, 1);
	return 0;
}
static http_parser_settings const settings = {
	.on_message_begin = on_message_begin,
	.on_url = on_url,
	.on_header_field = on_header_field,
	.on_header_value = on_header_value,
	.on_headers_complete = on_headers_complete,
	.on_body = on_body,
	.on_message_complete = on_message_complete,
};

