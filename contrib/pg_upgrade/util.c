/*
 *	util.c
 *
 *	utility functions
 *
 *	Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/util.c
 */

#include "pg_upgrade.h"

#include <signal.h>


LogOpts			log_opts;

/*
 * report_status()
 *
 *	Displays the result of an operation (ok, failed, error message,...)
 */
void
report_status(eLogType type, const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	pg_log(type, "%s\n", message);
}


/*
 * prep_status
 *
 *	Displays a message that describes an operation we are about to begin.
 *	We pad the message out to MESSAGE_WIDTH characters so that all of the "ok" and
 *	"failed" indicators line up nicely.
 *
 *	A typical sequence would look like this:
 *		prep_status("about to flarb the next %d files", fileCount );
 *
 *		if(( message = flarbFiles(fileCount)) == NULL)
 *		  report_status(PG_REPORT, "ok" );
 *		else
 *		  pg_log(PG_FATAL, "failed - %s", message );
 */
void
prep_status(const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	if (strlen(message) > 0 && message[strlen(message) - 1] == '\n')
		pg_log(PG_REPORT, "%s", message);
	else
		pg_log(PG_REPORT, "%-" MESSAGE_WIDTH "s", message);
}


void
pg_log(eLogType type, char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	if (log_opts.fd != NULL)
	{
		fwrite(message, strlen(message), 1, log_opts.fd);
		/* if we are using OVERWRITE_MESSAGE, add newline */
		if (strchr(message, '\r') != NULL)
			fwrite("\n", 1, 1, log_opts.fd);
		fflush(log_opts.fd);
	}

	switch (type)
	{
		case PG_INFO:
			if (log_opts.verbose)
				printf("%s", _(message));
			break;

		case PG_REPORT:
		case PG_WARNING:
			printf("%s", _(message));
			break;

		case PG_FATAL:
			printf("%s", "\n");
			printf("%s", _(message));
			exit_nicely(true);
			break;

		case PG_DEBUG:
			if (log_opts.debug)
				fprintf(log_opts.debug_fd, "%s\n", _(message));
			break;

		default:
			break;
	}
	fflush(stdout);
}


void
check_ok(void)
{
	/* all seems well */
	report_status(PG_REPORT, "ok");
	fflush(stdout);
}


/*
 * quote_identifier()
 *		Properly double-quote a SQL identifier.
 *
 * The result should be pg_free'd, but most callers don't bother because
 * memory leakage is not a big deal in this program.
 */
char *
quote_identifier(const char *s)
{
	char	   *result = pg_malloc(strlen(s) * 2 + 3);
	char	   *r = result;

	*r++ = '"';
	while (*s)
	{
		if (*s == '"')
			*r++ = *s;
		*r++ = *s;
		s++;
	}
	*r++ = '"';
	*r++ = '\0';

	return result;
}


/*
 * get_user_info()
 * (copied from initdb.c) find the current user
 */
int
get_user_info(char **user_name)
{
	int			user_id;

#ifndef WIN32
	struct passwd *pw = getpwuid(geteuid());

	user_id = geteuid();
#else							/* the windows code */
	struct passwd_win32
	{
		int			pw_uid;
		char		pw_name[128];
	}			pass_win32;
	struct passwd_win32 *pw = &pass_win32;
	DWORD		pwname_size = sizeof(pass_win32.pw_name) - 1;

	GetUserName(pw->pw_name, &pwname_size);

	user_id = 1;
#endif

	*user_name = pg_strdup(pw->pw_name);

	return user_id;
}


void
exit_nicely(bool need_cleanup)
{
	stop_postmaster(true, true);

	pg_free(log_opts.filename);

	if (log_opts.fd)
		fclose(log_opts.fd);

	if (log_opts.debug_fd)
		fclose(log_opts.debug_fd);

	/* terminate any running instance of postmaster */
	if (os_info.postmasterPID != 0)
		kill(os_info.postmasterPID, SIGTERM);

	if (need_cleanup)
	{
		/*
		 * FIXME must delete intermediate files
		 */
		exit(1);
	}
	else
		exit(0);
}


void *
pg_malloc(int n)
{
	void	   *p = malloc(n);

	if (p == NULL)
		pg_log(PG_FATAL, "%s: out of memory\n", os_info.progname);

	return p;
}


void
pg_free(void *p)
{
	if (p != NULL)
		free(p);
}


char *
pg_strdup(const char *s)
{
	char	   *result = strdup(s);

	if (result == NULL)
		pg_log(PG_FATAL, "%s: out of memory\n", os_info.progname);

	return result;
}


/*
 * getErrorText()
 *
 *	Returns the text of the error message for the given error number
 *
 *	This feature is factored into a separate function because it is
 *	system-dependent.
 */
const char *
getErrorText(int errNum)
{
#ifdef WIN32
	_dosmaperr(GetLastError());
#endif
	return strdup(strerror(errNum));
}


/*
 *	str2uint()
 *
 *	convert string to oid
 */
unsigned int
str2uint(const char *str)
{
	return strtoul(str, NULL, 10);
}
