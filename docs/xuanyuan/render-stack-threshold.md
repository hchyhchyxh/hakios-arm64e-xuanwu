# Render thread stack threshold

The actual big_pthread_create predicate contradicted its comment: stack==0 or stack>=4 MB entered the 24 MB replacement path. Explicit small stacks below 4 MB were left unchanged and large caller-selected stacks could shrink. The predicate now selects only stack<4 MB, including the default zero value.

A host check extracts the actual predicate and compiles boundary cases: default, 1 MB, just below 4 MB, exactly 4 MB, 24 MB, and 64 MB. This verifies selection only, not Darwin pthread behavior. The existing allocation/error handling is unchanged. This is a separate correctness fix; it does not establish a cause for the missing transient-surface assertion. Userland rebuild and device deployment are pending, so the running filter-merging control is unchanged.
