/*
 * JSON Filter
 * by Craig Mustard - University of British Columbia
 * craigm@ece.ubc.ca
 *
 */

#include <ctype.h>

#include <common/standard.h>
#include <common/time.h>
#include <common/tools.h>
#include <common/hathreads.h>

#include <types/channel.h>
#include <types/filters.h>
#include <types/global.h>
#include <types/proxy.h>
#include <types/stream.h>

#include <proto/filters.h>
#include <proto/hdr_idx.h>
#include <proto/log.h>
#include <proto/stream.h>

#include <jsonwrapper.h>

struct flt_ops json_ops;

struct json_config {
	struct proxy *proxy;
	char         *name;
	int           noop;
	int           one_record_per_parse;
};

#define TRACE(conf, fmt, ...)						\
	fprintf(stderr, "%d.%06d [%-20s] " fmt "\n",			\
		(int)now.tv_sec, (int)now.tv_usec, (conf)->name,	\
		##__VA_ARGS__)

#define STRM_TRACE(conf, strm, fmt, ...)						\
	fprintf(stderr, "%d.%06d [%-20s] [strm %p(%x) 0x%08x 0x%08x] " fmt "\n",	\
		(int)now.tv_sec, (int)now.tv_usec, (conf)->name,			\
		strm, (strm ? ((struct stream *)strm)->uniq_id : ~0U),			\
		(strm ? strm->req.analysers : 0), (strm ? strm->res.analysers : 0),	\
		##__VA_ARGS__)


static const char *
channel_label(const struct channel *chn)
{
	return (chn->flags & CF_ISRESP) ? "RESPONSE" : "REQUEST";
}

static const char *
proxy_mode(const struct stream *s)
{
	struct proxy *px = (s->flags & SF_BE_ASSIGNED ? s->be : strm_fe(s));

	return (px->mode == PR_MODE_HTTP) ? "HTTP" : "TCP";
}

static const char *
stream_pos(const struct stream *s)
{
	return (s->flags & SF_BE_ASSIGNED) ? "backend" : "frontend";
}

static const char *
filter_type(const struct filter *f)
{
	return (f->flags & FLT_FL_IS_BACKEND_FILTER) ? "backend" : "frontend";
}

#if 0
static void
json_hexdump(struct buffer *buf, int len, int out)
{
	unsigned char p[len];
	int block1, block2, i, j, padding;

	block1 = len;
	if (block1 > b_contig_data(buf, out))
		block1 = b_contig_data(buf, out);
	block2 = len - block1;

	memcpy(p, b_head(buf), block1);
	memcpy(p+block1, b_orig(buf), block2);

	padding = ((len % 16) ? (16 - len % 16) : 0);
	for (i = 0; i < len + padding; i++) {
                if (!(i % 16))
                        fprintf(stderr, "\t0x%06x: ", i);
		else if (!(i % 8))
                        fprintf(stderr, "  ");

                if (i < len)
                        fprintf(stderr, "%02x ", p[i]);
                else
                        fprintf(stderr, "   ");

                /* print ASCII dump */
                if (i % 16 == 15) {
                        fprintf(stderr, "  |");
                        for(j = i - 15; j <= i && j < len; j++)
				fprintf(stderr, "%c", (isprint(p[j]) ? p[j] : '.'));
                        fprintf(stderr, "|\n");
                }
        }
}
#endif

/***************************************************************************
 * Hooks that manage the filter lifecycle (init/check/deinit)
 **************************************************************************/
/* Initialize the filter. Returns -1 on error, else 0. */
static int
json_init(struct proxy *px, struct flt_conf *fconf)
{
	struct json_config *conf = fconf->conf;

	if (conf->name)
		memprintf(&conf->name, "%s/%s", conf->name, px->id);
	else
		memprintf(&conf->name, "TRACE/%s", px->id);
	fconf->conf = conf;
	TRACE(conf, "filter initialized [noop=%s - one-record-per-parse=%s]",
	      (conf->noop ? "true" : "false"),
	      (conf->one_record_per_parse ? "true" : "false")
	);
	return 0;
}

/* Free ressources allocated by the trace filter. */
static void
json_deinit(struct proxy *px, struct flt_conf *fconf)
{
	struct json_config *conf = fconf->conf;

	if (conf) {
		TRACE(conf, "filter deinitialized");
		free(conf->name);
		free(conf);
	}
	fconf->conf = NULL;
}

/* Check configuration of a trace filter for a specified proxy.
 * Return 1 on error, else 0. */
static int
json_check(struct proxy *px, struct flt_conf *fconf)
{
	if(px->mode != PR_MODE_TCP){
		ha_alert("json filter can only be used in HTTP mode");
		return 1;
	}
	return 0;
}

/* Initialize the filter for each thread. Return -1 on error, else 0. */
static int
json_init_per_thread(struct proxy *px, struct flt_conf *fconf)
{
	struct json_config *conf = fconf->conf;

	TRACE(conf, "filter initialized for thread tid %u", tid);
	return 0;
}

/* Free ressources allocate by the trace filter for each thread. */
static void
json_deinit_per_thread(struct proxy *px, struct flt_conf *fconf)
{
	struct json_config *conf = fconf->conf;

	if (conf)
		TRACE(conf, "filter deinitialized for thread tid %u", tid);
}

/**************************************************************************
 * Hooks to handle start/stop of streams
 *************************************************************************/
/* Called when a filter instance is created and attach to a stream */
static int
json_attach(struct stream *s, struct filter *filter)
{
	struct json_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: filter-type=%s",
		   __FUNCTION__, filter_type(filter));
	/* can return 0 here to ignore the filter */
	return 1;
}

/* Called when a filter instance is detach from a stream, just before its
 * destruction */
static void
json_detach(struct stream *s, struct filter *filter)
{
	struct json_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: filter-type=%s",
		   __FUNCTION__, filter_type(filter));
}

