RFC 0029 independently initialized singleton ownership regression.
Frozen against candidate93a before evaluating candidate94a. Unlike the
separately retained bare calloc-return population, this constructor spells
out its initialized links and selector. Actual owned heads can accept fresh
children and preserve them across an independent reader update, including
helper forwarding. Lost, released and disowned children remain negative.
