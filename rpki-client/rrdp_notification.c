/*
 * Copyright (c) 2020 Nils Fisher <nils_fisher@hotmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <expat.h>

#include "extern.h"
#include "rrdp.h"

/* notification */
#define STATE_FILENAME ".state"

enum notification_scope {
	NOTIFICATION_SCOPE_START,
	NOTIFICATION_SCOPE_NOTIFICATION,
	NOTIFICATION_SCOPE_SNAPSHOT,
	NOTIFICATION_SCOPE_NOTIFICATION_POST_SNAPSHOT,
	NOTIFICATION_SCOPE_DELTA,
	NOTIFICATION_SCOPE_END
};

enum notification_state {
	NOTIFICATION_STATE_SNAPSHOT,
	NOTIFICATION_STATE_DELTAS,
	NOTIFICATION_STATE_NONE,
	NOTIFICATION_STATE_ERROR
};

struct delta_item {
	char			*uri;
	char			*hash;
	long long		 serial;
	TAILQ_ENTRY(delta_item)	 q;
};

TAILQ_HEAD(delta_q, delta_item);

struct notification_xml {
	XML_Parser		parser;
	char			*session_id;
	char			*current_session_id;
	char			*snapshot_uri;
	char			*snapshot_hash;
	struct delta_q		delta_q;
	long long		serial;
	long long		current_serial;
	int			version;
	enum notification_scope	scope;
	enum notification_state	state;
};

static int
add_delta(struct notification_xml *nxml, const char *uri, const char *hash,
    long long serial)
{
	struct delta_item *d, *n;

	if ((d = calloc(1, sizeof(struct delta_item))) == NULL)
		err(1, "%s - calloc", __func__);

	d->serial = serial;
	d->uri = xstrdup(uri);
	d->hash = xstrdup(hash);

	/* XXX this is strange */
	n = TAILQ_LAST(&nxml->delta_q, delta_q);
	if (!n || serial < n->serial) {
		TAILQ_FOREACH(n, &nxml->delta_q, q) {
			if (n->serial == serial) {
				warnx("duplicate delta serial %lld ", serial);
				return 0;
			}
			if (n->serial < serial)
				break;
		}
	}

	if (n)
		TAILQ_INSERT_AFTER(&nxml->delta_q, n, d, q);
	else
		TAILQ_INSERT_HEAD(&nxml->delta_q, d, q);

	return 1;
}

static void
free_delta(struct delta_item *d)
{
	free(d->uri);
	free(d->hash);
	free(d);
}

void
check_state(struct notification_xml *nxml)
{
	struct delta_item *d;
	int serial_counter = 0;
	int serial_diff;

	/* Already have an error or already up to date keep it persistent */
	if (nxml->state == NOTIFICATION_STATE_ERROR ||
	    nxml->state == NOTIFICATION_STATE_NONE)
		return;

	/* No current data have to go from the snapshot */
	if (nxml->current_session_id == NULL || nxml->current_serial == 0) {
		nxml->state = NOTIFICATION_STATE_SNAPSHOT;
		return;
	}

	/* No data and yet check_state was called */
	if (nxml->session_id == NULL || nxml->serial == 0) {
		nxml->state = NOTIFICATION_STATE_ERROR;
		return;
	}

	/* New session available should go from snapshot */
	if(strcmp(nxml->current_session_id, nxml->session_id) != 0) {
		nxml->state = NOTIFICATION_STATE_SNAPSHOT;
		return;
	}

	serial_diff = nxml->serial - nxml->current_serial;

	if (serial_diff == 0) {
		/* Up to date, no further action needed */
		nxml->state = NOTIFICATION_STATE_NONE;
		return;
	}

	if (serial_diff < 0) {
		/* current serial is larger! can't even go from snapshot */
		warnx("serial_diff is negative %lld vs %lld",
		    nxml->serial, nxml->current_serial);
		nxml->state = NOTIFICATION_STATE_ERROR;
		return;
	}

	/* Exit early if we have not yet parsed the deltas */
	if (nxml->scope <= NOTIFICATION_SCOPE_DELTA) {
		return;
	}

	/* current serial is greater lets try deltas */
	TAILQ_FOREACH(d, &(nxml->delta_q), q) {
		serial_counter++;
		if (nxml->current_serial + serial_counter != d->serial) {
			/* missing delta fall back to snapshot */
			nxml->state = NOTIFICATION_STATE_SNAPSHOT;
			return;
		}
	}
	/* all deltas present? */
	if (serial_counter != serial_diff) {
		warnx("Mismatch # expected deltas vs. # found deltas");
		nxml->state = NOTIFICATION_STATE_SNAPSHOT;
		return;
	}
	log_debuginfo("Happy to apply %d deltas", serial_counter);
	/* All serials matched */
	nxml->state = NOTIFICATION_STATE_DELTAS;
}