/* Called when a stream is created */
static int
json_stream_start(struct stream *s, struct filter *filter)
{
	struct json_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s",
		   __FUNCTION__);
	return 0;
}


/* Called when a backend is set for a stream */
static int
json_stream_set_backend(struct stream *s, struct filter *filter,
			 struct proxy *be)
{
	struct json_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: backend=%s",
		   __FUNCTION__, be->id);
	return 0;
}

/* Called when a stream is destroyed */
static void
json_stream_stop(struct stream *s, struct filter *filter)
{
	struct json_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s",
		   __FUNCTION__);
}

/* Called when the stream is woken up because of an expired timer */
static void
json_check_timeouts(struct stream *s, struct filter *filter)
{
	struct json_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s",
		   __FUNCTION__);
}

/**************************************************************************
 * Hooks to handle channels activity
 *************************************************************************/
/* Called when analyze starts for a given channel */
static int
json_chn_start_analyze(struct stream *s, struct filter *filter,
			struct channel *chn)
{
	struct json_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s)",
		   __FUNCTION__,
		   channel_label(chn), proxy_mode(s), stream_pos(s));
	filter->pre_analyzers  |= (AN_REQ_ALL | AN_RES_ALL);
	filter->post_analyzers |= (AN_REQ_ALL | AN_RES_ALL);
	register_data_filter(s, chn, filter);
	return 1;
}

