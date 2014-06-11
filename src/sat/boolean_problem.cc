// Copyright 2010-2013 Google
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
#include "sat/boolean_problem.h"

#include "base/hash.h"

#include "base/commandlineflags.h"
#include "base/join.h"
#include "base/map_util.h"
#include "base/hash.h"
#include "algorithms/find_graph_symmetries.h"
#include "graph/graph.h"
#include "graph/util.h"

DEFINE_string(debug_dump_symmetry_graph_to_file, "",
              "If this flag is non-empty, an undirected graph whose"
              " automorphism group is in one-to-one correspondence with the"
              " symmetries of the SAT problem will be dumped to a file every"
              " time FindLinearBooleanProblemSymmetries() is called.");

namespace operations_research {
namespace sat {

void ExtractAssignment(const LinearBooleanProblem& problem,
                       const SatSolver& solver, std::vector<bool>* assignemnt) {
  assignemnt->clear();
  for (int i = 0; i < problem.num_variables(); ++i) {
    assignemnt->push_back(
        solver.Assignment().IsLiteralTrue(Literal(VariableIndex(i), true)));
  }
}

namespace {

// Used by BooleanProblemIsValid() to test that there is no duplicate literals,
// that they are all within range and that there is no zero coefficient.
template <typename LinearTerms>
bool IsValid(const LinearTerms& terms, std::vector<bool>* variable_seen) {
  for (int i = 0; i < terms.literals_size(); ++i) {
    if (terms.literals(i) == 0) return false;
    if (terms.coefficients(i) == 0) return false;
    const int var = Literal(terms.literals(i)).Variable().value();
    if (var >= variable_seen->size() || (*variable_seen)[var]) return false;
    (*variable_seen)[var] = true;
  }
  for (int i = 0; i < terms.literals_size(); ++i) {
    const int var = Literal(terms.literals(i)).Variable().value();
    (*variable_seen)[var] = false;
  }
  return true;
}

}  // namespace

bool BooleanProblemIsValid(const LinearBooleanProblem& problem) {
  std::vector<bool> variable_seen(problem.num_variables(), false);
  for (const LinearBooleanConstraint& constraint : problem.constraints()) {
    if (!IsValid(constraint, &variable_seen)) return false;
  }
  if (!IsValid(problem.objective(), &variable_seen)) return false;
  return true;
}

bool LoadBooleanProblem(const LinearBooleanProblem& problem,
                        SatSolver* solver) {
  DCHECK(BooleanProblemIsValid(problem));
  if (solver->parameters().log_search_progress()) {
    LOG(INFO) << "Loading problem '" << problem.name() << "', "
              << problem.num_variables() << " variables, "
              << problem.constraints_size() << " constraints.";
  }
  solver->SetNumVariables(problem.num_variables());
  std::vector<LiteralWithCoeff> cst;
  int64 num_terms = 0;
  for (const LinearBooleanConstraint& constraint : problem.constraints()) {
    cst.clear();
    num_terms += constraint.literals_size();
    for (int i = 0; i < constraint.literals_size(); ++i) {
      const Literal literal(constraint.literals(i));
      if (literal.Variable() >= problem.num_variables()) {
        LOG(WARNING) << "Literal out of bound: " << literal;
        return false;
      }
      cst.push_back(LiteralWithCoeff(literal, constraint.coefficients(i)));
    }
    if (!solver->AddLinearConstraint(
            constraint.has_lower_bound(), Coefficient(constraint.lower_bound()),
            constraint.has_upper_bound(), Coefficient(constraint.upper_bound()),
            &cst)) {
      return false;
    }
  }
  if (solver->parameters().log_search_progress()) {
    LOG(INFO) << "The problem contains " << num_terms << " terms.";
  }
  return true;
}

void UseObjectiveForSatAssignmentPreference(const LinearBooleanProblem& problem,
                                            SatSolver* solver) {
  // Initialize the heuristic to look for a good solution.
  if (problem.type() == LinearBooleanProblem::MINIMIZATION ||
      problem.type() == LinearBooleanProblem::MAXIMIZATION) {
    const int sign =
        (problem.type() == LinearBooleanProblem::MAXIMIZATION) ? -1 : 1;
    const LinearObjective& objective = problem.objective();
    double max_weight = 0;
    for (int i = 0; i < objective.literals_size(); ++i) {
      max_weight =
          std::max(max_weight, fabs(static_cast<double>(objective.coefficients(i))));
    }
    for (int i = 0; i < objective.literals_size(); ++i) {
      const double weight =
          fabs(static_cast<double>(objective.coefficients(i))) / max_weight;
      if (sign * objective.coefficients(i) > 0) {
        solver->SetAssignmentPreference(
            Literal(objective.literals(i)).Negated(), weight);
      } else {
        solver->SetAssignmentPreference(Literal(objective.literals(i)), weight);
      }
    }
  }
}

bool AddObjectiveConstraint(const LinearBooleanProblem& problem,
                            bool use_lower_bound, Coefficient lower_bound,
                            bool use_upper_bound, Coefficient upper_bound,
                            SatSolver* solver) {
  if (problem.type() != LinearBooleanProblem::MINIMIZATION &&
      problem.type() != LinearBooleanProblem::MAXIMIZATION) {
    return true;
  }
  std::vector<LiteralWithCoeff> cst;
  const LinearObjective& objective = problem.objective();
  for (int i = 0; i < objective.literals_size(); ++i) {
    const Literal literal(objective.literals(i));
    if (literal.Variable() >= problem.num_variables()) {
      LOG(WARNING) << "Literal out of bound: " << literal;
      return false;
    }
    cst.push_back(LiteralWithCoeff(literal, objective.coefficients(i)));
  }
  return solver->AddLinearConstraint(use_lower_bound, lower_bound,
                                     use_upper_bound, upper_bound, &cst);
}

Coefficient ComputeObjectiveValue(const LinearBooleanProblem& problem,
                                  const std::vector<bool>& assignment) {
  CHECK_EQ(assignment.size(), problem.num_variables());
  Coefficient sum(0);
  const LinearObjective& objective = problem.objective();
  for (int i = 0; i < objective.literals_size(); ++i) {
    const Literal literal(objective.literals(i));
    if (assignment[literal.Variable().value()] == literal.IsPositive()) {
      sum += objective.coefficients(i);
    }
  }
  return sum;
}

bool IsAssignmentValid(const LinearBooleanProblem& problem,
                       const std::vector<bool>& assignment) {
  CHECK_EQ(assignment.size(), problem.num_variables());

  // Check that all constraints are satisfied.
  for (const LinearBooleanConstraint& constraint : problem.constraints()) {
    Coefficient sum(0);
    for (int i = 0; i < constraint.literals_size(); ++i) {
      const Literal literal(constraint.literals(i));
      if (assignment[literal.Variable().value()] == literal.IsPositive()) {
        sum += constraint.coefficients(i);
      }
    }
    if (constraint.has_lower_bound() && sum < constraint.lower_bound()) {
      LOG(WARNING) << "Unsatisfied constraint! sum: " << sum << "\n"
                   << constraint.DebugString();
      return false;
    }
    if (constraint.has_upper_bound() && sum > constraint.upper_bound()) {
      LOG(WARNING) << "Unsatisfied constraint! sum: " << sum << "\n"
                   << constraint.DebugString();
      return false;
    }
  }
  return true;
}

// Note(user): This function makes a few assumptions about the format of the
// given LinearBooleanProblem. All constraint coefficients must be 1 (and of the
// form >= 1) and all objective weights must be strictly positive.
std::string LinearBooleanProblemToCnfString(const LinearBooleanProblem& problem) {
  std::string output;
  const bool is_wcnf = (problem.type() == LinearBooleanProblem::MINIMIZATION);
  const LinearObjective& objective = problem.objective();

  // Hack: We know that all the variables with index greater than this have been
  // created "artificially" in order to encode a max-sat problem into our
  // format. Each extra variable appear only once, and was used as a slack to
  // reify a soft clause.
  const int first_slack_variable = problem.original_num_variables();

  // This will contains the objective.
  hash_map<int, int64> literal_to_weight;
  std::vector<std::pair<int, int64>> non_slack_objective;

  // This will be the weight of the "hard" clauses in the wcnf format. It must
  // be greater than the sum of the weight of all the soft clauses, so we will
  // just set it to this sum + 1.
  int64 hard_weigth = 1;
  if (is_wcnf) {
    int i = 0;
    for (int64 weight : objective.coefficients()) {
      CHECK_NE(weight, 0);
      int signed_literal = objective.literals(i);

      // There is no direct support for an objective offset in the wcnf format.
      // So this is not a perfect translation of the objective. It is however
      // possible to achieve the same effect by adding a new variable x, and two
      // soft clauses: x with weight offset, and -x with weight offset.
      //
      // TODO(user): implement this trick.
      if (weight < 0) {
        signed_literal = -signed_literal;
        weight = -weight;
      }
      literal_to_weight[objective.literals(i)] = weight;
      if (Literal(signed_literal).Variable() < first_slack_variable) {
        non_slack_objective.push_back(std::make_pair(signed_literal, weight));
      }
      hard_weigth += weight;
      ++i;
    }
    output += StringPrintf("p wcnf %d %d %lld\n", first_slack_variable,
                           static_cast<int>(problem.constraints_size() +
                                            non_slack_objective.size()),
                           hard_weigth);
  } else {
    output += StringPrintf("p cnf %d %d\n", problem.num_variables(),
                           problem.constraints_size());
  }

  std::string constraint_output;
  for (const LinearBooleanConstraint& constraint : problem.constraints()) {
    if (constraint.literals_size() == 0) return "";  // Assumption.
    constraint_output.clear();
    int64 weight = hard_weigth;
    for (int i = 0; i < constraint.literals_size(); ++i) {
      if (constraint.coefficients(i) != 1) return "";  // Assumption.
      if (is_wcnf && abs(constraint.literals(i)) - 1 >= first_slack_variable) {
        weight = literal_to_weight[constraint.literals(i)];
      } else {
        if (i > 0) constraint_output += " ";
        constraint_output += Literal(constraint.literals(i)).DebugString();
      }
    }
    if (is_wcnf) {
      output += StringPrintf("%lld ", weight);
    }
    output += constraint_output + " 0\n";
  }

  // Output the rest of the objective as singleton constraints.
  if (is_wcnf) {
    for (std::pair<int, int64> p : non_slack_objective) {
      // Since it is falsifying this clause that cost "weigth", we need to take
      // its negation.
      const Literal literal(-p.first);
      output +=
          StringPrintf("%lld %s 0\n", p.second, literal.DebugString().c_str());
    }
  }

  return output;
}

void StoreAssignment(const VariablesAssignment& assignment,
                     BooleanAssignment* output) {
  output->clear_literals();
  for (VariableIndex var(0); var < assignment.NumberOfVariables(); ++var) {
    if (assignment.IsVariableAssigned(var)) {
      output->add_literals(
          assignment.GetTrueLiteralForAssignedVariable(var).SignedValue());
    }
  }
}

void ExtractSubproblem(const LinearBooleanProblem& problem,
                       const std::vector<int>& constraint_indices,
                       LinearBooleanProblem* subproblem) {
  subproblem->CopyFrom(problem);
  subproblem->set_name("Subproblem of " + problem.name());
  subproblem->clear_constraints();
  for (int index : constraint_indices) {
    CHECK_LT(index, problem.constraints_size());
    subproblem->add_constraints()->MergeFrom(problem.constraints(index));
  }
}

namespace {
// A simple class to generate equivalence class number for
// GenerateGraphForSymmetryDetection().
class IdGenerator {
 public:
  IdGenerator() {}

