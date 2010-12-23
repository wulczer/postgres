/*
 * contrib/src/hstore_plpython.c
 *
 * bidirectional transformation between hstores and Python dictionary objects
 */

/* Only build if PL/Python support is needed */
#if defined(HSTORE_PLPYTHON_SUPPORT)

#if defined(_MSC_VER) && defined(_DEBUG)
/* Python uses #pragma to bring in a non-default libpython on VC++ if
 * _DEBUG is defined */
#undef _DEBUG
/* Also hide away errcode, since we load Python.h before postgres.h */
#define errcode __msvc_errcode
#include <Python.h>
#undef errcode
#define _DEBUG
#elif defined (_MSC_VER)
#define errcode __msvc_errcode
#include <Python.h>
#undef errcode
#else
#include <Python.h>
#endif

#include "postgres.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "catalog/namespace.h"

#include "plpython.h"
#include "hstore.h"

static Oid get_hstore_oid(const char *name);
static void set_hstore_parsers(Oid);

static PyObject *hstore_to_dict(void *, Datum);
static Datum dict_to_hstore(void *, int32, PyObject *);

/* GUC variables */

static char *hstore_name;

/* Previous hstore OID */

static Oid previous;

PLyParsers parsers = {
	.in = hstore_to_dict,
	.out = dict_to_hstore
};

static PyObject *
hstore_to_dict(void *ignored, Datum d)
{
	HStore		*hstore = DatumGetHStoreP(d);
    char        *base;
    HEntry      *entries;
    int          count;
    int          i;
    PyObject    *ret;

	base = STRPTR(hstore);
    entries = ARRPTR(hstore);

    ret = PyDict_New();

    count = HS_COUNT(hstore);

    for (i = 0; i < count; i++)
	{
            PyObject *key, *val;

            key = PyString_FromStringAndSize(HS_KEY(entries, base, i),
                                             HS_KEYLEN(entries, i));
            if (HS_VALISNULL(entries, i)) {
                Py_INCREF(Py_None);
                val = Py_None;
            }
            else {
                val = PyString_FromStringAndSize(HS_VAL(entries, base, i),
                                                 HS_VALLEN(entries, i));
            }

            PyDict_SetItem(ret, key, val);
        }

    return ret;
}

static Datum
dict_to_hstore(void *ignored, int32 typmod, PyObject *dict)
{
    HStore      *hstore;
    int          pcount;
	Pairs		*pairs;
	PyObject	*key;
	PyObject	*value;
	Py_ssize_t	 pos;
	char		*keys;
	char		*vals;
	int			 keylen;
	int			 vallen;
	int			 buflen;
	int			 i;

	if (!PyDict_Check(dict))
		ereport(ERROR,
				(errmsg("hstores can only be constructed "
						"from Python dictionaries")));

	pcount = PyDict_Size(dict);
	pairs = palloc(pcount * sizeof(Pairs));
	pos = i = 0;
	/* loop over the dictionary, creating a Pair for each key/value pair */
	while (PyDict_Next(dict, &pos, &key, &value)) {
		if (!PyString_Check(key))
			elog(ERROR, "hstore keys have to be strings");

		PyString_AsStringAndSize(key, &keys, &keylen);

		if (strlen(keys) != keylen)
			elog(ERROR, "hstore keys cannot contain NUL bytes");

		pairs[i].key = pstrdup(keys);
		pairs[i].keylen = hstoreCheckKeyLen(keylen);
		pairs[i].needfree = true;

		if (value == Py_None) {
			pairs[i].val = NULL;
			pairs[i].vallen = 0;
			pairs[i].isnull = true;
		}
		else {
			if (!PyString_Check(value))
				elog(ERROR, "hstore values have to be strings");

			PyString_AsStringAndSize(value, &vals, &vallen);

			if (strlen(vals) != vallen)
				elog(ERROR, "hstore values cannot contain NUL bytes");

			pairs[i].val = pstrdup(vals);
			pairs[i].vallen = hstoreCheckValLen(vallen);
			pairs[i].isnull = false;
		}

		i++;
	}
	pcount = hstoreUniquePairs(pairs, pcount, &buflen);
	hstore = hstorePairs(pairs, pcount, buflen);

	return PointerGetDatum(hstore);
}

static const char *
recheck_hstore_oid(const char *newvalue, bool doit, GucSource source)
{
	Oid	hstore_oid;

	if (newvalue == NULL)
		return NULL;

	hstore_oid = get_hstore_oid(newvalue);

	if (*newvalue && !OidIsValid(hstore_oid))
		return NULL;

	if (doit)
		set_hstore_parsers(hstore_oid);

	return newvalue;
}

void
hstore_plpython_init(void)
{
	DefineCustomStringVariable("plpython.hstore",
	  "The fully qualified name of the hstore type.",
							   NULL,
							   &hstore_name,
							   NULL,
							   PGC_SUSET,
							   0,
							   recheck_hstore_oid,
							   NULL);

	EmitWarningsOnPlaceholders("plpython");

	previous = InvalidOid;

	if (hstore_name && *hstore_name)
		recheck_hstore_oid(hstore_name, true, PGC_S_FILE);
}

static Oid
get_hstore_oid(const char *name)
{
	text		*text_name;
	List		*hstore_name;
	char		*type_name;
	Oid			 type_namespace;
	Oid			 typoid;

	Assert(name != NULL);

	if (!(*name))
		return InvalidOid;

	text_name = cstring_to_text(name);
	hstore_name = textToQualifiedNameList(text_name);
	pfree(text_name);

	type_namespace = QualifiedNameGetCreationNamespace(hstore_name, &type_name);

	typoid = GetSysCacheOid2(TYPENAMENSP,
							 CStringGetDatum(type_name),
							 ObjectIdGetDatum(type_namespace));

	return typoid;
}

static void
set_hstore_parsers(Oid hstore_oid)
{
	char		 name[NAMEDATALEN];

	if (OidIsValid(previous))
	{
		snprintf(name, NAMEDATALEN, PARSERS_VARIABLE_PATTERN, previous);
		*find_rendezvous_variable(name) = NULL;
	}

	if (OidIsValid(hstore_oid))
	{
		snprintf(name, NAMEDATALEN, PARSERS_VARIABLE_PATTERN, hstore_oid);
		*find_rendezvous_variable(name) = &parsers;
		previous = hstore_oid;
	}
}

#else	/* !defined(HSTORE_PLPYTHON_SUPPORT) */

void
hstore_plpython_init(void) {};

#endif	/* defined(HSTORE_PLPYTHON_SUPPORT) */
