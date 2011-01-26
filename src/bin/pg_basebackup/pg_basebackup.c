/*-------------------------------------------------------------------------
 *
 * pg_basebackup.c - receive a base backup using streaming replication protocol
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_basebackup.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "libpq-fe.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "getopt_long.h"


/* Global options */
static const char *progname;
char	   *basedir = NULL;
char		format = 'p';		/* p(lain)/t(ar) */
char	   *label = "pg_basebackup base backup";
bool		showprogress = false;
int			verbose = 0;
int			compresslevel = 0;
bool		fastcheckpoint = false;
char	   *dbhost = NULL;
char	   *dbuser = NULL;
char	   *dbport = NULL;
int			dbgetpassword = 0;	/* 0=auto, -1=never, 1=always */

/* Progress counters */
static uint64 totalsize;
static uint64 totaldone;
static int	tablespacecount;

/* Connection kept global so we can disconnect easily */
static PGconn *conn = NULL;

#define disconnect_and_exit(code)				\
	{											\
	if (conn != NULL) PQfinish(conn);			\
	exit(code);									\
	}

/* Function headers */
static char *xstrdup(const char *s);
static void *xmalloc0(int size);
static void usage(void);
static void verify_dir_is_empty_or_create(char *dirname);
static void progress_report(int tablespacenum, char *fn);
static PGconn *GetConnection(void);

static void ReceiveTarFile(PGconn *conn, PGresult *res, int rownum);
static void ReceiveAndUnpackTarFile(PGconn *conn, PGresult *res, int rownum);
static void BaseBackup();

#ifdef HAVE_LIBZ
static const char *
get_gz_error(gzFile *gzf)
{
	int			errnum;
	const char *errmsg;

	errmsg = gzerror(gzf, &errnum);
	if (errnum == Z_ERRNO)
		return strerror(errno);
	else
		return errmsg;
}
#endif

/*
 * strdup() and malloc() replacements that prints an error and exits
 * if something goes wrong. Can never return NULL.
 */
static char *
xstrdup(const char *s)
{
	char	   *result;

	result = strdup(s);
	if (!result)
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		exit(1);
	}
	return result;
}

static void *
xmalloc0(int size)
{
	void	   *result;

	result = malloc(size);
	if (!result)
	{
		fprintf(stderr, _("%s: out of memory\n"), progname);
		exit(1);
	}
	MemSet(result, 0, size);
	return result;
}


