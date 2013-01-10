// Copyright 2010-2012 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "constraint_solver/routing.h"

#include <math.h>
#include <stddef.h>
#include <string.h>
#include <algorithm>
#include "base/hash.h"
#include <map>

#include "base/callback.h"
#include "base/commandlineflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/bitmap.h"
#include "base/concise_iterator.h"
#include "base/map-util.h"
#include "base/small_map.h"
#include "base/stl_util.h"
#include "base/hash.h"
#include "constraint_solver/constraint_solveri.h"
#include "graph/linear_assignment.h"

namespace operations_research {
class LocalSearchPhaseParameters;
}  // namespace operations_research

// Neighborhood deactivation
DEFINE_bool(routing_no_lns, false,
            "Routing: forbids use of Large Neighborhood Search.");
DEFINE_bool(routing_no_relocate, false,
            "Routing: forbids use of Relocate neighborhood.");
DEFINE_bool(routing_no_exchange, false,
            "Routing: forbids use of Exchange neighborhood.");
DEFINE_bool(routing_no_cross, false,
            "Routing: forbids use of Cross neighborhood.");
DEFINE_bool(routing_no_2opt, false,
            "Routing: forbids use of 2Opt neighborhood.");
DEFINE_bool(routing_no_oropt, false,
            "Routing: forbids use of OrOpt neighborhood.");
DEFINE_bool(routing_no_make_active, false,
            "Routing: forbids use of MakeActive/SwapActive/MakeInactive "
            "neighborhoods.");
DEFINE_bool(routing_no_lkh, false,
            "Routing: forbids use of LKH neighborhood.");
DEFINE_bool(routing_no_tsp, true,
            "Routing: forbids use of TSPOpt neighborhood.");
DEFINE_bool(routing_no_tsplns, true,
            "Routing: forbids use of TSPLNS neighborhood.");
DEFINE_bool(routing_use_extended_swap_active, false,
            "Routing: use extended version of SwapActive neighborhood.");

// Search limits
DEFINE_int64(routing_solution_limit, kint64max,
             "Routing: number of solutions limit.");
DEFINE_int64(routing_time_limit, kint64max,
             "Routing: time limit in ms.");
DEFINE_int64(routing_lns_time_limit, 100,
             "Routing: time limit in ms for LNS sub-decisionbuilder.");

// Meta-heuritics
DEFINE_bool(routing_guided_local_search, false, "Routing: use GLS.");
DEFINE_double(routing_guided_local_search_lamda_coefficient, 0.1,
              "Lamda coefficient in GLS.");
DEFINE_bool(routing_simulated_annealing, false,
            "Routing: use simulated annealing.");
DEFINE_bool(routing_tabu_search, false, "Routing: use tabu search.");

// Search control
DEFINE_bool(routing_dfs, false,
            "Routing: use a complete deoth-first search.");
DEFINE_string(routing_first_solution, "",
              "Routing: first solution heuristic; possible values include "
              "Default, GlobalCheapestArc, LocalCheapestArc, PathCheapestArc, "
              "BestInsertion, Savings, Sweep. "
              "See ParseRoutingStrategy() in the code to get a full list.");
DEFINE_bool(routing_use_first_solution_dive, false,
            "Dive (left-branch) for first solution.");
DEFINE_int64(routing_optimization_step, 1, "Optimization step.");

// Filtering control
DEFINE_bool(routing_use_objective_filter, true,
            "Use objective filter to speed up local search.");
DEFINE_bool(routing_use_path_cumul_filter, true,
            "Use PathCumul constraint filter to speed up local search.");
DEFINE_bool(routing_use_pickup_and_delivery_filter, true,
            "Use filter which filters precedence and same route constraints.");
DEFINE_bool(routing_use_disjunction_filter, true,
            "Use filter which filters node disjunction constraints.");
DEFINE_double(savings_route_shape_parameter, 1.0,
              "Coefficient of the added arc in the savings definition."
              "Variation of this parameter may provide heuristic solutions "
              "which are closer to the optimal solution than otherwise obtained"
              " via the traditional algorithm where it is equal to 1.");
DEFINE_int64(savings_filter_neighbors, 0,
             "Use filter which filters the pair of orders considered in "
             "Savings first solution heuristic by limiting the number "
             "of neighbors considered for each node.");
DEFINE_int64(savings_filter_radius, 0,
             "Use filter which filters the pair of orders considered in "
             "Savings first solution heuristic by limiting the distance "
             "up to which a neighbor is considered for each node.");
DEFINE_int64(sweep_sectors, 1,
             "The number of sectors the space is divided before it is sweeped "
             "by the ray.");

// Propagation control
DEFINE_bool(routing_use_light_propagation, false,
            "Use constraints with light propagation in routing model.");

// Misc
DEFINE_bool(routing_cache_callbacks, false, "Cache callback calls.");
DEFINE_int64(routing_max_cache_size, 1000,
             "Maximum cache size when callback caching is on.");
DEFINE_bool(routing_trace, false, "Routing: trace search.");
DEFINE_bool(routing_search_trace, false,
            "Routing: use SearchTrace for monitoring search.");
DEFINE_bool(routing_use_homogeneous_costs, true,
            "Routing: use homogeneous cost model when possible.");
DEFINE_bool(routing_check_compact_assignment, true,
            "Routing::CompactAssignment calls Solver::CheckAssignment on the "
            "compact assignment.");

#if defined(_MSC_VER)
namespace stdext {
template<> size_t hash_value<operations_research::RoutingModel::NodeIndex>(
    const operations_research::RoutingModel::NodeIndex& a) {
  return a.value();
}
}  //  namespace stdext
#endif  // _MSC_VER

namespace operations_research {

// Set of "light" constraints, well-suited for use within Local Search.
// These constraints are "checking" constraints, only triggered on WhenBound
// events. The provide very little (or no) domain filtering.
// TODO(user): Move to core constraintsolver library.

// Light one-dimension function-based element constraint ensuring:
// var == values(index).
// Doesn't perform bound reduction of the resulting variable until the index
// variable is bound.
// Ownership of the 'values' callback is taken by the constraint.
class LightFunctionElementConstraint : public Constraint {
 public:
  LightFunctionElementConstraint(Solver* const solver,
                                 IntVar* const var,
                                 IntVar* const index,
                                 ResultCallback1<int64, int64>* const values)
      : Constraint(solver), var_(var), index_(index), values_(values) {
    CHECK_NOTNULL(values_);
    values_->CheckIsRepeatable();
  }
  virtual ~LightFunctionElementConstraint() {}

  virtual void Post() {
    Demon* demon =
        MakeConstraintDemon0(solver(),
                             this,
                             &LightFunctionElementConstraint::IndexBound,
                             "IndexBound");
    index_->WhenBound(demon);
  }

  virtual void InitialPropagate() {
    if (index_->Bound()) {
      IndexBound();
    }
  }

  virtual string DebugString() const {
    return "LightFunctionElementConstraint";
  }

  void Accept(ModelVisitor* const visitor) const {
    LOG(FATAL) << "Not yet implemented";
    // TODO(user): IMPLEMENT ME.
  }

 private:
  void IndexBound() {
    var_->SetValue(values_->Run(index_->Value()));
  }

  IntVar* const var_;
  IntVar* const index_;
  scoped_ptr<ResultCallback1<int64, int64> > values_;
};

Constraint* MakeLightElement(Solver* const solver,
                             IntVar* const var,
                             IntVar* const index,
                             ResultCallback1<int64, int64>* const values) {
  return solver->RevAlloc(
      new LightFunctionElementConstraint(solver, var, index, values));
}

// Light two-dimension function-based element constraint ensuring:
// var == values(index1, index2).
// Doesn't perform bound reduction of the resulting variable until the index
// variables are bound.
// Ownership of the 'values' callback is taken by the constraint.
class LightFunctionElement2Constraint : public Constraint {
 public:
  LightFunctionElement2Constraint(
      Solver* const solver,
      IntVar* const var,
      IntVar* const index1,
      IntVar* const index2,
      ResultCallback2<int64, int64, int64>* const values)
      : Constraint(solver),
        var_(var), index1_(index1), index2_(index2), values_(values) {
    CHECK_NOTNULL(values_);
    values_->CheckIsRepeatable();
  }
  virtual ~LightFunctionElement2Constraint() {}
  virtual void Post() {
    Demon* demon =
        MakeConstraintDemon0(solver(),
                             this,
                             &LightFunctionElement2Constraint::IndexBound,
                             "IndexBound");
    index1_->WhenBound(demon);
    index2_->WhenBound(demon);
  }
  virtual void InitialPropagate() {
    IndexBound();
  }

  virtual string DebugString() const {
    return "LightFunctionElement2Constraint";
  }

  void Accept(ModelVisitor* const visitor) const {
    LOG(FATAL) << "Not yet implemented";
    // TODO(user): IMPLEMENT ME.
  }

 private:
  void IndexBound() {
    if (index1_->Bound() && index2_->Bound()) {
      var_->SetValue(values_->Run(index1_->Value(), index2_->Value()));
    }
  }

  IntVar* const var_;
  IntVar* const index1_;
  IntVar* const index2_;
  scoped_ptr<ResultCallback2<int64, int64, int64> > values_;
};

Constraint* MakeLightElement2(
    Solver* const solver,
    IntVar* const var,
    IntVar* const index1,
    IntVar* const index2,
    ResultCallback2<int64, int64, int64>* const values) {
  return solver->RevAlloc(
      new LightFunctionElement2Constraint(solver, var, index1, index2, values));
}

// Pair-based neighborhood operators, designed to move nodes by pairs (pairs
// are static and given). These neighborhoods are very useful for Pickup and
// Delivery problems where pickup and delivery nodes must remain on the same
// route.
// TODO(user): Add option to prune neighbords where the order of node pairs
//                is violated (ie precedence between pickup and delivery nodes).
// TODO(user): Move this to local_search.cc if it's generic enough.
// TODO(user): Detect pairs automatically by parsing the constraint model;
//                we could then get rid of the pair API in the RoutingModel
//                class.

// Operator which inserts pairs of inactive nodes into a path.
// Possible neighbors for the path 1 -> 2 -> 3 with pair (A, B) inactive
// (where 1 and 3 are first and last nodes of the path) are:
//   1 -> [A] -> [B] ->  2  ->  3
//   1 -> [B] ->  2 ->  [A] ->  3
//   1 -> [A] ->  2  -> [B] ->  3
//   1 ->  2  -> [A] -> [B] ->  3
// Note that this operator does not expicitely insert the nodes of a pair one
// after the other which forbids the following solutions:
//   1 -> [B] -> [A] ->  2  ->  3
//   1 ->  2  -> [B] -> [A] ->  3
// which can only be obtained by inserting A after B.
class MakePairActiveOperator : public PathOperator {
 public:
  MakePairActiveOperator(const IntVar* const* vars,
                         const IntVar* const* secondary_vars,
                         const RoutingModel::NodePairs& pairs,
                         int size)
      : PathOperator(vars, secondary_vars, size, 2),
        inactive_pair_(0),
        pairs_(pairs) {}
  virtual ~MakePairActiveOperator() {}
  virtual bool MakeNextNeighbor(Assignment* delta, Assignment* deltadelta);
  virtual bool MakeNeighbor();

 protected:
  virtual bool OnSamePathAsPreviousBase(int64 base_index) {
    // Both base nodes have to be on the same path since they represent the
    // nodes after which unactive node pairs will be moved.
    return true;
  }
  virtual int64 GetBaseNodeRestartPosition(int base_index) {
    // Base node 1 must be after base node 0 if they are both on the same path.
    if (base_index == 0
        || StartNode(base_index) != StartNode(base_index - 1)) {
      return StartNode(base_index);
    } else {
      return BaseNode(base_index - 1);
    }
  }

 private:
  virtual void OnNodeInitialization();

  int inactive_pair_;
  RoutingModel::NodePairs pairs_;
};

void MakePairActiveOperator::OnNodeInitialization() {
  for (int i = 0; i < pairs_.size(); ++i) {
    if (IsInactive(pairs_[i].first) && IsInactive(pairs_[i].second)) {
      inactive_pair_ = i;
      return;
    }
  }
  inactive_pair_ = pairs_.size();
}

bool MakePairActiveOperator::MakeNextNeighbor(Assignment* delta,
                                              Assignment* deltadelta) {
  while (inactive_pair_ < pairs_.size()) {
    if (!IsInactive(pairs_[inactive_pair_].first)
        || !IsInactive(pairs_[inactive_pair_].second)
        || !PathOperator::MakeNextNeighbor(delta, deltadelta)) {
      ResetPosition();
      ++inactive_pair_;
    } else {
      return true;
    }
  }
  return false;
}

bool MakePairActiveOperator::MakeNeighbor() {
  DCHECK_EQ(StartNode(0), StartNode(1));
  // Inserting the second node of the pair before the first one which ensures
  // that the only solutions where both nodes are next to each other have the
  // first node before the second (the move is not symmetric and doing it this
  // way ensures that a potential precedence constraint between the nodes of the
  // pair is not violated).
  return MakeActive(pairs_[inactive_pair_].second, BaseNode(1))
      && MakeActive(pairs_[inactive_pair_].first, BaseNode(0));
}

LocalSearchOperator* MakePairActive(Solver* const solver,
                                    const IntVar* const* vars,
                                    const IntVar* const* secondary_vars,
                                    const RoutingModel::NodePairs& pairs,
                                    int size) {
  return solver->RevAlloc(new MakePairActiveOperator(vars,
                                                     secondary_vars,
                                                     pairs,
                                                     size));
}

// Operator which moves a pair of nodes to another position.
// Possible neighbors for the path 1 -> A -> B -> 2 -> 3 (where (1, 3) are
// first and last nodes of the path and can therefore not be moved, and (A, B)
// is a pair of nodes):
//   1 -> [A] ->  2  -> [B] -> 3
//   1 ->  2  -> [A] -> [B] -> 3
//   1 -> [B] -> [A] ->  2  -> 3
//   1 -> [B] ->  2  -> [A] -> 3
//   1 ->  2  -> [B] -> [A] -> 3
class PairRelocateOperator : public PathOperator {
 public:
  PairRelocateOperator(const IntVar* const* vars,
                       const IntVar* const* secondary_vars,
                       const RoutingModel::NodePairs& pairs,
                       int size)
      : PathOperator(vars, secondary_vars, size, 3) {
    int64 index_max = 0;
    for (int i = 0; i < size; ++i) {
      index_max = std::max(index_max, vars[i]->Max());
    }
    prevs_.resize(index_max + 1, -1);
    is_first_.resize(index_max + 1, false);
    int max_pair_index = -1;
    for (int i = 0; i < pairs.size(); ++i) {
      max_pair_index = std::max(max_pair_index, pairs[i].first);
      max_pair_index = std::max(max_pair_index, pairs[i].second);
    }
    pairs_.resize(max_pair_index + 1, -1);
    for (int i = 0; i < pairs.size(); ++i) {
      pairs_[pairs[i].first] = pairs[i].second;
      pairs_[pairs[i].second] = pairs[i].first;
      is_first_[pairs[i].first] = true;
    }
  }
  virtual ~PairRelocateOperator() {}
  virtual bool MakeNeighbor();

 protected:
  virtual bool OnSamePathAsPreviousBase(int64 base_index) {
    // Base node of index 0 and its sibling are the pair of nodes to move.
    // They are being moved after base nodes index 1 and 2 which must be on the
    // same path.
    return base_index == 2;
  }
  virtual int64 GetBaseNodeRestartPosition(int base_index) {
    // Base node 2 must be after base node 1 if they are both on the same path
    // and if the operator is about to move a "second" node (second node in a
    // node pair, ie. a delivery in a pickup and delivery pair).
    DCHECK_LT(BaseNode(0), is_first_.size());
    const bool moving_first = is_first_[BaseNode(0)];
    if (!moving_first
        && base_index == 2
        && StartNode(base_index) == StartNode(base_index - 1)) {
      return BaseNode(base_index - 1);
    } else {
      return StartNode(base_index);
    }
  }

 private:
  virtual void OnNodeInitialization();
  virtual bool RestartAtPathStartOnSynchronize() {
    return true;
  }

