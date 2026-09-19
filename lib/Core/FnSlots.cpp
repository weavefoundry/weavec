//===- FnSlots.cpp - Function-pointer slots (RFC 0030) --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/FnSlots.h"

#include <charconv>
#include <deque>
#include <system_error>

namespace weavec::core {

//===----------------------------------------------------------------------===//
// Keys and constraints
//===----------------------------------------------------------------------===//

SlotKey SlotKey::field(std::string record, std::string field) {
  return SlotKey{.kind = SlotKind::Field,
                 .scope = std::move(record),
                 .name = std::move(field)};
}

SlotKey SlotKey::global(std::string name) {
  return SlotKey{.kind = SlotKind::Global, .name = std::move(name)};
}

SlotKey SlotKey::staticGlobal(std::string unit, std::string name) {
  return SlotKey{.kind = SlotKind::Static,
                 .scope = std::move(unit),
                 .name = std::move(name)};
}

SlotKey SlotKey::param(std::string function, std::uint32_t index) {
  return SlotKey{
      .kind = SlotKind::Param, .scope = std::move(function), .index = index};
}

SlotKey SlotKey::result(std::string function) {
  return SlotKey{.kind = SlotKind::Result, .scope = std::move(function)};
}

SlotKey SlotKey::local(std::string function, std::string name) {
  return SlotKey{.kind = SlotKind::Local,
                 .scope = std::move(function),
                 .name = std::move(name)};
}

SlotKey SlotKey::callParam(const SlotKey &callee, std::uint32_t index) {
  return SlotKey{
      .kind = SlotKind::CallParam, .scope = callee.toString(), .index = index};
}

SlotKey SlotKey::callResult(const SlotKey &callee) {
  return SlotKey{.kind = SlotKind::CallResult, .scope = callee.toString()};
}

std::string SlotKey::toString() const {
  switch (kind) {
  case SlotKind::Field:
    return "field " + scope + " " + name;
  case SlotKind::Global:
    return "global " + name;
  case SlotKind::Static:
    return "static " + scope + ":" + name;
  case SlotKind::Param:
    return "param " + scope + " " + std::to_string(index);
  case SlotKind::Result:
    return "result " + scope;
  case SlotKind::Local:
    return "local " + scope + " " + name;
  case SlotKind::CallParam:
    return "call-param " + std::to_string(index) + " " + scope;
  case SlotKind::CallResult:
    return "call-result " + scope;
  }
  return "<invalid>";
}

static bool consumePrefix(std::string_view &text, std::string_view prefix) {
  if (!text.starts_with(prefix))
    return false;
  text.remove_prefix(prefix.size());
  return true;
}

static bool parseIndex(std::string_view text, std::uint32_t &value) {
  if (text.empty())
    return false;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return ec == std::errc() && end == text.data() + text.size();
}

/// Splits `text` at its last occurrence of `separator` into two non-empty
/// parts.
static std::optional<std::pair<std::string_view, std::string_view>>
splitLast(std::string_view text, char separator) {
  const std::size_t at = text.rfind(separator);
  if (at == std::string_view::npos || at == 0 || at + 1 == text.size())
    return std::nullopt;
  return std::pair{text.substr(0, at), text.substr(at + 1)};
}

std::optional<SlotKey> SlotKey::parse(std::string_view text) {
  if (consumePrefix(text, "field ")) {
    const auto parts = splitLast(text, ' ');
    if (!parts)
      return std::nullopt;
    return field(std::string(parts->first), std::string(parts->second));
  }
  if (consumePrefix(text, "global ")) {
    if (text.empty())
      return std::nullopt;
    return global(std::string(text));
  }
  if (consumePrefix(text, "static ")) {
    const auto parts = splitLast(text, ':');
    if (!parts)
      return std::nullopt;
    return staticGlobal(std::string(parts->first), std::string(parts->second));
  }
  if (consumePrefix(text, "param ")) {
    const auto parts = splitLast(text, ' ');
    std::uint32_t index = 0;
    if (!parts || !parseIndex(parts->second, index))
      return std::nullopt;
    return param(std::string(parts->first), index);
  }
  if (consumePrefix(text, "result ")) {
    if (text.empty())
      return std::nullopt;
    return result(std::string(text));
  }
  if (consumePrefix(text, "local ")) {
    const auto parts = splitLast(text, ' ');
    if (!parts)
      return std::nullopt;
    return local(std::string(parts->first), std::string(parts->second));
  }
  if (consumePrefix(text, "call-param ")) {
    const std::size_t space = text.find(' ');
    std::uint32_t index = 0;
    if (space == std::string_view::npos ||
        !parseIndex(text.substr(0, space), index))
      return std::nullopt;
    const auto callee = parse(text.substr(space + 1));
    if (!callee)
      return std::nullopt;
    return callParam(*callee, index);
  }
  if (consumePrefix(text, "call-result ")) {
    const auto callee = parse(text);
    if (!callee)
      return std::nullopt;
    return callResult(*callee);
  }
  return std::nullopt;
}

std::optional<SlotKey> SlotKey::callee() const {
  if (kind != SlotKind::CallParam && kind != SlotKind::CallResult)
    return std::nullopt;
  return parse(scope);
}

bool SlotKey::isLocal() const {
  switch (kind) {
  case SlotKind::Local:
    return true;
  case SlotKind::CallParam:
  case SlotKind::CallResult: {
    const auto calleeKey = callee();
    return calleeKey && calleeKey->isLocal();
  }
  case SlotKind::Field:
  case SlotKind::Global:
  case SlotKind::Static:
  case SlotKind::Param:
  case SlotKind::Result:
    return false;
  }
  return false;
}

SlotConstraint SlotConstraint::member(std::string function, SlotKey slot) {
  return SlotConstraint{.kind = Kind::Member,
                        .slot = std::move(slot),
                        .function = std::move(function)};
}

SlotConstraint SlotConstraint::subset(SlotKey from, SlotKey to) {
  return SlotConstraint{
      .kind = Kind::Subset, .slot = std::move(to), .from = std::move(from)};
}

SlotConstraint SlotConstraint::open(SlotKey slot, std::string detail) {
  return SlotConstraint{
      .kind = Kind::Open, .slot = std::move(slot), .detail = std::move(detail)};
}

std::string_view toString(IndirectCallKind kind) noexcept {
  switch (kind) {
  case IndirectCallKind::ClosedEmpty:
    return "closed-empty";
  case IndirectCallKind::ClosedSingle:
    return "closed-single";
  case IndirectCallKind::ClosedJoin:
    return "closed-join";
  case IndirectCallKind::OpenKnown:
    return "open-known";
  case IndirectCallKind::OpenUnknown:
    return "open-unknown";
  }
  return "<invalid>";
}

std::optional<FacetDecision>
openCallTemporalDecision(const CallResolution &resolution) {
  std::string detail = resolution.open ? resolution.open->detail : "";
  switch (resolution.kind) {
  case IndirectCallKind::OpenKnown:
    return FacetDecision::trustedFor(TrustReason::ExternContract,
                                     std::move(detail));
  case IndirectCallKind::OpenUnknown:
    return FacetDecision::unresolvedFor(UnresolvedReason::Callback,
                                        std::move(detail));
  case IndirectCallKind::ClosedEmpty:
  case IndirectCallKind::ClosedSingle:
  case IndirectCallKind::ClosedJoin:
    return std::nullopt;
  }
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// The seed rules
//===----------------------------------------------------------------------===//

namespace {
/// What the closed-slot rules say about one slot on its own.
struct Seed {
  std::optional<OpenSource> open;
  /// Values in the slot are visible to code outside the solved program.
  bool escaping = false;
};
} // namespace

static std::string_view outsideCode(const SlotRules &rules) {
  return rules.scope == SlotScope::Unit ? "code outside this unit"
                                        : "code outside the analysed program";
}

static Seed seedOf(const SlotKey &key, const SlotRules &rules) {
  const bool closedWorld = rules.closedWorld();
  Seed seed;
  switch (key.kind) {
  case SlotKind::Field:
    if (!closedWorld && !rules.confinedRecords.contains(key.scope)) {
      seed.open = OpenSource{.seed = key,
                             .detail = "'" + key.scope + "." + key.name +
                                       "' may be stored by " +
                                       std::string(outsideCode(rules))};
      seed.escaping = true;
    }
    return seed;
  case SlotKind::Global:
    if (!closedWorld) {
      seed.open = OpenSource{.seed = key,
                             .detail = "'" + key.name + "' may be stored by " +
                                       std::string(outsideCode(rules))};
      seed.escaping = true;
    }
    return seed;
  case SlotKind::Static:
    if (rules.escapedStatics.contains(key.staticName())) {
      seed.open = OpenSource{
          .seed = key, .detail = "the address of '" + key.name + "' escapes"};
      seed.escaping = true;
    }
    return seed;
  case SlotKind::Param:
    if (!closedWorld && rules.exported.contains(key.scope))
      seed.open =
          OpenSource{.seed = key,
                     .detail = "values stored by '" + key.scope +
                               "' parameter " + std::to_string(key.index)};
    // Passing a value to a function without a body hands it to unknown
    // code.
    seed.escaping = !rules.defined.contains(key.scope);
    return seed;
  case SlotKind::Result:
    if (!rules.defined.contains(key.scope))
      seed.open = OpenSource{
          .seed = key,
          .detail = key.scope == UnknownFunction
                        ? std::string("the result of an unknown callee")
                        : "the result of '" + key.scope +
                              "', which has no body here"};
    seed.escaping = !closedWorld && rules.exported.contains(key.scope);
    return seed;
  case SlotKind::Local:
  case SlotKind::CallParam:
  case SlotKind::CallResult:
    return seed;
  }
  return seed;
}

/// The open source of a parameter of `f` once `f`'s address reached an open
/// position.
static OpenSource escapedSource(const SlotKey &param, const SlotRules &rules) {
  return OpenSource{.seed = param,
                    .detail = "'" + param.scope + "' may be called by " +
                              std::string(outsideCode(rules))};
}

//===----------------------------------------------------------------------===//
// The solver
//===----------------------------------------------------------------------===//

namespace {
struct SolverNode {
  SlotKey key;
  std::set<std::string> targets;
  std::optional<OpenSource> open;
  /// Inclusion edges `this ⊆ out[i]`.
  std::vector<std::size_t> out;
  /// Call keys whose callee is this node.
  std::vector<std::size_t> callParams;
  std::vector<std::size_t> callResults;
  /// Call keys: the callee's node.
  std::optional<std::size_t> callee;
  bool escapingSeed = false;
};

/// A worklist over nodes; a node is processed when its targets or its
/// openness grew, and pushes what it reaches.
class Solver {
public:
  explicit Solver(const SlotRules &rules) : rules(rules) {}

  void load(const std::set<SlotConstraint> &constraints);
  void run();

  std::vector<SolverNode> nodes;
  std::set<std::string, std::less<>> escaped;
  std::size_t steps = 0;

private:
  std::size_t intern(const SlotKey &key);
  bool addEdge(std::size_t from, std::size_t to);
  void push(std::size_t node);
  void markEscaped(const std::string &function);
  void process(std::size_t node);

  const SlotRules &rules;
  std::map<SlotKey, std::size_t> ids;
  std::set<std::pair<std::size_t, std::size_t>> edges;
  std::map<std::string, std::vector<std::size_t>, std::less<>> paramNodes;
  std::deque<std::size_t> work;
  std::vector<bool> queued;
};
} // namespace

std::size_t Solver::intern(const SlotKey &key) {
  if (const auto found = ids.find(key); found != ids.end())
    return found->second;
  const std::size_t id = nodes.size();
  ids.emplace(key, id);
  const Seed seed = seedOf(key, rules);
  nodes.push_back(SolverNode{.key = key,
                             .targets = {},
                             .open = seed.open,
                             .out = {},
                             .callParams = {},
                             .callResults = {},
                             .callee = std::nullopt,
                             .escapingSeed = seed.escaping});
  queued.push_back(false);
  if (key.kind == SlotKind::Param) {
    paramNodes[key.scope].push_back(id);
    if (!nodes[id].open && escaped.contains(key.scope))
      nodes[id].open = escapedSource(key, rules);
  }
  if (const auto calleeKey = key.callee()) {
    const std::size_t callee = intern(*calleeKey);
    nodes[id].callee = callee;
    if (key.kind == SlotKind::CallParam)
      nodes[callee].callParams.push_back(id);
    else
      nodes[callee].callResults.push_back(id);
    // The callee connects the new call key to its targets.
    push(callee);
  }
  push(id);
  return id;
}

bool Solver::addEdge(std::size_t from, std::size_t to) {
  if (!edges.emplace(from, to).second)
    return false;
  nodes[from].out.push_back(to);
  return true;
}

void Solver::push(std::size_t node) {
  if (queued[node])
    return;
  queued[node] = true;
  work.push_back(node);
}

void Solver::markEscaped(const std::string &function) {
  if (!escaped.insert(function).second)
    return;
  const auto found = paramNodes.find(function);
  if (found == paramNodes.end())
    return;
  for (const std::size_t param : found->second) {
    if (!nodes[param].open) {
      nodes[param].open = escapedSource(nodes[param].key, rules);
      push(param);
    }
  }
}

void Solver::load(const std::set<SlotConstraint> &constraints) {
  for (const SlotConstraint &constraint : constraints) {
    switch (constraint.kind) {
    case SlotConstraint::Kind::Member:
      nodes[intern(constraint.slot)].targets.insert(constraint.function);
      break;
    case SlotConstraint::Kind::Subset: {
      const std::size_t from = intern(constraint.from);
      const std::size_t to = intern(constraint.slot);
      addEdge(from, to);
      break;
    }
    case SlotConstraint::Kind::Open:
      // An explicit source names the open value better than a rule does.
      nodes[intern(constraint.slot)].open =
          OpenSource{.seed = constraint.slot, .detail = constraint.detail};
      break;
    }
  }
}

void Solver::process(std::size_t node) {
  ++steps;
  // Inclusions.
  for (std::size_t i = 0; i < nodes[node].out.size(); ++i) {
    const std::size_t to = nodes[node].out[i];
    bool changed = false;
    for (const std::string &target : nodes[node].targets)
      changed |= nodes[to].targets.insert(target).second;
    if (nodes[node].open && !nodes[to].open) {
      nodes[to].open = nodes[node].open;
      changed = true;
    }
    if (changed)
      push(to);
  }
  // Dynamic call inclusions through this node. `intern` may grow `nodes`,
  // so everything is re-read by index.
  const std::vector<std::string> targets(nodes[node].targets.begin(),
                                         nodes[node].targets.end());
  for (std::size_t k = 0; k < nodes[node].callParams.size(); ++k) {
    const std::size_t callParam = nodes[node].callParams[k];
    const std::uint32_t index = nodes[callParam].key.index;
    for (const std::string &target : targets) {
      const std::size_t param = intern(SlotKey::param(target, index));
      if (addEdge(callParam, param))
        push(callParam);
    }
    // Through an open callee, the arguments reach unknown code.
    if (nodes[node].open)
      push(callParam);
  }
  for (std::size_t k = 0; k < nodes[node].callResults.size(); ++k) {
    const std::size_t callResult = nodes[node].callResults[k];
    for (const std::string &target : targets) {
      const std::size_t result = intern(SlotKey::result(target));
      if (addEdge(result, callResult))
        push(result);
    }
    if (nodes[node].open && !nodes[callResult].open) {
      nodes[callResult].open =
          OpenSource{.seed = nodes[callResult].key,
                     .detail = "the result of a call through '" +
                               nodes[node].key.toString() + "'"};
      push(callResult);
    }
  }
  // A function whose address reaches an open position may be called by
  // code outside the solved program, with any arguments.
  const std::optional<std::size_t> callee = nodes[node].callee;
  const bool escaping = nodes[node].escapingSeed ||
                        (nodes[node].key.kind == SlotKind::CallParam &&
                         callee && nodes[*callee].open.has_value());
  if (escaping) {
    const std::vector<std::string> functions(nodes[node].targets.begin(),
                                             nodes[node].targets.end());
    for (const std::string &function : functions)
      markEscaped(function);
  }
}

void Solver::run() {
  while (!work.empty()) {
    const std::size_t node = work.front();
    work.pop_front();
    queued[node] = false;
    process(node);
  }
}

SlotSolution FnSlots::solve(const SlotRules &rules) const {
  Solver solver(rules);
  solver.load(all);
  solver.run();
  SlotSolution solution;
  solution.rules = rules;
  for (SolverNode &node : solver.nodes)
    solution.states.emplace(
        std::move(node.key),
        SlotSolution::SlotState{.targets = std::move(node.targets),
                                .open = std::move(node.open)});
  solution.escaped = std::move(solver.escaped);
  solution.stepCount = solver.steps;
  return solution;
}

//===----------------------------------------------------------------------===//
// Queries
//===----------------------------------------------------------------------===//

const std::set<std::string> &SlotSolution::targets(const SlotKey &slot) const {
  static const std::set<std::string> None;
  const auto found = states.find(slot);
  return found == states.end() ? None : found->second.targets;
}

std::optional<OpenSource> SlotSolution::openSource(const SlotKey &slot) const {
  if (const auto found = states.find(slot); found != states.end())
    return found->second.open;
  // A slot no constraint mentions holds no value from the solved program;
  // the rules alone decide.
  switch (slot.kind) {
  case SlotKind::Param:
    if (auto open = seedOf(slot, rules).open)
      return open;
    if (escaped.contains(slot.scope))
      return escapedSource(slot, rules);
    return std::nullopt;
  case SlotKind::CallResult: {
    const auto calleeKey = slot.callee();
    if (calleeKey && isOpen(*calleeKey))
      return OpenSource{.seed = slot,
                        .detail = "the result of a call through '" +
                                  calleeKey->toString() + "'"};
    return std::nullopt;
  }
  case SlotKind::Field:
  case SlotKind::Global:
  case SlotKind::Static:
  case SlotKind::Result:
  case SlotKind::Local:
  case SlotKind::CallParam:
    return seedOf(slot, rules).open;
  }
  return std::nullopt;
}

bool SlotSolution::escapes(std::string_view function) const {
  return escaped.contains(function);
}

CallResolution SlotSolution::resolveCall(const SlotKey &callee) const {
  const std::set<std::string> &found = targets(callee);
  CallResolution resolution{
      .targets = std::vector<std::string>(found.begin(), found.end()),
      .open = openSource(callee)};
  if (resolution.open)
    resolution.kind = resolution.targets.empty() ? IndirectCallKind::OpenUnknown
                                                 : IndirectCallKind::OpenKnown;
  else if (resolution.targets.empty())
    resolution.kind = IndirectCallKind::ClosedEmpty;
  else
    resolution.kind = resolution.targets.size() == 1
                          ? IndirectCallKind::ClosedSingle
                          : IndirectCallKind::ClosedJoin;
  return resolution;
}

std::vector<SlotKey> SlotSolution::slots() const {
  std::vector<SlotKey> keys;
  keys.reserve(states.size());
  for (const auto &[key, state] : states)
    keys.push_back(key);
  return keys;
}

//===----------------------------------------------------------------------===//
// Building, merging, export
//===----------------------------------------------------------------------===//

void FnSlots::add(SlotConstraint constraint) {
  all.insert(std::move(constraint));
}

void FnSlots::addMember(std::string function, SlotKey slot) {
  add(SlotConstraint::member(std::move(function), std::move(slot)));
}

void FnSlots::addSubset(SlotKey from, SlotKey to) {
  add(SlotConstraint::subset(std::move(from), std::move(to)));
}

void FnSlots::addOpen(SlotKey slot, std::string detail) {
  add(SlotConstraint::open(std::move(slot), std::move(detail)));
}

void FnSlots::addDirectCall(std::string_view callee,
                            std::span<const std::optional<SlotKey>> arguments,
                            const std::optional<SlotKey> &receiver) {
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (arguments[i])
      addSubset(*arguments[i], SlotKey::param(std::string(callee),
                                              static_cast<std::uint32_t>(i)));
  }
  if (receiver)
    addSubset(SlotKey::result(std::string(callee)), *receiver);
}

void FnSlots::addIndirectCall(const SlotKey &callee,
                              std::span<const std::optional<SlotKey>> arguments,
                              const std::optional<SlotKey> &receiver) {
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (arguments[i])
      addSubset(*arguments[i],
                SlotKey::callParam(callee, static_cast<std::uint32_t>(i)));
  }
  if (receiver)
    addSubset(SlotKey::callResult(callee), *receiver);
}