/* Called before a processing happens on a given channel */
/*
static int
json_chn_analyze(struct stream *s, struct filter *filter,
		  struct channel *chn, unsigned an_bit)
{
	struct json_config *conf = FLT_CONF(filter);
	char                *ana;

	switch (an_bit) {
		case AN_REQ_INSPECT_FE:
			ana = "AN_REQ_INSPECT_FE";
			break;
		case AN_REQ_WAIT_HTTP:
			ana = "AN_REQ_WAIT_HTTP";
			break;
		case AN_REQ_HTTP_BODY:
			ana = "AN_REQ_HTTP_BODY";
			break;
		case AN_REQ_HTTP_PROCESS_FE:
			ana = "AN_REQ_HTTP_PROCESS_FE";
			break;
		case AN_REQ_SWITCHING_RULES:
			ana = "AN_REQ_SWITCHING_RULES";
			break;
		case AN_REQ_INSPECT_BE:
			ana = "AN_REQ_INSPECT_BE";
			break;
		case AN_REQ_HTTP_PROCESS_BE:
			ana = "AN_REQ_HTTP_PROCESS_BE";
			break;
		case AN_REQ_SRV_RULES:
			ana = "AN_REQ_SRV_RULES";
			break;
		case AN_REQ_HTTP_INNER:
			ana = "AN_REQ_HTTP_INNER";
			break;
		case AN_REQ_HTTP_TARPIT:
			ana = "AN_REQ_HTTP_TARPIT";
			break;
		case AN_REQ_STICKING_RULES:
			ana = "AN_REQ_STICKING_RULES";
			break;
		case AN_REQ_PRST_RDP_COOKIE:
			ana = "AN_REQ_PRST_RDP_COOKIE";
			break;
		case AN_REQ_HTTP_XFER_BODY:
			ana = "AN_REQ_HTTP_XFER_BODY";
			break;
		case AN_RES_INSPECT:
			ana = "AN_RES_INSPECT";
			break;
		case AN_RES_WAIT_HTTP:
			ana = "AN_RES_WAIT_HTTP";
			break;
		case AN_RES_HTTP_PROCESS_FE: // AN_RES_HTTP_PROCESS_BE
			ana = "AN_RES_HTTP_PROCESS_FE/BE";
			break;
		case AN_RES_STORE_RULES:
			ana = "AN_RES_STORE_RULES";
			break;
		case AN_RES_HTTP_XFER_BODY:
			ana = "AN_RES_HTTP_XFER_BODY";
			break;
		default:
			ana = "unknown";
	}

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s) - "
		   "analyzer=%s - step=%s",
		   __FUNCTION__,
		   channel_label(chn), proxy_mode(s), stream_pos(s),
		   ana, ((chn->analysers & an_bit) ? "PRE" : "POST"));
	return 1;
}
*/

/* Called when analyze ends for a given channel */
static int
json_chn_end_analyze(struct stream *s, struct filter *filter,
		      struct channel *chn)
{
	struct json_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s)",
		   __FUNCTION__,
		   channel_label(chn), proxy_mode(s), stream_pos(s));
	unregister_data_filter(s, chn, filter);
	return 1;
}

/**************************************************************************
 * Hooks to filter TCP data
 *************************************************************************/
