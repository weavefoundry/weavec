# Conditional scalar call arguments (RFC 0029)

Frozen in the validation directory before candidate 53 while the prior full
suite runs; move unchanged into the evaluation inventory after that run.
Conditional values retain their actual converted range at call entry. Both
arms contribute, and another loop invocation cannot reuse an earlier value.
