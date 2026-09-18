# Test harness audit

The original inventory remains immutable. Its functions shared one file, so
ordinary errors in unselected negative functions also made the positive CLI
invocations fail. The audited population isolates the exact same function
bodies into separate files. `partial` was rejected with the intended bounds
obligation, "access interval must fit its object", which the original reason
regex omitted. The audited matcher includes that explanation. No true negative
or positive property changed. Original results remain in zero-counters29b.json.
