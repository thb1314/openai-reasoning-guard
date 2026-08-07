# Per-upstream Retry-After override for final transient errors

Each upstream profile may configure an optional whole-second `retry_after_override_sec` value. When non-empty, the proxy adds or replaces `Retry-After` on the final response after internal retries are exhausted, but only when that final response is a real upstream HTTP 429, 502, or 503; locally generated proxy and guard errors must not receive a fabricated `Retry-After`. An empty value preserves the upstream response unchanged, and the setting never changes internal retry scheduling.

Status compatibility is controlled independently by the per-profile `map_upstream_errors_to_502` boolean. When enabled, a final real upstream `400..599` status is exposed to the client as `502` after retry decisions are complete. Header filtering, including Retry-After eligibility, continues to use the original upstream status; response bodies and permitted headers are preserved.
