\set VERBOSITY terse

SET search_path = 'public';
CREATE EXTENSION pg_pathman;
CREATE SCHEMA array_qual;



CREATE TABLE array_qual.test(a INT4 NOT NULL, b INT4);
SELECT create_range_partitions('array_qual.test', 'a', 1, 100, 10);
INSERT INTO array_qual.test SELECT i, i FROM generate_series(1, 1000) g(i);

ANALYZE;

/*
 * Test expr IN (...)
 */

/* a IN (...) - pruning should work */
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a IN (1, 2, 3, 4);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a IN (100, 200, 300, 400);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a IN (-100, 100);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a IN (-100, -200, -300);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a IN (-100, -200, -300, NULL);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a IN (NULL, NULL, NULL, NULL);

/* b IN (...) - pruning should not work */
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE b IN (1, 2, 3, 4);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE b IN (100, 200, 300, 400);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE b IN (-100, 100);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE b IN (-100, -200, -300);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE b IN (-100, -200, -300, NULL);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE b IN (NULL, NULL, NULL, NULL);


/*
 * Test expr = ANY (...)
 */

/* a = ANY (...) - pruning should work */
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ANY (array[100, 100]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ANY (array[100, 200, 300, 400]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ANY (array[array[100, 200], array[300, 400]]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ANY (array[array[100, 200], array[300, 400], array[NULL, NULL]::int4[]]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ANY (array[array[100, 200], array[300, NULL]]);


/*
 * Test expr = ALL (...)
 */

/* a = ALL (...) - pruning should work */
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ALL (array[100, 100]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ALL (array[100, 200, 300, 400]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ALL (array[array[100, 200], array[300, 400]]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ALL (array[array[100, 200], array[300, 400], array[NULL, NULL]::int4[]]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ALL (array[array[100, 200], array[300, NULL]]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a = ALL (array[NULL, NULL]::int4[]);


/*
 * Test expr < ANY (...)
 */

/* a < ANY (...) - pruning should work */
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a < ANY (array[100, 100]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a < ANY (array[99, 100, 101]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a < ANY (array[500, 550]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a < ANY (array[100, 700]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a < ANY (array[NULL, 700]);

SET pg_pathman.enable = f;
SELECT count(*) FROM array_qual.test WHERE a < ANY (array[NULL, 700]);
SET pg_pathman.enable = t;
SELECT count(*) FROM array_qual.test WHERE a < ANY (array[NULL, 700]);


/*
 * Test expr < ALL (...)
 */

/* a < ALL (...) - pruning should work */
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a < ALL (array[100, 100]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a < ALL (array[99, 100, 101]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a < ALL (array[500, 550]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a < ALL (array[100, 700]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a < ALL (array[NULL, 700]);

SET pg_pathman.enable = f;
SELECT count(*) FROM array_qual.test WHERE a < ALL (array[NULL, 700]);
SET pg_pathman.enable = t;
SELECT count(*) FROM array_qual.test WHERE a < ALL (array[NULL, 700]);


/*
 * Test expr > ANY (...)
 */

/* a > ANY (...) - pruning should work */
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a > ANY (array[100, 100]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a > ANY (array[99, 100, 101]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a > ANY (array[500, 550]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a > ANY (array[100, 700]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a > ANY (array[NULL, 700]);

SET pg_pathman.enable = f;
SELECT count(*) FROM array_qual.test WHERE a > ANY (array[NULL, 700]);
SET pg_pathman.enable = t;
SELECT count(*) FROM array_qual.test WHERE a > ANY (array[NULL, 700]);


/*
 * Test expr > ALL (...)
 */

/* a > ALL (...) - pruning should work */
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a > ALL (array[100, 100]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a > ALL (array[99, 100, 101]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a > ALL (array[500, 550]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a > ALL (array[100, 700]);
EXPLAIN (COSTS OFF) SELECT * FROM array_qual.test WHERE a > ALL (array[NULL, 700]);

SET pg_pathman.enable = f;
SELECT count(*) FROM array_qual.test WHERE a > ALL (array[NULL, 700]);
SET pg_pathman.enable = t;
SELECT count(*) FROM array_qual.test WHERE a > ALL (array[NULL, 700]);


/*
 * Test expr > ANY (... $1 ...)
 */

PREPARE q(int4) AS SELECT * FROM array_qual.test WHERE a > ANY (array[$1, 100, 600]);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
DEALLOCATE q;

PREPARE q(int4) AS SELECT * FROM array_qual.test WHERE a > ANY (array[100, 600, $1]);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
DEALLOCATE q;


/*
 * Test expr > ALL (... $1 ...)
 */

PREPARE q(int4) AS SELECT * FROM array_qual.test WHERE a > ALL (array[$1, 100, 600]);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
DEALLOCATE q;

PREPARE q(int4) AS SELECT * FROM array_qual.test WHERE a > ALL (array[100, $1, 600]);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
DEALLOCATE q;

PREPARE q(int4) AS SELECT * FROM array_qual.test WHERE a > ALL (array[100, 600, $1]);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
DEALLOCATE q;

PREPARE q(int4) AS SELECT * FROM array_qual.test WHERE a > ALL (array[array[100, 600], array[1, $1]]);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);

/* check query plan: EXECUTE q(999) */
DO language plpgsql
$$
	DECLARE
		query	text;
		result	jsonb;
		num		int;

	BEGIN
		query := 'EXECUTE q(999)';

		EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, FORMAT JSON) %s', query)
		INTO result;

		SELECT count(*) FROM jsonb_array_elements_text(result->0->'Plan'->'Plans') INTO num;

		RAISE notice '%: number of partitions: %', query, num;
	END
$$;

DEALLOCATE q;

PREPARE q(int4[]) AS SELECT * FROM array_qual.test WHERE a > ALL (array[array[100, 600], $1]);
EXPLAIN (COSTS OFF) EXECUTE q('{1, 1}');
EXPLAIN (COSTS OFF) EXECUTE q('{1, 1}');
EXPLAIN (COSTS OFF) EXECUTE q('{1, 1}');
EXPLAIN (COSTS OFF) EXECUTE q('{1, 1}');
EXPLAIN (COSTS OFF) EXECUTE q('{1, 1}');
EXPLAIN (COSTS OFF) EXECUTE q('{1, 1}');
EXPLAIN (COSTS OFF) EXECUTE q('{1, 1}');

/* check query plan: EXECUTE q('{1, 999}') */
DO language plpgsql
$$
	DECLARE
		query	text;
		result	jsonb;
		num		int;

	BEGIN
		query := 'EXECUTE q(''{1, 999}'')';

		EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, FORMAT JSON) %s', query)
		INTO result;

		SELECT count(*) FROM jsonb_array_elements_text(result->0->'Plan'->'Plans') INTO num;

		RAISE notice '%: number of partitions: %', query, num;
	END
$$;

DEALLOCATE q;

PREPARE q(int4) AS SELECT * FROM array_qual.test WHERE a > ALL (array[$1, 898]);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);

/* check query plan: EXECUTE q(999) */
DO language plpgsql
$$
	DECLARE
		query	text;
		result	jsonb;
		num		int;

	BEGIN
		query := 'EXECUTE q(999)';

		EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, FORMAT JSON) %s', query)
		INTO result;

		SELECT count(*) FROM jsonb_array_elements_text(result->0->'Plan'->'Plans') INTO num;

		RAISE notice '%: number of partitions: %', query, num;
	END
$$;

DEALLOCATE q;


/*
 * Test expr = ALL (... $1 ...)
 */

PREPARE q(int4) AS SELECT * FROM array_qual.test WHERE a = ALL (array[$1, 100, 600]);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
DEALLOCATE q;

PREPARE q(int4) AS SELECT * FROM array_qual.test WHERE a = ALL (array[100, 600, $1]);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
DEALLOCATE q;

PREPARE q(int4) AS SELECT * FROM array_qual.test WHERE a = ALL (array[100, $1]);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXPLAIN (COSTS OFF) EXECUTE q(1);
EXECUTE q(1);
EXECUTE q(100);
DEALLOCATE q;



DROP SCHEMA array_qual CASCADE;
DROP EXTENSION pg_pathman;
