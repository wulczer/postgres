/**********************************************************************
 * plpython.c - Python as a procedural language for PostgreSQL
 *
 *	src/pl/plpython/plpython.c
 *
 **********************************************************************/

#include "postgres.h"

#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "plpython.h"


static bool PLy_procedure_is_trigger(Form_pg_proc);
static void plpython_error_callback(void *);
static void plpython_inline_error_callback(void *);

static const int plpython_python_version = PY_MAJOR_VERSION;

/* initialize global variables */
PyObject *PLy_interp_globals = NULL;

List *explicit_subtransactions = NIL;

PyObject *PLy_exc_error = NULL;
PyObject *PLy_exc_fatal = NULL;
PyObject *PLy_exc_spi_error = NULL;

HTAB *PLy_procedure_cache = NULL;
HTAB *PLy_trigger_cache = NULL;
HTAB *PLy_spi_exceptions = NULL;

PLyProcedure *PLy_curr_procedure = NULL;

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(plpython_validator);
PG_FUNCTION_INFO_V1(plpython_call_handler);
PG_FUNCTION_INFO_V1(plpython_inline_handler);

#if PY_MAJOR_VERSION < 3
PG_FUNCTION_INFO_V1(plpython2_validator);
PG_FUNCTION_INFO_V1(plpython2_call_handler);
PG_FUNCTION_INFO_V1(plpython2_inline_handler);
#endif

void
_PG_init(void)
{
	/* Be sure we do initialization only once (should be redundant now) */
	static bool inited = false;
	const int **version_ptr;
	HASHCTL		hash_ctl;

	if (inited)
		return;

	/* Be sure we don't run Python 2 and 3 in the same session (might crash) */
	version_ptr = (const int **) find_rendezvous_variable("plpython_python_version");
	if (!(*version_ptr))
		*version_ptr = &plpython_python_version;
	else
	{
		if (**version_ptr != plpython_python_version)
			ereport(FATAL,
					(errmsg("Python major version mismatch in session"),
					 errdetail("This session has previously used Python major version %d, and it is now attempting to use Python major version %d.",
							   **version_ptr, plpython_python_version),
					 errhint("Start a new session to use a different Python major version.")));
	}

	pg_bindtextdomain(TEXTDOMAIN);

#if PY_MAJOR_VERSION >= 3
	PyImport_AppendInittab("plpy", PyInit_plpy);
#endif
	Py_Initialize();
#if PY_MAJOR_VERSION >= 3
	PyImport_ImportModule("plpy");
#endif
	PLy_init_interp();
	PLy_init_plpy();
	if (PyErr_Occurred())
		PLy_elog(FATAL, "untrapped error in initialization");

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(PLyProcedureEntry);
	hash_ctl.hash = oid_hash;
	PLy_procedure_cache = hash_create("PL/Python procedures", 32, &hash_ctl,
									  HASH_ELEM | HASH_FUNCTION);

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(PLyProcedureEntry);
	hash_ctl.hash = oid_hash;
	PLy_trigger_cache = hash_create("PL/Python triggers", 32, &hash_ctl,
									HASH_ELEM | HASH_FUNCTION);

	explicit_subtransactions = NIL;

	inited = true;
}

Datum
plpython_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc procStruct;
	bool		is_trigger;

	if (!check_function_bodies)
	{
		PG_RETURN_VOID();
	}

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	procStruct = (Form_pg_proc) GETSTRUCT(tuple);

	is_trigger = PLy_procedure_is_trigger(procStruct);

	ReleaseSysCache(tuple);

	PLy_procedure_get(funcoid, is_trigger);

	PG_RETURN_VOID();
}

#if PY_MAJOR_VERSION < 3
Datum
plpython2_validator(PG_FUNCTION_ARGS)
{
	return plpython_validator(fcinfo);
}
#endif   /* PY_MAJOR_VERSION < 3 */

Datum
plpython_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval;
	PLyProcedure *save_curr_proc;
	ErrorContextCallback plerrcontext;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	save_curr_proc = PLy_curr_procedure;

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpython_error_callback;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	PG_TRY();
	{
		PLyProcedure *proc;

		if (CALLED_AS_TRIGGER(fcinfo))
		{
			HeapTuple	trv;

			proc = PLy_procedure_get(fcinfo->flinfo->fn_oid, true);
			PLy_curr_procedure = proc;
			trv = PLy_trigger_handler(fcinfo, proc);
			retval = PointerGetDatum(trv);
		}
		else
		{
			proc = PLy_procedure_get(fcinfo->flinfo->fn_oid, false);
			PLy_curr_procedure = proc;
			retval = PLy_function_handler(fcinfo, proc);
		}
	}
	PG_CATCH();
	{
		PLy_curr_procedure = save_curr_proc;
		PyErr_Clear();
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Pop the error context stack */
	error_context_stack = plerrcontext.previous;

	PLy_curr_procedure = save_curr_proc;

	return retval;
}

#if PY_MAJOR_VERSION < 3
Datum
plpython2_call_handler(PG_FUNCTION_ARGS)
{
	return plpython_call_handler(fcinfo);
}
#endif   /* PY_MAJOR_VERSION < 3 */

