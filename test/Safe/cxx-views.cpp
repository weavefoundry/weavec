// RFC 0044 §7: views, and where a borrow may go: escape, with accepted and
// rejected forms (returned, stored, erased, kept by a callee).
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <weavec.h>

struct Item {
  int v = 0;
  std::string name;
};
struct Token {
  std::string_view text;
  int kind = 0;
};
std::string make(int n);
std::vector<Token> lex(std::string_view source);

// returned
std::string_view viewOfLocal(int n) {
  std::string s = make(n);
  return s; // expected-error {{what is returned points into 's', which does not outlive the function [weavec-safe:escape]}}
}
const std::string &referenceToLocal(int n) {
  std::string s = make(n);
  return s; // expected-error {{a reference to 's' is returned, and it does not outlive the function [weavec-safe:escape]}}
}
const int *pointerToLocal() {
  int local = 3;
  return &local; // expected-error {{a pointer to 'local' is returned, and it does not outlive the function [weavec-safe:escape]}}
}
std::string_view viewOfValueParameter(std::string s) {
  return s; // expected-error {{what is returned points into 's', which does not outlive the function [weavec-safe:escape]}}
}
std::function<int()> lambdaOfLocal(int n) {
  int x = n + 5;
  return [&] { return x; }; // expected-error {{what is returned points into 'x', which does not outlive the function [weavec-safe:escape]}}
}
std::function<int()> lambdaByCopy(int n) {
  int x = n + 5;
  return [x] { return x; };
}
std::vector<Token> tokensOfLocal(int n) {
  std::string source = make(n);
  std::vector<Token> tokens = lex(source);
  return tokens; // expected-error {{what is returned points into 'source', which does not outlive the function [weavec-safe:escape]}}
}
Token firstOfLocal(int n) {
  std::string line = make(n);
  std::vector<Token> tokens = lex(line);
  return tokens[0]; // expected-error {{what is returned points into 'line', which does not outlive the function [weavec-safe:escape]}}
}
Token firstOfParameter(std::string_view line) {
  std::vector<Token> tokens = lex(line);
  return tokens[0];
}
std::string_view trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ')
    s.remove_prefix(1);
  return s;
}
const std::string &longer(const std::string &a, const std::string &b) {
  return a.size() < b.size() ? b : a;
}
std::string_view ofLiteral() { return "a literal lives for the program"; }
std::string ownsItsData(int n) {
  std::string s = make(n);
  std::string_view view = s;
  return std::string(view);
}
struct Table {
  std::vector<Item> items;
  std::shared_ptr<Item> shared;
  const Item &first() const { return items.front(); }
  Item *find(int v) {
    for (Item &item : items)
      if (item.v == v)
        return &item;
    return nullptr;
  }
  std::string_view name() const { return items.front().name; }
  Item &sharedItem() {
    std::shared_ptr<Item> local = shared;
    return *local; // other holders keep it alive
  }
};

// a view class lives in a local
struct Lexer {
  std::string text;
  std::size_t pos = 0;
  char next() { return pos < text.size() ? text[pos++] : '\0'; }
};
struct Parser {
  Lexer &lexer;
  explicit Parser(Lexer &l) : lexer(l) {}
  int parse() { return lexer.next(); }
};
int onTheStack(Lexer &lexer) {
  Parser p(lexer);
  return p.parse();
}
std::unique_ptr<Parser> onTheHeap() {
  Lexer local;
  return std::make_unique<Parser>(local); // expected-error {{what is returned points into 'local', which does not outlive the function [weavec-safe:escape]}}
}
int lexerChanged(Lexer &lexer) {
  Parser p(lexer);
  lexer = Lexer{}; // a view made from a reference points into it
  return p.parse(); // expected-error {{'p' points into 'lexer', which 'operator=' may have changed [weavec-safe:stale-borrow]}}
}
int lexerGone() {
  std::unique_ptr<Parser> p;
  {
    Lexer inner;
    p = std::make_unique<Parser>(inner);
  }
  return p->parse(); // expected-error {{'p' points into 'inner', whose lifetime has ended [weavec-safe:stale-borrow]}}
}