static void
usage(void)
{
	printf(_("%s takes base backups of running PostgreSQL servers\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions controlling the output:\n"));
	printf(_("  -D, --pgdata=directory    receive base backup into directory\n"));
	printf(_("  -F, --format=p|t          output format (plain, tar)\n"));
	printf(_("  -Z, --compress=0-9        compress tar output\n"));
	printf(_("\nGeneral options:\n"));
	printf(_("  -c, --checkpoint=fast|spread\n"
			 "                            set fast or spread checkpointing\n"));
	printf(_("  -l, --label=label         set backup label\n"));
	printf(_("  -P, --progress            show progress information\n"));
	printf(_("  -v, --verbose             output verbose messages\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -w, --no-password        never prompt for password\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));
	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}


/*
 * Verify that the given directory exists and is empty. If it does not
 * exist, it is created. If it exists but is not empty, an error will
 * be give and the process ended.
 */
static void
verify_dir_is_empty_or_create(char *dirname)
{
	switch (pg_check_dir(dirname))
	{
		case 0:

			/*
			 * Does not exist, so create
			 */
			if (pg_mkdir_p(dirname, S_IRWXU) == -1)
			{
				fprintf(stderr,
						_("%s: could not create directory \"%s\": %s\n"),
						progname, dirname, strerror(errno));
				disconnect_and_exit(1);
			}
			return;
		case 1:

			/*
			 * Exists, empty
			 */
			return;
		case 2:

			/*
			 * Exists, not empty
			 */
			fprintf(stderr,
					_("%s: directory \"%s\" exists but is not empty\n"),
					progname, dirname);
			disconnect_and_exit(1);
		case -1:

			/*
			 * Access problem
			 */
			fprintf(stderr, _("%s: could not access directory \"%s\": %s\n"),
					progname, dirname, strerror(errno));
			disconnect_and_exit(1);
	}
}


/*
 * Print a progress report based on the global variables. If verbose output
 * is enabled, also print the current file name.
 */
static void
progress_report(int tablespacenum, char *fn)
{
	if (verbose)
		fprintf(stderr,
				INT64_FORMAT "/" INT64_FORMAT " kB (%i%%) %i/%i tablespaces (%-30s)\r",
				totaldone / 1024, totalsize,
				(int) ((totaldone / 1024) * 100 / totalsize),
				tablespacenum, tablespacecount, fn);
	else
		fprintf(stderr, INT64_FORMAT "/" INT64_FORMAT " kB (%i%%) %i/%i tablespaces\r",
				totaldone / 1024, totalsize,
				(int) ((totaldone / 1024) * 100 / totalsize),
				tablespacenum, tablespacecount);
}


/*
 * Receive a tar format file from the connection to the server, and write
 * the data from this file directly into a tar file. If compression is
 * enabled, the data will be compressed while written to the file.
 *
 * The file will be named base.tar[.gz] if it's for the main data directory
 * or <tablespaceoid>.tar[.gz] if it's for another tablespace.
 *
 * No attempt to inspect or validate the contents of the file is done.
 */
static void
ReceiveTarFile(PGconn *conn, PGresult *res, int rownum)
{
	char		fn[MAXPGPATH];
	char	   *copybuf = NULL;
	FILE	   *tarfile = NULL;

#ifdef HAVE_LIBZ
	gzFile	   *ztarfile = NULL;
#endif

	if (PQgetisnull(res, rownum, 0))

		/*
		 * Base tablespaces
		 */
		if (strcmp(basedir, "-") == 0)
			tarfile = stdout;
		else
		{
#ifdef HAVE_LIBZ
			if (compresslevel > 0)
			{
				snprintf(fn, sizeof(fn), "%s/base.tar.gz", basedir);
				ztarfile = gzopen(fn, "wb");
				if (gzsetparams(ztarfile, compresslevel, Z_DEFAULT_STRATEGY) != Z_OK)
				{
					fprintf(stderr, _("%s: could not set compression level %i: %s\n"),
							progname, compresslevel, get_gz_error(ztarfile));
					disconnect_and_exit(1);
				}
			}
			else
#endif
			{
				snprintf(fn, sizeof(fn), "%s/base.tar", basedir);
				tarfile = fopen(fn, "wb");
			}
		}
	else
	{
		/*
		 * Specific tablespace
		 */
#ifdef HAVE_LIBZ
		if (compresslevel > 0)
		{
			snprintf(fn, sizeof(fn), "%s/%s.tar.gz", basedir, PQgetvalue(res, rownum, 0));
			ztarfile = gzopen(fn, "wb");
			if (gzsetparams(ztarfile, compresslevel, Z_DEFAULT_STRATEGY) != Z_OK)
			{
				fprintf(stderr, _("%s: could not set compression level %i: %s\n"),
						progname, compresslevel, get_gz_error(ztarfile));
				disconnect_and_exit(1);
			}
		}
		else
#endif
		{
			snprintf(fn, sizeof(fn), "%s/%s.tar", basedir, PQgetvalue(res, rownum, 0));
			tarfile = fopen(fn, "wb");
		}
	}

#ifdef HAVE_LIBZ
	if (compresslevel > 0)
	{
		if (!ztarfile)
		{
			/* Compression is in use */
			fprintf(stderr, _("%s: could not create compressed file \"%s\": %s\n"),
					progname, fn, get_gz_error(ztarfile));
			disconnect_and_exit(1);
		}
	}
	else
#endif
	{
		/* Either no zlib support, or zlib support but compresslevel = 0 */
		if (!tarfile)
		{
			fprintf(stderr, _("%s: could not create file \"%s\": %s\n"),
					progname, fn, strerror(errno));
			disconnect_and_exit(1);
		}
	}

	/*
	 * Get the COPY data stream
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
	{
		fprintf(stderr, _("%s: could not get COPY data stream: %s\n"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}

	while (1)
	{
		int			r;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		r = PQgetCopyData(conn, &copybuf, 0);
		if (r == -1)
		{
			/*
			 * End of chunk. Close file (but not stdout).
			 *
			 * Also, write two completely empty blocks at the end of the tar
			 * file, as required by some tar programs.
			 */
			char		zerobuf[1024];

			MemSet(zerobuf, 0, sizeof(zerobuf));
#ifdef HAVE_LIBZ
			if (ztarfile != NULL)
			{
				if (gzwrite(ztarfile, zerobuf, sizeof(zerobuf)) != sizeof(zerobuf))
				{
					fprintf(stderr, _("%s: could not write to compressed file \"%s\": %s\n"),
							progname, fn, get_gz_error(ztarfile));
				}
			}
			else
#endif
			{
				if (fwrite(zerobuf, sizeof(zerobuf), 1, tarfile) != 1)
				{
					fprintf(stderr, _("%s: could not write to file \"%s\": %s\n"),
							progname, fn, strerror(errno));
					disconnect_and_exit(1);
				}
			}

			if (strcmp(basedir, "-") != 0)
			{
#ifdef HAVE_LIBZ
				if (ztarfile != NULL)
					gzclose(ztarfile);
#endif
				if (tarfile != NULL)
					fclose(tarfile);
			}

			break;
		}
		else if (r == -2)
		{
			fprintf(stderr, _("%s: could not read COPY data: %s\n"),
					progname, PQerrorMessage(conn));
			disconnect_and_exit(1);
		}

#ifdef HAVE_LIBZ
		if (ztarfile != NULL)
		{
			if (gzwrite(ztarfile, copybuf, r) != r)
			{
				fprintf(stderr, _("%s: could not write to compressed file \"%s\": %s\n"),
						progname, fn, get_gz_error(ztarfile));
			}
		}
		else
#endif
		{
			if (fwrite(copybuf, r, 1, tarfile) != 1)
			{
				fprintf(stderr, _("%s: could not write to file \"%s\": %s\n"),
						progname, fn, strerror(errno));
				disconnect_and_exit(1);
			}
		}
		totaldone += r;
		if (showprogress)
			progress_report(rownum, fn);
	}							/* while (1) */

	if (copybuf != NULL)
		PQfreemem(copybuf);
}

