/*
 * PL/R - PostgreSQL support for R as a
 *	      procedural language (PL)
 *
 * Copyright (c) 2003 by Joseph E. Conway
 * ALL RIGHTS RESERVED
 * 
 * Joe Conway <mail@joeconway.com>
 * 
 * Based on pltcl by Jan Wieck
 * and inspired by REmbeddedPostgres by
 * Duncan Temple Lang <duncan@research.bell-labs.com>
 * http://www.omegahat.org/RSPostgres/
 *
 * License: GPL version 2 or newer. http://www.gnu.org/copyleft/gpl.html
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * pg_conversion.c - functions for converting arguments from pg types to
 *                   R types, and for converting return values from R types
 *                   to pg types
 */
#include "plr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "catalog/catversion.h"

static void pg_get_one_r(char *value, Oid arg_out_fn_oid, SEXP *obj, int elnum);
static SEXP get_r_vector(Oid typtype, int numels);
static Datum get_tuplestore(SEXP rval, plr_function *function, FunctionCallInfo fcinfo, bool *isnull);
static Datum get_array_datum(SEXP rval, plr_function *function, bool *isnull);
static Datum get_frame_array_datum(SEXP rval, plr_function *function, bool *isnull);
static Datum get_md_array_datum(SEXP rval, int ndims, plr_function *function, bool *isnull);
static Datum get_generic_array_datum(SEXP rval, plr_function *function, bool *isnull);
static Tuplestorestate *get_frame_tuplestore(SEXP rval,
											 plr_function *function,
											 AttInMetadata *attinmeta,
											 MemoryContext per_query_ctx,
											 bool retset);
static Tuplestorestate *get_matrix_tuplestore(SEXP rval,
											 plr_function *function,
											 AttInMetadata *attinmeta,
											 MemoryContext per_query_ctx,
											 bool retset);
static Tuplestorestate *get_generic_tuplestore(SEXP rval,
											 plr_function *function,
											 AttInMetadata *attinmeta,
											 MemoryContext per_query_ctx,
											 bool retset);

/*
 * given a scalar pg value, convert to a one row R vector
 */
SEXP
pg_scalar_get_r(Datum dvalue, Oid arg_typid, FmgrInfo arg_out_func)
{
	SEXP		result;
	char	   *value;

	value = DatumGetCString(FunctionCall3(&arg_out_func,
										  dvalue,
							 			  (Datum) 0,
										  Int32GetDatum(-1)));

	if (value != NULL)
	{
		/* get new vector of the appropriate type, length 1 */
		PROTECT(result = get_r_vector(arg_typid, 1));

		/* add our value to it */
		pg_get_one_r(value, arg_typid, &result, 0);
	}
	else
	{
		PROTECT(result = NEW_CHARACTER(1));
		SET_STRING_ELT(result, 0, NA_STRING);
	}

	UNPROTECT(1);

	return result;
}


/*
 * given an array pg value, convert to a multi-row R vector
 */