void FnSlots::merge(const FnSlots &other) {
  all.insert(other.all.begin(), other.all.end());
}

namespace {
/// What may flow out of a TU-private slot: non-local slots it copies,
/// functions stored in it, and outside values.
struct LocalValue {
  std::set<SlotKey> slots;
  std::set<std::string> functions;
  std::set<std::string> opens;

  /// Adds `other`; whether anything was new.
  bool absorb(const LocalValue &other) {
    const std::size_t before = slots.size() + functions.size() + opens.size();
    slots.insert(other.slots.begin(), other.slots.end());
    functions.insert(other.functions.begin(), other.functions.end());
    opens.insert(other.opens.begin(), other.opens.end());
    return slots.size() + functions.size() + opens.size() != before;
  }
};
} // namespace

/// What reading `key` yields, in terms of non-local slots, functions and
/// outside values. `values` holds the `local` slots' values so far.
static LocalValue valueOf(const SlotKey &key,
                          const std::map<SlotKey, LocalValue> &values) {
  LocalValue value;
  if (!key.isLocal()) {
    value.slots.insert(key);
    return value;
  }
  if (key.kind == SlotKind::Local) {
    if (const auto found = values.find(key); found != values.end())
      value = found->second;
    return value;
  }
  if (key.kind == SlotKind::CallResult) {
    // A call through a local calls every callee the local holds.
    const LocalValue callee = valueOf(*key.callee(), values);
    for (const SlotKey &slot : callee.slots)
      value.slots.insert(SlotKey::callResult(slot));
    for (const std::string &function : callee.functions)
      value.slots.insert(SlotKey::result(function));
    if (!callee.opens.empty())
      value.slots.insert(SlotKey::result(std::string(UnknownFunction)));
  }
  // A `call-param` key is only ever written.
  return value;
}

