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
#ifndef OR_TOOLS_FLATZINC_PRESOLVE_H_
#define OR_TOOLS_FLATZINC_PRESOLVE_H_

#include <string>
#include "base/hash.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/hash.h"
#include "flatzinc2/model.h"

namespace operations_research {
// This struct stores linear mapping to one variable.
// This represents var * coefficient + offset.
struct AffineMapping {
  FzIntegerVariable* variable;
  int64 coefficient;
  int64 offset;
  FzConstraint* constraint;

  AffineMapping()
      : variable(nullptr), coefficient(0), offset(0), constraint(nullptr) {}
  AffineMapping(FzIntegerVariable* v, int64 c, int64 o, FzConstraint* ct)
      : variable(v), coefficient(c), offset(o), constraint(ct) {}
};


// The FzPresolver "pre-solves" a FzModel by applying some iterative
// transformations to it, which may simplify and/or reduce the model.
class FzPresolver {
 public:
  // Recursively apply all the pre-solve rules to the model, until exhaustion.
  // The reduced model will:
  // - Have some unused variables
  // - Have some unused constraints (marked as "is_trivially_true").
  // - Have some modified constraints (for example, they will no longer
  //   refer to unused variables)
  // TODO(user): compute on the fly, and add an API to access the set of
  // unused variables.
  //
  // This returns true iff some transformations were applied to the model
  bool Run(FzModel* model);

  // Cleans the model for the CP solver.
  // In particular, it knows about the sat connection and will remove the link
  // (defining_constraint, target_variable) for boolean constraints.
  void CleanUpModelForTheCpSolver(FzModel* model);

 private:
  // Returns true iff the model was modified.
  bool PresolveOneConstraint(FzConstraint* ct);

  // Substitution support.
  void SubstituteEverywhere(FzModel* model);
  void SubstituteAnnotation(FzAnnotation* ann);

  // Presolve rules.
  bool PresolveBool2Int(FzConstraint* ct);
  bool PresolveIntEq(FzConstraint* ct);
  void Unreify(FzConstraint* ct);
  bool PresolveInequalities(FzConstraint* ct);
  bool PresolveIntNe(FzConstraint* ct);
  bool PresolveSetIn(FzConstraint* ct);
  bool PresolveArrayBoolAnd(FzConstraint* ct);
  bool PresolveArrayBoolOr(FzConstraint* ct);
  bool PresolveBoolEqNeReif(FzConstraint* ct);
  bool PresolveArrayIntElement(FzConstraint* ct);
  bool PresolveIntDiv(FzConstraint* ct);
  bool PresolveIntTimes(FzConstraint* ct);
  bool PresolveIntLinGt(FzConstraint* ct);
  bool PresolveIntLinLt(FzConstraint* ct);
  bool PresolveLinear(FzConstraint* ct);
  bool PresolvePropagatePositiveLinear(FzConstraint* ct);
  bool PresolveStoreMapping(FzConstraint* ct);
  bool PresolveSimplifyElement(FzConstraint* ct);

  // The presolver will discover some equivalence classes of variables [two
  // variable are equivalent when replacing one by the other leads to the same
  // logical model]. We will store them here, using a Union-find data structure.
  // See http://en.wikipedia.org/wiki/Disjoint-set_data_structure.
  void MarkVariablesAsEquivalent(FzIntegerVariable* from,
                                 FzIntegerVariable* to);
  FzIntegerVariable* FindRepresentativeOfVar(FzIntegerVariable* var);
  hash_map<const FzIntegerVariable*, FzIntegerVariable*>
      var_representative_map_;

  // Stores abs_map_[x] = y if x = abs(y).
  hash_map<const FzIntegerVariable*, FzIntegerVariable*> abs_map_;

  // Stores linear_map_[x] = a * y + b.
  hash_map<const FzIntegerVariable*, AffineMapping> affine_map_;
};
}  // namespace operations_research

#endif  // OR_TOOLS_FLATZINC_PRESOLVE_H_