SEXP
pg_array_get_r(Datum dvalue, FmgrInfo out_func, int typlen, bool typbyval, char typalign)
{
	/*
	 * Loop through and convert each scalar value.
	 * Use the converted values to build an R vector.
	 */
	SEXP		result;
	char	   *value;
	ArrayType  *v = (ArrayType *) dvalue;
	Oid			element_type;
	int			i, j, k,
				nitems,
				nr = 1,
				nc = 1,
				nz = 1,
				ndim,
			   *dim;
	char	   *p;

	ndim = ARR_NDIM(v);
	element_type = ARR_ELEMTYPE(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	/* pass an NA if the array is empty */
	if (nitems == 0)
	{
		PROTECT(result = NEW_CHARACTER(1));
		SET_STRING_ELT(result, 0, NA_STRING);
		UNPROTECT(1);

		return result;
	}

	if (ndim == 1)
		nr = nitems;
	else if (ndim == 2)
	{
		nr = dim[0];
		nc = dim[1];
	}
	else if (ndim == 3)
	{
		nr = dim[0];
		nc = dim[1];
		nz = dim[2];
	}
	else
		elog(ERROR, "plr: 4 (or more) dimension arrays are not yet supported as function arguments");

	/* get new vector of the appropriate type and length */
	PROTECT(result = get_r_vector(element_type, nitems));

	/* Convert all values to their R form and build the vector */
	p = ARR_DATA_PTR(v);
	for (i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
		{
			for (k = 0; k < nz; k++)
			{
				Datum		itemvalue;
				int			idx = (k * nr * nc) + (j * nr) + i;

				itemvalue = fetch_att(p, typbyval, typlen);
				value = DatumGetCString(FunctionCall3(&out_func,
														  itemvalue,
														  (Datum) 0,
														  Int32GetDatum(-1)));
				p = att_addlength(p, typlen, PointerGetDatum(p));
				p = (char *) att_align(p, typalign);

				if (value != NULL)
					pg_get_one_r(value, element_type, &result, idx);
				else
					SET_STRING_ELT(result, idx, NA_STRING);
			}
		}
	}
	UNPROTECT(1);

	if (ndim > 1)
	{
		SEXP	matrix_dims;

		/* attach dimensions */
		PROTECT(matrix_dims = allocVector(INTSXP, ndim));
		for (i = 0; i < ndim; i++)
			INTEGER_DATA(matrix_dims)[i] = dim[i];

		setAttrib(result, R_DimSymbol, matrix_dims);
		UNPROTECT(1);
	}

	return result;
}

/*
 * given an array of pg tuples, convert to an R data.frame
 */
SEXP
pg_tuple_get_r_frame(int ntuples, HeapTuple *tuples, TupleDesc tupdesc)
{
	int			nr = ntuples;
	int			nc = tupdesc->natts;
	int			i = 0;
	int			j = 0;
	Oid			element_type;
	Oid			typelem;
	SEXP		names;
	SEXP		row_names;
	char		buf[256];
	SEXP		result;
	SEXP		fldvec;

	if (tuples == NULL || ntuples < 1)
		return(R_NilValue);

	/*
	 * Allocate the data.frame initially as a list,
	 * and also allocate a names vector for the column names
	 */
	PROTECT(result = NEW_LIST(nc));
    PROTECT(names = NEW_CHARACTER(nc));

	/*
	 * Loop by columns
	 */
	for (j = 0; j < nc; j++)		
	{
		int16		typlen;
		bool		typbyval;
		char		typdelim;
		Oid			typoutput,
					elemtypelem;
		FmgrInfo	outputproc;
		char		typalign;

		/* set column name */
		SET_STRING_ELT(names, j,  mkChar(SPI_fname(tupdesc, j + 1)));

		/* get column datatype oid */
		element_type = SPI_gettypeid(tupdesc, j + 1);

		/* special case -- NAME looks like an array, but treat as a scalar */
		if (element_type == NAMEOID)
			typelem = 0;
		else
			/* check to see it it is an array type */
			typelem = get_element_type(element_type);

		/* get new vector of the appropriate type and length */
		if (typelem == 0)
			PROTECT(fldvec = get_r_vector(element_type, nr));
		else
		{
			PROTECT(fldvec = NEW_LIST(nr));
			get_type_io_data(typelem, IOFunc_output, &typlen, &typbyval,
							 &typalign, &typdelim, &elemtypelem, &typoutput);

			fmgr_info(typoutput, &outputproc);
		}

		/* loop rows for this column */
		for (i = 0; i < nr; i++)
		{
			if (typelem == 0)
			{
				/* not an array type */
				char	   *value;

				value = SPI_getvalue(tuples[i], tupdesc, j + 1);
				if (value != NULL)
					pg_get_one_r(value, element_type, &fldvec, i);
				else
					SET_STRING_ELT(fldvec, i, NA_STRING);
			}
			else
			{
				/* array type */
				Datum		dvalue;
				bool		isnull;
				SEXP		fldvec_elem;

				dvalue = SPI_getbinval(tuples[i], tupdesc, j + 1, &isnull);
				if (!isnull)
					PROTECT(fldvec_elem = pg_array_get_r(dvalue, outputproc, typlen, typbyval, typalign));
				else
				{
					PROTECT(fldvec_elem = NEW_CHARACTER(1));
					SET_STRING_ELT(fldvec_elem, 0, NA_STRING);
				}

				SET_VECTOR_ELT(fldvec, i, fldvec_elem);
				UNPROTECT(1);
			}
		}

		SET_VECTOR_ELT(result, j, fldvec);
		UNPROTECT(1);
	}

	/* attach the column names */
    setAttrib(result, R_NamesSymbol, names);

	/* attach row names - basically just the row number, zero based */
	PROTECT(row_names = allocVector(STRSXP, nr));
	for (i=0; i<nr; i++)
	{
	    sprintf(buf, "%d", i+1);
	    SET_STRING_ELT(row_names, i, COPY_TO_USER_STRING(buf));
	}
	setAttrib(result, R_RowNamesSymbol, row_names);

	/* finally, tell R we are a "data.frame" */
    setAttrib(result, R_ClassSymbol, mkString("data.frame"));

	UNPROTECT(3);
	return result;
}

/*
 * create an R vector of a given type and size based on pg output function oid
 */
static SEXP
get_r_vector(Oid typtype, int numels)
{
	SEXP	result;

	switch (typtype)
	{
		case INT2OID:
		case INT4OID:
			/* 2 and 4 byte integer pgsql datatype => use R INTEGER */
			PROTECT(result = NEW_INTEGER(numels));
		    break;
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case CASHOID:
		case NUMERICOID:
			/*
			 * Other numeric types => use R REAL
			 * Note pgsql int8 is mapped to R REAL
			 * because R INTEGER is only 4 byte
			 */
			PROTECT(result = NEW_NUMERIC(numels));
		    break;
		case BOOLOID:
			PROTECT(result = NEW_LOGICAL(numels));
		    break;
		default:
			/* Everything else is defaulted to string */
			PROTECT(result = NEW_CHARACTER(numels));
	}
	UNPROTECT(1);

	return result;
}

/*
 * given a single non-array pg value, convert to its R value representation
 */
static void
pg_get_one_r(char *value, Oid typtype, SEXP *obj, int elnum)
{
	switch (typtype)
	{
		case INT2OID:
		case INT4OID:
			/* 2 and 4 byte integer pgsql datatype => use R INTEGER */
			INTEGER_DATA(*obj)[elnum] = atoi(value);
		    break;
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case CASHOID:
		case NUMERICOID:
			/*
			 * Other numeric types => use R REAL
			 * Note pgsql int8 is mapped to R REAL
			 * because R INTEGER is only 4 byte
			 */
			NUMERIC_DATA(*obj)[elnum] = atof(value);
		    break;
		case BOOLOID:
			LOGICAL_DATA(*obj)[elnum] = ((*value == 't') ? 1 : 0);
		    break;
		default:
			/* Everything else is defaulted to string */
			SET_STRING_ELT(*obj, elnum, COPY_TO_USER_STRING(value));
	}
}

/*
 * given an R value, convert to its pg representation
 */
Datum
r_get_pg(SEXP rval, plr_function *function, FunctionCallInfo fcinfo)
{
	bool	isnull = false;
	Datum	result;

	if (function->result_istuple || fcinfo->flinfo->fn_retset)
		result = get_tuplestore(rval, function, fcinfo, &isnull);
	else
	{
		/* short circuit if return value is Null */
		if (rval == R_NilValue ||
			isNull(rval) ||
			length(rval) == 0)		/* probably redundant */
		{
			fcinfo->isnull = true;
			return (Datum) 0;
		}

		if (function->result_elem == 0)
			result = get_scalar_datum(rval, function->result_in_func, function->result_elem, &isnull);
		else
			result = get_array_datum(rval, function, &isnull);

	}

	if (isnull)
		fcinfo->isnull = true;

	return result;
}

static Datum
get_tuplestore(SEXP rval, plr_function *function, FunctionCallInfo fcinfo, bool *isnull)
{
	bool			retset = fcinfo->flinfo->fn_retset;
	ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc		tupdesc;
	AttInMetadata  *attinmeta;
	MemoryContext	per_query_ctx;
	MemoryContext	oldcontext;
	int				nc;

	/* check to see if caller supports us returning a tuplestore */
	if (!rsinfo || !(rsinfo->allowedModes & SFRM_Materialize))
		elog(ERROR, "plr: Materialize mode required, but it is not "
			 "allowed in this context");

	if (isFrame(rval))
		nc = length(rval);
	else if (isMatrix(rval))
		nc = ncols(rval);
	else
		nc = 1;

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* get the requested return tuple description */
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);

	/*
	 * Check to make sure we have the same number of columns
	 * to return as there are attributes in the return tuple.
	 *
	 * Note we will attempt to coerce the R values into whatever
	 * the return attribute type is and depend on the "in"
	 * function to complain if needed.
	 */
	if (nc != tupdesc->natts)
		elog(ERROR, "plr: Query-specified return tuple and " \
					"function returned data.frame are not compatible");

	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	/* OK, go to work */
	rsinfo->returnMode = SFRM_Materialize;

	if (isFrame(rval))
		rsinfo->setResult = get_frame_tuplestore(rval, function, attinmeta, per_query_ctx, retset);
	else if (isMatrix(rval))
		rsinfo->setResult = get_matrix_tuplestore(rval, function, attinmeta, per_query_ctx, retset);
	else
		rsinfo->setResult = get_generic_tuplestore(rval, function, attinmeta, per_query_ctx, retset);

	/*
	 * SFRM_Materialize mode expects us to return a NULL Datum. The actual
	 * tuples are in our tuplestore and passed back through
	 * rsinfo->setResult. rsinfo->setDesc is set to the tuple description
	 * that we actually used to build our tuples with, so the caller can
	 * verify we did what it was expecting.
	 */
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	*isnull = true;
	return (Datum) 0;
}