/*
 * Receive a tar format stream from the connection to the server, and unpack
 * the contents of it into a directory. Only files, directories and
 * symlinks are supported, no other kinds of special files.
 *
 * If the data is for the main data directory, it will be restored in the
 * specified directory. If it's for another tablespace, it will be restored
 * in the original directory, since relocation of tablespaces is not
 * supported.
 */
static void
ReceiveAndUnpackTarFile(PGconn *conn, PGresult *res, int rownum)
{
	char		current_path[MAXPGPATH];
	char		fn[MAXPGPATH];
	int			current_len_left;
	int			current_padding = 0;
	char	   *copybuf = NULL;
	FILE	   *file = NULL;

	if (PQgetisnull(res, rownum, 0))
		strcpy(current_path, basedir);
	else
		strcpy(current_path, PQgetvalue(res, rownum, 1));

	/*
	 * Make sure we're unpacking into an empty directory
	 */
	verify_dir_is_empty_or_create(current_path);

	/*
	 * Get the COPY data
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
	{
		fprintf(stderr, _("%s: could not get COPY data stream: %s\n"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}

	while (1)
	{
		int			r;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		r = PQgetCopyData(conn, &copybuf, 0);

		if (r == -1)
		{
			/*
			 * End of chunk
			 */
			if (file)
				fclose(file);

			break;
		}
		else if (r == -2)
		{
			fprintf(stderr, _("%s: could not read COPY data: %s\n"),
					progname, PQerrorMessage(conn));
			disconnect_and_exit(1);
		}

		if (file == NULL)
		{
			int		filemode;

			/*
			 * No current file, so this must be the header for a new file
			 */
			if (r != 512)
			{
				fprintf(stderr, _("%s: Invalid tar block header size: %i\n"),
						progname, r);
				disconnect_and_exit(1);
			}
			totaldone += 512;

			if (sscanf(copybuf + 124, "%11o", &current_len_left) != 1)
			{
				fprintf(stderr, _("%s: could not parse file size!\n"),
						progname);
				disconnect_and_exit(1);
			}

			/* Set permissions on the file */
			if (sscanf(&copybuf[100], "%07o ", &filemode) != 1)
			{
				fprintf(stderr, _("%s: could not parse file mode!\n"),
						progname);
				disconnect_and_exit(1);
			}

			/*
			 * All files are padded up to 512 bytes
			 */
			current_padding =
				((current_len_left + 511) & ~511) - current_len_left;

			/*
			 * First part of header is zero terminated filename
			 */
			snprintf(fn, sizeof(fn), "%s/%s", current_path, copybuf);
			if (fn[strlen(fn) - 1] == '/')
			{
				/*
				 * Ends in a slash means directory or symlink to directory
				 */
				if (copybuf[156] == '5')
				{
					/*
					 * Directory
					 */
					fn[strlen(fn) - 1] = '\0';	/* Remove trailing slash */
					if (mkdir(fn, S_IRWXU) != 0)
					{
						fprintf(stderr,
							_("%s: could not create directory \"%s\": %s\n"),
								progname, fn, strerror(errno));
						disconnect_and_exit(1);
					}
#ifndef WIN32
					if (chmod(fn, (mode_t) filemode))
						fprintf(stderr, _("%s: could not set permissions on directory \"%s\": %s\n"),
								progname, fn, strerror(errno));
#endif
				}
				else if (copybuf[156] == '2')
				{
					/*
					 * Symbolic link
					 */
					fn[strlen(fn) - 1] = '\0';	/* Remove trailing slash */
					if (symlink(&copybuf[157], fn) != 0)
					{
						fprintf(stderr,
								_("%s: could not create symbolic link from %s to %s: %s\n"),
								progname, fn, &copybuf[157], strerror(errno));
						disconnect_and_exit(1);
					}
				}
				else
				{
					fprintf(stderr, _("%s: unknown link indicator \"%c\"\n"),
							progname, copybuf[156]);
					disconnect_and_exit(1);
				}
				continue;		/* directory or link handled */
			}

			/*
			 * regular file
			 */
			file = fopen(fn, "wb");
			if (!file)
			{
				fprintf(stderr, _("%s: could not create file \"%s\": %s\n"),
						progname, fn, strerror(errno));
				disconnect_and_exit(1);
			}

#ifndef WIN32
			if (chmod(fn, (mode_t) filemode))
				fprintf(stderr, _("%s: could not set permissions on file \"%s\": %s\n"),
						progname, fn, strerror(errno));
#endif

			if (current_len_left == 0)
			{
				/*
				 * Done with this file, next one will be a new tar header
				 */
				fclose(file);
				file = NULL;
				continue;
			}
		}						/* new file */
		else
		{
			/*
			 * Continuing blocks in existing file
			 */
			if (current_len_left == 0 && r == current_padding)
			{
				/*
				 * Received the padding block for this file, ignore it and
				 * close the file, then move on to the next tar header.
				 */
				fclose(file);
				file = NULL;
				totaldone += r;
				continue;
			}

			if (fwrite(copybuf, r, 1, file) != 1)
			{
				fprintf(stderr, _("%s: could not write to file \"%s\": %s\n"),
						progname, fn, strerror(errno));
				disconnect_and_exit(1);
			}
			totaldone += r;
			if (showprogress)
				progress_report(rownum, fn);

			current_len_left -= r;
			if (current_len_left == 0 && current_padding == 0)
			{
				/*
				 * Received the last block, and there is no padding to be
				 * expected. Close the file and move on to the next tar
				 * header.
				 */
				fclose(file);
				file = NULL;
				continue;
			}
		}						/* continuing data in existing file */
	}							/* loop over all data blocks */

	if (file != NULL)
	{
		fprintf(stderr, _("%s: last file was never finsihed!\n"), progname);
		disconnect_and_exit(1);
	}

	if (copybuf != NULL)
		PQfreemem(copybuf);
}


