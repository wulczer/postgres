/**********************************************************************
 * plpython_plpy.c - interpreter initialization and the plpy module
 *
 *	src/pl/plpython/plpython_plpy.c
 *
 **********************************************************************/

#include "postgres.h"

#include "plpython.h"

static void PLy_add_exceptions(PyObject *);
static void PLy_generate_spi_exceptions(PyObject *, PyObject *);

static PyObject *PLy_interp_safe_globals = NULL;

/* A list of all known exceptions, generated from backend/utils/errcodes.txt */
typedef struct ExceptionMap
{
	char	   *name;
	char	   *classname;
	int			sqlstate;
} ExceptionMap;

static const ExceptionMap exception_map[] = {
#include "spiexceptions.h"
	{NULL, NULL, 0}
};

static PyMethodDef PLy_methods[] = {
	/*
	 * logging methods
	 */
	{"debug", PLy_debug, METH_VARARGS, NULL},
	{"log", PLy_log, METH_VARARGS, NULL},
	{"info", PLy_info, METH_VARARGS, NULL},
	{"notice", PLy_notice, METH_VARARGS, NULL},
	{"warning", PLy_warning, METH_VARARGS, NULL},
	{"error", PLy_error, METH_VARARGS, NULL},
	{"fatal", PLy_fatal, METH_VARARGS, NULL},

	/*
	 * create a stored plan
	 */
	{"prepare", PLy_spi_prepare, METH_VARARGS, NULL},

	/*
	 * execute a plan or query
	 */
	{"execute", PLy_spi_execute, METH_VARARGS, NULL},

	/*
	 * escaping strings
	 */
	{"quote_literal", PLy_quote_literal, METH_VARARGS, NULL},
	{"quote_nullable", PLy_quote_nullable, METH_VARARGS, NULL},
	{"quote_ident", PLy_quote_ident, METH_VARARGS, NULL},

	/*
	 * create the subtransaction context manager
	 */
	{"subtransaction", PLy_subtransaction_new, METH_NOARGS, NULL},

	/*
	 * create a cursor
	 */
	{"cursor", PLy_cursor, METH_VARARGS, NULL},

	{NULL, NULL, 0, NULL}
};

static PyMethodDef PLy_exc_methods[] = {
	{NULL, NULL, 0, NULL}
};

#if PY_MAJOR_VERSION >= 3
static PyModuleDef PLy_module = {
	PyModuleDef_HEAD_INIT,		/* m_base */
	"plpy",						/* m_name */
	NULL,						/* m_doc */
	-1,							/* m_size */
	PLy_methods,				/* m_methods */
};

static PyModuleDef PLy_exc_module = {
	PyModuleDef_HEAD_INIT,		/* m_base */
	"spiexceptions",			/* m_name */
	NULL,						/* m_doc */
	-1,							/* m_size */
	PLy_exc_methods,			/* m_methods */
	NULL,						/* m_reload */
	NULL,						/* m_traverse */
	NULL,						/* m_clear */
	NULL						/* m_free */
};

/*
 * Must have external linkage, because PyMODINIT_FUNC does dllexport on
 * Windows-like platforms.
 */
PyMODINIT_FUNC
PyInit_plpy(void)
{
	PyObject   *m;

	m = PyModule_Create(&PLy_module);
	if (m == NULL)
		return NULL;

	PLy_add_exceptions(m);

	return m;
}
#endif /* PY_MAJOR_VERSION >= 3 */

/*
 * These should only be called once from _PG_init. Initialize the Python
 * interpreter and global data.
 */
void
PLy_init_interp(void)
{
	PyObject   *mainmod;

	mainmod = PyImport_AddModule("__main__");
	if (mainmod == NULL || PyErr_Occurred())
		PLy_elog(ERROR, "could not import \"__main__\" module");
	Py_INCREF(mainmod);
	PLy_interp_globals = PyModule_GetDict(mainmod);
	PLy_interp_safe_globals = PyDict_New();
	PyDict_SetItemString(PLy_interp_globals, "GD", PLy_interp_safe_globals);
	Py_DECREF(mainmod);
	if (PLy_interp_globals == NULL || PyErr_Occurred())
		PLy_elog(ERROR, "could not initialize globals");
}