static int
json_tcp_data(struct stream *s, struct filter *filter, struct channel *chn)
{
	struct json_config *conf = FLT_CONF(filter);
	int                  avail = ci_data(chn) - FLT_NXT(filter, chn);
	int                  ret  = avail;
	int left;

	if(avail == 0){
		return 0;
	}

	left = ci_contig_data(chn);
	STRM_TRACE(conf, s, "%-25s: START channel=%-10s - mode=%-5s (%s) - next=%u - avail=%u",
		   __FUNCTION__,
		   channel_label(chn), proxy_mode(s), stream_pos(s),
		   FLT_NXT(filter, chn), avail);

#if 0 
	char *start;
	/* scan for a newline record delimeter */
	ret = 0;
	start = ci_head(chn) + FLT_NXT(filter, chn);
	for(int i = 0; i<left; i++){
		if(start[i] == '\n'){
			ret = i+1;
			//break; /* allow one record at a time through */
		}
	}

	if(unlikely(left < avail)){
		/* more data than is contiguous, so wrap and and continue scanning */
		start = c_orig(chn);
		for(int i = 0; i<(avail-left); i++){
			if(start[i] == '\n'){
				ret = left+i+1;
				//break; /* allow one record at a time through */
			}
		}
	}
#endif

	/* handle wrapped buffer */
	char* parse_start = ci_head(chn) + FLT_NXT(filter, chn);
	char* origin =  c_orig(chn);
	char* buffer_end = parse_start + left;  //the first byte after the last valid byte in the buffer
	char* parse_end; //should be the last character to parse
	char* parsed_til;
	if(unlikely(left < avail)){
		parse_end = c_orig(chn) + (avail-left) - 1;
		printf("wrapped parse end: %p \n", parse_end);
		//guaranteed to be non zero because of above
	} else {
		/*parse_end = c_orig(chn) + (left) - 1;*/
		parse_end = parse_start + (left) - 1;
		printf("unwrapped parse end: %p \n", parse_end);
	}

	int parsed_records = 0;
	int failed_records = 0;
	ret = 0;
	while(parse_start != parse_end){
		printf("parsing json\n");
		json_passed_t r = json_parse_wrap(origin, parse_start, parse_end, buffer_end, &parsed_til);
		if(r == JSON_PASS){
			//move up the pointer because we successfully parsed a record
			int change;
			if(unlikely( parsed_til < parse_start )){
				/* wrapped */
				change = (buffer_end - parse_start) + (parsed_til - origin);
			} else {
				change = (parsed_til - parse_start); 
			}
			ret += change;
			printf("success, ret += %ld\n", (change));
			parse_start = parsed_til;
			parsed_records++;
		} else {
			printf("json parse failed at: %p\n", parsed_til);
			failed_records++;
			/* on a failed record, we don't move the ret up, we have to reparse */
			break;
		}
	}
	/* forward through remaining whitespace */
	/*while(ret < left && *parse_start == '\n'){*/
		/*parse_start++;*/
		/*ret++;*/
	/*}*/

	STRM_TRACE(conf, s, "%-25s: DONE channel=%-10s - mode=%-5s (%s) - next=%u - avail=%u - consume=%d",
		   __FUNCTION__,
		   channel_label(chn), proxy_mode(s), stream_pos(s),
		   FLT_NXT(filter, chn), avail, ret);

	if (ret != avail)
		task_wakeup(s->task, TASK_WOKEN_MSG);
	return ret;
}
static int
json_tcp_data_orpp(struct stream *s, struct filter *filter, struct channel *chn)
{
	/*struct json_config *conf = FLT_CONF(filter);*/
	int                  avail = ci_data(chn) - FLT_NXT(filter, chn);
	int                  ret  = avail;
	int left;
	char *start;

	if(avail == 0){
		return 0;
	}

	left = ci_contig_data(chn);

	/* scan for a newline record delimeter */
	ret = 0;
	start = ci_head(chn) + FLT_NXT(filter, chn);
	for(int i = 0; i<left; i++){
		if(start[i] == '\n'){
			ret = i+1;
			break; /* allow one record at a time through */
		}
	}

	if(unlikely(left < avail)){
		/* more data than is contiguous, so wrap and and continue scanning */
		start = c_orig(chn);
		for(int i = 0; i<(avail-left); i++){
			if(start[i] == '\n'){
				ret = left+i+1;
				break; /* allow one record at a time through */
			}
		}
	}

	/*STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s) - next=%u - avail=%u - consume=%d",*/
		   /*__FUNCTION__,*/
		   /*channel_label(chn), proxy_mode(s), stream_pos(s),*/
		   /*FLT_NXT(filter, chn), avail, ret);*/

	if (ret != avail)
		task_wakeup(s->task, TASK_WOKEN_MSG);
	return ret;
}
static int
json_tcp_data_noop(struct stream *s, struct filter *filter, struct channel *chn)
{
	int                  avail = ci_data(chn) - FLT_NXT(filter, chn);
	int                  ret  = avail;

	if (ret != avail)
		task_wakeup(s->task, TASK_WOKEN_MSG);
	return ret;
}

