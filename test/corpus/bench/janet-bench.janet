# Janet benchmark for scripts/corpus-gate.py (test/corpus/manifest.json,
# config janet; RFC 0034, gate F7): calls, tables, arrays and sorting,
# buffers, strings and PEG matching, a few rounds of each.
# Deterministic; prints a checksum only.
(defn fib [n] (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(def words (peg/compile ~(any (+ (group (* (capture :a+) (constant :w))) (capture :d+) 1))))
(var sum 0)
(for round 0 6
  (def t @{})
  (for i 0 200000 (put t (string "k" (% i (+ 5000 round))) i))
  (def arr @[])
  (for i 0 200000 (array/push arr (% (+ round (* i 7919)) 100003)))
  (sort arr)
  (var s 0)
  (each x arr (+= s x))
  (def b @"")
  (for i 0 100000 (buffer/push-string b (string i) (if (= 0 (% i 7)) " abc " "")))
  (def m (peg/match words b))
  (def st @{})
  (for i 0 50000 (put st [(% i 101) (% i 7)] (struct :a i :b (% i 13))))
  (+= sum (fib 25) (length t) (% s 1000003) (length b) (length m) (length st)))
(print sum)
