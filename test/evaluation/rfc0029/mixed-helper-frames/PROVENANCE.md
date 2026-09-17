RFC 0029 mixed entry/fresh-helper frame regression, frozen against candidate91b.
The incoming singleton is extended with a verified fresh helper allocation.
A later store targets a separate entry record. Lost, released and disowned
children must retain their actual ownership failures.