static int
json_tcp_forward_data(struct stream *s, struct filter *filter, struct channel *chn,
		 unsigned int len)
{
	/*struct json_config *conf = FLT_CONF(filter);*/
	int                  ret  = len;

	/*STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s) - len=%u - fwd=%u - forward=%d",*/
		   /*__FUNCTION__,*/
		   /*channel_label(chn), proxy_mode(s), stream_pos(s), len,*/
		   /*FLT_FWD(filter, chn), ret);*/

	/*if (conf->hexdump) {*/
		/*c_adv(chn, FLT_FWD(filter, chn));*/
		/*json_hexdump(&chn->buf, ret, co_data(chn));*/
		/*c_rew(chn, FLT_FWD(filter, chn));*/
	/*}*/

	if (ret != len)
		task_wakeup(s->task, TASK_WOKEN_MSG);
	return ret;
}

/********************************************************************
 * Functions that manage the filter initialization
 ********************************************************************/
struct flt_ops json_ops = {
	/* Manage trace filter, called for each filter declaration */
	.init              = json_init,
	.deinit            = json_deinit,
	.check             = json_check,
	.init_per_thread   = json_init_per_thread,
	.deinit_per_thread = json_deinit_per_thread,

	/* Handle start/stop of streams */
	.attach             = json_attach,
	.detach             = json_detach,
	.stream_start       = json_stream_start,
	.stream_set_backend = json_stream_set_backend,
	.stream_stop        = json_stream_stop,
	.check_timeouts     = json_check_timeouts,

	/* Handle channels activity */
	/* start and end needed to (un)register data filter */
	.channel_start_analyze = json_chn_start_analyze,
	/*.channel_pre_analyze   = json_chn_analyze,*/
	/*.channel_post_analyze  = json_chn_analyze,*/
	.channel_end_analyze   = json_chn_end_analyze,

	/* Filter TCP data */
	.tcp_data         = json_tcp_data,
	.tcp_forward_data = json_tcp_forward_data,
};

/* Return -1 on error, else 0 */
static int
parse_json_flt(char **args, int *cur_arg, struct proxy *px,
                struct flt_conf *fconf, char **err, void *private)
{
	struct json_config *conf;
	int                  pos = *cur_arg;

	conf = calloc(1, sizeof(*conf));
	if (!conf) {
		memprintf(err, "%s: out of memory", args[*cur_arg]);
		return -1;
	}
	conf->proxy = px;

	if (!strcmp(args[pos], "json")) {
		pos++;

		while (*args[pos]) {
			if (!strcmp(args[pos], "name")) {
				if (!*args[pos + 1]) {
					memprintf(err, "'%s' : '%s' option without value",
						  args[*cur_arg], args[pos]);
					goto error;
				}
				conf->name = strdup(args[pos + 1]);
				if (!conf->name) {
					memprintf(err, "%s: out of memory", args[*cur_arg]);
					goto error;
				}
				pos++;
			}
			else if (!strcmp(args[pos], "noop"))
				conf->noop = 1;
			else if (!strcmp(args[pos], "record-per-parse"))
				conf->one_record_per_parse = 1;
			else
				break;
			pos++;
		}
		*cur_arg = pos;

		if(conf->noop){
			json_ops.tcp_data = json_tcp_data_noop;
		}
		if(conf->one_record_per_parse){
			json_ops.tcp_data = json_tcp_data_orpp;
		}

		fconf->ops  = &json_ops;
	}

	fconf->conf = conf;
	return 0;

 error:
	if (conf->name)
		free(conf->name);
	free(conf);
	return -1;
}

/* Declare the filter parser for "trace" keyword */
static struct flt_kw_list flt_kws = { "JSON", { }, {
		{ "json", parse_json_flt, NULL },
		{ NULL, NULL, NULL },
	}
};

__attribute__((constructor))
static void
__flt_json_init(void)
{
	flt_register_keywords(&flt_kws);
}