  std::vector<int> pairs_;
  std::vector<int> prevs_;
  std::vector<bool> is_first_;
};

bool PairRelocateOperator::MakeNeighbor() {
  DCHECK_EQ(StartNode(1), StartNode(2));
  const int64 prev = prevs_[BaseNode(0)];
  if (prev < 0) {
    return false;
  }
  const int sibling = BaseNode(0) < pairs_.size() ? pairs_[BaseNode(0)] : -1;
  if (sibling < 0) {
    return false;
  }
  const int64 prev_sibling = prevs_[sibling];
  if (prev_sibling < 0) {
    return false;
  }
  return MoveChain(prev_sibling, sibling, BaseNode(1))
      && MoveChain(prev, BaseNode(0), BaseNode(2));
}

void PairRelocateOperator::OnNodeInitialization() {
  for (int i = 0; i < number_of_nexts(); ++i) {
    prevs_[Next(i)] = i;
  }
}

LocalSearchOperator* MakePairRelocate(Solver* const solver,
                                      const IntVar* const* vars,
                                      const IntVar* const* secondary_vars,
                                      const RoutingModel::NodePairs& pairs,
                                      int size) {
  return solver->RevAlloc(new PairRelocateOperator(vars,
                                                   secondary_vars,
                                                   pairs,
                                                   size));
}

// Cached callbacks

class RoutingCache {
 public:
  RoutingCache(RoutingModel::NodeEvaluator2* callback, int size)
      : cached_(size), cache_(size), callback_(callback) {
    for (RoutingModel::NodeIndex i(0); i < RoutingModel::NodeIndex(size); ++i) {
      cached_[i].resize(size, false);
      cache_[i].resize(size, 0);
    }
    callback->CheckIsRepeatable();
  }
  int64 Run(RoutingModel::NodeIndex i, RoutingModel::NodeIndex j) {
    // This method does lazy caching of results of callbacks: first
    // checks if it has been run with these parameters before, and
    // returns previous result if so, or runs underlaying callback and
    // stores its result.
    // Not MT-safe.
    if (cached_[i][j]) {
      return cache_[i][j];
    } else {
      const int64 cached_value = callback_->Run(i, j);
      cached_[i][j] = true;
      cache_[i][j] = cached_value;
      return cached_value;
    }
  }

 private:
  ITIVector<RoutingModel::NodeIndex,
            ITIVector<RoutingModel::NodeIndex, bool> > cached_;
  ITIVector<RoutingModel::NodeIndex,
            ITIVector<RoutingModel::NodeIndex, int64> > cache_;
  scoped_ptr<RoutingModel::NodeEvaluator2> callback_;
};

namespace {

// Node disjunction filter class.

class NodeDisjunctionFilter : public IntVarLocalSearchFilter {
 public:
  explicit NodeDisjunctionFilter(const RoutingModel& routing_model)
      : IntVarLocalSearchFilter(routing_model.Nexts().data(),
                                routing_model.Nexts().size()),
        routing_model_(routing_model),
        active_per_disjunction_(routing_model.GetNumberOfDisjunctions(), 0),
        penalty_value_(0),
        current_objective_value_(0) {
  }
  virtual bool Accept(const Assignment* delta,
                      const Assignment* deltadelta) {
    const int64 kUnassigned = -1;
    const Assignment::IntContainer& container = delta->IntVarContainer();
    const int delta_size = container.Size();
    small_map<std::map<RoutingModel::DisjunctionIndex, int> >
        disjunction_active_deltas;
    bool lns_detected = false;
    for (int i = 0; i < delta_size; ++i) {
      const IntVarElement& new_element = container.Element(i);
      const IntVar* const var = new_element.Var();
      int64 index = kUnassigned;
      if (FindIndex(var, &index)) {
        RoutingModel::DisjunctionIndex disjunction_index(kUnassigned);
        if (routing_model_.GetDisjunctionIndexFromVariableIndex(
                index,
                &disjunction_index)) {
          const bool was_inactive = (Value(index) == index);
          const bool is_inactive =
              (new_element.Min() <= index && new_element.Max() >= index);
          if (new_element.Min() != new_element.Max()) {
            lns_detected = true;
          }
          if (was_inactive && !is_inactive) {
            ++LookupOrInsert(&disjunction_active_deltas, disjunction_index, 0);
          } else if (!was_inactive && is_inactive) {
            --LookupOrInsert(&disjunction_active_deltas, disjunction_index, 0);
          }
        }
      }
    }
    int64 new_objective_value = current_objective_value_ + penalty_value_;
    for (ConstIter<small_map<std::map<RoutingModel::DisjunctionIndex, int> > >
             it(disjunction_active_deltas); !it.at_end(); ++it) {
      const int active_nodes = active_per_disjunction_[it->first] + it->second;
      if (active_nodes > 1) {
        return false;
      }
      if (!lns_detected) {
        const int64 penalty = routing_model_.GetDisjunctionPenalty(it->first);
        if (it->second < 0) {
          if (penalty < 0) {
            return false;
          } else {
            new_objective_value += penalty;
          }
        } else if (it->second > 0) {
          new_objective_value -= penalty;
        }
      }
    }
    if (lns_detected) {
      return true;
    } else {
      IntVar* const cost_var = routing_model_.CostVar();
      return new_objective_value <= cost_var->Max()
          && new_objective_value >= cost_var->Min();
    }
  }
  void InjectObjectiveValue(int64 objective_value) {
    current_objective_value_ = objective_value;
  }

 private:
  virtual void OnSynchronize() {
    for (RoutingModel::DisjunctionIndex i(0);
         i < active_per_disjunction_.size();
         ++i) {
      active_per_disjunction_[i] = 0;
      std::vector<int> disjunction_nodes;
      routing_model_.GetDisjunctionIndices(i, &disjunction_nodes);
      for (int j = 0; j < disjunction_nodes.size(); ++j) {
        const int node = disjunction_nodes[j];
        if (Value(node) != node) {
          ++active_per_disjunction_[i];
        }
      }
    }
    penalty_value_ = 0;
    for (RoutingModel::DisjunctionIndex i(0);
         i < active_per_disjunction_.size();
         ++i) {
      const int64 penalty = routing_model_.GetDisjunctionPenalty(i);
      if (active_per_disjunction_[i] == 0 && penalty > 0) {
        penalty_value_ += penalty;
      }
    }
  }

  const RoutingModel& routing_model_;
  ITIVector<RoutingModel::DisjunctionIndex, int> active_per_disjunction_;
  int64 penalty_value_;
  int64 current_objective_value_;
};
}  // namespace

LocalSearchFilter* MakeNodeDisjunctionFilter(
    const RoutingModel& routing_model) {
  return routing_model.solver()->RevAlloc(
      new NodeDisjunctionFilter(routing_model));
}

namespace {

// Generic path-based filter class.
// TODO(user): Move all filters to local_search.cc.

class BasePathFilter : public IntVarLocalSearchFilter {
 public:
  BasePathFilter(const IntVar* const* nexts,
                 int nexts_size,
                 int next_domain_size,
                 const string& name);
  virtual ~BasePathFilter() {}
  virtual bool Accept(const Assignment* delta, const Assignment* deltadelta);
  virtual void OnSynchronize();

 protected:
  static const int64 kUnassigned;

  int64 GetNext(const Assignment::IntContainer& container, int64 node) const;

 private:
  virtual bool AcceptPath(const Assignment::IntContainer& container,
                          int64 path_start) = 0;

  std::vector<int64> node_path_starts_;
  const string name_;
};

const int64 BasePathFilter::kUnassigned = -1;

BasePathFilter::BasePathFilter(const IntVar* const* nexts,
                               int nexts_size,
                               int next_domain_size,
                               const string& name)
    : IntVarLocalSearchFilter(nexts, nexts_size),
      node_path_starts_(next_domain_size),
      name_(name) {}

bool BasePathFilter::Accept(const Assignment* delta,
                            const Assignment* deltadelta) {
  const Assignment::IntContainer& container = delta->IntVarContainer();
  const int delta_size = container.Size();
  // Determining touched paths. Number of touched paths should be very small
  // given the set of available operators (1 or 2 paths), so performing
  // a linear search to find an element is faster than using a set.
  std::vector<int64> touched_paths;
  for (int i = 0; i < delta_size; ++i) {
    const IntVarElement& new_element = container.Element(i);
    const IntVar* const var = new_element.Var();
    int64 index = kUnassigned;
    if (FindIndex(var, &index)) {
      const int64 start = node_path_starts_[index];
      if (start != kUnassigned
          && find(touched_paths.begin(), touched_paths.end(), start)
          == touched_paths.end()) {
        touched_paths.push_back(start);
      }
    }
  }
  // Checking feasibility of touched paths.
  const int touched_paths_size = touched_paths.size();
  for (int i = 0; i < touched_paths_size; ++i) {
    if (!AcceptPath(container, touched_paths[i])) {
      return false;
    }
  }
  return true;
}

void BasePathFilter::OnSynchronize() {
  const int nexts_size = Size();
  // Detecting path starts, used to track which node belongs to which path.
  std::vector<int64> path_starts;
  // TODO(user): Replace Bitmap by std::vector<bool> if it's as fast.
  Bitmap has_prevs(nexts_size, false);
  for (int i = 0; i < nexts_size; ++i) {
    const int next = Value(i);
    if (next < nexts_size) {
      has_prevs.Set(next, true);
    }
  }
  for (int i = 0; i < nexts_size; ++i) {
    if (!has_prevs.Get(i)) {
      path_starts.push_back(i);
    }
  }
  // Marking unactive nodes (which are not on a path).
  node_path_starts_.assign(node_path_starts_.size(), kUnassigned);
  // Marking nodes on a path and storing next values.
  for (int i = 0; i < path_starts.size(); ++i) {
    const int64 start = path_starts[i];
    int node = start;
    node_path_starts_[node] = start;
    int next = Value(node);
    while (next < nexts_size) {
      node = next;
      node_path_starts_[node] = start;
      next = Value(node);
    }
    node_path_starts_[next] = start;
  }
}

int64 BasePathFilter::GetNext(const Assignment::IntContainer& container,
                              int64 node) const {
  const IntVar* const next_var = Var(node);
  int64 next = Value(node);
  if (container.Contains(next_var)) {
    const IntVarElement& element = container.Element(next_var);
    if (element.Bound()) {
      next = element.Value();
    } else {
      return kUnassigned;
    }
  }
  return next;
}

// PathCumul filter.

class PathCumulFilter : public BasePathFilter {
 public:
  // Does not take ownership of evaluator.
  PathCumulFilter(const IntVar* const* nexts,
                  int nexts_size,
                  const IntVar* const* cumuls,
                  int cumuls_size,
                  Solver::IndexEvaluator2* evaluator,
                  const string& name);
  virtual ~PathCumulFilter() {}

 private:
  virtual bool AcceptPath(const Assignment::IntContainer& container,
                          int64 path_start);

  scoped_array<IntVar*> cumuls_;
  const int cumuls_size_;
  Solver::IndexEvaluator2* const evaluator_;
};

PathCumulFilter::PathCumulFilter(const IntVar* const* nexts,
                                 int nexts_size,
                                 const IntVar* const* cumuls,
                                 int cumuls_size,
                                 Solver::IndexEvaluator2* evaluator,
                                 const string& name)
    : BasePathFilter(nexts, nexts_size, cumuls_size, name),
      cumuls_size_(cumuls_size),
      evaluator_(evaluator) {
  cumuls_.reset(new IntVar*[cumuls_size_]);
  memcpy(cumuls_.get(), cumuls, cumuls_size_ * sizeof(*cumuls));
}

bool PathCumulFilter::AcceptPath(const Assignment::IntContainer& container,
                                 int64 path_start) {
  int64 node = path_start;
  int64 cumul = cumuls_[node]->Min();
  while (node < Size()) {
    const int64 next = GetNext(container, node);
    if (next == kUnassigned) {
      // LNS detected, return true since path was ok up to now.
      return true;
    }
    cumul += evaluator_->Run(node, next);
    if (cumul > cumuls_[next]->Max()) {
      return false;
    }
    cumul = std::max(cumuls_[next]->Min(), cumul);
    node = next;
  }
  return true;
}

// Node precedence filter, resulting from pickup and delivery pairs.
class NodePrecedenceFilter : public BasePathFilter {
 public:
  NodePrecedenceFilter(const IntVar* const* nexts,
                       int nexts_size,
                       int next_domain_size,
                       const RoutingModel::NodePairs& pairs,
                       const string& name);
  virtual ~NodePrecedenceFilter() {}
  bool AcceptPath(const Assignment::IntContainer& container, int64 path_start);

 private:
  std::vector<int> pair_firsts_;
  std::vector<int> pair_seconds_;
};

NodePrecedenceFilter::NodePrecedenceFilter(const IntVar* const* nexts,
                                           int nexts_size,
                                           int next_domain_size,
                                           const RoutingModel::NodePairs& pairs,
                                           const string& name)
    : BasePathFilter(nexts, nexts_size, next_domain_size, name),
      pair_firsts_(next_domain_size, kUnassigned),
      pair_seconds_(next_domain_size, kUnassigned) {
  for (int i = 0; i < pairs.size(); ++i) {
    pair_firsts_[pairs[i].first] = pairs[i].second;
    pair_seconds_[pairs[i].second] = pairs[i].first;
  }
}

bool NodePrecedenceFilter::AcceptPath(const Assignment::IntContainer& container,
                                      int64 path_start) {
  std::vector<bool> visited(Size(), false);
  int64 node = path_start;
  int64 path_length = 1;
  while (node < Size()) {
    if (path_length > Size()) {
      return false;
    }
    if (pair_firsts_[node] != kUnassigned
        && visited[pair_firsts_[node]]) {
      return false;
    }
    if (pair_seconds_[node] != kUnassigned
        && !visited[pair_seconds_[node]]) {
      return false;
    }
    visited[node] = true;
    const int64 next = GetNext(container, node);
    if (next == kUnassigned) {
      // LNS detected, return true since path was ok up to now.
      return true;
    }
    node = next;
    ++path_length;
  }
  return true;
}

// Evaluators

class MatrixEvaluator : public BaseObject {
 public:
  MatrixEvaluator(const int64* const* values,
                  int nodes,
                  RoutingModel* model)
      : values_(new int64*[nodes]), nodes_(nodes), model_(model) {
    CHECK(values) << "null pointer";
    for (int i = 0; i < nodes_; ++i) {
      values_[i] = new int64[nodes_];
      memcpy(values_[i], values[i], nodes_ * sizeof(*values[i]));
    }
  }
  virtual ~MatrixEvaluator() {
    for (int i = 0; i < nodes_; ++i) {
      delete [] values_[i];
    }
    delete [] values_;
  }
  int64 Value(RoutingModel::NodeIndex i, RoutingModel::NodeIndex j) const {
    return values_[i.value()][j.value()];
  }