Datum
get_scalar_datum(SEXP rval, FmgrInfo result_in_func, Oid result_elem, bool *isnull)
{
	Datum		dvalue;
	SEXP		obj;
	char	   *value;

	/*
	 * if the element type is zero, we don't have an array,
	 * so coerce to string and take the first element as a scalar
	 */
	PROTECT(obj = AS_CHARACTER(rval));
	value = CHAR(STRING_ELT(obj, 0));

	if (STRING_ELT(obj, 0) == NA_STRING)
	{
		*isnull = true;
		dvalue = (Datum) 0;
	}
	else if (value != NULL)
	{
		dvalue = FunctionCall3(&result_in_func,
								CStringGetDatum(value),
								ObjectIdGetDatum(result_elem),
								Int32GetDatum(-1));
	}
	else
	{
		*isnull = true;
		dvalue = (Datum) 0;
	}

	UNPROTECT(1);

	return dvalue;
}

static Datum
get_array_datum(SEXP rval, plr_function *function, bool *isnull)
{
	SEXP	rdims;
	int		ndims;

	/* two supported special cases */
	if (isFrame(rval))
		return get_frame_array_datum(rval, function, isnull);
	else if (isMatrix(rval))
		return get_md_array_datum(rval, 2 /* matrix is 2D */, function, isnull);

	PROTECT(rdims = getAttrib(rval, R_DimSymbol));
	ndims = length(rdims);
	UNPROTECT(1);

	/* 2D and 3D arrays are specifically supported too */
	if (ndims == 2 || ndims == 3)
		return get_md_array_datum(rval, ndims, function, isnull);

	/* everything else */
	return get_generic_array_datum(rval, function, isnull);
}