  // If the pair (type, coefficient) was never seen before, then generate
  // a new id, otherwise return the previously generated id.
  int GetId(int type, Coefficient coefficient) {
    const std::pair<int, int64> key(type, coefficient.value());
    return LookupOrInsert(&id_map_, key, id_map_.size());
  }

 private:
#if defined(_MSC_VER)
  hash_map<std::pair<int, int64>, int, PairIntInt64Hasher> id_map_;
#else
  hash_map<std::pair<int, int64>, int> id_map_;
#endif
};
}  // namespace.

// Returns a graph whose automorphisms can be mapped back to the symmetries of
// the given LinearBooleanProblem.
//
// Any permutation of the graph that respects the initial_equivalence_classes
// output can be mapped to a symmetry of the given problem simply by taking its
// restriction on the first 2 * num_variables nodes and interpreting its index
// as a literal index. In a sense, a node with a low enough index #i is in
// one-to-one correspondance with a literals #i (using the index representation
// of literal).
//
// The format of the initial_equivalence_classes is the same as the one
// described in GraphSymmetryFinder::FindSymmetries(). The classes must be dense
// in [0, num_classes) and any symmetry will only map nodes with the same class
// between each other.
template <typename Graph>
Graph* GenerateGraphForSymmetryDetection(
    const LinearBooleanProblem& problem,
    std::vector<int>* initial_equivalence_classes) {
  // First, we convert the problem to its canonical representation.
  const int num_variables = problem.num_variables();
  CanonicalBooleanLinearProblem canonical_problem;
  std::vector<LiteralWithCoeff> cst;
  for (const LinearBooleanConstraint& constraint : problem.constraints()) {
    cst.clear();
    for (int i = 0; i < constraint.literals_size(); ++i) {
      const Literal literal(constraint.literals(i));
      cst.push_back(LiteralWithCoeff(literal, constraint.coefficients(i)));
    }
    CHECK(canonical_problem.AddLinearConstraint(
        constraint.has_lower_bound(), Coefficient(constraint.lower_bound()),
        constraint.has_upper_bound(), Coefficient(constraint.upper_bound()),
        &cst));
  }

  // TODO(user): reserve the memory for the graph? not sure it is worthwhile
  // since it would require some linear scan of the problem though.
  Graph* graph = new Graph();
  initial_equivalence_classes->clear();

  // We will construct a graph with 3 different types of node that must be
  // in different equivalent classes.
  enum NodeType { LITERAL_NODE, CONSTRAINT_NODE, CONSTRAINT_COEFFICIENT_NODE };
  IdGenerator id_generator;

  // First, we need one node per literal with an edge between each literal
  // and its negation.
  for (int i = 0; i < num_variables; ++i) {
    // We have two nodes for each variable.
    // Note that the indices are in [0, 2 * num_variables) and in one to one
    // correspondance with the index representation of a literal.
    const Literal literal = Literal(VariableIndex(i), true);
    graph->AddArc(literal.Index().value(), literal.NegatedIndex().value());
    graph->AddArc(literal.NegatedIndex().value(), literal.Index().value());
  }

  // We use 0 for their initial equivalence class, but that may be modified
  // with the objective coefficient (see below).
  initial_equivalence_classes->assign(
      2 * num_variables,
      id_generator.GetId(NodeType::LITERAL_NODE, Coefficient(0)));

  // Literals with different objective coeffs shouldn't be in the same class.
  if (problem.type() == LinearBooleanProblem::MINIMIZATION ||
      problem.type() == LinearBooleanProblem::MAXIMIZATION) {
    // We need to canonicalize the objective to regroup literals corresponding
    // to the same variables.
    std::vector<LiteralWithCoeff> expr;
    const LinearObjective& objective = problem.objective();
    for (int i = 0; i < objective.literals_size(); ++i) {
      const Literal literal(objective.literals(i));
      expr.push_back(LiteralWithCoeff(literal, objective.coefficients(i)));
    }
    // Note that we don't care about the offset or optimization direction here,
    // we just care about literals with the same canonical coefficient.
    Coefficient shift;
    Coefficient max_value;
    ComputeBooleanLinearExpressionCanonicalForm(&expr, &shift, &max_value);
    for (LiteralWithCoeff term : expr) {
      (*initial_equivalence_classes)[term.literal.Index().value()] =
          id_generator.GetId(NodeType::LITERAL_NODE, term.coefficient);
    }
  }

  // Then, for each constraint, we will have one or more nodes.
  for (int i = 0; i < canonical_problem.NumConstraints(); ++i) {
    // First we have a node for the constraint with an equivalence class
    // depending on the rhs.
    //
    // Note: Since we add nodes one by one, initial_equivalence_classes->size()
    // gives the number of nodes at any point, which we use as next node index.
    const int constraint_node_index = initial_equivalence_classes->size();
    initial_equivalence_classes->push_back(id_generator.GetId(
        NodeType::CONSTRAINT_NODE, canonical_problem.Rhs(i)));

    // This node will also be connected to all literals of the constraint
    // with a coefficient of 1. Literals with new coefficients will be grouped
    // under a new node connected to the constraint_node_index.
    //
    // Note that this works because a canonical constraint is sorted by
    // increasing coefficient value (all positive).
    int current_node_index = constraint_node_index;
    Coefficient previous_coefficient(1);
    for (LiteralWithCoeff term : canonical_problem.Constraint(i)) {
      if (term.coefficient != previous_coefficient) {
        current_node_index = initial_equivalence_classes->size();
        initial_equivalence_classes->push_back(id_generator.GetId(
            NodeType::CONSTRAINT_COEFFICIENT_NODE, term.coefficient));
        previous_coefficient = term.coefficient;

        // Connect this node to the constraint node. Note that we don't
        // technically need the arcs in both directions, but that may help a bit
        // the algorithm to find symmetries.
        graph->AddArc(constraint_node_index, current_node_index);
        graph->AddArc(current_node_index, constraint_node_index);
      }

      // Connect this node to the associated term.literal node. Note that we
      // don't technically need the arcs in both directions, but that may help a
      // bit the algorithm to find symmetries.
      graph->AddArc(current_node_index, term.literal.Index().value());
      graph->AddArc(term.literal.Index().value(), current_node_index);
    }
  }
  graph->Build();
  DCHECK_EQ(graph->num_nodes(), initial_equivalence_classes->size());
  return graph;
}

void MakeAllLiteralsPositive(LinearBooleanProblem* problem) {
  // Objective.
  LinearObjective* mutable_objective = problem->mutable_objective();
  int64 objective_offset = 0;
  for (int i = 0; i < mutable_objective->literals_size(); ++i) {
    const int signed_literal = mutable_objective->literals(i);
    if (signed_literal < 0) {
      const int64 coefficient = mutable_objective->coefficients(i);
      mutable_objective->set_literals(i, -signed_literal);
      mutable_objective->set_coefficients(i, -coefficient);
      objective_offset += coefficient;
    }
  }
  mutable_objective->set_offset(mutable_objective->offset() + objective_offset);

  // Constraints.
  for (LinearBooleanConstraint& constraint :
       *(problem->mutable_constraints())) {
    int64 sum = 0;
    for (int i = 0; i < constraint.literals_size(); ++i) {
      if (constraint.literals(i) < 0) {
        sum += constraint.coefficients(i);
        constraint.set_literals(i, -constraint.literals(i));
        constraint.set_coefficients(i, -constraint.coefficients(i));
      }
    }
    if (constraint.has_lower_bound()) {
      constraint.set_lower_bound(constraint.lower_bound() - sum);
    }
    if (constraint.has_upper_bound()) {
      constraint.set_upper_bound(constraint.upper_bound() - sum);
    }
  }
}

void FindLinearBooleanProblemSymmetries(
    const LinearBooleanProblem& problem,
    std::vector<std::unique_ptr<SparsePermutation>>* generators) {
  typedef GraphSymmetryFinder::Graph Graph;
  std::vector<int> equivalence_classes;
  std::unique_ptr<Graph> graph(
      GenerateGraphForSymmetryDetection<Graph>(problem, &equivalence_classes));
  LOG(INFO) << "Graph has " << graph->num_nodes() << " nodes and "
            << graph->num_arcs() / 2 << " edges.";
  if (!FLAGS_debug_dump_symmetry_graph_to_file.empty()) {
    // Remap the graph nodes to sort them by equivalence classes.
    std::vector<int> new_node_index(graph->num_nodes(), -1);
    const int num_classes = 1 + *std::max_element(equivalence_classes.begin(),
                                                  equivalence_classes.end());
    std::vector<int> class_size(num_classes, 0);
    for (const int c : equivalence_classes) ++class_size[c];
    std::vector<int> next_index_by_class(num_classes, 0);
    std::partial_sum(class_size.begin(), class_size.end() - 1,
                     next_index_by_class.begin() + 1);
    for (int node = 0; node < graph->num_nodes(); ++node) {
      new_node_index[node] = next_index_by_class[equivalence_classes[node]]++;
    }
    std::unique_ptr<Graph> remapped_graph(
        RemapGraph(*graph, new_node_index).ValueOrDie());
    const util::Status status = WriteGraphToFile(
        *remapped_graph, FLAGS_debug_dump_symmetry_graph_to_file,
        /*directed=*/false, class_size);
    if (!status.ok()) {
      LOG(DFATAL) << "Error when writing the symmetry graph to file: "
                  << status.ToString();
    }
  }
  GraphSymmetryFinder symmetry_finder(*graph.get(),
                                      /*graph_is_undirected=*/true);
  std::vector<int> factorized_automorphism_group_size;
  // TODO(user): inject the appropriate time limit here.
  CHECK_OK(symmetry_finder.FindSymmetries(
      /*time_limit_seconds=*/std::numeric_limits<double>::infinity(),
      &equivalence_classes, generators, &factorized_automorphism_group_size));

  // Remove from the permutations the part not concerning the literals.
  // Note that some permutation may becomes empty, which means that we had
  // duplicates constraints. TODO(user): Remove them beforehand?
  double average_support_size = 0.0;
  int num_generators = 0;
  for (int i = 0; i < generators->size(); ++i) {
    SparsePermutation* permutation = (*generators)[i].get();
    std::vector<int> to_delete;
    for (int j = 0; j < permutation->NumCycles(); ++j) {
      if (*(permutation->Cycle(j).begin()) >= 2 * problem.num_variables()) {
        to_delete.push_back(j);
        if (DEBUG_MODE) {
          // Verify that the cycle's entire support does not touch any variable.
          for (const int node : permutation->Cycle(j)) {
            DCHECK_GE(node, 2 * problem.num_variables());
          }
        }
      }
    }
    permutation->RemoveCycles(to_delete);
    if (!permutation->Support().empty()) {
      average_support_size += permutation->Support().size();
      swap((*generators)[num_generators], (*generators)[i]);
      ++num_generators;
    }
  }
  generators->resize(num_generators);
  average_support_size /= num_generators;
  LOG(INFO) << "# of generators: " << num_generators;
  LOG(INFO) << "Average support size: " << average_support_size;
}

}  // namespace sat
}  // namespace operations_research
