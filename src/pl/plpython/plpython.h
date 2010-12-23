/*
 * src/pl/plpython/plpython.h
 */
#ifndef __PLPYTHON_H__
#define __PLPYTHON_H__



/*
 * Rendezvous variable pattern for parsers exported from other extensions
 *
 * An extension providing parsres for type X should look up the type's OID and
 * set a rendezvous variable using this pattern that points to a PLyParsers
 * structure. PL/Python will then use these parsers for arguments with that
 * OID.
 */
#define PARSERS_VARIABLE_PATTERN "plpython_%u_parsers"

/*
 * Types for parsres functions that other modules can export to transform
 * Datums into PyObjects and back. The types need to be compatible with
 * PLyObToDatumFunc and PLyDatumToObFunc, but we don't want to expose too much
 * of plpython.c's guts here, so the first arguments is mandated to be a void
 * pointer that should not be touched. An extension should know exactly what
 * it's dealing with, so there's no need for it to look at anything contained
 * in PLyTypeInfo, which is what gets passed here.
 *
 * The output parser also gets the type's typmod, which might actually be
 * useful.
 */
typedef PyObject *(*PLyParserIn) (void *, Datum);
typedef Datum (*PLyParserOut) (void *, int32, PyObject *);

typedef struct PLyParsers
{
	PLyParserIn			in;
	PLyParserOut		out;
} PLyParsers;

#endif   /* __PLPYTHON_H__ */
