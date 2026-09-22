#!/usr/bin/env python3
"""Prints a unit ledger's rows for FileCheck, one per facet (RFC 0030 §12.1).

usage: ledger-rows.py LEDGER.json

Each line is `<function>:<line> <site text> <facet>=<outcome>[/<reason>][:<template>]`,
followed by one line per requirement record, `  [a<arg>] <outcome>[/<reason>][:<template>]`.
"""
import json
import sys


def spell(decision):
    text = decision.get("outcome", "?")
    if decision.get("reason"):
        text += "/" + decision["reason"]
    check = decision.get("check")
    if check and check.get("template"):
        text += ":" + check["template"]
    return text


def main():
    with open(sys.argv[1], encoding="utf-8") as stream:
        ledger = json.load(stream)
    for unit in ledger.get("units") or []:
        for function in unit.get("functions") or []:
            for site in function.get("sites") or []:
                for facet, decision in sorted((site.get("facets") or {}).items()):
                    print(f"{function['name']}:{site.get('line')} {site.get('text')} "
                          f"{facet}={spell(decision)}")
                    for record in decision.get("requirements") or []:
                        print(f"  [a{record.get('arg')}] {spell(record)}")


if __name__ == "__main__":
    main()
