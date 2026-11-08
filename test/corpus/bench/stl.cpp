// The motivation's STL program (RFC 0039), a workload of the corpus gate's
// benchmarks: strings, sorting, the ordered and unordered containers, a
// deque and a list, string streams, regular expressions, a tree of
// unique_ptr nodes with virtual calls, shared and weak pointers, exceptions
// thrown through frames with destructors, variant, optional, any,
// std::function, threads with a mutex and an atomic, std::async,
// std::format, file streams and std::filesystem, 20 rounds.
//
//   stl-bench
//
// The gate builds this file alone (see test/corpus/manifest.json) and
// compares the number it prints between the reference and the WeaveC build.
#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>
#include <fstream>
#include <filesystem>
#include <format>

struct Node { virtual ~Node() = default; virtual int eval() const = 0; };
struct Lit : Node { int v; explicit Lit(int v) : v(v) {} int eval() const override { return v; } };
struct Add : Node { std::unique_ptr<Node> a, b; Add(std::unique_ptr<Node> a, std::unique_ptr<Node> b) : a(std::move(a)), b(std::move(b)) {} int eval() const override { return a->eval() + b->eval(); } };

static int thrower(int depth) {
  char buf[64]; std::snprintf(buf, sizeof buf, "depth %d", depth);
  std::string s(buf);
  if (depth == 0) throw std::runtime_error(s);
  return thrower(depth - 1) + (int)s.size();
}

int main() {
  long acc = 0;
  for (int round = 0; round < 20; ++round) {
    std::vector<std::string> words;
    std::mt19937 rng(round);
    for (int i = 0; i < 20000; ++i) words.push_back(std::to_string(rng()) + "x" + std::string(i % 40, 'a'));
    std::sort(words.begin(), words.end());
    std::map<std::string, int> m; std::unordered_map<std::string, int> um; std::set<int> st; std::deque<int> dq; std::list<int> ls;
    for (auto &w : words) { m[w]++; um[w] += (int)w.size(); st.insert((int)w.size()); dq.push_front((int)w.size()); ls.push_back((int)w[0]); if (dq.size() > 100) dq.pop_back(); }
    acc += (long)m.size() + (long)um.size() + (long)st.size() + std::accumulate(dq.begin(), dq.end(), 0L) + (long)ls.size();
    std::ostringstream os; for (int i = 0; i < 1000; ++i) os << i << ' ' << 3.5 * i << '\n';
    std::istringstream is(os.str()); int a; double b; while (is >> a >> b) acc += a;
    std::regex re("([0-9]+)x(a*)"); std::smatch mt; for (int i = 0; i < 200; ++i) if (std::regex_search(words[i], mt, re)) acc += (long)mt[2].length();
    std::unique_ptr<Node> e = std::make_unique<Lit>(1); for (int i = 0; i < 1000; ++i) e = std::make_unique<Add>(std::move(e), std::make_unique<Lit>(i));
    acc += e->eval();
    std::shared_ptr<std::vector<int>> sp = std::make_shared<std::vector<int>>(1000, 7); std::weak_ptr<std::vector<int>> wp = sp; acc += (long)wp.lock()->size();
    for (int i = 0; i < 50; ++i) { try { thrower(i % 10); } catch (const std::exception &ex) { acc += (long)std::string(ex.what()).size(); } }
    std::variant<int, std::string> v = std::string("hello"); acc += (long)std::get<std::string>(v).size();
    std::optional<std::string> o = "opt"; acc += (long)o->size(); std::any an = 5; acc += std::any_cast<int>(an);
    std::function<int(int)> f = [&](int x) { return x + (int)words.size(); }; acc += f(1);
    std::mutex mu; std::atomic<long> at{0}; std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back([&, t] { std::vector<std::string> loc; for (int i = 0; i < 5000; ++i) { loc.emplace_back(std::to_string(i * t)); } std::lock_guard<std::mutex> g(mu); at += (long)loc.size(); });
    for (auto &t : ts) t.join(); acc += at;
    auto fut = std::async(std::launch::async, [] { std::string s; for (int i = 0; i < 1000; ++i) s += std::to_string(i); return s.size(); }); acc += (long)fut.get();
    acc += (long)std::format("{}-{:>8}-{:.3f}", round, "fmt", 1.5).size();
    { std::ofstream out("stl-bench.txt"); out << words[0] << '\n'; } { std::ifstream in("stl-bench.txt"); std::string l; std::getline(in, l); acc += (long)l.size(); }
    acc += (long)std::filesystem::path("/a/b/c.txt").extension().string().size();
    std::vector<int> big(100000); std::iota(big.begin(), big.end(), 0); std::reverse(big.begin(), big.end()); std::stable_sort(big.begin(), big.end()); acc += big[500];
  }
  std::cout << acc << std::endl;
  return 0;
}