void
log_notification_xml(struct notification_xml *nxml)
{
	logx("scope: %d", nxml->scope);
	logx("state: %d", nxml->state);
	logx("version: %d", nxml->version);
	logx("current_session_id: %s", nxml->current_session_id ?: "NULL");
	logx("current_serial: %lld", nxml->current_serial);
	logx("session_id: %s", nxml->session_id ?: "NULL");
	logx("serial: %lld", nxml->serial);
	logx("snapshot_uri: %s", nxml->snapshot_uri ?: "NULL");
	logx("snapshot_hash: %s", nxml->snapshot_hash ?: "NULL");
}


static void
start_notification_elem(struct notification_xml *nxml, const char **attr)
{
	XML_Parser p = nxml->parser;
	int has_xmlns = 0;
	size_t i;

	if (nxml->scope != NOTIFICATION_SCOPE_START) {
		PARSE_FAIL(p, "parse failed - entered notification "
		    "elem unexpectedely");
	}
	for (i = 0; attr[i]; i += 2) {
		const char *errstr = NULL;
		if (strcmp("xmlns", attr[i]) == 0) {
			has_xmlns = 1;
		} else if (strcmp("session_id", attr[i]) == 0) {
			nxml->session_id = xstrdup(attr[i+1]);
		} else if (strcmp("version", attr[i]) == 0) {
			nxml->version = strtonum(attr[i + 1],
			    1, MAX_VERSION, &errstr);
		} else if (strcmp("serial", attr[i]) == 0) {
			nxml->serial = strtonum(attr[i + 1],
			    1, LLONG_MAX, &errstr);
		} else {
			PARSE_FAIL(p, "parse failed - non conforming "
			    "attribute found in notification elem");
		}
		if (errstr != NULL) {
			PARSE_FAIL(p, "parse failed - non conforming "
			    "attribute found in notification elem");
		}
	}
	if (!(has_xmlns && nxml->version && nxml->session_id && nxml->serial)) {
		PARSE_FAIL(p, "parse failed - incomplete "
		    "notification attributes");
	}

	check_state(nxml);
	nxml->scope = NOTIFICATION_SCOPE_NOTIFICATION;
}

static void
end_notification_elem(struct notification_xml *nxml)
{
	XML_Parser p = nxml->parser;

	if (nxml->scope != NOTIFICATION_SCOPE_NOTIFICATION_POST_SNAPSHOT) {
		PARSE_FAIL(p, "parse failed - exited notification "
		    "elem unexpectedely");
	}
	nxml->scope = NOTIFICATION_SCOPE_END;
	/* check the state to see if we have enough delta info */
	check_state(nxml);
}

static void
start_snapshot_elem(struct notification_xml *nxml, const char **attr)
{
	XML_Parser p = nxml->parser;
	int i;

	if (nxml->scope != NOTIFICATION_SCOPE_NOTIFICATION) {
		PARSE_FAIL(p, "parse failed - entered snapshot "
		    "elem unexpectedely");
	}
	for (i = 0; attr[i]; i += 2) {
		if (strcmp("uri", attr[i]) == 0)
			nxml->snapshot_uri = xstrdup(attr[i+1]);
		else if (strcmp("hash", attr[i]) == 0)
			nxml->snapshot_hash = xstrdup(attr[i+1]);
		else {
			PARSE_FAIL(p, "parse failed - non conforming "
			    "attribute found in snapshot elem");
		}
	}
	if (!nxml->snapshot_uri || !nxml->snapshot_hash)
		PARSE_FAIL(p, "parse failed - incomplete snapshot attributes");

	nxml->scope = NOTIFICATION_SCOPE_SNAPSHOT;
}

