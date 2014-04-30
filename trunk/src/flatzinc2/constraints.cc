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
#include <string>
#include "base/commandlineflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/hash.h"
#include "flatzinc2/model.h"
#include "flatzinc2/search.h"
#include "flatzinc2/solver.h"
#include "constraint_solver/constraint_solver.h"
#include "util/string_array.h"

DECLARE_bool(verbose_logging);

namespace operations_research {
// List of tricks to add to the current flatzinc extraction process.
//   - store all different
//   - array_var_int_element -> value bound => array_var_int_position


void ExtractAllDifferentInt(FzSolver* fzsolver, FzConstraint* ct) {
  Solver* const s = fzsolver->solver();
  const std::vector<IntVar*> vars = fzsolver->GetVariableArray(ct->Arg(0));
  s->AddConstraint(s->MakeAllDifferent(vars));
}

void ExtractAlldifferentExcept0(FzSolver* fzsolver, FzConstraint* ct) {
  Solver* const s = fzsolver->solver();
  const std::vector<IntVar*> vars = fzsolver->GetVariableArray(ct->Arg(0));
  s->AddConstraint(s->MakeAllDifferentExcept(vars, 0));
}

void ExtractArrayBoolAnd(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractArrayBoolOr(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractArrayBoolXor(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractArrayIntElement(FzSolver* fzsolver, FzConstraint* ct) {
  Solver* const solver =  fzsolver->solver();
  IntExpr* const index = fzsolver->GetExpression(ct->Arg(0));
  const std::vector<int64>& values = ct->Arg(1).values;
  const int64 imin = std::max(index->Min(), 1LL);
  const int64 imax =
      std::min(index->Max(), static_cast<int64>(values.size()) + 1LL);
  IntVar* const shifted_index = solver->MakeSum(index, -imin)->Var();
  const int64 size = imax - imin + 1;
  std::vector<int64> coefficients(size);
  for (int i = 0; i < size; ++i) {
    coefficients[i] = values[i + imin - 1];
  }
  if (ct->target_variable != nullptr) {
    DCHECK_EQ(ct->Arg(2).Var(), ct->target_variable);
    IntExpr* const target = solver->MakeElement(coefficients, shifted_index);
    FZVLOG << "  - creating " << ct->Arg(2).DebugString()
           << " := " << target->DebugString() << FZENDL;
    fzsolver->SetExtracted(ct->target_variable, target);
  } else {
    IntVar* const target = fzsolver->GetExpression(ct->Arg(2))->Var();
    Constraint* const ct =
        solver->MakeElementEquality(coefficients, shifted_index, target);
    FZVLOG << "  - posted " << ct->DebugString() << FZENDL;
    solver->AddConstraint(ct);
  }
}

void ExtractArrayVarIntElement(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractArrayVarIntPosition(FzSolver* solver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractBool2int(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL)
      << "Constraint should have been presolved out: " << ct->DebugString();
}

void ExtractBoolAnd(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractBoolClause(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractBoolLeftImp(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractBoolNot(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractBoolOr(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractBoolRightImp(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractBoolXor(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractCircuit(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractCountEq(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractCountGeq(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractCountGt(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractCountLeq(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractCountLt(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractCountNeq(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractCountReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractDiffn(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractFixedCumulative(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractGlobalCardinality(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractGlobalCardinalityClosed(FzSolver* fzsolver,
                                    FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractGlobalCardinalityLowUp(FzSolver* fzsolver,
                                   FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractGlobalCardinalityLowUpClosed(FzSolver* fzsolver,
                                         FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractGlobalCardinalityOld(FzSolver* fzsolver,
                                 FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntAbs(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntDiv(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntEq(FzSolver* fzsolver, FzConstraint* ct) {
  Solver* const s = fzsolver->solver();
  if (ct->Arg(0).type == FzArgument::INT_VAR_REF) {
    IntExpr* const left = fzsolver->GetExpression(ct->Arg(0));
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeEquality(left, right));
    } else {
      const int64 right = ct->Arg(1).Value();
      s->AddConstraint(s->MakeEquality(left, right));
    }
  } else {
    const int64 left = ct->Arg(0).Value();
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeEquality(right, left));
    } else {
      const int64 right = ct->Arg(1).Value();
      if (left != right) {
        s->AddConstraint(s->MakeFalseConstraint());
      }
    }
  }
}

void ExtractIntEqReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntGe(FzSolver* fzsolver, FzConstraint* ct) {
  Solver* const s = fzsolver->solver();
  if (ct->Arg(0).type == FzArgument::INT_VAR_REF) {
    IntExpr* const left = fzsolver->GetExpression(ct->Arg(0));
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeGreaterOrEqual(left, right));
    } else {
      const int64 right = ct->Arg(1).Value();
      s->AddConstraint(s->MakeGreaterOrEqual(left, right));
    }
  } else {
    const int64 left = ct->Arg(0).Value();
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeLessOrEqual(right, left));
    } else {
      const int64 right = ct->Arg(1).Value();
      if (left < right) {
        s->AddConstraint(s->MakeFalseConstraint());
      }
    }
  }
}

void ExtractIntGeReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntGt(FzSolver* fzsolver, FzConstraint* ct) {
  Solver* const s = fzsolver->solver();
  if (ct->Arg(0).type == FzArgument::INT_VAR_REF) {
    IntExpr* const left = fzsolver->GetExpression(ct->Arg(0));
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeGreater(left, right));
    } else {
      const int64 right = ct->Arg(1).Value();
      s->AddConstraint(s->MakeGreater(left, right));
    }
  } else {
    const int64 left = ct->Arg(0).Value();
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeLess(right, left));
    } else {
      const int64 right = ct->Arg(1).Value();
      if (left <= right) {
        s->AddConstraint(s->MakeFalseConstraint());
      }
    }
  }
}

void ExtractIntGtReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntIn(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntLe(FzSolver* fzsolver, FzConstraint* ct) {
  Solver* const s = fzsolver->solver();
  if (ct->Arg(0).type == FzArgument::INT_VAR_REF) {
    IntExpr* const left = fzsolver->GetExpression(ct->Arg(0));
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeLessOrEqual(left, right));
    } else {
      const int64 right = ct->Arg(1).Value();
      s->AddConstraint(s->MakeLessOrEqual(left, right));
    }
  } else {
    const int64 left = ct->Arg(0).Value();
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeGreaterOrEqual(right, left));
    } else {
      const int64 right = ct->Arg(1).Value();
      if (left > right) {
        s->AddConstraint(s->MakeFalseConstraint());
      }
    }
  }
}

void ExtractIntLeReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntLinEq(FzSolver* fzsolver, FzConstraint* ct) {
  Solver* const s = fzsolver->solver();
  const std::vector<int64>& coefficients = ct->Arg(0).values;
  std::vector<IntVar*> vars = fzsolver->GetVariableArray(ct->Arg(1));
  const int64 rhs = ct->Arg(2).Value();
  s->AddConstraint(s->MakeScalProdEquality(vars, coefficients, rhs));
}

void ExtractIntLinEqReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntLinGe(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntLinGeReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntLinLe(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntLinLeReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntLinNe(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntLinNeReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntLt(FzSolver* fzsolver, FzConstraint* ct) {
  Solver* const s = fzsolver->solver();
  if (ct->Arg(0).type == FzArgument::INT_VAR_REF) {
    IntExpr* const left = fzsolver->GetExpression(ct->Arg(0));
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeLess(left, right));
    } else {
      const int64 right = ct->Arg(1).Value();
      s->AddConstraint(s->MakeLess(left, right));
    }
  } else {
    const int64 left = ct->Arg(0).Value();
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeGreater(right, left));
    } else {
      const int64 right = ct->Arg(1).Value();
      if (left >= right) {
        s->AddConstraint(s->MakeFalseConstraint());
      }
    }
  }
}

void ExtractIntLtReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntMax(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntMin(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntMinus(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntMod(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntNe(FzSolver* fzsolver, FzConstraint* ct) {
  Solver* const s = fzsolver->solver();
  if (ct->Arg(0).type == FzArgument::INT_VAR_REF) {
    IntExpr* const left = fzsolver->GetExpression(ct->Arg(0));
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeNonEquality(left, right));
    } else {
      const int64 right = ct->Arg(1).Value();
      s->AddConstraint(s->MakeNonEquality(left, right));
    }
  } else {
    const int64 left = ct->Arg(0).Value();
    if (ct->Arg(1).type == FzArgument::INT_VAR_REF) {
      IntExpr* const right = fzsolver->GetExpression(ct->Arg(1));
      s->AddConstraint(s->MakeNonEquality(right, left));
    } else {
      const int64 right = ct->Arg(1).Value();
      if (left == right) {
        s->AddConstraint(s->MakeFalseConstraint());
      }
    }
  }
}

void ExtractIntNeReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntNegate(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntPlus(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractIntTimes(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractInverse(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractLexLessBool(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractLexLessInt(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractLexLesseqBool(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractLexLesseqInt(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractMaximumInt(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractMinimumInt(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractNvalue(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractRegular(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractSetIn(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractSetInReif(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractSlidingSum(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractSort(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractTableBool(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractTableInt(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractTrueConstraint(FzSolver* fzsolver, FzConstraint* ct) {}

void ExtractVarCumulative(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void ExtractVariableCumulative(FzSolver* fzsolver, FzConstraint* ct) {
  LOG(FATAL) << "Not implemented: Extract " << ct->DebugString();
}

void FzSolver::ExtractConstraint(FzConstraint* ct) {
  FZVLOG << "Extracting " << ct->DebugString() << std::endl;
  const std::string& type = ct->type;
  if (type == "all_different_int") {
    ExtractAllDifferentInt(this, ct);
  } else if (type == "alldifferent_except_0") {
    ExtractAlldifferentExcept0(this, ct);
  } else if (type == "array_bool_and") {
    ExtractArrayBoolAnd(this, ct);
  } else if (type == "array_bool_element") {
    ExtractArrayIntElement(this, ct);
  } else if (type == "array_bool_or") {
    ExtractArrayBoolOr(this, ct);
  } else if (type == "array_bool_xor") {
    ExtractArrayBoolXor(this, ct);
  } else if (type == "array_int_element") {
    ExtractArrayIntElement(this, ct);
  } else if (type == "array_var_bool_element") {
    ExtractArrayVarIntElement(this, ct);
  } else if (type == "array_var_int_element") {
    ExtractArrayVarIntElement(this, ct);
  } else if (type == "array_var_int_position") {
    ExtractArrayVarIntPosition(this, ct);
  } else if (type == "bool2int") {
    ExtractBool2int(this, ct);
  } else if (type == "bool_and") {
    ExtractBoolAnd(this, ct);
  } else if (type == "bool_clause") {
    ExtractBoolClause(this, ct);
  } else if (type == "bool_eq") {
    ExtractIntEq(this, ct);
  } else if (type == "bool_eq_reif") {
    ExtractIntEqReif(this, ct);
  } else if (type == "bool_ge") {
    ExtractIntGe(this, ct);
  } else if (type == "bool_ge_reif") {
    ExtractIntGeReif(this, ct);
  } else if (type == "bool_gt") {
    ExtractIntGt(this, ct);
  } else if (type == "bool_gt_reif") {
    ExtractIntGtReif(this, ct);
  } else if (type == "bool_le") {
    ExtractIntLe(this, ct);
  } else if (type == "bool_le_reif") {
    ExtractIntLeReif(this, ct);
  } else if (type == "bool_left_imp") {
    ExtractBoolLeftImp(this, ct);
  } else if (type == "bool_lin_eq") {
    ExtractIntLinEq(this, ct);
  } else if (type == "bool_lin_le") {
    ExtractIntLinLe(this, ct);
  } else if (type == "bool_lt") {
    ExtractIntLt(this, ct);
  } else if (type == "bool_lt_reif") {
    ExtractIntLtReif(this, ct);
  } else if (type == "bool_ne") {
    ExtractIntNe(this, ct);
  } else if (type == "bool_ne_reif") {
    ExtractIntNeReif(this, ct);
  } else if (type == "bool_not") {
    ExtractBoolNot(this, ct);
  } else if (type == "bool_or") {
    ExtractBoolOr(this, ct);
  } else if (type == "bool_right_imp") {
    ExtractBoolRightImp(this, ct);
  } else if (type == "bool_xor") {
    ExtractBoolXor(this, ct);
  } else if (type == "circuit") {
    ExtractCircuit(this, ct);
  } else if (type == "count_eq") {
    ExtractCountEq(this, ct);
  } else if (type == "count_geq") {
    ExtractCountGeq(this, ct);
  } else if (type == "count_gt") {
    ExtractCountGt(this, ct);
  } else if (type == "count_leq") {
    ExtractCountLeq(this, ct);
  } else if (type == "count_lt") {
    ExtractCountLt(this, ct);
  } else if (type == "count_neq") {
    ExtractCountNeq(this, ct);
  } else if (type == "count_reif") {
    ExtractCountReif(this, ct);
  } else if (type == "diffn") {
    ExtractDiffn(this, ct);
  } else if (type == "fixed_cumulative") {
    ExtractFixedCumulative(this, ct);
  } else if (type == "global_cardinality") {
    ExtractGlobalCardinality(this, ct);
  } else if (type == "global_cardinality_closed") {
    ExtractGlobalCardinalityClosed(this, ct);
  } else if (type == "global_cardinality_low_up") {
    ExtractGlobalCardinalityLowUp(this, ct);
  } else if (type == "global_cardinality_low_up_closed") {
    ExtractGlobalCardinalityLowUpClosed(this, ct);
  } else if (type == "global_cardinality_old") {
    ExtractGlobalCardinalityOld(this, ct);
  } else if (type == "int_abs") {
    ExtractIntAbs(this, ct);
  } else if (type == "int_div") {
    ExtractIntDiv(this, ct);
  } else if (type == "int_eq") {
    ExtractIntEq(this, ct);
  } else if (type == "int_eq_reif") {
    ExtractIntEqReif(this, ct);
  } else if (type == "int_ge") {
    ExtractIntGe(this, ct);
  } else if (type == "int_ge_reif") {
    ExtractIntGeReif(this, ct);
  } else if (type == "int_gt") {
    ExtractIntGt(this, ct);
  } else if (type == "int_gt_reif") {
    ExtractIntGtReif(this, ct);
  } else if (type == "int_in") {
    ExtractIntIn(this, ct);
  } else if (type == "int_le") {
    ExtractIntLe(this, ct);
  } else if (type == "int_le_reif") {
    ExtractIntLeReif(this, ct);
  } else if (type == "int_lin_eq") {
    ExtractIntLinEq(this, ct);
  } else if (type == "int_lin_eq_reif") {
    ExtractIntLinEqReif(this, ct);
  } else if (type == "int_lin_ge") {
    ExtractIntLinGe(this, ct);
  } else if (type == "int_lin_ge_reif") {
    ExtractIntLinGeReif(this, ct);
  } else if (type == "int_lin_le") {
    ExtractIntLinLe(this, ct);
  } else if (type == "int_lin_le_reif") {
    ExtractIntLinLeReif(this, ct);
  } else if (type == "int_lin_ne") {
    ExtractIntLinNe(this, ct);
  } else if (type == "int_lin_ne_reif") {
    ExtractIntLinNeReif(this, ct);
  } else if (type == "int_lt") {
    ExtractIntLt(this, ct);
  } else if (type == "int_lt_reif") {
    ExtractIntLtReif(this, ct);
  } else if (type == "int_max") {
    ExtractIntMax(this, ct);
  } else if (type == "int_min") {
    ExtractIntMin(this, ct);
  } else if (type == "int_minus") {
    ExtractIntMinus(this, ct);
  } else if (type == "int_mod") {
    ExtractIntMod(this, ct);
  } else if (type == "int_ne") {
    ExtractIntNe(this, ct);
  } else if (type == "int_ne_reif") {
    ExtractIntNeReif(this, ct);
  } else if (type == "int_negate") {
    ExtractIntNegate(this, ct);
  } else if (type == "int_plus") {
    ExtractIntPlus(this, ct);
  } else if (type == "int_times") {
    ExtractIntTimes(this, ct);
  } else if (type == "inverse") {
    ExtractInverse(this, ct);
  } else if (type == "lex_less_bool") {
    ExtractLexLessBool(this, ct);
  } else if (type == "lex_less_int") {
    ExtractLexLessInt(this, ct);
  } else if (type == "lex_lesseq_bool") {
    ExtractLexLesseqBool(this, ct);
  } else if (type == "lex_lesseq_int") {
    ExtractLexLesseqInt(this, ct);
  } else if (type == "maximum_int") {
    ExtractMaximumInt(this, ct);
  } else if (type == "minimum_int") {
    ExtractMinimumInt(this, ct);
  } else if (type == "nvalue") {
    ExtractNvalue(this, ct);
  } else if (type == "regular") {
    ExtractRegular(this, ct);
  } else if (type == "set_in") {
    ExtractSetIn(this, ct);
  } else if (type == "set_in_reif") {
    ExtractSetInReif(this, ct);
  } else if (type == "sliding_sum") {
    ExtractSlidingSum(this, ct);
  } else if (type == "sort") {
    ExtractSort(this, ct);
  } else if (type == "table_bool") {
    ExtractTableBool(this, ct);
  } else if (type == "table_int") {
    ExtractTableInt(this, ct);
  } else if (type == "true_constraint") {
    ExtractTrueConstraint(this, ct);
  } else if (type == "var_cumulative") {
    ExtractVarCumulative(this, ct);
  } else if (type == "variable_cumulative") {
    ExtractVariableCumulative(this, ct);
  }
}
}  // namespace operations_research
