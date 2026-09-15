#!/usr/bin/env python3
"""Regenerate RFC 0027 concrete-heap clients into a separate output directory.

The oracle uses allocation identities and concrete DFS, independently of WeaveC.
Compare output bytes with test/evaluation/rfc0027/oracle; never update expected
results based on the checker. The frozen inventory is deliberately not rewritten.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path


def judge(edges, roots):
    """Consume actual allocation identities, following every owned edge."""
    released, active = set(), set()

    def destroy(node):
        if node is None:
            return None
        if node in active:
            return 'owning cycle'
        if node in released:
            return 'shared owned allocation'
        active.add(node)
        for child in edges[node]:
            failure = destroy(child)
            if failure:
                return failure
        active.remove(node)
        released.add(node)
        return None

    for node in roots:
        failure = destroy(node)
        if failure:
            return failure
    return None if released == set(range(len(edges))) else 'unreleased allocation'


def populations():
    selected = []
    # Exhaust all 4**6 directed three-node binary topologies. Retain the ten
    # finite ownership trees, then derive independent adversarial mutations.
    for flat in itertools.product((None, 0, 1, 2), repeat=6):
        edges = [list(flat[i:i + 2]) for i in range(0, 6, 2)]
        if judge(edges, [0]) is None:
            selected.append((edges, [0], None))
    selected += [([], [], None), ([[None, None]], [0], None),
                 ([[None, None], [None, None]], [0, 1], None)]
    positive = list(selected)
    for edges, roots, _ in positive:
        if len(edges) < 3:
            continue
        for variant in ('orphan', 'shared', 'cycle'):
            changed = [list(edge) for edge in edges]
            new_roots = list(roots)
            if variant == 'orphan':
                new_roots = []
            elif variant == 'shared':
                new_roots.append(roots[0])
            else:
                changed[roots[0]][0] = roots[0]
            reason = judge(changed, new_roots)
            assert reason
            selected.append((changed, new_roots, reason))
    assert len(positive) == 13 and len(selected) == 43
    return selected


def source(edges, roots):
    code = '''#include <stdlib.h>
struct node { unsigned value; struct node *left, *right; };
static void destroy(struct node *p) {
  if (!p) return;
  destroy(p->left); destroy(p->right); free(p);
}
int main(void) {
'''
    for i in range(len(edges)):
        code += f'  struct node *p{i}=malloc(sizeof *p{i});\n  if (!p{i}) {{'
        code += ''.join(f' free(p{j});' for j in range(i)) + ' return 0; }\n'
        code += f'  *p{i}=(struct node){{{i + 1},0,0}};\n'
    for i, children in enumerate(edges):
        for field, child in zip(('left', 'right'), children):
            if child is not None:
                code += f'  p{i}->{field}=p{child};\n'
    for i in roots:
        code += f'  destroy(p{i});\n'
    return code + '  return 0;\n}\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    frozen = Path(__file__).resolve().parent.parent / 'test/evaluation/rfc0027/oracle'
    if root == frozen:
        parser.error('use a separate output directory; frozen evidence is not overwritten')
    root.mkdir(parents=True, exist_ok=True)
    cases, truth = [], []
    for index, (edges, roots, reason) in enumerate(populations()):
        name = f'heap-{index:03d}'
        code = source(edges, roots)
        (root / (name + '.c')).write_text(code)
        case = dict(name=name, source=name + '.c', function='main',
                    expect='rejected' if reason else 'accepted', entry_requirements=0,
                    forbidden_trust=['unsafe', 'annotation assumptions'])
        if reason:
            case['reason'] = 'ownership|container|footprint|freed|release|live|leak'
        cases.append(case)
        truth.append(dict(name=name, edges=edges, roots=roots, failure=reason,
                          sha256=hashlib.sha256(code.encode()).hexdigest()))
    (root / 'manifest.json').write_text(json.dumps(dict(version=1, cases=cases), indent=2) + '\n')
    algorithm = ('Independent concrete DFS with active and released allocation sets; '
                 'every acquired allocation must be consumed once.')
    (root / 'concrete-truth.json').write_text(json.dumps(
        dict(version=1, algorithm=algorithm, cases=truth), indent=2) + '\n')
    print('13 positive, 30 negative')


if __name__ == '__main__':
    main()
