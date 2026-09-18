RFC 0029 repeated fresh-call regression, frozen against candidate96e.
A bounded byte traversal repeatedly allocates and appends independently owned
nodes, updates them through a separate helper, and publishes the complete
list under its live parent. Factory calls share a source site but each
successful acquisition is distinct from still-live preceding nodes.
Reused nodes, lost prefixes, released nodes and interior bases are negative.
