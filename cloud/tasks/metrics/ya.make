GO_LIBRARY()

SRCS(
    buckets.go
    metrics.go
)

GO_TEST_SRCS(
    buckets_test.go
)

END()

RECURSE(
    empty
    mocks
)

RECURSE_FOR_TESTS(
    tests
)
