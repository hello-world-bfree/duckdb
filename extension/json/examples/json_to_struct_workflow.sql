-- ========================================
-- JSON to STRUCT Workflow Example
-- ========================================
-- This demonstrates how to convert JSON to STRUCT for 10-100x performance improvement

-- ========================================
-- Part 1: The Problem (Slow JSON Queries)
-- ========================================

-- Load JSON data (slow storage)
CREATE TABLE events_json AS
SELECT * FROM read_json('examples/sample_events.jsonl');

-- Typical query - SLOW! (parses JSON multiple times per row)
SELECT
    json_col->>'user_id' as user_id,
    json_col->>'email' as email,
    json_col->>'status' as status,
    json_col->'metadata'->>'timestamp' as timestamp
FROM events_json
WHERE json_col->>'status' = 'active';

-- Problem: Parsing JSON 5 times per row!
-- On 1M rows: ~15 seconds


-- ========================================
-- Part 2: Solution - json_to_struct for Literal Arrays
-- ========================================

-- json_to_struct is ideal for converting literal JSON arrays with schema inference
-- It automatically normalizes schemas and infers types
SELECT data.id, data.name, data.age, data.city
FROM json_to_struct([
    '{"id": 1, "name": "Alice", "age": 30}'::JSON,
    '{"id": 2, "name": "Bob"}'::JSON,
    '{"id": 3, "age": 25, "city": "NYC"}'::JSON
]);

-- Result: All records have consistent schema (missing keys become NULL)


-- ========================================
-- Part 3: Converting Table Data (Production Approach)
-- ========================================

-- IMPORTANT: json_to_struct requires literal arrays at bind time
-- For table data, use json_normalize + json_transform with explicit schema

-- Sample inconsistent JSON data
CREATE TABLE inconsistent_json (json_col JSON);
INSERT INTO inconsistent_json VALUES
    ('{"id": 1, "name": "Alice", "age": 30}'),
    ('{"id": 2, "name": "Bob"}'),
    ('{"id": 3, "age": 25, "city": "NYC"}');

-- Step 1: Normalize JSON schemas
SELECT json_normalize(json_col) as normalized_json
FROM inconsistent_json;
-- Result: All records now have all keys (missing ones are NULL)

-- Step 2: Convert to STRUCT with explicit schema
CREATE TABLE struct_data AS
SELECT json_transform(json_normalize(json_col), '{
    "id": "BIGINT",
    "name": "VARCHAR",
    "age": "BIGINT",
    "city": "VARCHAR"
}') as data
FROM inconsistent_json;

-- Query efficiently - all records have consistent schema
SELECT
    data.id,
    data.name,
    data.age,
    data.city
FROM struct_data;


-- ========================================
-- Part 4: Manual Schema Definition
-- ========================================

-- If you know the schema, use json_transform directly:
CREATE TABLE events_struct_manual AS
SELECT
    json_transform(json_col, '{
        "user_id": "BIGINT",
        "email": "VARCHAR",
        "status": "VARCHAR",
        "age": "INTEGER",
        "score": "DOUBLE",
        "metadata": {
            "timestamp": "TIMESTAMP",
            "source": "VARCHAR",
            "version": "VARCHAR"
        }
    }') as data
FROM events_json;

-- Same query - FAST! (native struct access)
SELECT
    data.user_id,
    data.email,
    data.status,
    data.metadata.timestamp
FROM events_struct_manual
WHERE data.status = 'active';

-- On 1M rows: ~150 milliseconds (100x faster!)


-- ========================================
-- Part 5: Performance Comparison
-- ========================================

-- Create test data: 100K rows
CREATE TABLE perf_test_json AS
SELECT ('{"user_id": ' || i || ', "email": "user' || i || '@example.com", "status": "active", "age": ' || (20 + (i % 50)) || ', "score": ' || (i * 1.5) || '}')::JSON as json_col
FROM range(100000) t(i);

-- Convert to STRUCT
CREATE TABLE perf_test_struct AS
SELECT json_transform(json_col, '{
    "user_id": "BIGINT",
    "email": "VARCHAR",
    "status": "VARCHAR",
    "age": "BIGINT",
    "score": "DOUBLE"
}') as data
FROM perf_test_json;

-- Benchmark: Multi-field extraction with filter

-- JSON version (SLOW)
.timer on
SELECT
    json_col->>'user_id',
    json_col->>'email',
    json_col->>'status',
    json_col->>'age'
FROM perf_test_json
WHERE json_col->>'status' = 'active'
  AND CAST(json_col->>'age' AS INTEGER) > 30;
.timer off
-- Expected: ~2-5 seconds

-- STRUCT version (FAST)
.timer on
SELECT
    data.user_id,
    data.email,
    data.status,
    data.age
FROM perf_test_struct
WHERE data.status = 'active'
  AND data.age > 30;
.timer off
-- Expected: ~20-50 milliseconds (100x faster!)


-- ========================================
-- Summary
-- ========================================

/*
Key Points:

1. json_to_struct() works with literal JSON arrays for schema inference
2. For table data, use json_normalize() + json_transform() with explicit schema
3. STRUCT is 10-100x faster than JSON for structured data
4. Use json_normalize() to standardize inconsistent JSON schemas

Performance Impact:
- Single field: 10-20x faster
- Multi-field: 50-100x faster
- Aggregations: 100x+ faster

Usage Patterns:

1. Literal arrays (schema inference):
   SELECT * FROM json_to_struct([...JSON literals...]);

2. Table data (explicit schema):
   SELECT json_transform(json_normalize(json_col), '{"key": "TYPE"}')
   FROM table;

3. Known consistent schema:
   SELECT json_transform(json_col, '{"key": "TYPE"}')
   FROM table;
*/
