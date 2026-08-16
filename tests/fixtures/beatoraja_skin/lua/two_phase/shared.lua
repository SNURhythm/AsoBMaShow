__trace_shared_load_count = (__trace_shared_load_count or 0) + 1

return {
  identity = "synthetic-shared-v1",
  load_count = __trace_shared_load_count,
  mutation = "initial"
}
