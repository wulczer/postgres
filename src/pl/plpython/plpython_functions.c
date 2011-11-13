/**********************************************************************
 * plpython_functions.c - user-visible utility functions for plpython
 *
 *	src/pl/plpython/plpython_function.c
 *
 **********************************************************************/

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/builtins.h"

#include "plpython.h"


/* the python interface to the elog function
 * don't confuse these with PLy_elog
 */
static PyObject *PLy_output(volatile int, PyObject *, PyObject *);

PyObject *
PLy_debug(PyObject *self, PyObject *args)
{
	return PLy_output(DEBUG2, self, args);
}

PyObject *
PLy_log(PyObject *self, PyObject *args)
{
	return PLy_output(LOG, self, args);
}

PyObject *
PLy_info(PyObject *self, PyObject *args)
{
	return PLy_output(INFO, self, args);
}

PyObject *
PLy_notice(PyObject *self, PyObject *args)
{
	return PLy_output(NOTICE, self, args);
}

PyObject *
PLy_warning(PyObject *self, PyObject *args)
{
	return PLy_output(WARNING, self, args);
}

PyObject *
PLy_error(PyObject *self, PyObject *args)
{
	return PLy_output(ERROR, self, args);
}

PyObject *
PLy_fatal(PyObject *self, PyObject *args)
{
	return PLy_output(FATAL, self, args);
}

PyObject *
PLy_quote_literal(PyObject *self, PyObject *args)
{
	const char *str;
	char	   *quoted;
	PyObject   *ret;

	if (!PyArg_ParseTuple(args, "s", &str))
		return NULL;

	quoted = quote_literal_cstr(str);
	ret = PyString_FromString(quoted);
	pfree(quoted);

	return ret;
}

PyObject *
PLy_quote_nullable(PyObject *self, PyObject *args)
{
	const char *str;
	char	   *quoted;
	PyObject   *ret;

	if (!PyArg_ParseTuple(args, "z", &str))
		return NULL;

	if (str == NULL)
		return PyString_FromString("NULL");

	quoted = quote_literal_cstr(str);
	ret = PyString_FromString(quoted);
	pfree(quoted);

	return ret;
}

PyObject *
PLy_quote_ident(PyObject *self, PyObject *args)
{
	const char *str;
	const char *quoted;
	PyObject   *ret;

	if (!PyArg_ParseTuple(args, "s", &str))
		return NULL;

	quoted = quote_identifier(str);
	ret = PyString_FromString(quoted);

	return ret;
}

static PyObject *
PLy_output(volatile int level, PyObject *self, PyObject *args)
{
	PyObject   *volatile so;
	char	   *volatile sv;
	volatile MemoryContext oldcontext;

	if (PyTuple_Size(args) == 1)
	{
		/*
		 * Treat single argument specially to avoid undesirable ('tuple',)
		 * decoration.
		 */
		PyObject   *o;

		PyArg_UnpackTuple(args, "plpy.elog", 1, 1, &o);
		so = PyObject_Str(o);
	}
	else
		so = PyObject_Str(args);
	if (so == NULL || ((sv = PyString_AsString(so)) == NULL))
	{
		level = ERROR;
		sv = dgettext(TEXTDOMAIN, "could not parse error message in plpy.elog");
	}

	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		pg_verifymbstr(sv, strlen(sv), false);
		elog(level, "%s", sv);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/*
		 * Note: If sv came from PyString_AsString(), it points into storage
		 * owned by so.  So free so after using sv.
		 */
		Py_XDECREF(so);

		/* Make Python raise the exception */
		PLy_exception_set(PLy_exc_error, "%s", edata->message);
		return NULL;
	}
	PG_END_TRY();

	Py_XDECREF(so);

	/*
	 * return a legal object so the interpreter will continue on its merry way
	 */
	Py_INCREF(Py_None);
	return Py_None;
}