/// The non-local slots a write to `key` reaches.
static std::vector<SlotKey>
sinksOf(const SlotKey &key, const std::map<SlotKey, LocalValue> &values) {
  if (!key.isLocal())
    return {key};
  std::vector<SlotKey> sinks;
  if (key.kind == SlotKind::CallParam) {
    const LocalValue callee = valueOf(*key.callee(), values);
    for (const SlotKey &slot : callee.slots)
      sinks.push_back(SlotKey::callParam(slot, key.index));
    for (const std::string &function : callee.functions)
      sinks.push_back(SlotKey::param(function, key.index));
    if (!callee.opens.empty())
      sinks.push_back(SlotKey::param(std::string(UnknownFunction), key.index));
  }
  // Writes to `local` slots are accounted in their values.
  return sinks;
}

FnSlots FnSlots::withoutLocals() const {
  std::map<SlotKey, LocalValue> values;
  for (const SlotConstraint &constraint : all) {
    if (constraint.slot.kind != SlotKind::Local)
      continue;
    if (constraint.kind == SlotConstraint::Kind::Member)
      values[constraint.slot].functions.insert(constraint.function);
    else if (constraint.kind == SlotConstraint::Kind::Open)
      values[constraint.slot].opens.insert(constraint.detail);
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (const SlotConstraint &constraint : all) {
      if (constraint.kind != SlotConstraint::Kind::Subset ||
          constraint.slot.kind != SlotKind::Local)
        continue;
      const LocalValue incoming = valueOf(constraint.from, values);
      changed |= values[constraint.slot].absorb(incoming);
    }
  }

  FnSlots exported;
  for (const SlotConstraint &constraint : all) {
    const std::vector<SlotKey> sinks = sinksOf(constraint.slot, values);
    if (sinks.empty())
      continue;
    switch (constraint.kind) {
    case SlotConstraint::Kind::Member:
      for (const SlotKey &sink : sinks)
        exported.addMember(constraint.function, sink);
      break;
    case SlotConstraint::Kind::Open:
      for (const SlotKey &sink : sinks)
        exported.addOpen(sink, constraint.detail);
      break;
    case SlotConstraint::Kind::Subset: {
      const LocalValue source = valueOf(constraint.from, values);
      for (const SlotKey &sink : sinks) {
        for (const SlotKey &slot : source.slots)
          exported.addSubset(slot, sink);
        for (const std::string &function : source.functions)
          exported.addMember(function, sink);
        for (const std::string &detail : source.opens)
          exported.addOpen(sink, detail);
      }
      break;
    }
    }
  }
  return exported;
}

