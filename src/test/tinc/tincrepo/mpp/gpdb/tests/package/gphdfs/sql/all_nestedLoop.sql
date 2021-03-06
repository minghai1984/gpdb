\echo --start_ignore
drop external table all_heap;
drop external table all_readhdfs_mapreduce;
drop external table all_readhdfs_mapred;
\echo --end_ignore

create readable external table all_heap(
datatype_all varchar, xcount_all bigint,
col1_time time,col2_time time, col3_time time, col4_time time, col5_time time, col6_time time, col7_time time, col8_time time, col9_time time, nullcol_time time,
col1_timestamp timestamp,col2_timestamp timestamp, col3_timestamp timestamp, nullcol_timestamp timestamp,
col1_date date,col2_date date, col3_date date, col4_date date, col5_date date, col6_date date, col7_date date, nullcol_date date,
max_bigint bigint, min_bigint bigint, x_bigint bigint, reverse_bigint bigint, increment_bigint bigint, nullcol_bigint bigint,
max_int int, min_int int, x_int int, reverse_int int, increment_int int, nullcol_int int,
max_smallint smallint, min_smallint smallint, x_smallint smallint, reverse_smallint smallint, increment_smallint smallint, nullcol_smallint smallint,
max_real real, min_real real, pi_real real, piX_real real, nullcol_real real,
max_float float, min_float float, pi_float float, piX_float float, nullcol_float float,
col1_boolean boolean, nullcol_boolean boolean,
col1_varchar varchar,col2_varchar varchar, nullcol_varchar varchar,
col1_bpchar bpchar,col2_bpchar bpchar, nullcol_bpchar bpchar,
max_numeric numeric, min_numeric numeric, x_numeric numeric, reverse_numeric numeric, increment_numeric numeric, nullcol_numeric numeric,
col1_text text,col2_text text, nullcol_text text
) location ('gpfdist://%localhost%:%gpfdistPort%/all.txt')format 'TEXT'(ESCAPE  'OFF');

\!%cmdstr% -DcompressionType=none javaclasses/TestHadoopIntegration mapreduce Mapreduce_mapper_TextIn /plaintext/all.txt /mapreduce/all 
create readable external table all_readhdfs_mapreduce(like all_heap) location ('gphdfs://%HADOOP_HOST%/mapreduce/all')format 'custom' (formatter='gphdfs_import');

\!%cmdstr% -DcompressionType=none javaclasses/TestHadoopIntegration mapred Mapred_mapper_TextIn /plaintext/all.txt /mapred/all
create readable external table all_readhdfs_mapred(like all_readhdfs_mapreduce) location ('gphdfs://%HADOOP_HOST%/mapred/all')format 'custom' (formatter='gphdfs_import');

select count(*) from all_readhdfs_mapreduce;
select count(*) from all_readhdfs_mapred;

set enable_nestloop = on;
set enable_hashjoin = off;
--explain select count(a.x_numeric) from all_readhdfs_mapreduce a, all_readhdfs_mapred b where a.xcount_all > 100 and a.xcount_all < 105 and a.x_bigint = b.x_bigint;
select count(a.x_numeric) from all_readhdfs_mapreduce a, all_readhdfs_mapred b where a.xcount_all > 100 and a.xcount_all < 105 and a.x_bigint = b.x_bigint;
set enable_nestloop = off;
set enable_hashjoin = on;



