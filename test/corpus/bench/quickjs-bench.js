// QuickJS benchmark for the corpus gate (RFC 0034, gate F7): calls, object
// allocation and sorting, Map and string building, regular expressions.
// Deterministic; prints a checksum only.
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
var sum = 0;
for (var round = 0; round < 3; round++) {
  var a = [];
  for (var i = 0; i < 300000; i++) a.push({ k: (i * 7919 + round) % 100003, s: "x" + i });
  a.sort(function (x, y) { return x.k - y.k; });
  var m = new Map();
  for (var i = 0; i < 200000; i++) m.set("k" + (i % (5000 + round)), i);
  var str = "";
  for (var i = 0; i < 20000; i++) str += String.fromCharCode(97 + (i + round) % 26);
  var cnt = 0;
  for (var i = 0; i < 200; i++) cnt += str.replace(/[aeiou]/g, "").length;
  var o = JSON.parse(JSON.stringify(a.slice(0, 50000)));
  sum += fib(27) + a[1000].k + m.size + cnt + o[49999].k + o[123].s.length;
}
print(sum);
