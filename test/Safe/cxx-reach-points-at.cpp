// RFC 0052 §6, amendment 2: a view made at an object of its member's type
// points at the object, which a change of the object's value keeps, only
// when nothing the view holds could name what the object owns. A view that
// also keeps a borrow into the object, or one of an object that owns others
// of its own type, borrows what the object holds: an assignment ends it.
// RUN: %weavec_cxx -std=c++23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct Lexer {
  std::string text;
  int at = 0;
};
struct Stats {
  int hits = 0;
};

// The view names the object and nothing in it.
struct Parser {
  Lexer &lexer;
  explicit Parser(Lexer &l) : lexer(l) {}
  int parse() { return lexer.at; }
};
int assigned(Lexer &lexer) {
  Parser p(lexer);
  lexer = Lexer{};
  return p.parse();
}
// Two objects, neither of which reaches the other's type.
struct Counted {
  Lexer &lexer;
  Stats &stats;
  Counted(Lexer &l, Stats &s) : lexer(l), stats(s) {}
  int parse() {
    ++stats.hits;
    return lexer.at;
  }
};
int assignedBoth(Lexer &lexer, Stats &stats) {
  Counted p(lexer, stats);
  lexer = Lexer{};
  stats = Stats{};
  return p.parse();
}
int wrapped(std::vector<int> &v) {
  auto r = std::ref(v);
  v.push_back(1);
  return int(r.get().size());
}
int spanned() {
  int a[4] = {1, 2, 3, 4};
  std::span<int> s(a);
  a[0] = 9;
  return s[0];
}

// A second member that can point into the object: a view of its text.
struct Scanner {
  Lexer &lexer;
  std::string_view rest;
  explicit Scanner(Lexer &l) : lexer(l), rest(l.text) {}
  char first() const { return rest.empty() ? '\0' : rest[0]; }
};
char textFreed(Lexer &lexer) {
  Scanner s(lexer);
  lexer = Lexer{};
  return s.first(); // expected-error {{'s' points into 'lexer', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
// A pointer to an element the object owns.
struct Table {
  std::vector<long> rows{1, 2, 3};
};
struct Cursor {
  Table &table;
  long *at;
  explicit Cursor(Table &t) : table(t), at(&t.rows[0]) {}
  void set(long v) { *at = v; }
};
long rowsFreed(Table &table) {
  Cursor c(table);
  table = Table{};
  c.set(42); // expected-error {{'c' points into 'table', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  return table.rows[0];
}
// One member of the object's type, of an object that owns others of it: the
// reference can name a child.
struct Node {
  long value = 1;
  std::vector<std::unique_ptr<Node>> kids;
};
struct Child {
  Node &node;
  explicit Child(Node &parent) : node(*parent.kids[0]) {}
  void set(long v) { node.value = v; }
};
long childFreed(Node &root) {
  Child c(root);
  root = Node{};
  c.set(42); // expected-error {{'c' points into 'root', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
  return root.value;
}
long childWrapped(Node &root) {
  auto r = std::ref(root);
  Node &kid = *r.get().kids[0];
  root = Node{};
  return kid.value; // expected-error {{'kid' points into 'root', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