 private:
  int64** const values_;
  const int nodes_;
  RoutingModel* const model_;
};

class VectorEvaluator : public BaseObject {
 public:
  VectorEvaluator(const int64* values, int64 nodes, RoutingModel* model)
      : values_(new int64[nodes]), nodes_(nodes), model_(model) {
    CHECK(values) << "null pointer";
    memcpy(values_.get(), values, nodes * sizeof(*values));
  }
  virtual ~VectorEvaluator() {}
  int64 Value(RoutingModel::NodeIndex i, RoutingModel::NodeIndex j) const;
 private:
  scoped_array<int64> values_;
  const int64 nodes_;
  RoutingModel* const model_;
};

int64 VectorEvaluator::Value(RoutingModel::NodeIndex i,
                             RoutingModel::NodeIndex j) const {
  return values_[i.value()];
}

class ConstantEvaluator : public BaseObject {
 public:
  explicit ConstantEvaluator(int64 value) : value_(value) {}
  virtual ~ConstantEvaluator() {}
  int64 Value(RoutingModel::NodeIndex i, RoutingModel::NodeIndex j) const {
    return value_;
  }
 private:
  const int64 value_;
};

// Left-branch dive branch selector

Solver::DecisionModification LeftDive(Solver* const s) {
  return Solver::KEEP_LEFT;
}

}  // namespace

// ----- Routing model -----

static const int kUnassigned = -1;
static const int64 kNoPenalty = -1;

const RoutingModel::NodeIndex RoutingModel::kFirstNode(0);
const RoutingModel::NodeIndex RoutingModel::kInvalidNodeIndex(-1);

const RoutingModel::DisjunctionIndex RoutingModel::kNoDisjunction(-1);

RoutingModel::RoutingModel(int nodes, int vehicles)
    : solver_(NULL),
      no_cycle_constraint_(NULL),
      homogeneous_costs_(FLAGS_routing_use_homogeneous_costs),
      vehicle_cost_classes_(vehicles, -1),
      cost_(0),
      fixed_costs_(vehicles),
      nodes_(nodes),
      vehicles_(vehicles),
      starts_(vehicles),
      ends_(vehicles),
      start_end_count_(vehicles > 0 ? 1 : 0),
      is_depot_set_(false),
      closed_(false),
      status_(ROUTING_NOT_SOLVED),
      first_solution_strategy_(ROUTING_DEFAULT_STRATEGY),
      first_solution_evaluator_(NULL),
      metaheuristic_(ROUTING_GREEDY_DESCENT),
      collect_assignments_(NULL),
      solve_db_(NULL),
      improve_db_(NULL),
      restore_assignment_(NULL),
      assignment_(NULL),
      preassignment_(NULL),
      time_limit_ms_(FLAGS_routing_time_limit),
      lns_time_limit_ms_(FLAGS_routing_lns_time_limit),
      limit_(NULL),
      ls_limit_(NULL),
      lns_limit_(NULL) {
  SolverParameters parameters;
  solver_.reset(new Solver("Routing", parameters));
  Initialize();
}

RoutingModel::RoutingModel(
    int nodes,
    int vehicles,
    const std::vector<std::pair<NodeIndex, NodeIndex> >& start_end)
    : solver_(NULL),
      no_cycle_constraint_(NULL),
      homogeneous_costs_(FLAGS_routing_use_homogeneous_costs),
      vehicle_cost_classes_(vehicles, -1),
      fixed_costs_(vehicles),
      nodes_(nodes),
      vehicles_(vehicles),
      starts_(vehicles),
      ends_(vehicles),
      is_depot_set_(false),
      closed_(false),
      status_(ROUTING_NOT_SOLVED),
      first_solution_strategy_(ROUTING_DEFAULT_STRATEGY),
      first_solution_evaluator_(NULL),
      metaheuristic_(ROUTING_GREEDY_DESCENT),
      collect_assignments_(NULL),
      solve_db_(NULL),
      improve_db_(NULL),
      restore_assignment_(NULL),
      assignment_(NULL),
      preassignment_(NULL),
      time_limit_ms_(FLAGS_routing_time_limit),
      lns_time_limit_ms_(FLAGS_routing_lns_time_limit),
      limit_(NULL),
      ls_limit_(NULL),
      lns_limit_(NULL) {
  SolverParameters parameters;
  solver_.reset(new Solver("Routing", parameters));
  CHECK_EQ(vehicles, start_end.size());
  hash_set<NodeIndex> depot_set;
  for (int i = 0; i < start_end.size(); ++i) {
    depot_set.insert(start_end[i].first);
    depot_set.insert(start_end[i].second);
  }
  start_end_count_ = depot_set.size();
  Initialize();
  SetStartEnd(start_end);
}

RoutingModel::RoutingModel(int nodes,
                           int vehicles,
                           const std::vector<NodeIndex>& starts,
                           const std::vector<NodeIndex>& ends)
    : solver_(NULL),
      no_cycle_constraint_(NULL),
      homogeneous_costs_(FLAGS_routing_use_homogeneous_costs),
      vehicle_cost_classes_(vehicles, -1),
      fixed_costs_(vehicles),
      nodes_(nodes),
      vehicles_(vehicles),
      starts_(vehicles),
      ends_(vehicles),
      is_depot_set_(false),
      closed_(false),
      status_(ROUTING_NOT_SOLVED),
      first_solution_strategy_(ROUTING_DEFAULT_STRATEGY),
      first_solution_evaluator_(NULL),
      metaheuristic_(ROUTING_GREEDY_DESCENT),
      collect_assignments_(NULL),
      solve_db_(NULL),
      improve_db_(NULL),
      restore_assignment_(NULL),
      assignment_(NULL),
      preassignment_(NULL),
      time_limit_ms_(FLAGS_routing_time_limit),
      lns_time_limit_ms_(FLAGS_routing_lns_time_limit),
      limit_(NULL),
      ls_limit_(NULL),
      lns_limit_(NULL) {
  SolverParameters parameters;
  solver_.reset(new Solver("Routing", parameters));
  CHECK_EQ(vehicles, starts.size());
  CHECK_EQ(vehicles, ends.size());
  hash_set<NodeIndex> depot_set;
  std::vector<std::pair<NodeIndex, NodeIndex> > start_end(starts.size());
  for (int i = 0; i < starts.size(); ++i) {
    depot_set.insert(starts[i]);
    depot_set.insert(ends[i]);
    start_end[i] = std::make_pair(starts[i], ends[i]);
  }
  start_end_count_ = depot_set.size();
  Initialize();
  SetStartEnd(start_end);
}

void RoutingModel::Initialize() {
  const int size = Size();
  // Next variables
  solver_->MakeIntVarArray(size,
                           0,
                           size + vehicles_ - 1,
                           "Nexts",
                           &nexts_);
  solver_->AddConstraint(solver_->MakeAllDifferent(nexts_, false));
  node_to_disjunction_.resize(size, kNoDisjunction);
  // Vehicle variables. In case that node i is not active, vehicle_vars_[i] is
  // bound to -1.
  solver_->MakeIntVarArray(size + vehicles_,
                           -1,
                           vehicles_ - 1,
                           "Vehicles",
                           &vehicle_vars_);
  // Active variables
  solver_->MakeBoolVarArray(size, "Active", &active_);
  // Cost cache
  cost_cache_.clear();
  cost_cache_.resize(size);
  for (int i = 0; i < size; ++i) {
    CostCacheElement& cache = cost_cache_[i];
    cache.node = kUnassigned;
    cache.cost_class = kUnassigned;
    cache.cost = 0;
  }
  preassignment_ = solver_->MakeAssignment();
}

RoutingModel::~RoutingModel() {
  STLDeleteElements(&routing_caches_);
  STLDeleteElements(&owned_node_callbacks_);
  STLDeleteElements(&owned_index_callbacks_);
  STLDeleteValues(&capacity_evaluators_);
}

void RoutingModel::AddNoCycleConstraintInternal() {
  CheckDepot();
  if (no_cycle_constraint_ == NULL) {
    no_cycle_constraint_ = solver_->MakeNoCycle(nexts_, active_);
    solver_->AddConstraint(no_cycle_constraint_);
  }
}

void RoutingModel::AddDimension(NodeEvaluator2* evaluator,
                                int64 slack_max,
                                int64 capacity,
                                bool fix_start_cumul_to_zero,
                                const string& name) {
  AddDimensionWithCapacityInternal(
      evaluator, slack_max, capacity, NULL, fix_start_cumul_to_zero, name);
}

void RoutingModel::AddDimensionWithVehicleCapacity(
    NodeEvaluator2* evaluator,
    int64 slack_max,
    VehicleEvaluator* vehicle_capacity,
    bool fix_start_cumul_to_zero,
    const string& name) {
  AddDimensionWithCapacityInternal(
      evaluator, slack_max, kint64max, vehicle_capacity,
      fix_start_cumul_to_zero, name);
}

void RoutingModel::AddDimensionWithCapacityInternal(
    NodeEvaluator2* evaluator,
    int64 slack_max,
    int64 capacity,
    VehicleEvaluator* vehicle_capacity,
    bool fix_start_cumul_to_zero,
    const string& name) {
  CheckDepot();
  const std::vector<IntVar*>& cumuls = GetOrMakeCumuls(vehicle_capacity,
                                                  capacity,
                                                  name);
  const std::vector<IntVar*>& transits =
      GetOrMakeTransits(NewCachedCallback(evaluator),
                        slack_max,
                        name);
  solver_->AddConstraint(solver_->MakePathCumul(nexts_,
                                                active_,
                                                cumuls,
                                                transits));
  if (fix_start_cumul_to_zero) {
    for (int i = 0; i < vehicles_; ++i) {
      IntVar* startCumul = cumuls[Start(i)];
      CHECK_EQ(0, startCumul->Min());
      startCumul->SetValue(0);
    }
  }
}

void RoutingModel::AddConstantDimension(int64 value,
                                        int64 capacity,
                                        bool fix_start_cumul_to_zero,
                                        const string& name) {
  ConstantEvaluator* evaluator =
      solver_->RevAlloc(new ConstantEvaluator(value));
  AddDimension(NewPermanentCallback(evaluator, &ConstantEvaluator::Value),
               0, capacity, fix_start_cumul_to_zero, name);
}

void RoutingModel::AddVectorDimension(const int64* values,
                                      int64 capacity,
                                      bool fix_start_cumul_to_zero,
                                      const string& name) {
  VectorEvaluator* evaluator =
      solver_->RevAlloc(new VectorEvaluator(values, nodes_, this));
  AddDimension(NewPermanentCallback(evaluator, &VectorEvaluator::Value),
               0, capacity, fix_start_cumul_to_zero, name);
}

void RoutingModel::AddMatrixDimension(const int64* const* values,
                                      int64 capacity,
                                      bool fix_start_cumul_to_zero,
                                      const string& name) {
  MatrixEvaluator* evaluator =
      solver_->RevAlloc(new MatrixEvaluator(values, nodes_, this));
  AddDimension(NewPermanentCallback(evaluator, &MatrixEvaluator::Value),
               0, capacity, fix_start_cumul_to_zero, name);
}

void RoutingModel::GetAllDimensions(std::vector<string>* names) {
  CHECK_NOTNULL(names);
  for (ConstIter<hash_map<string, VehicleEvaluator*> > it(capacity_evaluators_);
       !it.at_end(); ++it) {
    names->push_back(it->first);
  }
}

void RoutingModel::AddAllActive() {
  for (int i = 0; i < Size(); ++i) {
    if (active_[i]->Max() != 0) {
      active_[i]->SetValue(1);
    }
  }
}

void RoutingModel::SetCost(NodeEvaluator2* evaluator) {
  CHECK_LT(0, vehicles_);
  homogeneous_costs_ = FLAGS_routing_use_homogeneous_costs;
  for (int i = 0; i < vehicles_; ++i) {
    SetVehicleCostInternal(i, evaluator);
  }
}

int64 RoutingModel::GetRouteFixedCost() const {
  return GetVehicleFixedCost(0);
}

void RoutingModel::SetVehicleCost(int vehicle, NodeEvaluator2* evaluator) {
  homogeneous_costs_ = false;
  SetVehicleCostInternal(vehicle, evaluator);
}

void RoutingModel::SetVehicleCostInternal(int vehicle,
                                          NodeEvaluator2* evaluator) {
  CHECK_NOTNULL(evaluator);
  CHECK_LT(vehicle, vehicles_);
  // TODO(user): Support cost modification.
  CHECK_EQ(-1, GetVehicleCostClass(vehicle))
      << "Vehicle cost already set for " << vehicle;
  evaluator->CheckIsRepeatable();
  std::vector<int>* callback_vehicles =
      FindOrNull(cost_callback_vehicles_, evaluator);
  if (callback_vehicles == NULL) {
    cost_callback_vehicles_[evaluator].push_back(vehicle);
    SetVehicleCostClass(vehicle, costs_.size());
    costs_.push_back(NewCachedCallback(evaluator));
  } else {
    CHECK_NE(0, callback_vehicles->size());
    callback_vehicles->push_back(vehicle);
    SetVehicleCostClass(vehicle, GetVehicleCostClass(callback_vehicles->at(0)));
  }
}

void RoutingModel::SetRouteFixedCost(int64 cost) {
  for (int i = 0; i < vehicles_; ++i) {
    SetVehicleFixedCost(i, cost);
  }
}

int64 RoutingModel::GetVehicleFixedCost(int vehicle) const {
  CHECK_LT(vehicle, vehicles_);
  return fixed_costs_[vehicle];
}

void RoutingModel::SetVehicleFixedCost(int vehicle, int64 cost) {
  CHECK_LT(vehicle, vehicles_);
  fixed_costs_[vehicle] = cost;
}

void RoutingModel::AddDisjunction(const std::vector<NodeIndex>& nodes) {
  AddDisjunctionInternal(nodes, kNoPenalty);
}

void RoutingModel::AddDisjunction(const std::vector<NodeIndex>& nodes,
                                  int64 penalty) {
  CHECK_GE(penalty, 0) << "Penalty must be positive";
  AddDisjunctionInternal(nodes, penalty);
}

void RoutingModel::AddDisjunctionInternal(const std::vector<NodeIndex>& nodes,
                                          int64 penalty) {
  const int size = disjunctions_.size();
  disjunctions_.resize(size + 1);
  std::vector<int>& disjunction_nodes = disjunctions_.back().nodes;
  disjunction_nodes.resize(nodes.size());
  for (int i = 0; i < nodes.size(); ++i) {
    CHECK_NE(kUnassigned, node_to_index_[nodes[i]]);
    disjunction_nodes[i] = node_to_index_[nodes[i]];
  }
  disjunctions_.back().penalty = penalty;
  for (int i = 0; i < nodes.size(); ++i) {
    // TODO(user): support multiple disjunction per node
    node_to_disjunction_[node_to_index_[nodes[i]]] = DisjunctionIndex(size);
  }
}

IntVar* RoutingModel::CreateDisjunction(DisjunctionIndex disjunction) {
  const std::vector<int>& nodes = disjunctions_[disjunction].nodes;
  const int nodes_size = nodes.size();
  std::vector<IntVar*> disjunction_vars(nodes_size + 1);
  for (int i = 0; i < nodes_size; ++i) {
    const int node = nodes[i];
    CHECK_LT(node, Size());
    disjunction_vars[i] = ActiveVar(node);
  }
  IntVar* no_active_var = solver_->MakeBoolVar();
  disjunction_vars[nodes_size] = no_active_var;
  solver_->AddConstraint(solver_->MakeSumEquality(disjunction_vars, 1));
  const int64 penalty = disjunctions_[disjunction].penalty;
  if (penalty < 0) {
    no_active_var->SetMax(0);
    return NULL;
  } else {
    return solver_->MakeProd(no_active_var, penalty)->Var();
  }
}

void RoutingModel::AddLocalSearchOperator(LocalSearchOperator* ls_operator) {
  extra_operators_.push_back(ls_operator);
}

int64 RoutingModel::GetDepot() const {
  return vehicles() > 0 ? Start(0) : -1;
}

void RoutingModel::SetDepot(NodeIndex depot) {
  std::vector<std::pair<NodeIndex, NodeIndex> > start_end(
      vehicles_, std::make_pair(depot, depot));
  SetStartEnd(start_end);
}

void RoutingModel::SetStartEnd(
    const std::vector<std::pair<NodeIndex, NodeIndex> >& start_end) {
  if (is_depot_set_) {
    LOG(WARNING) << "A depot has already been specified, ignoring new ones";
    return;
  }
  CHECK_EQ(start_end.size(), vehicles_);
  const int size = Size();
  hash_set<NodeIndex> starts;
  hash_set<NodeIndex> ends;
  for (int i = 0; i < vehicles_; ++i) {
    const NodeIndex start = start_end[i].first;
    const NodeIndex end = start_end[i].second;
    CHECK_GE(start, 0);
    CHECK_GE(end, 0);
    CHECK_LE(start, nodes_);
    CHECK_LE(end, nodes_);
    starts.insert(start);
    ends.insert(end);
  }
  index_to_node_.resize(size + vehicles_);
  node_to_index_.resize(nodes_, kUnassigned);
  int index = 0;
  for (NodeIndex i = kFirstNode; i < nodes_; ++i) {
    if (starts.count(i) != 0 || ends.count(i) == 0) {
      index_to_node_[index] = i;
      node_to_index_[i] = index;
      ++index;
    }
  }
  hash_set<NodeIndex> node_set;
  index_to_vehicle_.resize(size + vehicles_, kUnassigned);
  for (int i = 0; i < vehicles_; ++i) {
    const NodeIndex start = start_end[i].first;
    if (node_set.count(start) == 0) {
      node_set.insert(start);
      const int start_index = node_to_index_[start];
      starts_[i] = start_index;
      CHECK_NE(kUnassigned, start_index);
      index_to_vehicle_[start_index] = i;
    } else {
      starts_[i] = index;
      index_to_node_[index] = start;
      index_to_vehicle_[index] = i;
      ++index;
    }
  }
  for (int i = 0; i < vehicles_; ++i) {
    NodeIndex end = start_end[i].second;
    index_to_node_[index] = end;
    ends_[i] = index;
    CHECK_LE(size, index);
    index_to_vehicle_[index] = i;
    ++index;
  }
  for (int i = 0; i < size; ++i) {
    for (int j = 0; j < vehicles_; ++j) {
      // "start" node: nexts_[i] != start
      solver_->AddConstraint(solver_->MakeNonEquality(nexts_[i], starts_[j]));
    }
    // Extra constraint to state a node can't point to itself
    solver_->AddConstraint(solver_->MakeIsDifferentCstCt(nexts_[i],
                                                         i,
                                                         active_[i]));
  }
  is_depot_set_ = true;

  // Logging model information.
  VLOG(1) << "Number of nodes: " << nodes_;
  VLOG(1) << "Number of vehicles: " << vehicles_;
  for (int index = 0; index < index_to_node_.size(); ++index) {
    VLOG(2) << "Variable index " << index
            << " -> Node index " << index_to_node_[index];
  }
  for (NodeIndex node = kFirstNode; node < node_to_index_.size(); ++node) {
    VLOG(2) << "Node index " << node
            << " -> Variable index " << node_to_index_[node];
  }
}

void RoutingModel::CloseModel() {
  if (closed_) {
    LOG(WARNING) << "Model already closed";
    return;
  }
  closed_ = true;

  CheckDepot();

  AddNoCycleConstraintInternal();

  const int size = Size();

  // Vehicle variable constraints
  for (int i = 0; i < vehicles_; ++i) {
    solver_->AddConstraint(solver_->MakeEquality(
        vehicle_vars_[starts_[i]], solver_->MakeIntConst(i)));
    solver_->AddConstraint(solver_->MakeEquality(
        vehicle_vars_[ends_[i]], solver_->MakeIntConst(i)));
  }
  std::vector<IntVar*> zero_transit(size, solver_->MakeIntConst(Zero()));
  solver_->AddConstraint(solver_->MakePathCumul(nexts_,
                                                active_,
                                                vehicle_vars_,
                                                zero_transit));

  // Add constraints to bind vehicle_vars_[i] to -1 in case that node i is not
  // active.
  for (int i = 0; i < size; ++i) {
    solver_->AddConstraint(solver_->MakeIsDifferentCstCt(vehicle_vars_[i], -1,
                                                         active_[i]));
  }

  // Set all active unless there are disjunctions
  if (disjunctions_.size() == 0) {
    AddAllActive();
  }

  // Associate first and "logical" last nodes
  for (int i = 0; i < vehicles_; ++i) {
    for (int j = 0; j < vehicles_; ++j) {
      if (i != j) {
        nexts_[starts_[i]]->RemoveValue(ends_[j]);
      }
    }
  }

  std::vector<IntVar*> cost_elements;
  // Arc costs: the cost of an arc (i, nexts_[i], vehicle_vars_[i]) is
  // costs_(nexts_[i], vehicle_vars_[i]); the total cost is the sum of arc
  // costs.
  if (vehicles_ > 0) {
    for (int i = 0; i < size; ++i) {
      if (FLAGS_routing_use_light_propagation) {
        // Only supporting positive costs.
        // TODO(user): Detect why changing lower bound to kint64min stalls
        // the search in GLS in some cases (Solomon instances for instance).
        IntVar* base_cost_var = solver_->MakeIntVar(0, kint64max);
        if (homogeneous_costs_) {
          solver_->AddConstraint(MakeLightElement(
              solver_.get(),
              base_cost_var,
              nexts_[i],
              NewPermanentCallback(this,
                                   &RoutingModel::GetHomogeneousCost,
                                   static_cast<int64>(i))));
        } else {
          solver_->AddConstraint(MakeLightElement2(
              solver_.get(),
              base_cost_var,
              nexts_[i],
              vehicle_vars_[i],
              NewPermanentCallback(this,
                                   &RoutingModel::GetCost,
                                   static_cast<int64>(i))));
        }
        IntVar* const var = solver_->MakeProd(base_cost_var, active_[i])->Var();
        cost_elements.push_back(var);
      } else {
        IntExpr* expr = NULL;
        // TODO(user): Remove the need for the homogeneous flag here once the
        // vehicle var to cost class element constraint is fast enough.
        if (homogeneous_costs_) {
          expr = solver_->MakeElement(
              NewPermanentCallback(this,
                                   &RoutingModel::GetHomogeneousCost,
                                   static_cast<int64>(i)),
              nexts_[i]);
        } else {
          IntVar* const vehicle_class_var =
              solver_->MakeElement(
                  NewPermanentCallback(this,
                                       &RoutingModel::GetSafeVehicleCostClass),
                  vehicle_vars_[i])->Var();
          expr = solver_->MakeElement(
              NewPermanentCallback(this,
                                   &RoutingModel::GetVehicleClassCost,
                                   static_cast<int64>(i)),
              nexts_[i],
              vehicle_class_var);
        }
        IntVar* const var = solver_->MakeProd(expr, active_[i])->Var();
        cost_elements.push_back(var);
      }
    }
  }
  // Penalty costs
  for (DisjunctionIndex i(0); i < disjunctions_.size(); ++i) {
    IntVar* penalty_var = CreateDisjunction(i);
    if (penalty_var != NULL) {
      cost_elements.push_back(penalty_var);
    }
  }
  cost_ = solver_->MakeSum(cost_elements)->Var();
  cost_->set_name("Cost");

  SetupSearch();
}

struct Link {
  Link(std::pair<int, int> link, double value, int vehicle_class,
          int64 start_depot, int64 end_depot)
    : link(link), value(value), vehicle_class(vehicle_class),
      start_depot(start_depot), end_depot(end_depot) { }
  ~Link() { }

