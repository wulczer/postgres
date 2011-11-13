/*-------------------------------------------------------------------------
 *
 * plpython.h - Python as a procedural language for PostgreSQL
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/pl/plpython/plpython.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLPYTHON_H
#define PLPYTHON_H

#include "access/htup.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "storage/itemptr.h"
#include "utils/hsearch.h"
#include "utils/palloc.h"
#include "utils/resowner.h"

/* Python imports and compatibility between Python versions */
#include "plpython_port.h"

/* define our text domain for translations */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("plpython")


/*----------------------
 * exported functions
 *----------------------
 */

#if PY_MAJOR_VERSION >= 3
/* Use separate names to avoid clash in pg_pltemplate */
#define plpython_validator plpython3_validator
#define plpython_call_handler plpython3_call_handler
#define plpython_inline_handler plpython3_inline_handler
#endif

extern void _PG_init(void);
extern Datum plpython_validator(PG_FUNCTION_ARGS);
extern Datum plpython_call_handler(PG_FUNCTION_ARGS);
extern Datum plpython_inline_handler(PG_FUNCTION_ARGS);

#if PY_MAJOR_VERSION < 3
/* Define aliases plpython2_call_handler etc */
extern Datum plpython2_validator(PG_FUNCTION_ARGS);
extern Datum plpython2_call_handler(PG_FUNCTION_ARGS);
extern Datum plpython2_inline_handler(PG_FUNCTION_ARGS);
#endif


/*--------------------------
 * common utility functions
 *--------------------------
 */

extern void *PLy_malloc(size_t);
extern void *PLy_malloc0(size_t);
extern char *PLy_strdup(const char *);
extern void PLy_free(void *);

extern PyObject *PLyUnicode_Bytes(PyObject *unicode);
extern char *PLyUnicode_AsString(PyObject *unicode);

#if PY_MAJOR_VERSION >= 3
extern PyObject *PLyUnicode_FromString(const char *s);
#endif

extern void PLy_exception_set(PyObject *, const char *,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)));

extern void PLy_exception_set_plural(PyObject *, const char *, const char *,
									 unsigned long n,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 5)))
__attribute__((format(PG_PRINTF_ATTRIBUTE, 3, 5)));


/*----------------------
 * plpython_io.c
 *----------------------
 */

struct PLyDatumToOb;
typedef PyObject *(*PLyDatumToObFunc) (struct PLyDatumToOb *, Datum);

typedef struct PLyDatumToOb
{
	PLyDatumToObFunc func;
	FmgrInfo	typfunc;		/* The type's output function */
	Oid			typoid;			/* The OID of the type */
	int32		typmod;			/* The typmod of the type */
	Oid			typioparam;
	bool		typbyval;
	int16		typlen;
	char		typalign;
	struct PLyDatumToOb *elm;
} PLyDatumToOb;

typedef struct PLyTupleToOb
{
	PLyDatumToOb *atts;
	int			natts;
} PLyTupleToOb;

typedef union PLyTypeInput
{
	PLyDatumToOb d;
	PLyTupleToOb r;
} PLyTypeInput;

/* convert PyObject to a Postgresql Datum or tuple.
 * output from Python
 */
struct PLyObToDatum;
typedef Datum (*PLyObToDatumFunc) (struct PLyObToDatum *, int32, PyObject *);

typedef struct PLyObToDatum
{
	PLyObToDatumFunc func;
	FmgrInfo	typfunc;		/* The type's input function */
	Oid			typoid;			/* The OID of the type */
	int32		typmod;			/* The typmod of the type */
	Oid			typioparam;
	bool		typbyval;
	int16		typlen;
	char		typalign;
	struct PLyObToDatum *elm;
} PLyObToDatum;

typedef struct PLyObToTuple
{
	PLyObToDatum *atts;
	int			natts;
} PLyObToTuple;

typedef union PLyTypeOutput
{
	PLyObToDatum d;
	PLyObToTuple r;
} PLyTypeOutput;

/* all we need to move Postgresql data to Python objects,
 * and vice versa
 */
typedef struct PLyTypeInfo
{
	PLyTypeInput in;
	PLyTypeOutput out;

	/*
	 * is_rowtype can be: -1 = not known yet (initial state); 0 = scalar
	 * datatype; 1 = rowtype; 2 = rowtype, but I/O functions not set up yet
	 */
	int			is_rowtype;
	/* used to check if the type has been modified */
	Oid			typ_relid;
	TransactionId typrel_xmin;
	ItemPointerData typrel_tid;
} PLyTypeInfo;

extern void PLy_typeinfo_init(PLyTypeInfo *);
extern void PLy_typeinfo_dealloc(PLyTypeInfo *);

extern void PLy_input_datum_func(PLyTypeInfo *, Oid, HeapTuple);
extern void PLy_output_datum_func(PLyTypeInfo *, HeapTuple);

extern void PLy_input_tuple_funcs(PLyTypeInfo *, TupleDesc);
extern void PLy_output_tuple_funcs(PLyTypeInfo *, TupleDesc);

extern void PLy_output_record_funcs(PLyTypeInfo *, TupleDesc);

/* conversion from Python objects to heap tuples */
extern HeapTuple PLyObject_ToTuple(PLyTypeInfo *, TupleDesc, PyObject *);

/* conversion from heap tuples to Python dictionaries */
extern PyObject *PLyDict_FromTuple(PLyTypeInfo *, HeapTuple, TupleDesc);


