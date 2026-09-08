// RUN: %weavec --checked --checked-report=%t.json %s --
// RUN: python3 -c "import json; d=json.load(open(r'%t.json')); assert d['version']==2; assert d['invocation_ok']; assert d['units'][0]['functions'][0]['selected']; assert d['units'][0]['functions'][0]['complete']"
// RUN: %weavec-cc -fweavec-checked -fweavec-checked-report=%t.driver.json -c %s -o %t.o
// RUN: python3 -c "import json; d=json.load(open(r'%t.driver.json')); assert d['invocation_ok']; assert d['units'][0]['functions'][0]['selected']"
// RFC 0018: selection and reports are compiler and tooling interfaces.
int safe(void) { int a[2] = {1, 2}; return a[1]; }