  std::pair<int, int> link;
  int64 value;
  int vehicle_class;
  int64 start_depot;
  int64 end_depot;
};

struct LinkSort {
  bool operator() (const Link& link1, const Link& link2) {
    return (link1.value > link2.value);
  }
} LinkComparator;

struct VehicleClass {
  VehicleClass(RoutingModel::NodeIndex start_node,
               RoutingModel::NodeIndex end_node,
               const int64 cost)
    : start_node(start_node), end_node(end_node), cost(cost),
      start_depot(-1), end_depot(-1), class_index(-1) { }

  static bool Equals(const VehicleClass& vehicle1,
                     const VehicleClass& vehicle2) {
    return (vehicle1.start_node == vehicle2.start_node &&
            vehicle1.end_node == vehicle2.end_node &&
            vehicle1.cost == vehicle2.cost);
  }

  RoutingModel::NodeIndex start_node;
  RoutingModel::NodeIndex end_node;
  int64 cost;
  int64 start_depot;
  int64 end_depot;
  int64 class_index;
};

struct VehicleSort {
  bool operator() (const VehicleClass& vehicle1,
                   const VehicleClass& vehicle2) {
    if (vehicle1.start_node < vehicle2.start_node) {
      return true;
    }
    if (vehicle1.end_node < vehicle2.end_node) {
      return true;
    }
    if (vehicle1.cost < vehicle2.cost) {
      return true;
    }
    return false;
  }
} VehicleComparator;

void RoutingModel::GetVehicleClasses(
        std::vector<VehicleClass>* vehicle_classes) const {
  std::vector<VehicleClass> all_vehicles;
  for (int vehicle = 0; vehicle < vehicles(); ++vehicle) {
    all_vehicles.push_back(
                  VehicleClass(IndexToNode(Start(vehicle)),
                               IndexToNode(End(vehicle)),
                               GetVehicleCostClass(vehicle)));
  }
  sort(all_vehicles.begin(), all_vehicles.end(), VehicleComparator);

  vehicle_classes->push_back(all_vehicles[0]);
  for (int i = 1; i < all_vehicles.size(); ++i) {
    if (!VehicleClass::Equals(all_vehicles[i], all_vehicles[i - 1])) {
      vehicle_classes->push_back(all_vehicles[i]);
    }
  }
  int class_index = 0;
  for (MutableIter<std::vector<VehicleClass> > it(*vehicle_classes);
       !it.at_end(); ++it) {
    it->start_depot = NodeToIndex(it->start_node);
    it->end_depot = NodeToIndex(it->end_node);
    it->class_index = class_index;
    ++class_index;
  }
}

// The RouteConstructor creates the routes of a VRP instance subject to its
// constraints by iterating on a list of arcs appearing in descending order
// of priority.
class RouteConstructor {
 public:
  RouteConstructor(Assignment* const assignment,
                   RoutingModel* const model,
                   bool check_assignment,
                   int depot,
                   int64 nodes_number,
                   const std::vector<Link>& links_list,
                   const std::vector<VehicleClass>& vehicle_classes)
    : assignment_(assignment),
      model_(model),
      check_assignment_(check_assignment),
      solver_(model_->solver()),
      depot_(depot),
      nodes_number_(nodes_number),
      links_list_(links_list),
      vehicle_classes_(vehicle_classes),
      no_more_feasible_routes_(false),
      nexts_(model_->Nexts()),
      in_route_(nodes_number_, -1),
      final_routes_(),
      node_to_chain_index_(nodes_number, -1),
      node_to_vehicle_class_index_(nodes_number, -1) {
        model_->GetAllDimensions(&dimensions_);
        cumuls_.resize(dimensions_.size());
        for (MutableIter<std::vector<std::vector<int64> > > it(cumuls_);
             !it.at_end(); ++it) {
          it->resize(nodes_number_);
        }
        new_possible_cumuls_.resize(dimensions_.size());
      }

  ~RouteConstructor() { }

  void Construct() {
    // Initial State: Each order is served by its own vehicle.
    for (int node = 0; node < nodes_number_; ++node) {
      if (!model_->IsStart(node) && !model_->IsEnd(node)) {
        std::vector<int> route(1, node);
        routes_.push_back(route);
        in_route_[node] = routes_.size() - 1;
      }
    }

    for (ConstIter<std::vector<Link> > it(links_list_);
         !it.at_end(); ++it) {
      if (no_more_feasible_routes_) {
        break;
      }
      const int node1 = it->link.first;
      const int node2 = it->link.second;
      const int vehicle_class = it->vehicle_class;
      const int64 start_depot = it->start_depot;
      const int64 end_depot = it->end_depot;

      // Initialisation of cumuls_ if the nodes are encountered for first time
      if (node_to_vehicle_class_index_[node1] < 0) {
        for (int dimension_index = 0; dimension_index < dimensions_.size();
           ++dimension_index) {
          cumuls_[dimension_index][node1] = std::max(
              model_->GetTransitValue(dimensions_[dimension_index],
                                      start_depot, node1),
              model_->CumulVar(node1, dimensions_[dimension_index])->Min());
        }
      }
      if (node_to_vehicle_class_index_[node2] < 0) {
        for (int dimension_index = 0; dimension_index < dimensions_.size();
           ++dimension_index) {
          cumuls_[dimension_index][node2] = std::max(
              model_->GetTransitValue(dimensions_[dimension_index],
                                      start_depot, node2),
              model_->CumulVar(node2, dimensions_[dimension_index])->Min());
        }
      }

      const int route_index1 = in_route_[node1];
      const int route_index2 = in_route_[node2];
      const bool merge =
            FeasibleMerge(routes_[route_index1], routes_[route_index2],
                          node1, node2, route_index1, route_index2,
                          vehicle_class, start_depot, end_depot);
      if (Merge(merge, route_index1, route_index2)) {
        node_to_vehicle_class_index_[node1] = vehicle_class;
        node_to_vehicle_class_index_[node2] = vehicle_class;
      }
    }

    for (int chain_index = 0; chain_index < chains_.size(); ++chain_index) {
      if (!ContainsKey(deleted_chains_, chain_index)) {
        final_chains_.push_back(chains_[chain_index]);
      }
    }
    std::sort(final_chains_.begin(), final_chains_.end(), ChainComparator);
    for (int route_index = 0; route_index < routes_.size(); ++route_index) {
      if (!ContainsKey(deleted_routes_, route_index)) {
        final_routes_.push_back(routes_[route_index]);
      }
    }
    std::sort(final_routes_.begin(), final_routes_.end(), RouteComparator);

    const int extra_vehicles = std::max(
        0, static_cast<int>(final_chains_.size()) - model_->vehicles());
    // Bind the Start and End of each chain
    int chain_index = 0;
    for (chain_index = extra_vehicles; chain_index < final_chains_.size();
         ++chain_index) {
      if (chain_index - extra_vehicles >= model_->vehicles()) {
          break;
      }
      const int start = final_chains_[chain_index].head;
      const int end = final_chains_[chain_index].tail;
      assignment_->Add(
          model_->NextVar(model_->Start(chain_index - extra_vehicles)));
      assignment_->SetValue(
          model_->NextVar(model_->Start(chain_index - extra_vehicles)), start);
      assignment_->Add(nexts_[end]);
      assignment_->SetValue(
          nexts_[end], model_->End(chain_index - extra_vehicles));
    }

    // Create the single order routes
    for (int route_index = 0; route_index < final_routes_.size();
         ++route_index) {
      if (chain_index - extra_vehicles >= model_->vehicles()) {
        break;
      }
      DCHECK_LT(route_index, final_routes_.size());
      const int head = final_routes_[route_index].front();
      const int tail = final_routes_[route_index].back();
      if (head == tail && head < model_->Size()) {
        assignment_->Add(
            model_->NextVar(model_->Start(chain_index - extra_vehicles)));
        assignment_->SetValue(
            model_->NextVar(model_->Start(chain_index - extra_vehicles)), head);
        assignment_->Add(nexts_[tail]);
        assignment_->SetValue(
            nexts_[tail], model_->End(chain_index - extra_vehicles));
        ++chain_index;
      }
    }

    // Unperformed
    for (int index = 0; index < model_->Size(); ++index) {
      IntVar* const next = nexts_[index];
      if (!assignment_->Contains(next)) {
        assignment_->Add(next);
        if (next->Contains(index)) {
          assignment_->SetValue(next, index);
        }
      }
    }
  }

  const std::vector<std::vector<int> >& final_routes() const { return final_routes_; }

 private:
  enum MergeStatus { FIRST_SECOND, SECOND_FIRST, NO_MERGE };

  struct RouteSort {
    bool operator() (const std::vector<int>& route1, const std::vector<int>& route2) {
      return (route1.size() < route2.size());
    }
  } RouteComparator;

  struct Chain {
    int head;
    int tail;
    int nodes;
  };

  struct ChainSort {
    bool operator() (const Chain& chain1, const Chain& chain2) {
      return (chain1.nodes < chain2.nodes);
    }
  } ChainComparator;

  bool Head(int node) const {
    return (node == routes_[in_route_[node]].front());
  }

  bool Tail(int node) const {
    return (node == routes_[in_route_[node]].back());
  }

  bool FeasibleRoute(const std::vector<int>& route, int64 route_cumul,
                     int dimension_index) {
    const string& name = dimensions_[dimension_index];
    ConstIter<std::vector<int> > it(route);
    int64 cumul = route_cumul;
    while (!it.at_end()) {
      const int previous = *it;
      const int64 cumul_previous = cumul;
      InsertOrDie(&(new_possible_cumuls_[dimension_index]),
                  previous, cumul_previous);
      ++it;
      if (it.at_end()) {
        return true;
      }
      const int next = *it;
      int64 available_from_previous = cumul_previous +
                                      model_->GetTransitValue(name,
                                                              previous,
                                                              next);
      int64 available_cumul_next = std::max(
          cumuls_[dimension_index][next],
          available_from_previous);

      const int64 slack = available_cumul_next - available_from_previous;
      if (slack > model_->SlackVar(previous, name)->Max()) {
        available_cumul_next = available_from_previous +
                               model_->SlackVar(previous, name)->Max();
      }

      if (available_cumul_next > model_->CumulVar(next, name)->Max()) {
        return false;
      }
      if (available_cumul_next <= cumuls_[dimension_index][next]) {
        return true;
      }
      cumul = available_cumul_next;
    }
    return true;
  }

  bool CheckRouteConnection(const std::vector<int>& route1,
                            const std::vector<int>& route2,
                            int dimension_index,
                            int64 start_depot, int64 end_depot) {
    const int tail1 = route1.back();
    const int head2 = route2.front();
    const int tail2 = route2.back();
    const string name = dimensions_[dimension_index];
    int non_depot_node = -1;
    for (int node = 0; node < nodes_number_; ++node) {
      if (!model_->IsStart(node) && !model_->IsEnd(node)) {
        non_depot_node = node;
        break;
      }
    }
    CHECK_GE(non_depot_node, 0);
    const int64 depot_threashold = std::max(
        model_->SlackVar(non_depot_node, name)->Max(),
        model_->CumulVar(non_depot_node, name)->Max());

    int64 available_from_tail1 = cumuls_[dimension_index][tail1] +
                                 model_->GetTransitValue(name, tail1, head2);
    int64 new_available_cumul_head2 = std::max(
        cumuls_[dimension_index][head2], available_from_tail1);

    const int64 slack = new_available_cumul_head2 - available_from_tail1;
    if (slack > model_->SlackVar(tail1, name)->Max()) {
      new_available_cumul_head2 = available_from_tail1 +
                                  model_->SlackVar(tail1, name)->Max();
    }

    bool feasible_route = true;
    if (new_available_cumul_head2 > model_->CumulVar(head2, name)->Max()) {
      return false;
    }
    if (new_available_cumul_head2 <= cumuls_[dimension_index][head2]) {
      return true;
    }

    feasible_route = FeasibleRoute(route2, new_available_cumul_head2,
                                        dimension_index);
    const int64 new_possible_cumul_tail2 =
              ContainsKey(new_possible_cumuls_[dimension_index], tail2) ?
              new_possible_cumuls_[dimension_index][tail2] :
              cumuls_[dimension_index][tail2];

    if (!feasible_route ||
        (new_possible_cumul_tail2 +
         model_->GetTransitValue(name, tail2, end_depot) >
         depot_threashold)) {
      return false;
    }
    return true;
  }

  bool FeasibleMerge(const std::vector<int>& route1,
                     const std::vector<int>& route2,
                     int node1, int node2,
                     int route_index1, int route_index2, int vehicle_class,
                     int64 start_depot, int64 end_depot) {
    if ((route_index1 == route_index2) ||
        !(Tail(node1) && Head(node2))) {
      return false;
    }

    // Vehicle Class Check
    if (!((node_to_vehicle_class_index_[node1] == -1 &&
           node_to_vehicle_class_index_[node2] == -1) ||
          (node_to_vehicle_class_index_[node1] == vehicle_class &&
           node_to_vehicle_class_index_[node2] == -1) ||
          (node_to_vehicle_class_index_[node1] == -1 &&
           node_to_vehicle_class_index_[node2] == vehicle_class) ||
          (node_to_vehicle_class_index_[node1] == vehicle_class &&
           node_to_vehicle_class_index_[node2] == vehicle_class))) {
      return false;
    }

    // Check Route1 -> Route2 connection for every dimension
    bool merge = true;
    for (int dimension_index = 0; dimension_index < dimensions_.size();
         ++dimension_index) {
      new_possible_cumuls_[dimension_index].clear();
      merge = merge && CheckRouteConnection(route1, route2, dimension_index,
                                            start_depot, end_depot);
      if (!merge) {
        return false;
      }
    }
    return true;
  }