/*----------------------
 * plpython_procedure.c
 *----------------------
 */

/* cached procedure data */
typedef struct PLyProcedure
{
	char	   *proname;		/* SQL name of procedure */
	char	   *pyname;			/* Python name of procedure */
	TransactionId fn_xmin;
	ItemPointerData fn_tid;
	bool		fn_readonly;
	PLyTypeInfo result;			/* also used to store info for trigger tuple
								 * type */
	bool		is_setof;		/* true, if procedure returns result set */
	PyObject   *setof;			/* contents of result set. */
	char	   *src;			/* textual procedure code, after mangling */
	char	  **argnames;		/* Argument names */
	PLyTypeInfo args[FUNC_MAX_ARGS];
	int			nargs;
	PyObject   *code;			/* compiled procedure code */
	PyObject   *statics;		/* data saved across calls, local scope */
	PyObject   *globals;		/* data saved across calls, global scope */
} PLyProcedure;

/* the procedure cache entry */
typedef struct PLyProcedureEntry
{
	Oid			fn_oid;			/* hash key */
	PLyProcedure *proc;
} PLyProcedureEntry;

/* PLyProcedure manipulation */
extern char *PLy_procedure_name(PLyProcedure *);
extern PLyProcedure *PLy_procedure_get(Oid, bool);
extern void PLy_procedure_compile(PLyProcedure *, const char *);
extern void PLy_procedure_delete(PLyProcedure *);


/*----------------------
 * plpython_exec.c
 *----------------------
 */

extern Datum PLy_function_handler(FunctionCallInfo, PLyProcedure *);
extern HeapTuple PLy_trigger_handler(FunctionCallInfo, PLyProcedure *);


/*----------------------
 * plpython_plpy.c
 *----------------------
 */

typedef struct PLyExceptionEntry
{
	int			sqlstate;		/* hash key, must be first */
	PyObject   *exc;			/* corresponding exception */
} PLyExceptionEntry;

#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC PyInit_plpy(void);
#endif
extern void PLy_init_interp(void);
extern void PLy_init_plpy(void);


/*----------------------
 * plpython_spi.c
 *----------------------
 */

extern PyObject *PLy_spi_prepare(PyObject *, PyObject *);
extern PyObject *PLy_spi_execute(PyObject *, PyObject *);


/*----------------------
 * plpython_result.c
 *----------------------
 */

typedef struct PLyResultObject
{
	PyObject_HEAD
	/* HeapTuple *tuples; */
	PyObject   *nrows;			/* number of rows returned by query */
	PyObject   *rows;			/* data rows, or None if no data returned */
	PyObject   *status;			/* query status, SPI_OK_*, or SPI_ERR_* */
} PLyResultObject;

extern void PLy_result_init_type(void);
extern PyObject *PLy_result_new(void);


/*----------------------
 * plpython_plan.c
 *----------------------
 */

typedef struct PLyPlanObject
{
	PyObject_HEAD
	SPIPlanPtr	plan;
	int			nargs;
	Oid		   *types;
	Datum	   *values;
	PLyTypeInfo *args;
} PLyPlanObject;

extern void PLy_plan_init_type(void);
extern PyObject *PLy_plan_new(void);
extern bool is_PLyPlanObject(PyObject *);


/*---------------------------
 * plpython_subtransaction.c
 *---------------------------
 */

typedef struct PLySubtransactionObject
{
	PyObject_HEAD
	bool		started;
	bool		exited;
} PLySubtransactionObject;

/* explicit subtransaction data */
typedef struct PLySubtransactionData
{
	MemoryContext oldcontext;
	ResourceOwner oldowner;
} PLySubtransactionData;

extern void PLy_subtransaction_init_type(void);
extern PyObject *PLy_subtransaction_new(PyObject *, PyObject *);


/*----------------------
 * plpython_functions.c
 *----------------------
 */

extern PyObject *PLy_debug(PyObject *, PyObject *);
extern PyObject *PLy_log(PyObject *, PyObject *);
extern PyObject *PLy_info(PyObject *, PyObject *);
extern PyObject *PLy_notice(PyObject *, PyObject *);
extern PyObject *PLy_warning(PyObject *, PyObject *);
extern PyObject *PLy_error(PyObject *, PyObject *);
extern PyObject *PLy_fatal(PyObject *, PyObject *);
extern PyObject *PLy_quote_literal(PyObject *, PyObject *);
extern PyObject *PLy_quote_nullable(PyObject *, PyObject *);
extern PyObject *PLy_quote_ident(PyObject *, PyObject *);


/*----------------------
 * plpython_elog.c
 *----------------------
 */

extern void PLy_elog(int, const char *,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)));


/*----------------------
 * global variables
 *----------------------
 */

/* the interpreter's globals dict */
extern PyObject *PLy_interp_globals;

/* a list of nested explicit subtransactions */
extern List *explicit_subtransactions;

/* global exception classes */
extern PyObject *PLy_exc_error;
extern PyObject *PLy_exc_fatal;
extern PyObject *PLy_exc_spi_error;

/* the procedure hash (separate for functions and triggers) */
extern HTAB *PLy_procedure_cache;
extern HTAB *PLy_trigger_cache;

/* A hash table mapping sqlstates to exceptions, for speedy lookup */
extern HTAB *PLy_spi_exceptions;

/* currently active plpython function */
extern PLyProcedure *PLy_curr_procedure;

#endif   /* PLPYTHON_H */
