
#include "eval.hpp"
#include "ast.hpp"
#include "scope.hpp"
#include "type.hpp"
#include "value.hpp"
#include "subst.hpp"

#include "lang/debug.hpp"

#include <iostream>
#include <set>

// -------------------------------------------------------------------------- //
// Evaluator class

Term*
Evaluator::operator()(Term* t) {
  return eval(t);
}


// -------------------------------------------------------------------------- //
// Multi-step evaluation
//
// The following function computes the multi-step evaluation (or
// simply evaluation) of a term t. Note that the evaluation is reflexive, 
// meaning that the evaluation of a value (or normal form) is simply
// an identity operation.

Term* eval(Term*);

namespace {

// Compute the multistep evaluation of an if term
//
//             t1 ->* true
//    ---------------------------- E-if-true
//    if t1 then t2 else t3 ->* t2
//
//             t1 ->* true
//    ---------------------------- E-if-false
//    if t1 then t2 else t3 ->* t2
Term*
eval_if(If* t) {
  Term* bv = eval(t->cond());
  if (is_true(bv))
    return eval(t->if_true());
  if (is_false(bv))
    return eval(t->if_false());
  lang_unreachable(format("'{}' is not a boolean value", pretty(bv)));
}

// Compute the multi-step evaluation of a successor term.
//
//         t ->* n
//    ---------------- E-succ
//    succ t ->* n + 1
//
// Here, 'n' is an integer value.
Term*
eval_succ(Succ* t) {
  Term* t1 = eval(t->arg());
  if (Int* n = as<Int>(t1)) {
    const Integer& z = n->value();
    return new Int(t->loc, get_type(t), z + 1);
  }
  lang_unreachable(format("'{}' is not a numeric value", pretty(t1)));
}

// Evalutae a predecessor term.
//
//      t ->* 0
//    ------------ E-pred-0
//    pred t ->* 0
//
//         t ->* n
//    ---------------- E-pred-succ
//    pred t ->* n - 1
//
// Here, 'n' is an integer value.
Term*
eval_pred(Pred* t) {
  Term* t1 = eval(t->arg());
  if (Int* n = as<Int>(t1)) {
    const Integer& z = n->value();
    if (z == 0)
      return n;
    else
      return new Int(t->loc, get_type(t), z - 1);
  }
  lang_unreachable(format("'{}' is not a numeric value", pretty(t1)));
}

// Evaluate an iszero term.
//
//         t ->* 0
//    ----------------- E-iszero-0
//    iszero t ->* true
//
//         t ->* n
//    ------------------ E-iszero-succ
//    iszero t ->* false
Term*
eval_iszero(Iszero* t) {
  Term* t1 = eval(t->arg());
  if (Int* n = as<Int>(t1)) {
    const Integer& z = n->value();
    if (z == 0)
      return new True(t->loc, get_bool_type());
    else
      return new False(t->loc, get_bool_type());
  }
  lang_unreachable(format("'{}' is not a numeric value", pretty(t1)));
}

// Evaluate an application.
//
//        t1 ->* \x:T.t
//    --------------------- E-app-1
//    t1 t2 ->* (\x:T.t) t2
//
//          t2 ->* v
//    --------------------- E-app-2
//    \x:T.t t2 ->* [x->v]t
Term*
eval_app(App* t) {
  Abs* fn = as<Abs>(eval(t->abs())); // E-app-1
  lang_assert(fn, format("ill-formed application target '{}'", pretty(t->abs())));

  Term* arg = eval(t->arg()); // E-app-2
    
  // Perform a beta reduction and evaluate the result.
  Subst sub {fn->var(), arg};
  Term* res = subst_term(fn->term(), sub);
  return eval(res);
}

// Evaluate a function call. This is virtually identical to
// application except that all arguments are evaluated in turn.
//
// TODO: Document the semantics of these operations.
Term*
eval_call(Call* t) {
  // Evaluate the function.
  Fn* fn = as<Fn>(eval(t->fn()));
  lang_assert(fn, format("ill-formed call target '{}'", pretty(t->fn())));

  // Evaluate arguments in place. That is, we're not creating
  // a new sequence of arguments, just replacing the entries
  // in the existing sequence.
  Term_seq* args = t->args();
  for (Term*& a : *args)
    a = eval(a);

  // Beta reduce and evaluate.
  Subst sub {fn->parms(), args};
  Term* result = subst_term(fn->term(), sub);
  return eval(result);
}

// Elaborate a declaration reference. When the reference
// is to a definition, replace it with the definition's value.
// Otherwise, preserve the reference.
//
// If the reference is to a type, then we can't evaluate this.
// Just return nullptr and hope that the caller knows how to
// handle the results.
Term*
eval_ref(Ref* t) {
  if (Def* def = as<Def>(t->decl())) {
    if (Term* replace = as<Term>(def->value()))
      return replace;
    else
      return nullptr;
  } else {
    return t;
  }
}

// Evaluate the definition by evaluating the defined term. When the
// definition's value is not a term, then there isn't anything
// interesting that we can do. Just return the value.
Term*
eval_def(Def* t) {
  if (Term* t0 = as<Term>(t->value())) {
    // This is a little weird. We're actually going to update
    // the defined term with its evaluated initializer. We do this
    // because other expressions may already refer to t and we don't
    // really want to re-resolve all of those things.
    //
    // Note that we could choose to do this during elaboration
    // in order to avoid the weirdness.
    t->t2 = eval(t0);
  }
  return t;
}

// Elaborate a print statement.
//
//          t ->* v
//    ------------------- E-print-term
//    print t ->* print v
//
//    --------------- E-print-value
//    print v -> unit
//
//    --------------- E-print-type
//    print T -> unit
//
Term*
eval_print(Print* t) {
  // Try to evaluate the expression.
  Term* val = nullptr;
  if (Term* term = as<Term>(t->expr()))
    val = eval(term);

  // Print the result, or if the expression is not
  // evaluable, just print the expression.
  if (val)
    std::cout << pretty(val) << '\n';
  else
    std::cout << pretty(t->expr()) << '\n';

  return new Unit(t->loc, get_unit_type());
}

// FIXME: Actually evaluate each expression in turn.
Term*
eval_comma(Comma* t) {
  return get_unit();
}

// Evaluate each statement in turn; the result of the program is
// the result of the last statemnt. 
//
//    for each i ei ->* vi
//    -------------------- E-prog
//     e1; ...; en ->* vn
Term*
eval_prog(Prog* t) {
  Term* tn;
  for (Term* ti : *t->stmts())
    tn = eval(ti);
  return tn;
}

// Evaluation for 't1 and t2'
//
// t1 ->* true   t2 -> true
// ------------------------
// t1 and t2 ->* true
//
// t1 ->* false   t2 -> true
// ------------------------
// t1 and t2 ->* false
//
// t1 ->* true   t2 -> false
// ------------------------
// t1 and t2 ->* false
//
// t1 ->* false  t2 -> false
// ------------------------
// t1 and t2 ->* false
Term*
eval_and(And* t) {
  Term* t1 = eval(t->t1);
  Term* t2 = eval(t->t2);

  if(is_true(t1) && is_true(t2))
    return get_true();
  else 
    return get_false();
}

// Evaluation for 't1 or t2'
//
// t1 ->* true   t2 -> true
// ------------------------
// t1 or t2 ->* true
//
// t1 ->* false   t2 -> true
// ------------------------
// t1 or t2 ->* true
//
// t1 ->* true   t2 -> false
// ------------------------
// t1 or t2 ->* true
//
// t1 ->* false  t2 -> false
// ------------------------
// t1 or t2 ->* false
//
Term*
eval_or(Or* t) {
  Term* t1 = eval(t->t1);
  Term* t2 = eval(t->t2);

  if(is_false(t1) && is_false(t2))
    return get_false();
  else 
    return get_true();
}

// Evaluation for 'not t1'
// 
// t1 ->* false
// --------------
// not t1 ->* true
//
// t1 ->* true
// --------------
// not t1 ->* false
//
Term*
eval_not(Not* t) {
  Term* t1 = eval(t->t1);

  if(is_true(t1))
    return get_false();
  if(is_false(t1))
    return get_true();
}

// Evaluation for t1 == t2
// 
// t1 ->* t1' t2 ->* t2*  is_same(t1',t2') -> true
// -----------------------------------------------
// t1 == t2 ->* true
//
// t1 ->* t1' t2 ->* t2'  is_same(t1,t2) -> false
// -----------------------------------------------
// t1 == t2 ->* false
//
Term*
eval_equals(Equals* t) {
  Term* t1 = eval(t->t1);
  Term* t2 = eval(t->t2);

  if(is_same(t1, t2))
    return get_true();
  else
    return get_false();
}

// Evaluation for the term 't1 < t2'
// 
// t1 ->* t1' t2 ->* t2*  is_less(t1', t2') -> true
// ------------------------------------------------
// t1 < t2 ->* true
//
// t1 ->* t1' t2 ->* t2'  is_less(t1',t2') -> false
// ------------------------------------------------
// t1 < t2 ->* false
//
Term*
eval_less(Less* t) {
  Term* t1 = eval(t->t1);
  Term* t2 = eval(t->t2);

  std::cout << pretty(t1);
  std::cout << pretty(t2);

  if(is_less(t1, t2))
    return get_true();
  else
    return get_false();
}

///////////////////////////////////
//
// Evaluation for Relational Algebra
//
///////////////////////////////////

// Returns a term from the record such that the label in the record matches l
// Returns nullptr if the label l does not match anything in record r
Term*
eval_record_project(Term* l, Record* r) {
  //TODO: Test this. Probably shady.
  for(auto t : *(r->members())) {
    if( is_same(as<Name>(l), as<Init>(t)->name()) ) {
      return as<Term>(as<Init>(t)->value());
    }
  }
  return nullptr;
}

// Perform projection a table's columns
// project should be a Comma term where each subterm is of type Init
Term*
eval_table_project(Comma* project, Table* t) {
  //TODO: Implement
  return t;
}

// Evaluate 
Term*
eval_proj(Proj* t) {
  //TODO Implement
}

// Evaluation for Mem term
Term*
eval_mem(Mem* t) {
  switch(t->record()->kind) {
  case record_term: eval_record_project(eval(t->member()), as<Record>(eval(t->record())));
  case table_term:  eval_table_project(as<Comma>(eval(t->member())), as<Table>(eval(t->record())));
  }
}

Term*
eval_table_select(Term* cond, Table* t) {
  Term_seq records = *(t->members());
  Table* result;
  for (auto r : records){
    //every value in r needs to be subsituted through cond
    //evaluate cond
    //if cond is true we add the record to the resulting table
    //else ignore
  } //not done
}

Term*
eval_table_product(Table* t1, Table* t2) {

}

//evaluation for select t1 from t2 where t3
Term*
eval_select_from_where(Select_from_where* t) {
  //perform selection
  Table* table = as<Table>(eval_table_select(t->t3, as<Table>(t->t2)));
  //perform projection
    //first we need to eval all the terms in t1 (the comma list)
  //populate new table
  //return new table
}

Term*
eval_join(Join* t) {
  //perform product between tables
  //perform selection of resulting product
  //perform projection on table
  //populate new table
  //ereturn new table
}

// Returns the intersection of two tables
// TODO: test this
Term*
eval_intersect_table(Table* t1, Table* t2) {
  Table* result;
  for(auto r1 : *(t1->members())) {
    for(auto r2 : *(t2->members())) {
      if(is_same(r1, r2)) {
        result->t1->push_back(r1);
      }
    }
  }
  return result;
}

Term*
eval_intersect(Intersect* t) {
  //eval t1
  Term* t1 = eval(t->t1);
  //eval t2
  Term* t2 = eval(t->t2);
  //perform intersection
  switch(t1->kind) {
  case table_term: return eval_intersect_table(as<Table>(t1), as<Table>(t2));
  }
}

// TODO: test this
Term*
eval_union_table(Table* t1, Table* t2) {
  Term_seq* schema = new Term_seq(*(t1->schema()));
  Term_seq* records = new Term_seq(*(t1->members()));
  for(auto r : *(t2->members())) {
    records->push_back(r);
  }
  //remove duplicates by converting to set then back
  std::set<Term*> s(records->begin(), records->end());
  records->assign(s.begin(), s.end());

  return new Table(get_kind_type(), schema, records);
}

Term*
eval_union(Union* t) {
  //eval t1
  Term* t1 = eval(t->t1);
  //eval t2
  Term* t2 = eval(t->t2);
  //perform union
  switch(t1->kind) {
  case table_term: return eval_union_table(as<Table>(t1), as<Table>(t2));
  }
}

// TODO: test this
Term*
eval_except_table(Table* t1, Table* t2) {
  Term_seq* diff;
  Term_seq* schema = new Term_seq(*(t1->schema()));
  for(auto r1 : *(t1->members())) {
    bool r2_contains = false;
    for(auto r2 : *(t2->members())) {
      if(is_same(r1, r2)) {
        r2_contains = true;
      }
    }
    if(!r2_contains) {
      diff->push_back(r1);
    }
  }
  return new Table(get_kind_type(), schema, diff);
}

Term*
eval_except(Except* t) {
  //eval t1
  Term* t1 = eval(t->t1);
  //eval t2
  Term* t2 = eval(t->t2);
  //perform difference
  switch(t1->kind) {
  case table_term: return eval_except_table(as<Table>(t1), as<Table>(t2));
  }
}

} // namespace