std::vector<SlotRow> FnSlots::rows() const {
  std::map<SlotKey, SlotRow> bySlot;
  for (const SlotConstraint &constraint : all) {
    SlotRow &row = bySlot[constraint.slot];
    row.slot = constraint.slot;
    switch (constraint.kind) {
    case SlotConstraint::Kind::Member:
      row.targets.push_back(constraint.function);
      break;
    case SlotConstraint::Kind::Subset:
      row.sources.push_back(constraint.from);
      break;
    case SlotConstraint::Kind::Open:
      // Constraints are sorted, so the first detail is the least.
      if (!row.open)
        row.open = constraint.detail;
      break;
    }
  }
  std::vector<SlotRow> rows;
  rows.reserve(bySlot.size());
  for (auto &[slot, row] : bySlot)
    rows.push_back(std::move(row));
  return rows;
}

FnSlots FnSlots::fromRows(std::span<const SlotRow> rows) {
  FnSlots slots;
  for (const SlotRow &row : rows) {
    for (const std::string &target : row.targets)
      slots.addMember(target, row.slot);
    for (const SlotKey &source : row.sources)
      slots.addSubset(source, row.slot);
    if (row.open)
      slots.addOpen(row.slot, *row.open);
  }
  return slots;
}

static std::string escapeField(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char c : text) {
    switch (c) {
    case '\\':
      escaped += "\\\\";
      break;
    case '\t':
      escaped += "\\t";
      break;
    case '\n':
      escaped += "\\n";
      break;
    default:
      escaped += c;
      break;
    }
  }
  return escaped;
}

