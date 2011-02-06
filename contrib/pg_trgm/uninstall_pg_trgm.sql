/* contrib/pg_trgm/uninstall_pg_trgm.sql */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP OPERATOR CLASS gist_trgm_ops USING gist;

DROP FUNCTION gtrgm_same(gtrgm, gtrgm, internal);

DROP FUNCTION gtrgm_union(bytea, internal);

DROP FUNCTION gtrgm_picksplit(internal, internal);

DROP FUNCTION gtrgm_penalty(internal,internal,internal);

DROP FUNCTION gtrgm_decompress(internal);

DROP FUNCTION gtrgm_compress(internal);

DROP FUNCTION gtrgm_consistent(internal,text,int,oid,internal);

DROP FUNCTION gtrgm_distance(internal,text,int,oid);

DROP TYPE gtrgm CASCADE;

DROP OPERATOR CLASS gin_trgm_ops USING gin;

DROP FUNCTION gin_extract_value_trgm(text, internal);

DROP FUNCTION gin_extract_query_trgm(text, internal, int2, internal, internal, internal, internal);

DROP FUNCTION gin_trgm_consistent(internal, int2, text, int4, internal, internal, internal, internal);

DROP OPERATOR % (text, text);

DROP FUNCTION similarity_op(text,text);

DROP OPERATOR <-> (text, text);

DROP FUNCTION similarity_dist(text,text);

DROP FUNCTION similarity(text,text);

DROP FUNCTION show_trgm(text);

DROP FUNCTION show_limit();

DROP FUNCTION set_limit(float4);
