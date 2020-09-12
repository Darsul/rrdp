#ifndef _RRDPH_
#define _RRDPH_

#include <stdio.h>
#include <sys/queue.h>
#include <expat.h>
#include <openssl/sha.h>

/* util */
#define BASE10 10
#define MAX_VERSION 1

/*
 * the log.c doesn't have verbosity levels.
 * Use this to distinguish between supressed info and supressed debug
 */
#define log_debuginfo(format, ...) log_debug(format, ##__VA_ARGS__)

struct opts {
	char *basedir_primary;
	char *basedir_working;
	int primary_dir;
	int working_dir;
	int delta_limit;
	int ignore_withdraw;
	int verbose;
	int uri_wpipe;
	int xml_rpipe;
	int res_rpipe;
};

int	b64_decode(char *, unsigned char **);
char 	*xstrdup(const char *);

FILE 		*open_primary_uri_read(char *, struct opts *);
FILE 		*open_working_uri_read(char *, struct opts *);
FILE 		*open_working_uri_write(char *, struct opts *);
void		 make_workdir(const char *, struct opts *);
void		 free_workdir(struct opts *);
const char	*fetch_filename_from_uri(const char *, const char *);

/* file_util */
enum action {
	ACTION_COPY,
	ACTION_DELETE
};

struct file_delta {
	char *filename;
	enum action action;
	SLIST_ENTRY(file_delta) file_list;
};

SLIST_HEAD(file_list, file_delta);

int mkpath_at(int, const char *);
int rm_dir(char *, int);
int mv_delta(int, int, struct file_list *);
int empty_file_list(struct file_list *);
void add_to_file_list(struct file_list *, const char *, int, int);

/* fetch_util */ 
#define TIME_FORMAT "%a, %d %b %Y %T GMT"
#define TIME_LEN 30
#define HASH_CHAR_LEN (SHA256_DIGEST_LENGTH * 2)

/* save everyone doing this code over and over */
#define PARSE_FAIL(p, ...) do {		\
	XML_StopParser(p, XML_FALSE);	\
	log_warnx(__VA_ARGS__);		\
	return;				\
} while(0)

enum uri_type {
	URI_TYPE_BASIC,
	URI_TYPE_HASH,
	URI_TYPE_MODIFIED_SINCE
};

struct uri_data {
	enum uri_type uri_type;
	char hash[HASH_CHAR_LEN];
	char modified_since[TIME_LEN];
	char *uri;
	int xml_wpipe;
	int res_wpipe;
	SHA256_CTX ctx;
};

struct xmldata {
	struct opts *opts;
	XML_Parser parser;
	char modified_since[TIME_LEN];
	void *xml_data;
};

int fetch_uri_data(char *, char *, char *, struct opts *, XML_Parser);
void read_uri(int, int, int, char *);

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
	char *uri;
	char *hash;
	int serial;
	TAILQ_ENTRY(delta_item) q;
};

TAILQ_HEAD(delta_q, delta_item);

struct delta_item	*new_delta(const char *, const char *, int);
void			free_delta(struct delta_item *);

struct notification_xml {
	enum notification_scope	scope;
	char			*xmlns;
	int			version;
	char			*session_id;
	int			serial;
	char			*current_session_id;
	int			current_serial;
	char			*snapshot_uri;
	char			*snapshot_hash;
	struct delta_q		delta_q;
	enum notification_state	state;
};

struct notification_xml	*new_notification_xml(void);
void			free_notification_xml(struct notification_xml *);
void			log_notification_xml(struct notification_xml *);
void			check_state(struct notification_xml *);

struct xmldata	*new_notification_xml_data(char *, struct opts *);
void		free_xml_data(struct xmldata *);
void		save_notification_data(struct xmldata *);

/* snapshot */
int fetch_snapshot_xml(char *, char *, struct opts *, struct notification_xml *,
    struct file_list *);

/* delta */
int fetch_delta_xml(char *, char *, struct opts *, struct notification_xml *,
    struct file_list *);

#endif /* _RRDPH_ */

