--
-- INDEX_BUILD_OPTIMIZATION
-- Test the optimization of CREATE INDEX WHERE using existing indexes
--

-- Test setup
CREATE TABLE index_opt_test (
    id SERIAL PRIMARY KEY,
    status TEXT,
    category INTEGER,
    score NUMERIC,
    active BOOLEAN,
    created_at TIMESTAMP DEFAULT now()
);

-- Insert test data
INSERT INTO index_opt_test (status, category, score, active)
SELECT
    CASE (i % 4)
        WHEN 0 THEN 'active'
        WHEN 1 THEN 'pending'
        WHEN 2 THEN 'completed'
        ELSE 'cancelled'
    END,
    (i % 10) + 1,
    random() * 100,
    (i % 2) = 0
FROM generate_series(1, 1000) i;

-- Create base indexes that can be used for optimization
CREATE INDEX idx_status ON index_opt_test(status);
CREATE INDEX idx_category ON index_opt_test(category);
CREATE INDEX idx_active ON index_opt_test(active);

-- Enable optimization and debug logging
SET enable_index_build_optimization = true;
SET debug_index_build_optimization = true;
SET client_min_messages = DEBUG1;

-- Test 1: Simple equality predicate (should use optimization)
-- This should use idx_status for filtering
CREATE INDEX test_idx_active_status ON index_opt_test(created_at)
WHERE status = 'active';

-- Test 2: Integer equality (should use optimization)
-- This should use idx_category for filtering
CREATE INDEX test_idx_cat_1 ON index_opt_test(score)
WHERE category = 1;

-- Test 3: Boolean predicate (should use optimization)
-- This should use idx_active for filtering
CREATE INDEX test_idx_is_active ON index_opt_test(id)
WHERE active = true;

-- Test 4: Multiple predicates with AND (should use optimization if any index matches)
CREATE INDEX test_idx_multi_and ON index_opt_test(score)
WHERE status = 'active' AND category = 2;

-- Test 5: Predicate that doesn't match any index (should NOT use optimization)
CREATE INDEX test_idx_no_match ON index_opt_test(id)
WHERE score > 50.0;

-- Test 6: Complex predicate (should NOT use optimization)
CREATE INDEX test_idx_complex ON index_opt_test(id)
WHERE length(status) > 5;

-- Test 7: Disable optimization and verify fallback
SET enable_index_build_optimization = false;
CREATE INDEX test_idx_disabled ON index_opt_test(created_at)
WHERE status = 'pending';
SET enable_index_build_optimization = true;

-- Verify indexes work correctly with some queries
EXPLAIN (COSTS OFF)
SELECT count(*) FROM index_opt_test WHERE status = 'active';

EXPLAIN (COSTS OFF)
SELECT * FROM index_opt_test WHERE category = 1 LIMIT 5;

EXPLAIN (COSTS OFF)
SELECT * FROM index_opt_test WHERE active = true LIMIT 5;

-- Verify partial indexes work correctly
EXPLAIN (COSTS OFF)
SELECT * FROM index_opt_test WHERE status = 'active' AND created_at > '2020-01-01';

-- Test correctness: compare results from optimized vs non-optimized indexes
-- Both should return the same number of rows
SELECT count(*) as active_rows FROM index_opt_test WHERE status = 'active';
SELECT count(*) as pending_rows FROM index_opt_test WHERE status = 'pending';

-- Check that indexes are valid and usable
SELECT indexname, indexdef
FROM pg_indexes
WHERE tablename = 'index_opt_test'
  AND indexname LIKE 'test_idx_%'
ORDER BY indexname;

-- Check index statistics (should show no usage yet since we just created them)
SELECT indexrelname, idx_scan, idx_tup_read, idx_tup_fetch
FROM pg_stat_user_indexes
WHERE relname = 'index_opt_test'
  AND indexrelname LIKE 'test_idx_%'
ORDER BY indexrelname;

-- Force some index usage to verify they work
SET enable_seqscan = false;
SET enable_bitmapscan = false;

-- These should use the partial indexes we created
EXPLAIN (COSTS OFF, ANALYZE OFF)
SELECT * FROM index_opt_test
WHERE status = 'active' AND created_at > '2020-01-01'
ORDER BY created_at LIMIT 1;

EXPLAIN (COSTS OFF, ANALYZE OFF)
SELECT * FROM index_opt_test
WHERE category = 1 AND score > 0
ORDER BY score LIMIT 1;

-- Reset settings
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET client_min_messages;

-- Cleanup
DROP TABLE index_opt_test CASCADE;