// Compute the multi-step evaluation of the term t. 
Term*
eval(Term* t) {
  switch (t->kind) {
  case if_term: return eval_if(as<If>(t));
  case and_term: return eval_and(as<And>(t));
  case or_term: return eval_or(as<Or>(t));
  case not_term: return eval_not(as<Not>(t));
  case equals_term: return eval_equals(as<Equals>(t));
  case less_term: return eval_less(as<Less>(t));
  case succ_term: return eval_succ(as<Succ>(t));
  case pred_term: return eval_pred(as<Pred>(t));
  case iszero_term: return eval_iszero(as<Iszero>(t));
  case app_term: return eval_app(as<App>(t));
  case call_term: return eval_call(as<Call>(t));
  case ref_term: return eval_ref(as<Ref>(t));
  case print_term: return eval_print(as<Print>(t));
  case def_term: return eval_def(as<Def>(t));
  case prog_term: return eval_prog(as<Prog>(t));
  case comma_term: return eval_comma(as<Comma>(t));
  case proj_term: return eval_proj(as<Proj>(t));
  case mem_term: return eval_mem(as<Mem>(t));
  case select_term: return eval_select_from_where(as<Select_from_where>(t));
  case join_on_term: return eval_join(as<Join>(t));
  case union_term: return eval_union(as<Union>(t));
  case inter_term: return eval_intersect(as<Intersect>(t));
  case except_term: return eval_except(as<Except>(t));
  default: break;
  }
  return t;
}


// Compute the one-step evaluation of the term t.
Term*
step(Term* t) {
  lang_unreachable("not implemented");
}