static PGconn *
GetConnection(void)
{
	PGconn	   *tmpconn;
	int			argcount = 4;	/* dbname, replication, fallback_app_name,
								 * password */
	int			i;
	const char **keywords;
	const char **values;
	char	   *password = NULL;

	if (dbhost)
		argcount++;
	if (dbuser)
		argcount++;
	if (dbport)
		argcount++;

	keywords = xmalloc0((argcount + 1) * sizeof(*keywords));
	values = xmalloc0((argcount + 1) * sizeof(*values));

	keywords[0] = "dbname";
	values[0] = "replication";
	keywords[1] = "replication";
	values[1] = "true";
	keywords[2] = "fallback_application_name";
	values[2] = progname;
	i = 3;
	if (dbhost)
	{
		keywords[i] = "host";
		values[i] = dbhost;
		i++;
	}
	if (dbuser)
	{
		keywords[i] = "user";
		values[i] = dbuser;
		i++;
	}
	if (dbport)
	{
		keywords[i] = "port";
		values[i] = dbport;
		i++;
	}

	while (true)
	{
		if (dbgetpassword == 1)
		{
			/* Prompt for a password */
			password = simple_prompt(_("Password: "), 100, false);
			keywords[argcount - 1] = "password";
			values[argcount - 1] = password;
		}

		tmpconn = PQconnectdbParams(keywords, values, true);
		if (password)
			free(password);

		if (PQstatus(tmpconn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(tmpconn) &&
			dbgetpassword != -1)
		{
			dbgetpassword = 1;	/* ask for password next time */
			PQfinish(tmpconn);
			continue;
		}

		if (PQstatus(tmpconn) != CONNECTION_OK)
		{
			fprintf(stderr, _("%s: could not connect to server: %s\n"),
					progname, PQerrorMessage(tmpconn));
			exit(1);
		}

		/* Connection ok! */
		free(values);
		free(keywords);
		return tmpconn;
	}
}

static void
BaseBackup()
{
	PGresult   *res;
	char		current_path[MAXPGPATH];
	char		escaped_label[MAXPGPATH];
	int			i;

	/*
	 * Connect in replication mode to the server
	 */
	conn = GetConnection();

	PQescapeStringConn(conn, escaped_label, label, sizeof(escaped_label), &i);
	snprintf(current_path, sizeof(current_path), "BASE_BACKUP LABEL '%s' %s %s",
			 escaped_label,
			 showprogress ? "PROGRESS" : "",
			 fastcheckpoint ? "FAST" : "");

	if (PQsendQuery(conn, current_path) == 0)
	{
		fprintf(stderr, _("%s: could not start base backup: %s\n"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}

	/*
	 * Get the header
	 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, _("%s: could not initiate base backup: %s\n"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}
	if (PQntuples(res) < 1)
	{
		fprintf(stderr, _("%s: no data returned from server.\n"), progname);
		disconnect_and_exit(1);
	}

	/*
	 * Sum up the total size, for progress reporting
	 */
	totalsize = totaldone = 0;
	tablespacecount = PQntuples(res);
	for (i = 0; i < PQntuples(res); i++)
	{
		if (showprogress)
			totalsize += atol(PQgetvalue(res, i, 2));

		/*
		 * Verify tablespace directories are empty. Don't bother with the
		 * first once since it can be relocated, and it will be checked before
		 * we do anything anyway.
		 */
		if (format == 'p' && i > 0)
			verify_dir_is_empty_or_create(PQgetvalue(res, i, 1));
	}

	/*
	 * When writing to stdout, require a single tablespace
	 */
	if (format == 't' && strcmp(basedir, "-") == 0 && PQntuples(res) > 1)
	{
		fprintf(stderr, _("%s: can only write single tablespace to stdout, database has %i.\n"),
				progname, PQntuples(res));
		disconnect_and_exit(1);
	}

	/*
	 * Start receiving chunks
	 */
	for (i = 0; i < PQntuples(res); i++)
	{
		if (format == 't')
			ReceiveTarFile(conn, res, i);
		else
			ReceiveAndUnpackTarFile(conn, res, i);
	}							/* Loop over all tablespaces */

	if (showprogress)
	{
		progress_report(PQntuples(res), "");
		fprintf(stderr, "\n");	/* Need to move to next line */
	}
	PQclear(res);

	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, _("%s: final receive failed: %s\n"),
				progname, PQerrorMessage(conn));
		disconnect_and_exit(1);
	}

	/*
	 * End of copy data. Final result is already checked inside the loop.
	 */
	PQfinish(conn);

	if (verbose)
		fprintf(stderr, "%s: base backup completed.\n", progname);
}


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"pgdata", required_argument, NULL, 'D'},
		{"format", required_argument, NULL, 'F'},
		{"checkpoint", required_argument, NULL, 'c'},
		{"compress", required_argument, NULL, 'Z'},
		{"label", required_argument, NULL, 'l'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"verbose", no_argument, NULL, 'v'},
		{"progress", no_argument, NULL, 'P'},
		{NULL, 0, NULL, 0}
	};
	int			c;

	int			option_index;

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_basebackup"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		else if (strcmp(argv[1], "-V") == 0
				 || strcmp(argv[1], "--version") == 0)
		{
			puts("pg_basebackup (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "D:F:l:Z:c:h:p:U:wWvP",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'D':
				basedir = xstrdup(optarg);
				break;
			case 'F':
				if (strcmp(optarg, "p") == 0 || strcmp(optarg, "plain") == 0)
					format = 'p';
				else if (strcmp(optarg, "t") == 0 || strcmp(optarg, "tar") == 0)
					format = 't';
				else
				{
					fprintf(stderr, _("%s: invalid output format \"%s\", must be \"plain\" or \"tar\"\n"),
							progname, optarg);
					exit(1);
				}
				break;
			case 'l':
				label = xstrdup(optarg);
				break;
			case 'Z':
				compresslevel = atoi(optarg);
				if (compresslevel <= 0 || compresslevel > 9)
				{
					fprintf(stderr, _("%s: invalid compression level \"%s\"\n"),
							progname, optarg);
					exit(1);
				}
				break;
			case 'c':
				if (pg_strcasecmp(optarg, "fast") == 0)
					fastcheckpoint = true;
				else if (pg_strcasecmp(optarg, "spread") == 0)
					fastcheckpoint = false;
				else
				{
					fprintf(stderr, _("%s: invalid checkpoint argument \"%s\", must be \"fast\" or \"spread\"\n"),
							progname, optarg);
					exit(1);
				}
				break;
			case 'h':
				dbhost = xstrdup(optarg);
				break;
			case 'p':
				if (atoi(optarg) <= 0)
				{
					fprintf(stderr, _("%s: invalid port number \"%s\"\n"),
							progname, optarg);
					exit(1);
				}
				dbport = xstrdup(optarg);
				break;
			case 'U':
				dbuser = xstrdup(optarg);
				break;
			case 'w':
				dbgetpassword = -1;
				break;
			case 'W':
				dbgetpassword = 1;
				break;
			case 'v':
				verbose++;
				break;
			case 'P':
				showprogress = true;
				break;
			default:

				/*
				 * getopt_long already emitted a complaint
				 */
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
		}
	}

	/*
	 * Any non-option arguments?
	 */
	if (optind < argc)
	{
		fprintf(stderr,
				_("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Required arguments
	 */
	if (basedir == NULL)
	{
		fprintf(stderr, _("%s: no target directory specified\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Mutually exclusive arguments
	 */
	if (format == 'p' && compresslevel > 0)
	{
		fprintf(stderr,
				_("%s: only tar mode backups can be compressed\n"),
				progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

#ifndef HAVE_LIBZ
	if (compresslevel > 0)
	{
		fprintf(stderr,
				_("%s: this build does not support compression\n"),
				progname);
		exit(1);
	}
#else
	if (compresslevel > 0 && strcmp(basedir, "-") == 0)
	{
		fprintf(stderr,
				_("%s: compression is not supported on standard output\n"),
				progname);
		exit(1);
	}
#endif

	/*
	 * Verify that the target directory exists, or create it. For plaintext
	 * backups, always require the directory. For tar backups, require it
	 * unless we are writing to stdout.
	 */
	if (format == 'p' || strcmp(basedir, "-") != 0)
		verify_dir_is_empty_or_create(basedir);


	BaseBackup();

	return 0;
}