Datum
plpython_inline_handler(PG_FUNCTION_ARGS)
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));
	FunctionCallInfoData fake_fcinfo;
	FmgrInfo	flinfo;
	PLyProcedure *save_curr_proc;
	PLyProcedure proc;
	ErrorContextCallback plerrcontext;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	save_curr_proc = PLy_curr_procedure;

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpython_inline_error_callback;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	MemSet(&fake_fcinfo, 0, sizeof(fake_fcinfo));
	MemSet(&flinfo, 0, sizeof(flinfo));
	fake_fcinfo.flinfo = &flinfo;
	flinfo.fn_oid = InvalidOid;
	flinfo.fn_mcxt = CurrentMemoryContext;

	MemSet(&proc, 0, sizeof(PLyProcedure));
	proc.pyname = PLy_strdup("__plpython_inline_block");
	proc.result.out.d.typoid = VOIDOID;

	PG_TRY();
	{
		PLy_procedure_compile(&proc, codeblock->source_text);
		PLy_curr_procedure = &proc;
		PLy_function_handler(&fake_fcinfo, &proc);
	}
	PG_CATCH();
	{
		PLy_procedure_delete(&proc);
		PLy_curr_procedure = save_curr_proc;
		PyErr_Clear();
		PG_RE_THROW();
	}
	PG_END_TRY();

	PLy_procedure_delete(&proc);

	/* Pop the error context stack */
	error_context_stack = plerrcontext.previous;

	PLy_curr_procedure = save_curr_proc;

	PG_RETURN_VOID();
}

#if PY_MAJOR_VERSION < 3
Datum
plpython2_inline_handler(PG_FUNCTION_ARGS)
{
	return plpython_inline_handler(fcinfo);
}
#endif   /* PY_MAJOR_VERSION < 3 */

/* call PyErr_SetString with a vprint interface and translation support */
void
PLy_exception_set(PyObject *exc, const char *fmt,...)
{
	char		buf[1024];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), dgettext(TEXTDOMAIN, fmt), ap);
	va_end(ap);

	PyErr_SetString(exc, buf);
}

/* same, with pluralized message */
void
PLy_exception_set_plural(PyObject *exc,
						 const char *fmt_singular, const char *fmt_plural,
						 unsigned long n,...)
{
	char		buf[1024];
	va_list		ap;

	va_start(ap, n);
	vsnprintf(buf, sizeof(buf),
			  dngettext(TEXTDOMAIN, fmt_singular, fmt_plural, n),
			  ap);
	va_end(ap);

	PyErr_SetString(exc, buf);
}

void *
PLy_malloc(size_t bytes)
{
	/* We need our allocations to be long-lived, so use TopMemoryContext */
	return MemoryContextAlloc(TopMemoryContext, bytes);
}

void *
PLy_malloc0(size_t bytes)
{
	void	   *ptr = PLy_malloc(bytes);

	MemSet(ptr, 0, bytes);
	return ptr;
}

char *
PLy_strdup(const char *str)
{
	char	   *result;
	size_t		len;

	len = strlen(str) + 1;
	result = PLy_malloc(len);
	memcpy(result, str, len);

	return result;
}

/* define this away */
void
PLy_free(void *ptr)
{
	pfree(ptr);
}

/*
 * Convert a Python unicode object to a Python string/bytes object in
 * PostgreSQL server encoding.	Reference ownership is passed to the
 * caller.
 */
PyObject *
PLyUnicode_Bytes(PyObject *unicode)
{
	PyObject   *rv;
	const char *serverenc;

	/*
	 * Python understands almost all PostgreSQL encoding names, but it doesn't
	 * know SQL_ASCII.
	 */
	if (GetDatabaseEncoding() == PG_SQL_ASCII)
		serverenc = "ascii";
	else
		serverenc = GetDatabaseEncodingName();
	rv = PyUnicode_AsEncodedString(unicode, serverenc, "strict");
	if (rv == NULL)
		PLy_elog(ERROR, "could not convert Python Unicode object to PostgreSQL server encoding");
	return rv;
}

/*
 * Convert a Python unicode object to a C string in PostgreSQL server
 * encoding.  No Python object reference is passed out of this
 * function.  The result is palloc'ed.
 *
 * Note that this function is disguised as PyString_AsString() when
 * using Python 3.	That function retuns a pointer into the internal
 * memory of the argument, which isn't exactly the interface of this
 * function.  But in either case you get a rather short-lived
 * reference that you ought to better leave alone.
 */
char *
PLyUnicode_AsString(PyObject *unicode)
{
	PyObject   *o = PLyUnicode_Bytes(unicode);
	char	   *rv = pstrdup(PyBytes_AsString(o));

	Py_XDECREF(o);
	return rv;
}

#if PY_MAJOR_VERSION >= 3
/*
 * Convert a C string in the PostgreSQL server encoding to a Python
 * unicode object.	Reference ownership is passed to the caller.
 */
PyObject *
PLyUnicode_FromString(const char *s)
{
	char	   *utf8string;
	PyObject   *o;

	utf8string = (char *) pg_do_encoding_conversion((unsigned char *) s,
													strlen(s),
													GetDatabaseEncoding(),
													PG_UTF8);

	o = PyUnicode_FromString(utf8string);

	if (utf8string != s)
		pfree(utf8string);

	return o;
}
#endif   /* PY_MAJOR_VERSION >= 3 */

static bool PLy_procedure_is_trigger(Form_pg_proc procStruct)
{
	return (procStruct->prorettype == TRIGGEROID ||
			(procStruct->prorettype == OPAQUEOID &&
			 procStruct->pronargs == 0));
}

static void
plpython_error_callback(void *arg)
{
	if (PLy_curr_procedure)
		errcontext("PL/Python function \"%s\"",
				   PLy_procedure_name(PLy_curr_procedure));
}

static void
plpython_inline_error_callback(void *arg)
{
	errcontext("PL/Python anonymous code block");
}