  bool CheckTempAssignment(Assignment* const temp_assignment,
                           int new_chain_index, int old_chain_index,
                           int head1, int tail1, int head2, int tail2) {
    const int start = head1;
    temp_assignment->Add(model_->NextVar(model_->Start(new_chain_index)));
    temp_assignment->SetValue(model_->NextVar(model_->Start(new_chain_index)),
                                              start);
    temp_assignment->Add(nexts_[tail1]);
    temp_assignment->SetValue(nexts_[tail1], head2);
    temp_assignment->Add(nexts_[tail2]);
    temp_assignment->SetValue(nexts_[tail2], model_->End(new_chain_index));
    for (int chain_index = 0; chain_index < chains_.size(); ++chain_index) {
      if ((chain_index != new_chain_index) &&
          (chain_index != old_chain_index) &&
          (!ContainsKey(deleted_chains_, chain_index))) {
        const int start = chains_[chain_index].head;
        const int end = chains_[chain_index].tail;
        temp_assignment->Add(model_->NextVar(model_->Start(chain_index)));
        temp_assignment->SetValue(model_->NextVar(model_->Start(chain_index)),
                                                  start);
        temp_assignment->Add(nexts_[end]);
        temp_assignment->SetValue(nexts_[end], model_->End(chain_index));
      }
    }
    return solver_->Solve(solver_->MakeRestoreAssignment(temp_assignment));
  }

  bool UpdateAssignment(const std::vector<int>& route1,
                        const std::vector<int>& route2) {
    bool feasible = true;
    const int head1 = route1.front();
    const int tail1 = route1.back();
    const int head2 = route2.front();
    const int tail2 = route2.back();
    const int chain_index1 = node_to_chain_index_[head1];
    const int chain_index2 = node_to_chain_index_[head2];
    if (chain_index1 < 0 && chain_index2 < 0) {
      const int chain_index = chains_.size();
      if (chain_index >= model_->vehicles()) {
        no_more_feasible_routes_ = true;
        return false;
      }
      if (check_assignment_) {
        Assignment* const temp_assignment =
            solver_->MakeAssignment(assignment_);
        feasible = CheckTempAssignment(temp_assignment, chain_index, -1,
                                       head1, tail1, head2, tail2);
      }
      if (feasible) {
        Chain chain;
        chain.head = head1;
        chain.tail = tail2;
        chain.nodes = 2;
        node_to_chain_index_[head1] = chain_index;
        node_to_chain_index_[tail2] = chain_index;
        chains_.push_back(chain);
      }
    } else if (chain_index1 >= 0 && chain_index2 < 0) {
      if (check_assignment_) {
        Assignment* const temp_assignment =
            solver_->MakeAssignment(assignment_);
        feasible = CheckTempAssignment(temp_assignment,
                                       chain_index1, chain_index2,
                                       head1, tail1, head2, tail2);
      }
      if (feasible) {
        node_to_chain_index_[tail2] = chain_index1;
        chains_[chain_index1].head = head1;
        chains_[chain_index1].tail = tail2;
        ++chains_[chain_index1].nodes;
      }
    } else if (chain_index1 < 0 && chain_index2 >= 0) {
      if (check_assignment_) {
        Assignment* const temp_assignment =
            solver_->MakeAssignment(assignment_);
        feasible = CheckTempAssignment(temp_assignment,
                                       chain_index2, chain_index1,
                                       head1, tail1, head2, tail2);
      }
      if (feasible) {
        node_to_chain_index_[head1] = chain_index2;
        chains_[chain_index2].head = head1;
        chains_[chain_index2].tail = tail2;
        ++chains_[chain_index2].nodes;
      }
    } else {
      if (check_assignment_) {
        Assignment* const temp_assignment =
            solver_->MakeAssignment(assignment_);
        feasible = CheckTempAssignment(temp_assignment,
                                       chain_index1, chain_index2,
                                       head1, tail1, head2, tail2);
      }
      if (feasible) {
        node_to_chain_index_[tail2] = chain_index1;
        chains_[chain_index1].head = head1;
        chains_[chain_index1].tail = tail2;
        chains_[chain_index1].nodes += chains_[chain_index2].nodes;
        deleted_chains_.insert(chain_index2);
      }
    }
    if (feasible) {
      assignment_->Add(nexts_[tail1]);
      assignment_->SetValue(nexts_[tail1], head2);
    }
    return feasible;
  }

  bool Merge(bool merge, int index1, int index2) {
    if (merge) {
      if (UpdateAssignment(routes_[index1], routes_[index2])) {
        // Connection Route1 -> Route2
        for (int node_index = 0; node_index < routes_[index2].size();
             ++node_index) {
          const int node = routes_[index2][node_index];
          in_route_[node] = index1;
          routes_[index1].push_back(node);
        }
        for (int dimension_index = 0; dimension_index < dimensions_.size();
             ++dimension_index) {
          for (ConstIter<hash_map<int, int64> >
                 it(new_possible_cumuls_[dimension_index]);
               !it.at_end(); ++it) {
            cumuls_[dimension_index][it->first] = it->second;
          }
        }
        deleted_routes_.insert(index2);
        return true;
      }
    }
    return false;
  }

  Assignment* const assignment_;
  RoutingModel* const model_;
  const bool check_assignment_;
  Solver* const solver_;
  const int depot_;
  const int64 nodes_number_;
  const std::vector<Link> links_list_;
  const std::vector<VehicleClass> vehicle_classes_;
  bool no_more_feasible_routes_;
  std::vector<IntVar*> nexts_;
  std::vector<string> dimensions_;
  std::vector<std::vector <int64> > cumuls_;
  std::vector<hash_map<int, int64> > new_possible_cumuls_;
  std::vector<std::vector <int> > routes_;
  std::vector<int> in_route_;
  hash_set<int> deleted_routes_;
  std::vector<std::vector<int> > final_routes_;
  std::vector<Chain> chains_;
  hash_set<int> deleted_chains_;
  std::vector<Chain> final_chains_;
  std::vector<int> node_to_chain_index_;
  std::vector<int> node_to_vehicle_class_index_;
};


// Desicion Builder building a first solution based on Savings (Clarke & Wright)
// heuristic for Vehicle Routing Problem.
class SavingsBuilder : public DecisionBuilder {
 public:
  SavingsBuilder(RoutingModel* const model,
                 bool check_assignment)
      : model_(model),
        check_assignment_(check_assignment) { }
  virtual ~SavingsBuilder() { }

  virtual Decision* Next(Solver* const solver) {
    // Setup the model of the instance for the Savings Algorithm
    ModelSetup();

    // Create the Savings List
    CreateSavingsList();

    // Build the assignment routes for the model
    Assignment* const assignment = solver->MakeAssignment();
    RouteConstructor route_constructor(assignment, model_, check_assignment_,
                                       depot_, nodes_number_, savings_list_,
                                       vehicle_classes_);
    route_constructor.Construct();
    assignment->Restore();

    return NULL;
  }

 private:
  void ModelSetup() {
    depot_ = model_->GetDepot();
    nodes_number_ = model_->nodes();
    neighbors_.resize(nodes_number_);
    route_shape_parameter_ = FLAGS_savings_route_shape_parameter;

    int64 savings_filter_neighbors = FLAGS_savings_filter_neighbors;
    int64 savings_filter_radius = FLAGS_savings_filter_radius;
    if (!savings_filter_neighbors && !savings_filter_radius) {
      savings_filter_neighbors = nodes_number_;
      savings_filter_radius = -1;
    }

    // For each node consider as neighbors the nearest nodes.
    {
      for (int node = 0; node < nodes_number_; ++node) {
        for (int neighbor = 0; neighbor < nodes_number_; ++neighbor) {
          neighbors_[node].push_back(neighbor);
        }
      }
    }

    // Setting Up Costs
    for (int node = 0; node < nodes_number_; ++node) {
      std::vector<int64> costs_from_node(nodes_number_);
      for (int neighbor_index = 0; neighbor_index < neighbors_[node].size();
           ++neighbor_index) {
        // TODO(user): Take vehicle class into account here.
        const int64 cost =
            model_->GetHomogeneousCost(node,
                                       neighbors_[node][neighbor_index]);
        costs_from_node[neighbors_[node][neighbor_index]] = cost;
      }
      costs_.push_back(costs_from_node);
    }

    // Find the different vehicle classes
    model_->GetVehicleClasses(&vehicle_classes_);
  }

  void CreateSavingsList() {
    for (ConstIter <std::vector<VehicleClass> > it(vehicle_classes_);
         !it.at_end(); ++it) {
      int64 start_depot = it->start_depot;
      int64 end_depot = it->end_depot;
      int class_index = it->class_index;
      for (int node = 0; node < nodes_number_; ++node) {
        for (int neighbor_index = 0; neighbor_index < neighbors_[node].size();
             ++neighbor_index) {
          if (node != start_depot && node != end_depot &&
              neighbors_[node][neighbor_index] != start_depot &&
              neighbors_[node][neighbor_index] != end_depot &&
              node != neighbors_[node][neighbor_index]) {
            const double saving =
              costs_[node][start_depot] +
              costs_[end_depot][neighbors_[node][neighbor_index]] -
              route_shape_parameter_ * costs_[node][neighbors_[node]
                                                              [neighbor_index]];
            Link link(std::make_pair(node, neighbors_[node][neighbor_index]),
                      saving, class_index, start_depot, end_depot);
            savings_list_.push_back(link);
          }
        }
      }
      sort(savings_list_.begin(), savings_list_.end(), LinkComparator);
    }
  }

  RoutingModel* const model_;
  const bool check_assignment_;
  std::vector<string> dimensions_;
  int64 nodes_number_;
  int depot_;
  std::vector<std::vector<int64> > costs_;
  std::vector<std::vector<int> > neighbors_;
  std::vector<Link> savings_list_;
  double route_shape_parameter_;
  std::vector<VehicleClass> vehicle_classes_;
};

struct SweepNode {
  SweepNode(const int node, const double angle, const double distance)
      : node(node), angle(angle), distance(distance) { }
  ~SweepNode() { }

  int node;
  double angle;
  double distance;
};

struct SweepNodeSortAngle {
  bool operator() (const SweepNode& node1, const SweepNode& node2) {
    return (node1.angle < node2.angle);
  }
} SweepNodeAngleComparator;

struct SweepNodeSortDistance {
  bool operator() (const SweepNode& node1, const SweepNode& node2) {
    return (node1.distance < node2.distance);
  }
} SweepNodeDistanceComparator;

// Splits the space of the nodes into sectors and sorts the nodes of each
// sector with ascending angle from the depot.
void SweepArranger::ArrangeNodes(std::vector<int>* nodes) {
  const double pi_rad = 3.14159265;
  // Suppose that the center is at x0, y0.
  const int x0 = points_[0];
  const int y0 = points_[1];

  std::vector<SweepNode>sweep_nodes;
  for (int node = 0; node < static_cast<int>(points_.size()) / 2; ++node) {
    const int x = points_[2 * node];
    const int y = points_[2 * node + 1];
    const double x_delta = x - x0;
    const double y_delta = y - y0;
    double square_distance = x_delta * x_delta + y_delta * y_delta;
    double angle = square_distance == 0 ? 0 :
                                          std::atan2(y_delta, x_delta);
    angle = angle >= 0 ? angle : 2 * pi_rad + angle;
    SweepNode sweep_node(node, angle, square_distance);
    sweep_nodes.push_back(sweep_node);
  }
  sort(sweep_nodes.begin(), sweep_nodes.end(), SweepNodeDistanceComparator);

  const int size = static_cast<int>(sweep_nodes.size()) / sectors_;
  for (int sector = 0; sector < sectors_; ++sector) {
    std::vector<SweepNode> cluster;
    std::vector<SweepNode>::iterator begin = sweep_nodes.begin() + sector * size;
    std::vector<SweepNode>::iterator end =
          sector == sectors_ - 1 ? sweep_nodes.end()
                                 : sweep_nodes.begin() + (sector + 1) * size;
    std::sort(begin, end, SweepNodeAngleComparator);
  }
  for (ConstIter<std::vector<SweepNode> > it(sweep_nodes); !it.at_end(); ++it) {
    nodes->push_back(it->node);
  }
}

// Desicion Builder building a first solution based on Sweep heuristic for
// Vehicle Routing Problem.
// Suitable only when distance is considered as the cost.
class SweepBuilder : public DecisionBuilder {
 public:
  SweepBuilder(RoutingModel* const model,
               bool check_assignment)
      : model_(model),
        check_assignment_(check_assignment) { }
  virtual ~SweepBuilder() { }

  virtual Decision* Next(Solver* const solver) {
    // Setup the model of the instance for the Sweep Algorithm
    ModelSetup();

    // Build the assignment routes for the model
    Assignment* const assignment = solver->MakeAssignment();
    RouteConstructor route_constructor(assignment, model_, check_assignment_,
                                       depot_, nodes_number_, links_,
                                       vehicle_classes_);
    route_constructor.Construct();
    assignment->Restore();

    return NULL;
  }

 private:
  void ModelSetup() {
    depot_ = model_->GetDepot();
    nodes_number_ = model_->nodes();
    if (FLAGS_sweep_sectors > 0 && FLAGS_sweep_sectors < nodes_number_) {
      model_->sweep_arranger()->SetSectors(FLAGS_sweep_sectors);
    }
    model_->sweep_arranger()->ArrangeNodes(&nodes_);
    for (int i = 0; i < nodes_.size() - 1; ++i) {
      const int first = nodes_[i];
      const int second = nodes_[i + 1];
      if (first != depot_ && second != depot_) {
        Link link(std::make_pair(first, second), 0, 0, depot_, depot_);
        links_.push_back(link);
      }
    }
  }

  RoutingModel* const model_;
  const bool check_assignment_;
  int64 nodes_number_;
  int depot_;
  std::vector<Link> links_;
  std::vector<int> nodes_;
  std::vector<VehicleClass> vehicle_classes_;
};

// Decision builder building a solution with a single path without propagating.
// Is very fast but has a very high probability of failing if the problem
// contains other constraints than path-related constraints.
// Based on an addition heuristics extending a path from its start node with
// the cheapest arc according to an evaluator.
class FastOnePathBuilder : public DecisionBuilder {
 public:
  // Takes ownership of evaluator.
  FastOnePathBuilder(RoutingModel* const model,
                     ResultCallback2<int64, int64, int64>* evaluator)
      : model_(model), evaluator_(evaluator) {
    evaluator_->CheckIsRepeatable();
  }
  virtual ~FastOnePathBuilder() {}
  virtual Decision* Next(Solver* const solver) {
    int64 index = -1;
    if (!FindPathStart(&index)) {
      return NULL;
    }
    IntVar* const * nexts = model_->Nexts().data();
    // Need to allocate in a reversible way so that if restoring the assignment
    // fails, the assignment gets de-allocated.
    Assignment* assignment = solver->MakeAssignment();
    int64 next = FindCheapestValue(index, *assignment);
    while (next >= 0) {
      assignment->Add(nexts[index]);
      assignment->SetValue(nexts[index], next);
      index  = next;
      std::vector<int> alternates;
      model_->GetDisjunctionIndicesFromIndex(index, &alternates);
      for (int alternate_index = 0;
           alternate_index < alternates.size();
           ++alternate_index) {
        const int alternate = alternates[alternate_index];
        if (index != alternate) {
          assignment->Add(nexts[alternate]);
          assignment->SetValue(nexts[alternate], alternate);
        }
      }
      next = FindCheapestValue(index, *assignment);
    }
    // Make unassigned nexts loop to themselves.
    // TODO(user): Make finalization more robust, might have some next
    // variables non-instantiated.
    for (int index = 0; index < model_->Size(); ++index) {
      IntVar* const next = nexts[index];
      if (!assignment->Contains(next)) {
        assignment->Add(next);
        if (next->Contains(index)) {
          assignment->SetValue(next, index);
        }
      }
    }
    assignment->Restore();
    return NULL;
  }

 private:
  bool FindPathStart(int64* index) const {
    IntVar* const * nexts = model_->Nexts().data();
    const int size = model_->Size();
    // Try to extend an existing path
    for (int i = size - 1; i >= 0; --i) {
      if (nexts[i]->Bound()) {
        const int next = nexts[i]->Value();
        if (next < size && !nexts[next]->Bound()) {
          *index = next;
          return true;
        }
      }
    }
    // Pick path start
    for (int i = size - 1; i >= 0; --i) {
      if (!nexts[i]->Bound()) {
        bool has_possible_prev = false;
        for (int j = 0; j < size; ++j) {
          if (nexts[j]->Contains(i)) {
            has_possible_prev = true;
            break;
          }
        }
        if (!has_possible_prev) {
          *index = i;
          return true;
        }
      }
    }
    // Pick first unbound
    for (int i = 0; i < size; ++i) {
      if (!nexts[i]->Bound()) {
        *index = i;
        return true;
      }
    }
    return false;
  }

  int64 FindCheapestValue(int index, const Assignment& assignment) const {
    IntVar* const * nexts = model_->Nexts().data();
    const int size = model_->Size();
    int64 best_evaluation = kint64max;
    int64 best_value = -1;
    if (index < size) {
      IntVar* const next = nexts[index];
      scoped_ptr<IntVarIterator> it(next->MakeDomainIterator(false));
      for (it->Init(); it->Ok(); it->Next()) {
        const int value = it->Value();
        if (value != index
            && (value >= size || !assignment.Contains(nexts[value]))) {
          const int64 evaluation = evaluator_->Run(index, value);
          if (evaluation <= best_evaluation) {
            best_evaluation = evaluation;
            best_value = value;
          }
        }
      }
    }
    return best_value;
  }