void
PLy_init_plpy(void)
{
	PyObject   *main_mod,
			   *main_dict,
			   *plpy_mod;
#if PY_MAJOR_VERSION < 3
	PyObject   *plpy;
#endif

	/*
	 * initialize plpy module
	 */
	PLy_plan_init_type();
	PLy_result_init_type();
	PLy_subtransaction_init_type();
	PLy_cursor_init_type();

#if PY_MAJOR_VERSION >= 3
	PyModule_Create(&PLy_module);
	/* for Python 3 we initialized the exceptions in PyInit_plpy */
#else
	plpy = Py_InitModule("plpy", PLy_methods);
	PLy_add_exceptions(plpy);
#endif

	/* PyDict_SetItemString(plpy, "PlanType", (PyObject *) &PLy_PlanType); */

	/*
	 * initialize main module, and add plpy
	 */
	main_mod = PyImport_AddModule("__main__");
	main_dict = PyModule_GetDict(main_mod);
	plpy_mod = PyImport_AddModule("plpy");
	PyDict_SetItemString(main_dict, "plpy", plpy_mod);
	if (PyErr_Occurred())
		elog(ERROR, "could not initialize plpy");
}

static void
PLy_add_exceptions(PyObject *plpy)
{
	PyObject   *excmod;
	HASHCTL		hash_ctl;

#if PY_MAJOR_VERSION < 3
	excmod = Py_InitModule("spiexceptions", PLy_exc_methods);
#else
	excmod = PyModule_Create(&PLy_exc_module);
#endif
	if (PyModule_AddObject(plpy, "spiexceptions", excmod) < 0)
		PLy_elog(ERROR, "could not add the spiexceptions module");

    /*
	 * XXX it appears that in some circumstances the reference count of the
	 * spiexceptions module drops to zero causing a Python assert failure when
	 * the garbage collector visits the module. This has been observed on the
	 * buildfarm. To fix this, add an additional ref for the module here.
	 *
	 * This shouldn't cause a memory leak - we don't want this garbage
	 * collected, and this function shouldn't be called more than once per
	 * backend.
	 */
	Py_INCREF(excmod);

	PLy_exc_error = PyErr_NewException("plpy.Error", NULL, NULL);
	PLy_exc_fatal = PyErr_NewException("plpy.Fatal", NULL, NULL);
	PLy_exc_spi_error = PyErr_NewException("plpy.SPIError", NULL, NULL);

	Py_INCREF(PLy_exc_error);
	PyModule_AddObject(plpy, "Error", PLy_exc_error);
	Py_INCREF(PLy_exc_fatal);
	PyModule_AddObject(plpy, "Fatal", PLy_exc_fatal);
	Py_INCREF(PLy_exc_spi_error);
	PyModule_AddObject(plpy, "SPIError", PLy_exc_spi_error);

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(int);
	hash_ctl.entrysize = sizeof(PLyExceptionEntry);
	hash_ctl.hash = tag_hash;
	PLy_spi_exceptions = hash_create("SPI exceptions", 256,
									 &hash_ctl, HASH_ELEM | HASH_FUNCTION);

	PLy_generate_spi_exceptions(excmod, PLy_exc_spi_error);
}

/*
 * Add all the autogenerated exceptions as subclasses of SPIError
 */
static void
PLy_generate_spi_exceptions(PyObject *mod, PyObject *base)
{
	int			i;

	for (i = 0; exception_map[i].name != NULL; i++)
	{
		bool		found;
		PyObject   *exc;
		PLyExceptionEntry *entry;
		PyObject   *sqlstate;
		PyObject   *dict = PyDict_New();

		sqlstate = PyString_FromString(unpack_sql_state(exception_map[i].sqlstate));
		PyDict_SetItemString(dict, "sqlstate", sqlstate);
		Py_DECREF(sqlstate);
		exc = PyErr_NewException(exception_map[i].name, base, dict);
		PyModule_AddObject(mod, exception_map[i].classname, exc);
		entry = hash_search(PLy_spi_exceptions, &exception_map[i].sqlstate,
							HASH_ENTER, &found);
		entry->exc = exc;
		Assert(!found);
	}
}