// widened: a view takes the borrows it is given
struct Named {
  std::string_view name;
  void set(std::string_view n) { name = n; }
  void keep(const std::string &n) {
    name = n; // expected-error {{'this->name' would keep a borrow of the parameter 'n', which is lent for the call only [weavec-safe:escape]}}
  }
};
std::string_view widened() {
  Named named;
  std::string s = make(1);
  named.set(s);
  return named.name; // expected-error {{what is returned points into 's', which does not outlive the function [weavec-safe:escape]}}
}
int collected(const std::vector<std::string> &lines) {
  std::vector<std::string_view> parts;
  for (const std::string &line : lines)
    parts.push_back(line);
  std::string extra = make(2);
  parts.push_back(extra);
  return int(parts.size());
}
std::vector<std::string_view> collectedAndReturned(const std::vector<std::string> &lines) {
  std::vector<std::string_view> parts;
  for (const std::string &line : lines)
    parts.push_back(line);
  std::string extra = make(2);
  parts.push_back(extra);
  return parts; // expected-error {{what is returned points into 'extra', which does not outlive the function [weavec-safe:escape]}}
}

// stored
struct Document {
  std::string buffer;
  std::string_view title;
  std::size_t titleLength = 0;
  void indexByView() {
    title = std::string_view(buffer).substr(0, 4); // expected-error {{'this->title' would point into 'buffer', a part of the same object [weavec-safe:escape]}}
  }
  void indexByLength() { titleLength = buffer.size() < 4 ? buffer.size() : 4; }
};
struct Wrong {
  const std::string &name;
  explicit Wrong(std::string n) : name(n) {} // expected-error {{a borrow of 'n' is stored in 'name', which outlives it [weavec-safe:escape]}}
};
struct Button {
  std::function<void()> onClick;
  int clicks = 0;
  void countInto(int &count) {
    onClick = [&count] { ++count; }; // expected-error {{'this->onClick' would keep a borrow of the parameter 'count', which is lent for the call only [weavec-safe:escape]}}
  }
  void countShared(std::shared_ptr<int> count) {
    onClick = [count] { ++*count; };
  }
  void countLocal() {
    int local = 0;
    onClick = [&local] { ++local; }; // expected-error {{a borrow of 'local' is stored in 'this->onClick', which outlives it [weavec-safe:escape]}}
  }
};
struct Cache {
  const Item *last = nullptr;
  void remember(const std::vector<std::unique_ptr<Item>> &all) {
    last = all.back().get(); // a reference: checked when it is used
  }
  void rememberLocal() {
    Item local;
    last = &local; // expected-error {{a borrow of 'local' is stored in 'this->last', which outlives it [weavec-safe:escape]}}
  }
};
std::string_view global;
void storeGlobal(const std::string &s) {
  (void)s;
}

// erased
std::thread spawnOverLocal() {
  int local = 0;
  std::thread worker([&] { ++local; });
  return worker; // expected-error {{what is returned points into 'local', which does not outlive the function [weavec-safe:escape]}}
}
int joinOverLocal() {
  int local = 0;
  std::thread worker([&] { ++local; });
  worker.join();
  return local;
}
int calledInPlace(const std::vector<int> &v) {
  int k = 3;
  std::function<int(int)> scale = [&](int x) { return x * k; };
  int s = 0;
  for (int x : v)
    s += scale(x);
  return s;
}

// kept by a callee
struct Registry {
  std::vector<const int *> seen;
  void keep(const int *WEAVEC_ESCAPES p) { seen.push_back(p); }
};
void keepsLocal(Registry &registry) {
  int local = 3;
  registry.keep(&local); // expected-error {{a borrow of 'local' is stored in what 'keep' keeps, which outlives it [weavec-safe:escape]}}
}
void keepsHeap(Registry &registry, const std::vector<int> &v) {
  registry.keep(&v[0]);
}