static Datum
get_frame_array_datum(SEXP rval, plr_function *function, bool *isnull)
{
	Datum		dvalue;
	SEXP		obj;
	char	   *value;
	Oid			result_elem = function->result_elem;
	FmgrInfo	in_func = function->result_elem_in_func;
	int			typlen = function->result_elem_typlen;
	bool		typbyval = function->result_elem_typbyval;
	char		typalign = function->result_elem_typalign;
	int			i;
	Datum	   *dvalues = NULL;
	ArrayType  *array;
	int			nr = 0;
	int			nc = length(rval);
	int			ndims = 2;
	int			dims[ndims];
	int			lbs[ndims];
	int			idx;
	SEXP		dfcol = NULL;
	int			j;

	for (j = 0; j < nc; j++)
	{
		if (TYPEOF(rval) == VECSXP)
			PROTECT(dfcol = VECTOR_ELT(rval, j));
		else if (TYPEOF(rval) == LISTSXP)
		{
			PROTECT(dfcol = CAR(rval));
			rval = CDR(rval);
		}
		else
			elog(ERROR, "plr: bad internal representation of data.frame");

		if (ATTRIB(dfcol) == R_NilValue)
			PROTECT(obj = AS_CHARACTER(dfcol));
		else
			PROTECT(obj = AS_CHARACTER(CAR(ATTRIB(dfcol))));

		if (j == 0)
		{
			nr = length(obj);
			dvalues = (Datum *) palloc(nr * nc * sizeof(Datum));
		}

		for(i = 0; i < nr; i++)
		{
			value = CHAR(STRING_ELT(obj, i));
			idx = ((i * nc) + j);

			if (STRING_ELT(obj, i) == NA_STRING || value == NULL)
				elog(ERROR, "plr: cannot return array with NULL elements");
			else
				dvalues[idx] = FunctionCall3(&in_func,
										CStringGetDatum(value),
										(Datum) 0,
										Int32GetDatum(-1));
	    }
		UNPROTECT(2);
	}

	dims[0] = nr;
	dims[1] = nc;
	lbs[0] = 1;
	lbs[1] = 1;

	array = construct_md_array(dvalues, ndims, dims, lbs,
								result_elem, typlen, typbyval, typalign);

	dvalue = PointerGetDatum(array);

	return dvalue;
}