static void
end_snapshot_elem(struct notification_xml *nxml)
{
	XML_Parser p = nxml->parser;

	if (nxml->scope != NOTIFICATION_SCOPE_SNAPSHOT) {
		PARSE_FAIL(p, "parse failed - exited snapshot "
		    "elem unexpectedely");
	}
	nxml->scope = NOTIFICATION_SCOPE_NOTIFICATION_POST_SNAPSHOT;
}

static void
start_delta_elem(struct notification_xml *nxml, const char **attr)
{
	XML_Parser p = nxml->parser;
	int i;
	const char *delta_uri = NULL;
	const char *delta_hash = NULL;
	long long delta_serial = 0;

	if (nxml->scope != NOTIFICATION_SCOPE_NOTIFICATION_POST_SNAPSHOT) {
		PARSE_FAIL(p, "parse failed - entered delta "
		    "elem unexpectedely");
	}
	for (i = 0; attr[i]; i += 2) {
		if (strcmp("uri", attr[i]) == 0)
			delta_uri = attr[i+1];
		else if (strcmp("hash", attr[i]) == 0)
			delta_hash = attr[i+1];
		else if (strcmp("serial", attr[i]) == 0) {
			const char *errstr;

			delta_serial = strtonum(attr[i + 1],
			    1, LLONG_MAX, &errstr);
			if (errstr != NULL) {
				PARSE_FAIL(p, "parse failed - non conforming "
				    "attribute found in notification elem");
			}
		} else {
			PARSE_FAIL(p, "parse failed - non conforming "
			    "attribute found in snapshot elem");
		}
	}
	/* Only add to the list if we are relevant */
	if (!delta_uri || !delta_hash || !delta_serial)
		PARSE_FAIL(p, "parse failed - incomplete delta attributes");

	if (nxml->current_serial && nxml->current_serial < delta_serial) {
		if (add_delta(nxml, delta_uri, delta_hash, delta_serial) == 0) {
			PARSE_FAIL(p, "parse failed - adding delta failed");
		}
log_debuginfo("adding delta %lld %s", delta_serial, delta_uri);
	}
	nxml->scope = NOTIFICATION_SCOPE_DELTA;
}

static void
end_delta_elem(struct notification_xml *nxml)
{
	XML_Parser p = nxml->parser;

	if (nxml->scope != NOTIFICATION_SCOPE_DELTA)
		PARSE_FAIL(p, "parse failed - exited delta elem unexpectedely");
	nxml->scope = NOTIFICATION_SCOPE_NOTIFICATION_POST_SNAPSHOT;
}

static void
notification_xml_elem_start(void *data, const char *el, const char **attr)
{
	struct notification_xml *nxml = data;
	XML_Parser p = nxml->parser;

	/*
	 * Can only enter here once as we should have no ways to get back to
	 * START scope
	 */
	if (strcmp("notification", el) == 0)
		start_notification_elem(nxml, attr);
	/*
	 * Will enter here multiple times, BUT never nested. will start
	 * collecting character data in that handler
	 * mem is cleared in end block, (TODO or on parse failure)
	 */
	else if (strcmp("snapshot", el) == 0)
		start_snapshot_elem(nxml, attr);
	else if (strcmp("delta", el) == 0)
		start_delta_elem(nxml, attr);
	else
		PARSE_FAIL(p, "parse failed - unexpected elem exit found");
}

