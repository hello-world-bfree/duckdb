-- JSON vs STRUCT Performance Benchmark
-- This script compares JSON field extraction vs STRUCT field access
-- Expected speedup: 10-100x for multi-field queries

LOAD json;

-- Setup: Create test data (100K rows)
CREATE OR REPLACE TABLE json_data AS
SELECT ('{"id": ' || i || ', "name": "user' || i || '", "email": "user' || i || '@example.com", "age": ' || (20 + (i % 50)) || ', "score": ' || (i * 0.1) || ', "active": true, "city": "City' || (i % 100) || '"}')::JSON as json_col
FROM range(100000) t(i);

-- Convert to STRUCT using json_to_struct
CREATE OR REPLACE TABLE struct_data AS
WITH json_array AS (
    SELECT ARRAY_AGG(json_col) as arr FROM json_data
)
SELECT *
FROM json_array, json_to_struct(json_array.arr);

-- ============================================
-- Benchmark 1: Single field extraction
-- ============================================
.timer on

-- JSON version
SELECT COUNT(*)
FROM json_data
WHERE json_col->>'name' LIKE '%user5%';

-- STRUCT version
SELECT COUNT(*)
FROM struct_data
WHERE data.name LIKE '%user5%';

-- ============================================
-- Benchmark 2: Multi-field extraction (6 fields)
-- ============================================

-- JSON version (SLOW - parses JSON 6+ times per row)
SELECT
    json_col->>'id',
    json_col->>'name',
    json_col->>'email',
    json_col->>'age',
    json_col->>'score',
    json_col->>'city'
FROM json_data
WHERE (json_col->>'active')::BOOLEAN = true
  AND (json_col->>'age')::INTEGER > 30
  AND (json_col->>'score')::DOUBLE > 5000.0
LIMIT 100;

-- STRUCT version (FAST - direct memory access)
SELECT
    data.id,
    data.name,
    data.email,
    data.age,
    data.score,
    data.city
FROM struct_data
WHERE data.active = true
  AND data.age > 30
  AND data.score > 5000.0
LIMIT 100;

-- ============================================
-- Benchmark 3: Aggregation
-- ============================================

-- JSON version
SELECT
    json_col->>'city' as city,
    COUNT(*) as count,
    AVG((json_col->>'age')::INTEGER) as avg_age,
    SUM((json_col->>'score')::DOUBLE) as total_score
FROM json_data
GROUP BY json_col->>'city'
ORDER BY count DESC
LIMIT 10;

-- STRUCT version
SELECT
    data.city,
    COUNT(*) as count,
    AVG(data.age) as avg_age,
    SUM(data.score) as total_score
FROM struct_data
GROUP BY data.city
ORDER BY count DESC
LIMIT 10;

.timer off

-- Cleanup
DROP TABLE json_data;
DROP TABLE struct_data;

-- Summary
SELECT 'Benchmark complete. Compare timing results above.' as summary;
SELECT 'Expected: STRUCT queries should be 10-100x faster than JSON queries.' as expected_result;