static Datum
get_md_array_datum(SEXP rval, int ndims, plr_function *function, bool *isnull)
{
	Datum		dvalue;
	SEXP		obj;
	SEXP		rdims;
	char	   *value;
	Oid			result_elem = function->result_elem;
	FmgrInfo	in_func = function->result_elem_in_func;
	int			typlen = function->result_elem_typlen;
	bool		typbyval = function->result_elem_typbyval;
	char		typalign = function->result_elem_typalign;
	int			i, j, k;
	Datum	   *dvalues = NULL;
	ArrayType  *array;
	int			nitems;
	int			nr = 1;
	int			nc = 1;
	int			nz = 1;
	int			dims[ndims];
	int			lbs[ndims];
	int			idx;
	int			cntr = 0;

	PROTECT(rdims = getAttrib(rval, R_DimSymbol));
	for(i = 0; i < ndims; i++)
	{
		dims[i] = INTEGER(rdims)[i];
		lbs[i] = 1;

		switch (i)
		{
			case 0:
				nr = dims[i];
			    break;
			case 1:
				nc = dims[i];
			    break;
			case 2:
				nz = dims[i];
			    break;
			default:
				/* anything higher is currently unsupported */
				elog(ERROR, "plr: returning arrays of greater than 3 " \
							"dimensions is currently not supported");
		}

	}
	UNPROTECT(1);

	nitems = nr * nc * nz;
	dvalues = (Datum *) palloc(nitems * sizeof(Datum));
	PROTECT(obj =  AS_CHARACTER(rval));

	for (i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
		{
			for (k = 0; k < nz; k++)
			{
				idx = (k * nr * nc) + (j * nr) + i;
				value = CHAR(STRING_ELT(obj, idx));

				if (STRING_ELT(obj, idx) == NA_STRING || value == NULL)
					elog(ERROR, "plr: cannot return array with NULL elements");
				else
					dvalues[cntr++] = FunctionCall3(&in_func,
											CStringGetDatum(value),
											(Datum) 0,
											Int32GetDatum(-1));
			}
		}
	}
	UNPROTECT(1);

	array = construct_md_array(dvalues, ndims, dims, lbs,
								result_elem, typlen, typbyval, typalign);

	dvalue = PointerGetDatum(array);

	return dvalue;
}

