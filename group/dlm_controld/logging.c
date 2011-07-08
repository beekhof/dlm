#include "dlm_daemon.h"
#include "config.h"
#include "ccs.h"

extern int ccs_handle;

#define DAEMON_NAME "dlm_controld"
#define DEFAULT_LOG_MODE LOG_MODE_OUTPUT_FILE|LOG_MODE_OUTPUT_SYSLOG
#define DEFAULT_SYSLOG_FACILITY		SYSLOGFACILITY
#define DEFAULT_SYSLOG_PRIORITY		SYSLOGLEVEL
#define DEFAULT_LOGFILE_PRIORITY	LOG_INFO /* ? */
#define DEFAULT_LOGFILE			LOGDIR "/" DAEMON_NAME ".log"

static int log_mode;
static int syslog_facility;
static int syslog_priority;
static int logfile_priority;
static char logfile[PATH_MAX];

void init_logging(void)
{
	log_mode = DEFAULT_LOG_MODE;
	syslog_facility = DEFAULT_SYSLOG_FACILITY;
	syslog_priority = DEFAULT_SYSLOG_PRIORITY;
	logfile_priority = DEFAULT_LOGFILE_PRIORITY;
	strcpy(logfile, DEFAULT_LOGFILE);

	/* logfile_priority is the only one of these options that
	   can be controlled from command line or environment variable */

	if (cfgd_debug_logfile)
		logfile_priority = LOG_DEBUG;

	log_debug("logging mode %d syslog f %d p %d logfile p %d %s",
		  log_mode, syslog_facility, syslog_priority,
		  logfile_priority, logfile);

	logt_init(DAEMON_NAME, log_mode, syslog_facility, syslog_priority,
		  logfile_priority, logfile);
}

void setup_logging(void)
{
	ccs_read_logging(ccs_handle, DAEMON_NAME,
			 &cfgd_debug_logfile, &log_mode,
			 &syslog_facility, &syslog_priority,
			 &logfile_priority, logfile);

	log_debug("logging mode %d syslog f %d p %d logfile p %d %s",
		  log_mode, syslog_facility, syslog_priority,
		  logfile_priority, logfile);

	logt_conf(DAEMON_NAME, log_mode, syslog_facility, syslog_priority,
		  logfile_priority, logfile);
}

void close_logging(void)
{
	logt_exit();
}

#define NAME_ID_SIZE 32
#define LOG_STR_LEN 512
static char log_str[LOG_STR_LEN];

static char log_dump[LOG_DUMP_SIZE];
static unsigned int log_point;
static unsigned int log_wrap;

static char log_dump_plock[LOG_DUMP_SIZE];
static unsigned int log_point_plock;
static unsigned int log_wrap_plock;

static void log_copy(char *buf, int *len, char *log_buf,
		     unsigned int *point, unsigned int *wrap)
{
	unsigned int p = *point;
	unsigned int w = *wrap;
	int tail_len;

	if (!w && !p) {
		*len = 0;
	} else if (*wrap) {
		tail_len = LOG_DUMP_SIZE - p;
		memcpy(buf, log_buf + p, tail_len);
		if (p)
			memcpy(buf+tail_len, log_buf, p);
		*len = LOG_DUMP_SIZE;
	} else {
		memcpy(buf, log_buf, p-1);
		*len = p-1;
	}
}

void copy_log_dump(char *buf, int *len)
{
	log_copy(buf, len, log_dump, &log_point, &log_wrap);
}

void copy_log_dump_plock(char *buf, int *len)
{
	log_copy(buf, len, log_dump_plock, &log_point_plock, &log_wrap_plock);
}

static void log_save_str(int level, int len, char *log_buf,
			 unsigned int *point, unsigned int *wrap)
{
	unsigned int p = *point;
	unsigned int w = *wrap;
	int i;

	if (len < LOG_DUMP_SIZE - p) {
		memcpy(log_buf + p, log_str, len);
		p += len;

		if (p == LOG_DUMP_SIZE) {
			p = 0;
			w = 1;
		}
		goto out;
	}

	for (i = 0; i < len; i++) {
		log_buf[p++] = log_str[i];

		if (p == LOG_DUMP_SIZE) {
			p = 0;
			w = 1;
		}
	}
 out:
	*point = p;
	*wrap = w;
}

void log_level(char *name_in, uint32_t level_in, const char *fmt, ...)
{
	va_list ap;
	char name[NAME_ID_SIZE + 1];
	uint32_t level = level_in & 0x0000FFFF;
	uint32_t extra = level_in & 0xFFFF0000;
	int ret, pos = 0;
	int len = LOG_STR_LEN - 2;
	int plock = extra & LOG_PLOCK;

	memset(name, 0, sizeof(name));

	if (name_in)
		snprintf(name, NAME_ID_SIZE, "%s ", name_in);

	ret = snprintf(log_str + pos, len - pos, "%llu %s",
		       (unsigned long long)time(NULL), name);

	pos += ret;

	va_start(ap, fmt);
	ret = vsnprintf(log_str + pos, len - pos, fmt, ap);
	va_end(ap);

	if (ret >= len - pos)
		pos = len - 1;
	else
		pos += ret;

	log_str[pos++] = '\n';
	log_str[pos++] = '\0';

	if (level)
		log_save_str(level, pos - 1, log_dump, &log_point, &log_wrap);
	if (plock)
		log_save_str(level, pos - 1, log_dump_plock, &log_point_plock, &log_wrap_plock);
	if (level)
		logt_print(level, "%s", log_str);

	if (!daemon_debug_opt)
		return;

	if (level || (plock && cfgd_plock_debug))
		fprintf(stderr, "%s", log_str);
}

