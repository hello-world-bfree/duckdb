-- JSON vs STRUCT Performance Benchmark
-- This script compares JSON field extraction vs STRUCT field access
--
-- IMPORTANT: json_to_struct requires literal arrays at bind time for schema inference
-- For dynamic table data, use json_transform with a known schema instead.
-- This benchmark shows the performance difference between JSON and STRUCT access.

LOAD json;

-- ============================================
-- Setup: Create test data (100K rows)
-- ============================================
CREATE OR REPLACE TABLE json_data AS
SELECT
    i as row_id,
    ('{"id": ' || i || ', "name": "user' || i || '", "email": "user' || i || '@example.com", "age": ' || (20 + (i % 50)) || ', "score": ' || (i * 0.1) || ', "active": true, "city": "City' || (i % 100) || '"}')::JSON as json_col
FROM range(100000) t(i);

-- Convert to STRUCT using json_transform (manual schema - production approach)
CREATE OR REPLACE TABLE struct_data AS
SELECT
    row_id,
    json_transform(json_col, '{
        "id": "BIGINT",
        "name": "VARCHAR",
        "email": "VARCHAR",
        "age": "BIGINT",
        "score": "DOUBLE",
        "active": "BOOLEAN",
        "city": "VARCHAR"
    }') as data
FROM json_data;

-- ============================================
-- Benchmark 1: Single field extraction
-- ============================================
.print '=== Benchmark 1: Single Field Extraction ==='
.timer on

.print 'JSON version:'
SELECT COUNT(*)
FROM json_data
WHERE json_col->>'name' LIKE '%user500%';

.print 'STRUCT version:'
SELECT COUNT(*)
FROM struct_data
WHERE data.name LIKE '%user500%';

-- ============================================
-- Benchmark 2: Multi-field extraction (6 fields)
-- ============================================
.print '=== Benchmark 2: Multi-Field Extraction (6 fields) ==='

.print 'JSON version (parses JSON 9+ times per row):'
SELECT
    json_col->>'id',
    json_col->>'name',
    json_col->>'email',
    json_col->>'age',
    json_col->>'score',
    json_col->>'city'
FROM json_data
WHERE (json_col->>'active')::BOOLEAN = true
  AND (json_col->>'age')::INTEGER > 45
  AND (json_col->>'score')::DOUBLE > 5000.0
LIMIT 100;

.print 'STRUCT version (direct memory access):'
SELECT
    data.id,
    data.name,
    data.email,
    data.age,
    data.score,
    data.city
FROM struct_data
WHERE data.active = true
  AND data.age > 45
  AND data.score > 5000.0
LIMIT 100;

-- ============================================
-- Benchmark 3: Aggregation
-- ============================================
.print '=== Benchmark 3: Aggregation ==='

.print 'JSON version:'
SELECT
    json_col->>'city' as city,
    COUNT(*) as count,
    AVG((json_col->>'age')::INTEGER) as avg_age,
    SUM((json_col->>'score')::DOUBLE) as total_score
FROM json_data
GROUP BY json_col->>'city'
ORDER BY count DESC
LIMIT 10;

.print 'STRUCT version:'
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

-- ============================================
-- Demonstrate json_to_struct with literal arrays
-- ============================================
.print '=== json_to_struct with literal arrays ==='
SELECT data.id, data.name, data.active
FROM json_to_struct([
    '{"id": 1, "name": "Alice", "active": true}'::JSON,
    '{"id": 2, "name": "Bob", "active": false}'::JSON,
    '{"id": 3, "name": "Charlie"}'::JSON
]);

-- Cleanup
DROP TABLE json_data;
DROP TABLE struct_data;

-- Summary
.print '=== Summary ==='
.print 'JSON extraction: Parses entire JSON document for each field access'
.print 'STRUCT access: Direct memory access to typed fields'
.print 'Expected speedup: 10-100x for multi-field queries'
.print ''
.print 'Recommendation:'
.print '- Use json_transform(json_col, schema) for known schemas'
.print '- Use json_normalize() to standardize inconsistent JSON schemas'
.print '- Use json_to_struct() for quick schema inference on literal arrays'