static Datum
get_generic_array_datum(SEXP rval, plr_function *function, bool *isnull)
{
	int			objlen = length(rval);
	Datum		dvalue;
	SEXP		obj;
	char	   *value;
	Oid			result_elem = function->result_elem;
	FmgrInfo	in_func = function->result_elem_in_func;
	int			typlen = function->result_elem_typlen;
	bool		typbyval = function->result_elem_typbyval;
	char		typalign = function->result_elem_typalign;
	int			i;
	Datum	   *dvalues = NULL;
	ArrayType  *array;
	int			ndims = 1;
	int			dims[ndims];
	int			lbs[ndims];

	dvalues = (Datum *) palloc(objlen * sizeof(Datum));
	PROTECT(obj =  AS_CHARACTER(rval));

	/* Loop is needed here as result value might be of length > 1 */
	for(i = 0; i < objlen; i++)
	{
		value = CHAR(STRING_ELT(obj, i));

		if (STRING_ELT(obj, i) == NA_STRING || value == NULL)
			elog(ERROR, "plr: cannot return array with NULL elements");
		else
			dvalues[i] = FunctionCall3(&in_func,
									CStringGetDatum(value),
									(Datum) 0,
									Int32GetDatum(-1));
    }
	UNPROTECT(1);

	dims[0] = objlen;
	lbs[0] = 1;

	array = construct_md_array(dvalues, ndims, dims, lbs,
								result_elem, typlen, typbyval, typalign);

	dvalue = PointerGetDatum(array);

	return dvalue;
}

static Tuplestorestate *
get_frame_tuplestore(SEXP rval,
					 plr_function *function,
					 AttInMetadata *attinmeta,
					 MemoryContext per_query_ctx,
					 bool retset)
{
	Tuplestorestate	   *tupstore;
	char			  **values;
	HeapTuple			tuple;
	MemoryContext		oldcontext;
	int					i, j;
	int					nr = 0;
	int					nc = length(rval);
	SEXP				dfcol;
	SEXP				result;

	/* switch to appropriate context to create the tuple store */
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* initialize our tuplestore */
	tupstore = TUPLESTORE_BEGIN_HEAP;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * If we return a set, get number of rows by examining the first column.
	 * Otherwise, stop at one row.
	 */
	if (retset)
	{
		PROTECT(dfcol = VECTOR_ELT(rval, 0));
		nr = length(dfcol);
		UNPROTECT(1);
	}
	else
		nr = 1;

	/* coerce columns to character in advance */
	PROTECT(result = NEW_LIST(nc));
	for (j = 0; j < nc; j++)
	{
		PROTECT(dfcol = VECTOR_ELT(rval, j));
		if(!isFactor(dfcol))
		{
			SEXP	obj;

			PROTECT(obj = AS_CHARACTER(dfcol));
			SET_VECTOR_ELT(result, j, obj);
			UNPROTECT(1);
		}
		else
		{
			SEXP 	t;

			for (t = ATTRIB(dfcol); t != R_NilValue; t = CDR(t))
			{
				if(TAG(t) == R_LevelsSymbol)
				{
					PROTECT(SETCAR(t, AS_CHARACTER(CAR(t))));
					UNPROTECT(1);
					break;
				}
			}
			SET_VECTOR_ELT(result, j, dfcol);
		}


		UNPROTECT(1);
	}

	values = (char **) palloc(nc * sizeof(char *));

	for(i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
		{
			PROTECT(dfcol = VECTOR_ELT(result, j));

			if(isFactor(dfcol))
			{
				SEXP t;
				for (t = ATTRIB(dfcol); t != R_NilValue; t = CDR(t))
				{
					if(TAG(t) == R_LevelsSymbol)
					{
						SEXP	obj;
						int		idx = (int) VECTOR_ELT(dfcol, i);

						PROTECT(obj = CAR(t));
						values[j] = pstrdup(CHAR(STRING_ELT(obj, idx - 1)));
						UNPROTECT(1);

						break;
					}
				}
			}
			else
			{
				if (STRING_ELT(dfcol, 0) != NA_STRING)
					values[j] = pstrdup(CHAR(STRING_ELT(dfcol, i)));
				else
					values[j] = NULL;
			}

			UNPROTECT(1);
		}

		/* construct the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* switch to appropriate context while storing the tuple */
		oldcontext = MemoryContextSwitchTo(per_query_ctx);

		/* now store it */
		tuplestore_puttuple(tupstore, tuple);

		/* now reset the context */
		MemoryContextSwitchTo(oldcontext);

		for (j = 0; j < nc; j++)
			if (values[j] != NULL)
				pfree(values[j]);
    }

	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tuplestore_donestoring(tupstore);
	MemoryContextSwitchTo(oldcontext);

	return tupstore;
}

