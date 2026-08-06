# Per-upstream Retry-After override for final transient errors

Each upstream profile may configure an optional whole-second `retry_after_override_sec` value. When non-empty, the proxy adds or replaces `Retry-After` on the final response after internal retries are exhausted, but only when that final response is a real upstream HTTP 429, 502, or 503; locally generated proxy and guard errors must not receive a fabricated `Retry-After`. An empty value preserves the upstream response unchanged, and the setting never changes internal retry scheduling.