  RoutingModel* const model_;
  scoped_ptr<ResultCallback2<int64, int64, int64> > evaluator_;
};

// Decision builder to build a solution with all nodes inactive. It does no
// branching and may fail if some nodes cannot be made inactive.

class AllUnperformed : public DecisionBuilder {
 public:
  // Does not take ownership of model.
  explicit AllUnperformed(RoutingModel* const model) : model_(model) {}
  virtual ~AllUnperformed() {}
  virtual Decision* Next(Solver* const solver) {
    for (int i = 0; i < model_->Size(); ++i) {
      if (!model_->IsStart(i)) {
        model_->ActiveVar(i)->SetValue(0);
      }
    }
    return NULL;
  }

 private:
  RoutingModel* const model_;
};

// Flags override strategy selection
RoutingModel::RoutingStrategy
RoutingModel::GetSelectedFirstSolutionStrategy() const {
  RoutingStrategy strategy;
  if (ParseRoutingStrategy(FLAGS_routing_first_solution, &strategy)) {
    return strategy;
  }
  return first_solution_strategy_;
}

RoutingModel::RoutingMetaheuristic
RoutingModel::GetSelectedMetaheuristic() const {
  if (FLAGS_routing_tabu_search) {
    return ROUTING_TABU_SEARCH;
  } else if (FLAGS_routing_simulated_annealing) {
    return ROUTING_SIMULATED_ANNEALING;
  } else if (FLAGS_routing_guided_local_search) {
    return ROUTING_GUIDED_LOCAL_SEARCH;
  }
  return metaheuristic_;
}

void RoutingModel::AddSearchMonitor(SearchMonitor* const monitor) {
  monitors_.push_back(monitor);
}

const Assignment* RoutingModel::Solve(const Assignment* assignment) {
  QuietCloseModel();
  const int64 start_time_ms = solver_->wall_time();
  if (NULL == assignment) {
    solver_->Solve(solve_db_, monitors_);
  } else {
    assignment_->Copy(assignment);
    solver_->Solve(improve_db_, monitors_);
  }
  const int64 elapsed_time_ms = solver_->wall_time() - start_time_ms;
  if (collect_assignments_->solution_count() == 1) {
    status_ = ROUTING_SUCCESS;
    return collect_assignments_->solution(0);
  } else {
    if (elapsed_time_ms >= time_limit_ms_) {
      status_ = ROUTING_FAIL_TIMEOUT;
    } else {
      status_ = ROUTING_FAIL;
    }
    return NULL;
  }
}

// Computing a lower bound to the cost of a vehicle routing problem solving a
// a linear assignment problem (minimum-cost perfect bipartite matching).
// A bipartite graph is created with left nodes representing the nodes of the
// routing problem and right nodes representing possible node successors; an
// arc between a left node l and a right node r is created if r can be the
// node folowing l in a route (Next(l) = r); the cost of the arc is the transit
// cost between l and r in the routing problem.
// This is a lower bound given the solution to assignment problem does not
// necessarily produce a (set of) closed route(s) from a starting node to an
// ending node.
int64 RoutingModel::ComputeLowerBound() {
  if (!closed_) {
    LOG(WARNING) << "Non-closed model not supported.";
    return 0;
  }
  if (!homogeneous_costs_) {
    LOG(WARNING) << "Non-homogeneous vehicle costs not supported";
    return 0;
  }
  if (disjunctions_.size() > 0) {
    LOG(WARNING)
        << "Node disjunction constraints or optional nodes not supported.";
    return 0;
  }
  const int num_nodes = Size() + vehicles_;
  ForwardStarGraph graph(2 * num_nodes, num_nodes * num_nodes);
  LinearSumAssignment<ForwardStarGraph> linear_sum_assignment(graph, num_nodes);
  // Adding arcs for non-end nodes, based on possible values of next variables.
  // Left nodes in the bipartite are indexed from 0 to num_nodes - 1; right
  // nodes are indexed from num_nodes to 2 * num_nodes - 1.
  for (int tail = 0; tail < Size(); ++tail) {
    scoped_ptr<IntVarIterator> iterator(
        nexts_[tail]->MakeDomainIterator(false));
    for (iterator->Init(); iterator->Ok(); iterator->Next()) {
      const int head = iterator->Value();
      // Given there are no disjunction constraints, a node cannot point to
      // itself. Doing this explicitely given that outside the search,
      // propagation hasn't removed this value from next variables yet.
      if (head == tail) {
        continue;
      }
      // The index of a right node in the bipartite graph is the index
      // of the successor offset by the number of nodes.
      const ArcIndex arc = graph.AddArc(tail, num_nodes + head);
      const CostValue cost = GetHomogeneousCost(tail, head);
      linear_sum_assignment.SetArcCost(arc, cost);
    }
  }
  // The linear assignment library requires having as many left and right nodes.
  // Therefore we are creating fake assignments for end nodes, forced to point
  // to the equivalent start node with a cost of 0.
  for (int tail = Size(); tail < num_nodes; ++tail) {
    const ArcIndex arc = graph.AddArc(tail, num_nodes + starts_[tail - Size()]);
    linear_sum_assignment.SetArcCost(arc, 0);
  }
  if (linear_sum_assignment.ComputeAssignment()) {
    return linear_sum_assignment.GetCost();
  }
  return 0;
}

bool RoutingModel::RouteCanBeUsedByVehicle(const Assignment& assignment,
                                           int start_index,
                                           int vehicle) const {
  int current_index = IsStart(start_index) ?
                      Next(assignment, start_index) : start_index;
  while (!IsEnd(current_index)) {
    const IntVar* const vehicle_var = VehicleVar(current_index);
    if (!vehicle_var->Contains(vehicle)) {
      return false;
    }
    const int next_index = Next(assignment, current_index);
    CHECK_NE(next_index, current_index) << "Inactive node inside a route";
    current_index = next_index;
  }
  return true;
}

bool RoutingModel::ReplaceUnusedVehicle(
    int unused_vehicle,
    int active_vehicle,
    Assignment* const compact_assignment) const {
  CHECK_NOTNULL(compact_assignment);
  CHECK(!IsVehicleUsed(*compact_assignment, unused_vehicle));
  CHECK(IsVehicleUsed(*compact_assignment, active_vehicle));
  // Swap NextVars at start nodes.
  const int unused_vehicle_start = Start(unused_vehicle);
  IntVar* const unused_vehicle_start_var = NextVar(unused_vehicle_start);
  const int unused_vehicle_end = End(unused_vehicle);
  const int active_vehicle_start = Start(active_vehicle);
  const int active_vehicle_end = End(active_vehicle);
  IntVar* const active_vehicle_start_var = NextVar(active_vehicle_start);
  const int active_vehicle_next =
      compact_assignment->Value(active_vehicle_start_var);
  compact_assignment->SetValue(unused_vehicle_start_var, active_vehicle_next);
  compact_assignment->SetValue(active_vehicle_start_var, End(active_vehicle));

  // Update VehicleVars along the route, update the last NextVar.
  int current_index = active_vehicle_next;
  while (!IsEnd(current_index)) {
    IntVar* const vehicle_var = VehicleVar(current_index);
    compact_assignment->SetValue(vehicle_var, unused_vehicle);
    const int next_index = Next(*compact_assignment, current_index);
    if (IsEnd(next_index)) {
      IntVar* const last_next_var = NextVar(current_index);
      compact_assignment->SetValue(last_next_var, End(unused_vehicle));
    }
    current_index = next_index;
  }

  // Update dimensions: update transits at the start.
  for (VarMap::const_iterator dimension = transits_.begin();
       dimension != transits_.end(); ++dimension) {
    const std::vector<IntVar*>& transit_variables = dimension->second;
    IntVar* const unused_vehicle_transit_var =
        transit_variables[unused_vehicle_start];
    IntVar* const active_vehicle_transit_var =
        transit_variables[active_vehicle_start];
    const bool contains_unused_vehicle_transit_var =
        compact_assignment->Contains(unused_vehicle_transit_var);
    const bool contains_active_vehicle_transit_var =
        compact_assignment->Contains(active_vehicle_transit_var);
    if (contains_unused_vehicle_transit_var !=
        contains_active_vehicle_transit_var) {
      LG << "The assignment contains transit variable for dimension '"
         << dimension->first << "' for some vehicles, but not for all";
      return false;
    }
    if (contains_unused_vehicle_transit_var) {
      const int64 old_unused_vehicle_transit =
          compact_assignment->Value(unused_vehicle_transit_var);
      const int64 old_active_vehicle_transit =
          compact_assignment->Value(active_vehicle_transit_var);
      compact_assignment->SetValue(unused_vehicle_transit_var,
                                   old_active_vehicle_transit);
      compact_assignment->SetValue(active_vehicle_transit_var,
                                   old_unused_vehicle_transit);
    }

    // Update dimensions: update cumuls at the end.
    const std::vector<IntVar*> empty;
    const std::vector<IntVar*>& cumul_variables =
        FindWithDefault(cumuls_, dimension->first, empty);
    IntVar* const unused_vehicle_cumul_var =
        cumul_variables[unused_vehicle_end];
    IntVar* const active_vehicle_cumul_var =
        cumul_variables[active_vehicle_end];
    const int64 old_unused_vehicle_cumul =
        compact_assignment->Value(unused_vehicle_cumul_var);
    const int64 old_active_vehicle_cumul =
        compact_assignment->Value(active_vehicle_cumul_var);
    compact_assignment->SetValue(unused_vehicle_cumul_var,
                                 old_active_vehicle_cumul);
    compact_assignment->SetValue(active_vehicle_cumul_var,
                                 old_unused_vehicle_cumul);
  }
  return true;
}

Assignment* RoutingModel::CompactAssignment(
    const Assignment& assignment) const {
  CHECK_EQ(assignment.solver(), solver_.get());
  if (!homogeneous_costs_) {
    LG << "The costs are not homogeneous, routes cannot be rearranged";
    return NULL;
  }

  Assignment* compact_assignment = new Assignment(&assignment);
  for (int vehicle = 0; vehicle < vehicles_ - 1; ++vehicle) {
    if (IsVehicleUsed(*compact_assignment, vehicle)) {
      continue;
    }
    const int vehicle_start = Start(vehicle);
    const int vehicle_end = End(vehicle);
    // Find the last vehicle, that can swap routes with this one.
    int swap_vehicle = vehicles_ - 1;
    bool has_more_vehicles_with_route = false;
    for (; swap_vehicle > vehicle; --swap_vehicle) {
      // If a vehicle was already swapped, it will appear in compact_assignment
      // as unused.
      if (!IsVehicleUsed(*compact_assignment, swap_vehicle)
          || !IsVehicleUsed(*compact_assignment, swap_vehicle)) {
        continue;
      }
      has_more_vehicles_with_route = true;
      const int swap_vehicle_start = Start(swap_vehicle);
      const int swap_vehicle_end = End(swap_vehicle);
      if (IndexToNode(vehicle_start) != IndexToNode(swap_vehicle_start)
          || IndexToNode(vehicle_end) != IndexToNode(swap_vehicle_end)) {
        continue;
      }

      // Check that updating VehicleVars is OK.
      if (RouteCanBeUsedByVehicle(*compact_assignment, swap_vehicle_start,
                                  vehicle)) {
        break;
      }
    }

    if (swap_vehicle == vehicle) {
      if (has_more_vehicles_with_route) {
        // No route can be assigned to this vehicle, but there are more vehicles
        // with a route left. This would leave a gap in the indices.
        LG << "No vehicle that can be swapped with " << vehicle << " was found";
        delete compact_assignment;
        return NULL;
      } else {
        break;
      }
    } else {
      if (!ReplaceUnusedVehicle(vehicle, swap_vehicle, compact_assignment)) {
        delete compact_assignment;
        return NULL;
      }
    }
  }
  if (FLAGS_routing_check_compact_assignment) {
    if (!solver_->CheckAssignment(compact_assignment)) {
      LG << "The compacted assignment is not a valid solution";
      delete compact_assignment;
      return NULL;
    }
  }
  return compact_assignment;
}

int RoutingModel::FindNextActive(int index, const std::vector<int>& nodes) const {
  ++index;
  CHECK_LE(0, index);
  const int size = nodes.size();
  while (index < size && ActiveVar(nodes[index])->Max() == 0) {
    ++index;
  }
  return index;
}

IntVar* RoutingModel::ApplyLocks(const std::vector<int>& locks) {
  // TODO(user): Replace calls to this method with calls to
  // ApplyLocksToAllVehicles and remove this method?
  CHECK_EQ(vehicles_, 1);
  preassignment_->Clear();
  IntVar* next_var = NULL;
  int lock_index = FindNextActive(-1, locks);
  const int size = locks.size();
  if (lock_index < size) {
    next_var = NextVar(locks[lock_index]);
    preassignment_->Add(next_var);
    for (lock_index = FindNextActive(lock_index, locks);
         lock_index < size;
         lock_index = FindNextActive(lock_index, locks)) {
      preassignment_->SetValue(next_var, locks[lock_index]);
      next_var = NextVar(locks[lock_index]);
      preassignment_->Add(next_var);
    }
  }
  return next_var;
}

bool RoutingModel::ApplyLocksToAllVehicles(
    const std::vector<std::vector<NodeIndex> >& locks, bool close_routes) {
  preassignment_->Clear();
  return RoutesToAssignment(locks, true, close_routes, preassignment_);
}

void RoutingModel::UpdateTimeLimit(int64 limit_ms) {
  time_limit_ms_ = limit_ms;
  if (limit_ != NULL) {
    solver_->UpdateLimits(time_limit_ms_,
                          kint64max,
                          kint64max,
                          FLAGS_routing_solution_limit,
                          limit_);
  }
  if (ls_limit_ != NULL) {
    solver_->UpdateLimits(time_limit_ms_,
                          kint64max,
                          kint64max,
                          1,
                          ls_limit_);
  }
}

void RoutingModel::UpdateLNSTimeLimit(int64 limit_ms) {
  lns_time_limit_ms_ = limit_ms;
  if (lns_limit_ != NULL) {
    solver_->UpdateLimits(lns_time_limit_ms_,
                          kint64max,
                          kint64max,
                          kint64max,
                          lns_limit_);
  }
}

void RoutingModel::SetCommandLineOption(const string& name,
                                        const string& value) {
  google::SetCommandLineOption(name.c_str(), value.c_str());
}

// static
const char* RoutingModel::RoutingStrategyName(RoutingStrategy strategy) {
  switch (strategy) {
    case ROUTING_DEFAULT_STRATEGY: return "DefaultStrategy";
    case ROUTING_GLOBAL_CHEAPEST_ARC: return "GlobalCheapestArc";
    case ROUTING_LOCAL_CHEAPEST_ARC: return "LocalCheapestArc";
    case ROUTING_PATH_CHEAPEST_ARC: return "PathCheapestArc";
    case ROUTING_EVALUATOR_STRATEGY: return "EvaluatorStrategy";
    case ROUTING_ALL_UNPERFORMED: return "AllUnperformed";
    case ROUTING_BEST_INSERTION: return "BestInsertion";
    case ROUTING_SAVINGS: return "Savings";
    case ROUTING_SWEEP: return "Sweep";
  }
  return NULL;
}

// static
bool RoutingModel::ParseRoutingStrategy(const string& strategy_str,
                                        RoutingStrategy* strategy) {
  for (int i = 0; ; ++i) {
    const RoutingStrategy cur_strategy = static_cast<RoutingStrategy>(i);
    const char* cur_name = RoutingStrategyName(cur_strategy);
    if (cur_name == NULL) return false;
    if (strategy_str == cur_name) {
      *strategy = cur_strategy;
      return true;
    }
  }
}

// static
const char* RoutingModel::RoutingMetaheuristicName(
    RoutingMetaheuristic metaheuristic) {
  switch (metaheuristic) {
    case ROUTING_GREEDY_DESCENT: return "GreedyDescent";
    case ROUTING_GUIDED_LOCAL_SEARCH: return "GuidedLocalSearch";
    case ROUTING_SIMULATED_ANNEALING: return "SimulatedAnnealing";
    case ROUTING_TABU_SEARCH: return "TabuSearch";
  }
  return NULL;
}

// static
bool RoutingModel::ParseRoutingMetaheuristic(
    const string& metaheuristic_str,
    RoutingMetaheuristic* metaheuristic) {
  for (int i = 0; ; ++i) {
    const RoutingMetaheuristic cur_metaheuristic =
        static_cast<RoutingMetaheuristic>(i);
    const char* cur_name = RoutingMetaheuristicName(cur_metaheuristic);
    if (cur_name == NULL) return false;
    if (metaheuristic_str == cur_name) {
      *metaheuristic = cur_metaheuristic;
      return true;
    }
  }
}

bool RoutingModel::WriteAssignment(const string& file_name) const {
  if (collect_assignments_->solution_count() == 1 && assignment_ != NULL) {
    assignment_->Copy(collect_assignments_->solution(0));
    return assignment_->Save(file_name);
  } else {
    return false;
  }
}

Assignment* RoutingModel::ReadAssignment(const string& file_name) {
  QuietCloseModel();
  CHECK(assignment_ != NULL);
  if (assignment_->Load(file_name)) {
    return DoRestoreAssignment();
  }
  return NULL;
}

Assignment* RoutingModel::RestoreAssignment(const Assignment& solution) {
  QuietCloseModel();
  CHECK(assignment_ != NULL);
  assignment_->Copy(&solution);
  return DoRestoreAssignment();
}

Assignment* RoutingModel::DoRestoreAssignment() {
  solver_->Solve(restore_assignment_, monitors_);
  if (collect_assignments_->solution_count() == 1) {
    status_ = ROUTING_SUCCESS;
    return collect_assignments_->solution(0);
  } else {
    status_ = ROUTING_FAIL;
    return NULL;
  }
  return NULL;
}

bool RoutingModel::RoutesToAssignment(const std::vector<std::vector<NodeIndex> >& routes,
                                      bool ignore_inactive_nodes,
                                      bool close_routes,
                                      Assignment* const assignment) const {
  CHECK_NOTNULL(assignment);
  if (!closed_) {
    LOG(ERROR) << "The model is not closed yet";
    return false;
  }
  const int num_routes = routes.size();
  if (num_routes > vehicles_) {
    LOG(ERROR) << "The number of vehicles in the assignment (" << routes.size()
               << ") is greater than the number of vehicles in the model ("
               << vehicles_ << ")";
    return false;
  }

  hash_set<int> visited_indices;
  // Set value to NextVars based on the routes.
  for (int vehicle = 0; vehicle < num_routes; ++vehicle) {
    const std::vector<NodeIndex>& route = routes[vehicle];
    int from_index = Start(vehicle);
    std::pair<hash_set<int>::iterator, bool> insert_result =
        visited_indices.insert(from_index);
    if (!insert_result.second) {
      LOG(ERROR) << "Index " << from_index << " (start node for vehicle "
                 << vehicle << ") was already used";
      return false;
    }

    for (int i = 0; i < route.size(); ++i) {
      const NodeIndex to_node = route[i];
      if (to_node < 0 || to_node >= nodes()) {
        LOG(ERROR) << "Invalid node index: " << to_node;
        return false;
      }
      const int to_index = NodeToIndex(to_node);
      if (to_index < 0 || to_index >= Size()) {
        LOG(ERROR) << "Invalid index: " << to_index << " from node "
                   << to_node;
        return false;
      }

      IntVar* const active_var = ActiveVar(to_index);
      if (active_var->Max() == 0) {
        if (ignore_inactive_nodes) {
          continue;
        } else {
          LOG(ERROR) << "Index " << to_index << " (node " << to_node
                     << ") is not active";
          return false;
        }
      }

      insert_result = visited_indices.insert(to_index);
      if (!insert_result.second) {
        LOG(ERROR) << "Index " << to_index << " (node " << to_node
                   << ") is used multiple times";
        return false;
      }

      const IntVar* const vehicle_var = VehicleVar(to_index);
      if (!vehicle_var->Contains(vehicle)) {
        LOG(ERROR) << "Vehicle " << vehicle << " is not allowed at index "
                   << to_index << " (node " << to_node << ")";
        return false;
      }

      IntVar* const from_var = NextVar(from_index);
      if (!assignment->Contains(from_var)) {
        assignment->Add(from_var);
      }
      assignment->SetValue(from_var, to_index);

      from_index = to_index;
    }

    if (close_routes) {
      IntVar* const last_var = NextVar(from_index);
      if (!assignment->Contains(last_var)) {
        assignment->Add(last_var);
      }
      assignment->SetValue(last_var, End(vehicle));
    }
  }

  // Do not use the remaining vehicles.
  for (int vehicle = num_routes; vehicle < vehicles_; ++vehicle) {
    const int start_index = Start(vehicle);
    // Even if close_routes is false, we still need to add the start index to
    // visited_indices so that deactivating other nodes works correctly.
    std::pair<hash_set<int>::iterator, bool> insert_result =
        visited_indices.insert(start_index);
    if (!insert_result.second) {
      LOG(ERROR) << "Index " << start_index << " is used multiple times";
      return false;
    }
    if (close_routes) {
      IntVar* const start_var = NextVar(start_index);
      if (!assignment->Contains(start_var)) {
        assignment->Add(start_var);
      }
      assignment->SetValue(start_var, End(vehicle));
    }
  }

  // Deactivate other nodes (by pointing them to themselves).
  if (close_routes) {
    for (int index = 0; index < Size(); ++index) {
      if (!ContainsKey(visited_indices, index)) {
        IntVar* const next_var = NextVar(index);
        if (!assignment->Contains(next_var)) {
          assignment->Add(next_var);
        }
        assignment->SetValue(next_var, index);
      }
    }
  }

  return true;
}

Assignment* RoutingModel::ReadAssignmentFromRoutes(
    const std::vector<std::vector<NodeIndex> >& routes,
    bool ignore_inactive_nodes) {
  QuietCloseModel();
  if (!RoutesToAssignment(routes, ignore_inactive_nodes, true, assignment_)) {
    return NULL;
  }
  // DoRestoreAssignment() might still fail when checking constraints (most
  // constraints are not verified by RoutesToAssignment) or when filling in
  // dimension variables.
  return DoRestoreAssignment();
}

void RoutingModel::AssignmentToRoutes(
    const Assignment& assignment,
    std::vector<std::vector<NodeIndex> >*  const routes) const {
  CHECK(closed_);
  CHECK_NOTNULL(routes);

  const int model_size = Size();
  routes->resize(vehicles_);
  for (int vehicle = 0; vehicle < vehicles_; ++vehicle) {
    std::vector<NodeIndex>* const vehicle_route = &routes->at(vehicle);
    vehicle_route->clear();

    int num_visited_nodes = 0;
    const int first_index = Start(vehicle);
    const IntVar* const first_var = NextVar(first_index);
    CHECK(assignment.Contains(first_var));
    CHECK(assignment.Bound(first_var));
    int current_index = assignment.Value(first_var);
    while (!IsEnd(current_index)) {
      vehicle_route->push_back(IndexToNode(current_index));

      const IntVar* const next_var = NextVar(current_index);
      CHECK(assignment.Contains(next_var));
      CHECK(assignment.Bound(next_var));
      current_index = assignment.Value(next_var);

      ++num_visited_nodes;
      CHECK_LE(num_visited_nodes, model_size)
          << "The assignment contains a cycle";
    }
  }
}

RoutingModel::NodeIndex RoutingModel::IndexToNode(int64 index) const {
  DCHECK_LT(index, index_to_node_.size());
  return index_to_node_[index];
}

int64 RoutingModel::NodeToIndex(NodeIndex node) const {
  DCHECK_LT(node, node_to_index_.size());
    DCHECK_NE(node_to_index_[node], kUnassigned);
  return node_to_index_[node];
}

int64 RoutingModel::GetArcCost(int64 i, int64 j, int64 cost_class) {
  if (cost_class >= 0) {
    CostCacheElement& cache = cost_cache_[i];
    if (cache.node == j && cache.cost_class == cost_class) {
      return cache.cost;
    }
    const NodeIndex node_i = IndexToNode(i);
    const NodeIndex node_j = IndexToNode(j);
    int64 cost = 0;
    if (!IsStart(i)) {
      cost = costs_[cost_class]->Run(node_i, node_j);
    } else if (!IsEnd(j)) {
      // Apply route fixed cost on first non-first/last node, in other words on
      // the arc from the first node to its next node if it's not the last node.
      cost = costs_[cost_class]->Run(node_i, node_j)
          + fixed_costs_[index_to_vehicle_[i]];
    } else {
      // If there's only the first and last nodes on the route, it is considered
      // as an empty route thus the cost of 0.
      cost = 0;
    }
    cache.node = j;
    cache.cost_class = cost_class;
    cache.cost = cost;
    return cost;
  } else {
    return 0;
  }
}

bool RoutingModel::IsStart(int64 index) const {
  return !IsEnd(index) && index_to_vehicle_[index] != kUnassigned;
}

bool RoutingModel::IsVehicleUsed(const Assignment& assignment,
                                 int vehicle) const {
  CHECK_GE(vehicle, 0);
  CHECK_LT(vehicle, vehicles_);
  CHECK_EQ(solver_.get(), assignment.solver());
  IntVar* const start_var = NextVar(Start(vehicle));
  CHECK(assignment.Contains(start_var));
  return !IsEnd(assignment.Value(start_var));
}

int64 RoutingModel::Next(const Assignment& assignment,
                         int64 index) const {
  CHECK_EQ(solver_.get(), assignment.solver());
  IntVar* const next_var = NextVar(index);
  CHECK(assignment.Contains(next_var));
  CHECK(assignment.Bound(next_var));
  return assignment.Value(next_var);
}

int64 RoutingModel::GetCost(int64 i, int64 j, int64 vehicle) {
  if (i != j && vehicle >= 0) {
    return GetArcCost(i, j, GetVehicleCostClass(vehicle));
  } else {
    return 0;
  }
}

int64 RoutingModel::GetVehicleClassCost(int64 i, int64 j, int64 cost_class) {
  if (i != j && cost_class >= 0) {
    return GetArcCost(i, j, cost_class);
  } else {
    return 0;
  }
}

// Return high cost if connecting to end node; used in cost-based first solution
int64 RoutingModel::GetFirstSolutionCost(int64 i, int64 j) {
  if (j < nodes_) {
    // TODO(user): Take vehicle into account.
    return GetCost(i, j, 0);
  } else {
    return kint64max;
  }
}

int64 RoutingModel::GetTransitValue(const string& dimension,
                                    int64 from_index, int64 to_index) const {
  Solver::IndexEvaluator2* evaluator =
      FindPtrOrNull(transit_evaluators_, dimension);
  if (evaluator != NULL) {
    return evaluator->Run(from_index, to_index);
  } else {
    return 0;
  }
}

void RoutingModel::CheckDepot() {
  if (!is_depot_set_) {
    LOG(WARNING) << "A depot must be specified, setting one at node 0";
    SetDepot(NodeIndex(0));
  }
}

Assignment* RoutingModel::GetOrCreateAssignment() {
  if (assignment_ == NULL) {
    assignment_ = solver_->MakeAssignment();
    assignment_->Add(nexts_);
    if (!homogeneous_costs_) {
      assignment_->Add(vehicle_vars_);
    }
    assignment_->AddObjective(cost_);
  }
  return assignment_;
}

SearchLimit* RoutingModel::GetOrCreateLimit() {
  if (limit_ == NULL) {
    limit_ = solver_->MakeLimit(time_limit_ms_,
                                kint64max,
                                kint64max,
                                FLAGS_routing_solution_limit,
                                true);
  }
  return limit_;
}

SearchLimit* RoutingModel::GetOrCreateLocalSearchLimit() {
  if (ls_limit_ == NULL) {
    ls_limit_ = solver_->MakeLimit(time_limit_ms_,
                                   kint64max,
                                   kint64max,
                                   1,
                                   true);
  }
  return ls_limit_;
}

SearchLimit* RoutingModel::GetOrCreateLargeNeighborhoodSearchLimit() {
  if (lns_limit_ == NULL) {
    lns_limit_ = solver_->MakeLimit(lns_time_limit_ms_,
                                    kint64max,
                                    kint64max,
                                    kint64max);
  }
  return lns_limit_;
}

LocalSearchOperator* RoutingModel::CreateInsertionOperator() {
  const int size = Size();
  if (pickup_delivery_pairs_.size() > 0) {
    const IntVar* const* vehicle_vars =
        homogeneous_costs_ ? NULL : vehicle_vars_.data();
    return MakePairActive(solver_.get(),
                          nexts_.data(),
                          vehicle_vars,
                          pickup_delivery_pairs_,
                          size);
  } else {
    if (homogeneous_costs_) {
      return solver_->MakeOperator(nexts_, Solver::MAKEACTIVE);
    } else {
      return solver_->MakeOperator(nexts_, vehicle_vars_, Solver::MAKEACTIVE);
    }
  }
}

#define CP_ROUTING_PUSH_BACK_OPERATOR(operator_type)                    \
  if (homogeneous_costs_) {                                             \
    operators.push_back(solver_->MakeOperator(nexts_, operator_type));  \
  } else {                                                              \
    operators.push_back(solver_->MakeOperator(nexts_,                   \
                                              vehicle_vars_,            \
                                              operator_type));          \
  }

#define CP_ROUTING_PUSH_BACK_CALLBACK_OPERATOR(operator_type)           \
  if (homogeneous_costs_) {                                             \
    operators.push_back(solver_->MakeOperator(nexts_,                   \
                                              BuildCostCallback(),      \
                                              operator_type));          \
  } else {                                                              \
    operators.push_back(solver_->MakeOperator(nexts_,                   \
                                              vehicle_vars_,            \
                                              BuildCostCallback(),      \
                                              operator_type));          \
  }

LocalSearchOperator* RoutingModel::CreateNeighborhoodOperators() {
  const int size = Size();
  std::vector<LocalSearchOperator*> operators = extra_operators_;
  if (pickup_delivery_pairs_.size() > 0) {
    const IntVar* const* vehicle_vars =
        homogeneous_costs_ ? NULL : vehicle_vars_.data();
    operators.push_back(MakePairRelocate(solver_.get(),
                                         nexts_.data(),
                                         vehicle_vars,
                                         pickup_delivery_pairs_,
                                         size));
  }
  if (vehicles_ > 1) {
    if (!FLAGS_routing_no_relocate) {
      CP_ROUTING_PUSH_BACK_OPERATOR(Solver::RELOCATE);
    }
    if (!FLAGS_routing_no_exchange) {
      CP_ROUTING_PUSH_BACK_OPERATOR(Solver::EXCHANGE);
    }
    if (!FLAGS_routing_no_cross) {
      CP_ROUTING_PUSH_BACK_OPERATOR(Solver::CROSS);
    }
  }
  if (!FLAGS_routing_no_lkh
      && !FLAGS_routing_tabu_search
      && !FLAGS_routing_simulated_annealing) {
    CP_ROUTING_PUSH_BACK_CALLBACK_OPERATOR(Solver::LK);
  }
  if (!FLAGS_routing_no_2opt) {
    CP_ROUTING_PUSH_BACK_OPERATOR(Solver::TWOOPT);
  }
  if (!FLAGS_routing_no_oropt) {
    CP_ROUTING_PUSH_BACK_OPERATOR(Solver::OROPT);
  }
  if (!FLAGS_routing_no_make_active && disjunctions_.size() != 0) {
    CP_ROUTING_PUSH_BACK_OPERATOR(Solver::MAKEINACTIVE);
    // TODO(user): On cases where we have a mix of node pairs and
    // individual nodes, only pairs are going to be made active. In practice
    // such cases should not appear, but we might want to be robust to them
    // anyway.
    operators.push_back(CreateInsertionOperator());
    if (!FLAGS_routing_use_extended_swap_active) {
      CP_ROUTING_PUSH_BACK_OPERATOR(Solver::SWAPACTIVE);
    } else {
      CP_ROUTING_PUSH_BACK_OPERATOR(Solver::EXTENDEDSWAPACTIVE);
    }
  }
  // TODO(user): move the following operators to a second local search loop.
  if (!FLAGS_routing_no_tsp
      && !FLAGS_routing_tabu_search
      && !FLAGS_routing_simulated_annealing) {
    CP_ROUTING_PUSH_BACK_CALLBACK_OPERATOR(Solver::TSPOPT);
  }
  if (!FLAGS_routing_no_tsplns
      && !FLAGS_routing_tabu_search
      && !FLAGS_routing_simulated_annealing) {
    CP_ROUTING_PUSH_BACK_CALLBACK_OPERATOR(Solver::TSPLNS);
  }
  if (!FLAGS_routing_no_lns) {
    CP_ROUTING_PUSH_BACK_OPERATOR(Solver::PATHLNS);
    if (disjunctions_.size() != 0) {
      CP_ROUTING_PUSH_BACK_OPERATOR(Solver::UNACTIVELNS);
    }
  }
  return solver_->ConcatenateOperators(operators);
}

#undef CP_ROUTING_PUSH_BACK_CALLBACK_OPERATOR
#undef CP_ROUTING_PUSH_BACK_OPERATOR

const std::vector<LocalSearchFilter*>&
RoutingModel::GetOrCreateLocalSearchFilters() {
  if (filters_.empty()) {
    NodeDisjunctionFilter* node_disjunction_filter = NULL;
    if (FLAGS_routing_use_disjunction_filter && !disjunctions_.empty()) {
      node_disjunction_filter =
          solver_->RevAlloc(new NodeDisjunctionFilter(*this));
    }
    if (FLAGS_routing_use_objective_filter) {
      Callback1<int64>* objective_callback = NULL;
      if (node_disjunction_filter != NULL) {
        objective_callback =
            NewPermanentCallback(node_disjunction_filter,
                                 &NodeDisjunctionFilter::InjectObjectiveValue);
      }
      if (homogeneous_costs_) {
        LocalSearchFilter* filter =
            solver_->MakeLocalSearchObjectiveFilter(
                nexts_,
                NewPermanentCallback(this,
                                     &RoutingModel::GetHomogeneousCost),
                objective_callback,
                cost_,
                Solver::EQ,
                Solver::SUM);
        filters_.push_back(filter);
      } else {
        LocalSearchFilter* filter =
            solver_->MakeLocalSearchObjectiveFilter(
                nexts_,
                vehicle_vars_,
                NewPermanentCallback(this, &RoutingModel::GetCost),
                objective_callback,
                cost_,
                Solver::EQ,
                Solver::SUM);
        filters_.push_back(filter);
      }
    }
    filters_.push_back(solver_->MakeVariableDomainFilter());
    if (node_disjunction_filter != NULL) {
      // Must be added after ObjectiveFilter.
      filters_.push_back(node_disjunction_filter);
    }
    if (FLAGS_routing_use_pickup_and_delivery_filter
        && pickup_delivery_pairs_.size() > 0) {
      filters_.push_back(solver_->RevAlloc(new NodePrecedenceFilter(
          nexts_.data(),
          Size(),
          Size() + vehicles_,
          pickup_delivery_pairs_,
          "")));
    }
    if (FLAGS_routing_use_path_cumul_filter) {
      for (ConstIter<VarMap> iter(cumuls_); !iter.at_end(); ++iter) {
        const string& name = iter->first;
        filters_.push_back(solver_->RevAlloc(
            new PathCumulFilter(nexts_.data(),
                                Size(),
                                iter->second.data(),
                                Size() + vehicles_,
                                transit_evaluators_[name],
                                name)));
      }
    }
  }
  return filters_;
}

DecisionBuilder* RoutingModel::CreateSolutionFinalizer() {
  return solver_->MakePhase(nexts_,
                            Solver::CHOOSE_FIRST_UNBOUND,
                            Solver::ASSIGN_MIN_VALUE);
}

DecisionBuilder* RoutingModel::CreateFirstSolutionDecisionBuilder() {
  DecisionBuilder* finalize_solution = CreateSolutionFinalizer();
  DecisionBuilder* first_solution = finalize_solution;
  const RoutingStrategy first_solution_strategy =
      GetSelectedFirstSolutionStrategy();
  LG << "Using first solution strategy: "
     << RoutingStrategyName(first_solution_strategy);
  switch (first_solution_strategy) {
    case ROUTING_GLOBAL_CHEAPEST_ARC:
      first_solution =
          solver_->MakePhase(nexts_,
                             NewPermanentCallback(
                                 this,
                                 &RoutingModel::GetFirstSolutionCost),
                             Solver::CHOOSE_STATIC_GLOBAL_BEST);
      break;
    case ROUTING_LOCAL_CHEAPEST_ARC:
      first_solution =
          solver_->MakePhase(nexts_,
                             Solver::CHOOSE_FIRST_UNBOUND,
                             NewPermanentCallback(
                                 this,
                                 &RoutingModel::GetFirstSolutionCost));
      break;
    case ROUTING_PATH_CHEAPEST_ARC:
      first_solution =
          solver_->MakePhase(nexts_,
                             Solver::CHOOSE_PATH,
                             NewPermanentCallback(
                                 this,
                                 &RoutingModel::GetFirstSolutionCost));
      if (vehicles() == 1) {
        DecisionBuilder* fast_one_path_builder =
            solver_->RevAlloc(new FastOnePathBuilder(
                this,
                NewPermanentCallback(
                    this,
                    &RoutingModel::GetFirstSolutionCost)));
        first_solution = solver_->Try(fast_one_path_builder, first_solution);
      }
      break;
    case ROUTING_EVALUATOR_STRATEGY:
      CHECK(first_solution_evaluator_ != NULL);
      first_solution =
          solver_->MakePhase(nexts_,
                             Solver::CHOOSE_PATH,
                             NewPermanentCallback(
                                 first_solution_evaluator_.get(),
                                 &Solver::IndexEvaluator2::Run));
      break;
    case ROUTING_DEFAULT_STRATEGY:
      break;
    case ROUTING_ALL_UNPERFORMED:
      first_solution =
          solver_->RevAlloc(new AllUnperformed(this));
      break;
    case ROUTING_BEST_INSERTION: {
      SearchLimit* const ls_limit = solver_->MakeLimit(time_limit_ms_,
                                                       kint64max,
                                                       kint64max,
                                                       kint64max,
                                                       true);
      DecisionBuilder* const finalize =
          solver_->MakeSolveOnce(finalize_solution,
                                 GetOrCreateLargeNeighborhoodSearchLimit());
      LocalSearchPhaseParameters* const insertion_parameters =
          solver_->MakeLocalSearchPhaseParameters(
              CreateInsertionOperator(),
              finalize,
              ls_limit,
              GetOrCreateLocalSearchFilters());
      std::vector<SearchMonitor*> monitors;
      monitors.push_back(GetOrCreateLimit());
      std::vector<IntVar*> decision_vars = nexts_;
      if (!homogeneous_costs_) {
        decision_vars.insert(decision_vars.end(),
                             vehicle_vars_.begin(),
                             vehicle_vars_.end());
      }
      first_solution = solver_->MakeNestedOptimize(
          solver_->MakeLocalSearchPhase(
              decision_vars,
              solver_->RevAlloc(new AllUnperformed(this)),
              insertion_parameters),
          GetOrCreateAssignment(),
          false,
          FLAGS_routing_optimization_step,
          monitors);
      first_solution = solver_->Compose(first_solution, finalize);
      break;
    }
    case ROUTING_SAVINGS: {
      first_solution =
          solver_->RevAlloc(new SavingsBuilder(this, true));
      DecisionBuilder* savings_builder =
            solver_->RevAlloc(new SavingsBuilder(this, false));
      first_solution = solver_->Try(savings_builder, first_solution);
      break;
    }
    case ROUTING_SWEEP: {
      first_solution =
          solver_->RevAlloc(new SweepBuilder(this, true));
      DecisionBuilder* sweep_builder =
            solver_->RevAlloc(new SweepBuilder(this, false));
      first_solution = solver_->Try(sweep_builder, first_solution);
      break;
    }
    default:
      LOG(WARNING) << "Unknown argument for routing_first_solution, "
          "using default";
  }
  if (FLAGS_routing_use_first_solution_dive) {
    DecisionBuilder* apply =
        solver_->MakeApplyBranchSelector(NewPermanentCallback(&LeftDive));
    first_solution = solver_->Compose(apply, first_solution);
  }
  return first_solution;
}

LocalSearchPhaseParameters* RoutingModel::CreateLocalSearchParameters() {
  return solver_->MakeLocalSearchPhaseParameters(
      CreateNeighborhoodOperators(),
      solver_->MakeSolveOnce(CreateSolutionFinalizer(),
                             GetOrCreateLargeNeighborhoodSearchLimit()),
      GetOrCreateLocalSearchLimit(),
      GetOrCreateLocalSearchFilters());
}

DecisionBuilder* RoutingModel::CreateLocalSearchDecisionBuilder() {
  const int size = Size();
  DecisionBuilder* first_solution = CreateFirstSolutionDecisionBuilder();
  LocalSearchPhaseParameters* parameters = CreateLocalSearchParameters();
  if (homogeneous_costs_) {
    return solver_->MakeLocalSearchPhase(nexts_,
                                         first_solution,
                                         parameters);
  } else {
    const int all_size = size + size + vehicles_;
    std::vector<IntVar*> all_vars(all_size);
    for (int i = 0; i < size; ++i) {
      all_vars[i] = nexts_[i];
    }
    for (int i = size; i < all_size; ++i) {
      all_vars[i] = vehicle_vars_[i - size];
    }
    return solver_->MakeLocalSearchPhase(all_vars, first_solution, parameters);
  }
}

void RoutingModel::SetupDecisionBuilders() {
  if (FLAGS_routing_dfs) {
    solve_db_ = CreateFirstSolutionDecisionBuilder();
  } else {
    solve_db_ = CreateLocalSearchDecisionBuilder();
  }
  CHECK_NOTNULL(preassignment_);
  DecisionBuilder* restore_preassignment =
      solver_->MakeRestoreAssignment(preassignment_);
  solve_db_ = solver_->Compose(restore_preassignment, solve_db_);
  improve_db_ =
      solver_->Compose(restore_preassignment,
                       solver_->MakeLocalSearchPhase(
                           GetOrCreateAssignment(),
                           CreateLocalSearchParameters()));
  restore_assignment_ =
      solver_->Compose(solver_->MakeRestoreAssignment(GetOrCreateAssignment()),
                       CreateSolutionFinalizer());
}

void RoutingModel::SetupMetaheuristics() {
  SearchMonitor* optimize;
  const RoutingMetaheuristic metaheuristic = GetSelectedMetaheuristic();
  LG << "Using metaheuristic: " << RoutingMetaheuristicName(metaheuristic);
  switch (metaheuristic) {
    case ROUTING_GUIDED_LOCAL_SEARCH:
      if (homogeneous_costs_) {
        optimize = solver_->MakeGuidedLocalSearch(
            false,
            cost_,
            NewPermanentCallback(this, &RoutingModel::GetHomogeneousCost),
            FLAGS_routing_optimization_step,
            nexts_,
            FLAGS_routing_guided_local_search_lamda_coefficient);
      } else {
        optimize = solver_->MakeGuidedLocalSearch(
            false,
            cost_,
            BuildCostCallback(),
            FLAGS_routing_optimization_step,
            nexts_,
            vehicle_vars_,
            FLAGS_routing_guided_local_search_lamda_coefficient);
      }
      break;
    case ROUTING_SIMULATED_ANNEALING:
      optimize =
          solver_->MakeSimulatedAnnealing(false, cost_,
                                          FLAGS_routing_optimization_step,
                                          100);
      break;
    case ROUTING_TABU_SEARCH:
      optimize = solver_->MakeTabuSearch(false, cost_,
                                         FLAGS_routing_optimization_step,
                                         nexts_,
                                         10, 10, .8);
      break;
    default:
      optimize = solver_->MakeMinimize(cost_, FLAGS_routing_optimization_step);
  }
  monitors_.push_back(optimize);
}

void RoutingModel::SetupAssignmentCollector() {
  Assignment* full_assignment = solver_->MakeAssignment();
  for (ConstIter<VarMap> it(cumuls_); !it.at_end(); ++it) {
    full_assignment->Add(it->second);
  }
  for (int i = 0; i < extra_vars_.size(); ++i) {
    full_assignment->Add(extra_vars_[i]);
  }
  full_assignment->Add(nexts_);
  full_assignment->Add(active_);
  full_assignment->Add(vehicle_vars_);
  full_assignment->AddObjective(cost_);

  collect_assignments_ =
      solver_->MakeBestValueSolutionCollector(full_assignment, false);
  monitors_.push_back(collect_assignments_);
}

void RoutingModel::SetupTrace() {
  if (FLAGS_routing_trace) {
    const int kLogPeriod = 10000;
    SearchMonitor* trace = solver_->MakeSearchLog(kLogPeriod, cost_);
    monitors_.push_back(trace);
  }
  if (FLAGS_routing_search_trace) {
    SearchMonitor* trace = solver_->MakeSearchTrace("Routing ");
    monitors_.push_back(trace);
  }
}

void RoutingModel::SetupSearchMonitors() {
  monitors_.push_back(GetOrCreateLimit());
  SetupMetaheuristics();
  SetupAssignmentCollector();
  SetupTrace();
}

void RoutingModel::SetupSearch() {
  SetupDecisionBuilders();
  SetupSearchMonitors();
}


IntVar* RoutingModel::CumulVar(int64 index, const string& name) const {
  const std::vector<IntVar*> empty;
  const std::vector<IntVar*>& named_cumuls = FindWithDefault(cumuls_, name, empty);
  if (!named_cumuls.empty()) {
    return named_cumuls[index];
  } else {
    return NULL;
  }
}

IntVar* RoutingModel::TransitVar(int64 index, const string& name) const {
  const std::vector<IntVar*> empty;
  const std::vector<IntVar*>& named_transits =
      FindWithDefault(transits_, name, empty);
  if (!named_transits.empty()) {
    return named_transits[index];
  } else {
    return NULL;
  }
}

IntVar* RoutingModel::SlackVar(int64 index, const string& name) const {
  const std::vector<IntVar*> empty;
  const std::vector<IntVar*>& named_slacks = FindWithDefault(slacks_, name, empty);
  if (!named_slacks.empty()) {
    return named_slacks[index];
  } else {
    return NULL;
  }
}

void RoutingModel::AddToAssignment(IntVar* const var) {
  extra_vars_.push_back(var);
}

RoutingModel::NodeEvaluator2* RoutingModel::NewCachedCallback(
    NodeEvaluator2* callback) {
  const int size = node_to_index_.size();
  if (FLAGS_routing_cache_callbacks && size <= FLAGS_routing_max_cache_size) {
    routing_caches_.push_back(new RoutingCache(callback, size));
    NodeEvaluator2* const cached_evaluator =
        NewPermanentCallback(routing_caches_.back(), &RoutingCache::Run);
    owned_node_callbacks_.insert(cached_evaluator);
    return cached_evaluator;
  } else {
    owned_node_callbacks_.insert(callback);
    return callback;
  }
}

Solver::IndexEvaluator3* RoutingModel::BuildCostCallback() {
  return NewPermanentCallback(this, &RoutingModel::GetCost);
}

int64 RoutingModel::WrappedVehicleEvaluator(
    VehicleEvaluator* evaluator,
    int64 vehicle) {
  if (vehicle >= 0) {
    return evaluator->Run(vehicle);
  } else {
    return kint64max;
  }
}

const std::vector<IntVar*>& RoutingModel::GetOrMakeCumuls(
    VehicleEvaluator* vehicle_capacity,
    int64 capacity,
    const string& name) {
  const std::vector<IntVar*> empty;
  const std::vector<IntVar*>& named_cumuls = FindWithDefault(cumuls_, name, empty);
  if (named_cumuls.empty()) {
    std::vector<IntVar*> cumuls;
    const int size = Size() + vehicles_;
    solver_->MakeIntVarArray(size, 0LL, capacity, name, &cumuls);
    if (vehicle_capacity != NULL) {
      for (int i = 0; i < size; ++i) {
        IntVar* capacity_var = NULL;
        if (FLAGS_routing_use_light_propagation) {
          capacity_var = solver_->MakeIntVar(0, kint64max);
          solver_->AddConstraint(MakeLightElement(
              solver_.get(),
              capacity_var,
              vehicle_vars_[i],
              NewPermanentCallback(
                  this,
                  &RoutingModel::WrappedVehicleEvaluator,
                  vehicle_capacity)));
        } else {
          capacity_var = solver_->MakeElement(
              NewPermanentCallback(
                  this,
                  &RoutingModel::WrappedVehicleEvaluator,
                  vehicle_capacity),
              vehicle_vars_[i])->Var();
        }
        if (i < Size()) {
          IntVar* const capacity_active = solver_->MakeBoolVar();
          solver_->AddConstraint(
              solver_->MakeLessOrEqual(ActiveVar(i),
                                       capacity_active));
          solver_->AddConstraint(
              solver_->MakeIsLessOrEqualCt(cumuls[i],
                                           capacity_var,
                                           capacity_active));
        } else {
          solver_->AddConstraint(
              solver_->MakeLessOrEqual(cumuls[i], capacity_var));
        }
      }
    }
    capacity_evaluators_[name] = vehicle_capacity;
    cumuls_[name] = cumuls;
    return cumuls_[name];
  }
  return named_cumuls;
}

int64 RoutingModel::WrappedEvaluator(NodeEvaluator2* evaluator,
                                     int64 from,
                                     int64 to) {
  return evaluator->Run(IndexToNode(from), IndexToNode(to));
}

const std::vector<IntVar*>& RoutingModel::GetOrMakeTransits(
    NodeEvaluator2* evaluator,
    int64 slack_max,
    const string& name) {
  evaluator->CheckIsRepeatable();
  const std::vector<IntVar*> empty;
  const std::vector<IntVar*>& named_transits =
      FindWithDefault(transits_, name, empty);
  if (named_transits.empty()) {
    const int size = Size();
    std::vector<IntVar*> transits(size);
    std::vector<IntVar*> slacks(size);
    for (int i = 0; i < size; ++i) {
      IntVar* fixed_transit = NULL;
      if (FLAGS_routing_use_light_propagation) {
        fixed_transit = solver_->MakeIntVar(kint64min, kint64max);
        solver_->AddConstraint(MakeLightElement(
            solver_.get(),
            fixed_transit,
            nexts_[i],
            NewPermanentCallback(
                this,
                &RoutingModel::WrappedEvaluator,
                evaluator,
                static_cast<int64>(i))));
      } else {
        fixed_transit =
            solver_->MakeElement(
                NewPermanentCallback(
                    this,
                    &RoutingModel::WrappedEvaluator,
                    evaluator,
                    static_cast<int64>(i)),
                nexts_[i])->Var();
      }
      if (slack_max == 0) {
        transits[i] = fixed_transit;
        slacks[i] = solver_->MakeIntConst(Zero());
      } else {
        IntVar* const slack_var = solver_->MakeIntVar(0, slack_max, "slack");
        transits[i] = solver_->MakeSum(slack_var, fixed_transit)->Var();
        slacks[i] = slack_var;
      }
    }
    transits_[name] = transits;
    slacks_[name] = slacks;
    transit_evaluators_[name] =
        NewPermanentCallback(this,
                             &RoutingModel::WrappedEvaluator,
                             evaluator);
    owned_index_callbacks_.insert(transit_evaluators_[name]);
    owned_node_callbacks_.insert(evaluator);
    return transits_[name];
  }
  return named_transits;
}
}  // namespace operations_research