static Tuplestorestate *
get_matrix_tuplestore(SEXP rval,
					 plr_function *function,
					 AttInMetadata *attinmeta,
					 MemoryContext per_query_ctx,
					 bool retset)
{
	Tuplestorestate	   *tupstore;
	char			  **values;
	HeapTuple			tuple;
	MemoryContext		oldcontext;
	SEXP				obj;
	int					i, j;
	int					nr;
	int					nc = ncols(rval);

	/* switch to appropriate context to create the tuple store */
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/*
	 * If we return a set, get number of rows.
	 * Otherwise, stop at one row.
	 */
	if (retset)
		nr = nrows(rval);
	else
		nr = 1;

	/* initialize our tuplestore */
	tupstore = TUPLESTORE_BEGIN_HEAP;

	MemoryContextSwitchTo(oldcontext);

	values = (char **) palloc(nc * sizeof(char *));

	PROTECT(obj =  AS_CHARACTER(rval));
	for(i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
			values[j] = CHAR(STRING_ELT(obj, (j * nr) + i));

		/* construct the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* switch to appropriate context while storing the tuple */
		oldcontext = MemoryContextSwitchTo(per_query_ctx);

		/* now store it */
		tuplestore_puttuple(tupstore, tuple);

		/* now reset the context */
		MemoryContextSwitchTo(oldcontext);
    }
	UNPROTECT(1);

	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tuplestore_donestoring(tupstore);
	MemoryContextSwitchTo(oldcontext);

	return tupstore;
}

static Tuplestorestate *
get_generic_tuplestore(SEXP rval,
					 plr_function *function,
					 AttInMetadata *attinmeta,
					 MemoryContext per_query_ctx,
					 bool retset)
{
	Tuplestorestate	   *tupstore;
	char			  **values;
	HeapTuple			tuple;
	MemoryContext		oldcontext;
	int					nr;
	int					nc = 1;
	SEXP				obj;
	int					i;

	/* switch to appropriate context to create the tuple store */
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/*
	 * If we return a set, get number of rows.
	 * Otherwise, stop at one row.
	 */
	if (retset)
		nr = length(rval);
	else
		nr = 1;

	/* initialize our tuplestore */
	tupstore = TUPLESTORE_BEGIN_HEAP;

	MemoryContextSwitchTo(oldcontext);

	values = (char **) palloc(nc * sizeof(char *));

	PROTECT(obj =  AS_CHARACTER(rval));

	for(i = 0; i < nr; i++)
	{
		values[0] = CHAR(STRING_ELT(obj, i));

		/* construct the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* switch to appropriate context while storing the tuple */
		oldcontext = MemoryContextSwitchTo(per_query_ctx);

		/* now store it */
		tuplestore_puttuple(tupstore, tuple);

		/* now reset the context */
		MemoryContextSwitchTo(oldcontext);
    }
	UNPROTECT(1);

	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tuplestore_donestoring(tupstore);
	MemoryContextSwitchTo(oldcontext);

	return tupstore;
}