static void
notification_xml_elem_end(void *data, const char *el)
{
	struct notification_xml *nxml = data;
	XML_Parser p = nxml->parser;

	if (strcmp("notification", el) == 0)
		end_notification_elem(nxml);
	else if (strcmp("snapshot", el) == 0)
		end_snapshot_elem(nxml);
	else if (strcmp("delta", el) == 0)
		end_delta_elem(nxml);
	else
		PARSE_FAIL(p, "parse failed - unexpected elem exit found");
}

/* XXXCJ this needs more cleanup and error checking */
void
save_notification_data(struct xmldata *xml_data)
{
	int fd;
	FILE *f = NULL;
	struct notification_xml *nxml = xml_data->xml_data;

	log_debuginfo("saving %s/%s", xml_data->opts->basedir_primary,
	    STATE_FILENAME);

	fd = openat(xml_data->opts->primary_dir, STATE_FILENAME,
	    O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);
	if (fd < 0 || !(f = fdopen(fd, "w")))
		err(1, "%s - fdopen", __func__);
	/*
	 * TODO maybe this should actually come from the snapshot/deltas that
	 * get written might not matter if we have verified consistency already
	 */
	fprintf(f, "%s\n%d\n%.*s\n", nxml->session_id, nxml->serial, TIME_LEN,
	    xml_data->modified_since);
	fclose(f);
}

#if 0
/* XXXCJ this needs more cleanup and error checking */
static void
fetch_existing_notification_data(struct xmldata *xml_data)
{
	int fd;
	FILE *f;
	struct notification_xml *nxml = xml_data->xml_data;
	char *line = NULL;
	size_t len = 0;
	ssize_t s;
	int l = 0;

	log_debuginfo("investigating %s/%s", xml_data->opts->basedir_primary,
	    STATE_FILENAME);

	fd = openat(xml_data->opts->primary_dir, STATE_FILENAME, O_RDONLY);
	if (fd < 0 || !(f = fdopen(fd, "r"))) {
		warnx("no state file found");
		return;
	}

	while (l < 3 && (s = getline(&line, &len, f)) != -1) {
		/* must have at least 1 char serial / session */
		if (s <= 1 && l < 2) {
			fclose(f);
			warnx("bad notification file");
			return;
		}
		line[s - 1] = '\0';
		if (l == 0)
			nxml->current_session_id = xstrdup(line);
		else if (l == 1) {
			/*
			 * XXXCJ use strtonum here and maybe 64bit int
			 */
			nxml->current_serial = (int)strtol(line, NULL, 10);
		} else if (l == 2) {
			if (strlen(line) == TIME_LEN - 1) {
				strncpy(xml_data->modified_since, line,
				    TIME_LEN);
			} else {
				memset(xml_data->modified_since, '\0',
				    TIME_LEN);
				warnx("bad time in notification file: '%s'",
				    line);
			}
		}
		l++;
	}
	logx("current session: %s\ncurrent serial: %lld\nmodified since:"
	    " %s\n",
	    nxml->current_session_id ?: "NULL", nxml->current_serial,
	    xml_data->modified_since);
	fclose(f);
}
#endif

struct notification_xml *
new_notification_xml(XML_Parser p)
{
	struct notification_xml *nxml;

	if ((nxml = calloc(1, sizeof(struct notification_xml))) == NULL)
		err(1, "%s - calloc", __func__);
	TAILQ_INIT(&(nxml->delta_q));
	nxml->parser = p;

	XML_SetElementHandler(nxml->parser, notification_xml_elem_start,
	    notification_xml_elem_end);
	XML_SetUserData(nxml->parser, nxml);

	return nxml;
}

void
free_notification_xml(struct notification_xml *nxml)
{
	if (nxml == NULL)
		return;

	free(nxml->session_id);
	free(nxml->snapshot_uri);
	free(nxml->snapshot_hash);
	while (!TAILQ_EMPTY(&nxml->delta_q)) {
		struct delta_item *d = TAILQ_FIRST(&nxml->delta_q);
		TAILQ_REMOVE(&nxml->delta_q, d, q);
		free_delta(d);
	}
	free(nxml);
}