static std::optional<std::string> unescapeField(std::string_view text) {
  std::string plain;
  plain.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\') {
      plain += text[i];
      continue;
    }
    if (++i == text.size())
      return std::nullopt;
    switch (text[i]) {
    case '\\':
      plain += '\\';
      break;
    case 't':
      plain += '\t';
      break;
    case 'n':
      plain += '\n';
      break;
    default:
      return std::nullopt;
    }
  }
  return plain;
}

std::string FnSlots::print() const {
  std::string text;
  for (const SlotConstraint &constraint : all) {
    switch (constraint.kind) {
    case SlotConstraint::Kind::Member:
      text += "member\t" + escapeField(constraint.function) + "\t" +
              escapeField(constraint.slot.toString());
      break;
    case SlotConstraint::Kind::Subset:
      text += "subset\t" + escapeField(constraint.from.toString()) + "\t" +
              escapeField(constraint.slot.toString());
      break;
    case SlotConstraint::Kind::Open:
      text += "open\t" + escapeField(constraint.slot.toString()) + "\t" +
              escapeField(constraint.detail);
      break;
    }
    text += '\n';
  }
  return text;
}

std::optional<FnSlots> FnSlots::parse(std::string_view text,
                                      std::string *error) {
  FnSlots slots;
  std::size_t lineNumber = 0;
  const auto fail = [&](const std::string &why) -> std::optional<FnSlots> {
    if (error != nullptr)
      *error = "line " + std::to_string(lineNumber) + ": " + why;
    return std::nullopt;
  };
  while (!text.empty()) {
    ++lineNumber;
    const std::size_t newline = text.find('\n');
    const std::string_view line = text.substr(0, newline);
    text.remove_prefix(newline == std::string_view::npos ? text.size()
                                                         : newline + 1);
    if (line.empty())
      continue;
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
      const std::size_t tab = line.find('\t', start);
      const auto field = unescapeField(line.substr(
          start, tab == std::string_view::npos ? std::string_view::npos
                                               : tab - start));
      if (!field)
        return fail("malformed escape");
      fields.push_back(*field);
      if (tab == std::string_view::npos)
        break;
      start = tab + 1;
    }
    if (fields.size() != 3)
      return fail("expected three fields");
    if (fields[0] == "member") {
      const auto slot = SlotKey::parse(fields[2]);
      if (!slot || fields[1].empty())
        return fail("malformed member constraint");
      slots.addMember(fields[1], *slot);
    } else if (fields[0] == "subset") {
      const auto from = SlotKey::parse(fields[1]);
      const auto to = SlotKey::parse(fields[2]);
      if (!from || !to)
        return fail("malformed subset constraint");
      slots.addSubset(*from, *to);
    } else if (fields[0] == "open") {
      const auto slot = SlotKey::parse(fields[1]);
      if (!slot)
        return fail("malformed open constraint");
      slots.addOpen(*slot, fields[2]);
    } else {
      return fail("unknown constraint '" + fields[0] + "'");
    }
  }
  return slots;
}

} // namespace weavec::core
