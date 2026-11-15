// gc_hazard: where native code holds a borrowed value (one it has no count
// of) across a call that can run the program's code, which can let the
// value's owner go and free it under the native code (docs/internals/vm.md
// §5.3). tools/checks/check_gc_hazard.sh runs it and holds the result to an
// allowlist.
//
// Three rules:
//   R1 callee : the callee handed to _jit_invoke* / _culebra_invoke* /
//               culebra_runtime_call* is borrowed (the call takes no count of
//               its callee).
//   R2 escape : a borrowed value is captured by a lambda that becomes a
//               std::function, or stored into a field: kept past the call
//               that lent it.
//   R3 live   : a borrowed value is live after a call that can run program
//               code: one that reaches a closure's fn_ptr ("invoke": also a
//               release, whose `drop` does, and an allocation, whose
//               collection's finalize does), or an unknown function pointer,
//               virtual or std::function ("indirect").
//
// "Borrowed" is by provenance, not type: a local initialized or assigned
// from a read through a pointer (`o->slots[i]`, `a->items[i]`), a call in
// kBorrowingCalls, or a cast of another borrowed value's `.data`.
// Parameters are the caller's to hold. A RAII holder (JitOwnedVal,
// JitHeldMethod) is not a value type, so what it holds is owned. What a
// runtime-built closure's body reads from its own `captures` (the running
// closure: the JitClosure* a JitFn takes second, a JitIterFastFn first) is
// what the runtime made it with, and is not counted.
//
// Not seen: the IR the JIT emits, and code main.cc's translation unit does
// not include (the gate checks every file that calls in was analyzed).
//
// Output, one per line:
//   H <rule> <file> <function> <var> <decl> <site> <tier> <callee>
//   seen <file>
// <function> is the enclosing named function (a lambda's is its outer one),
// unqualified by parameters; <decl> tells same-named variables apart.
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/Analyses/LiveVariables.h"
#include "clang/Analysis/CFG.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace clang;

namespace {

enum Tier : unsigned { kInvoke = 1, kIndirect = 2 };

const std::set<std::string> kBorrowingCalls = {
    "culebra_runtime_object_get",
    "culebra_runtime_object_get_ic",
    "_jit_dispatcher_mono_body",
    "culebra::vm::ReplSession::value",
};

bool is_invoke_entry(const std::string& n) {
  return n.rfind("_jit_invoke", 0) == 0 || n.rfind("_culebra_invoke", 0) == 0 ||
         n.rfind("culebra_runtime_call", 0) == 0;
}

// The value types: JitValue, or a pointer to a counted runtime struct.
bool is_value_type(QualType t) {
  if (t.isNull()) return false;
  t = t.getCanonicalType().getUnqualifiedType();
  bool pointer = false;
  if (const auto* p = t->getAs<PointerType>()) {
    t = p->getPointeeType().getCanonicalType().getUnqualifiedType();
    pointer = true;
  }
  const auto* rd = t->getAsCXXRecordDecl();
  if (!rd || !rd->getIdentifier()) return false;
  // A JitValue* points at storage (a frame's registers, an argument run),
  // not at a counted object.
  if (pointer && rd->getName() == "JitValue") return false;
  static const std::set<std::string> names = {
      "JitValue", "JitArray", "JitTensor", "JitObject",
      "JitCell",  "JitClosure", "JitSet",  "JitHandle"};
  if (names.count(rd->getName().str())) return true;
  if (rd->hasDefinition())
    for (const auto& b : rd->bases())
      if (const auto* br = b.getType()->getAsCXXRecordDecl())
        if (br->getName() == "JitHandle") return true;
  return false;
}

// `std::function<...>::operator()`.
bool is_std_function_call(const FunctionDecl* d) {
  const auto* m = dyn_cast<CXXMethodDecl>(d);
  if (!m || m->getOverloadedOperator() != OO_Call) return false;
  const auto* rd = m->getParent();
  return rd->getName() == "function" && rd->isInStdNamespace();
}

// ---------------------------------------------------------------------------
// Pass 1: which functions can run program code.

struct Graph {
  std::map<const FunctionDecl*, std::set<const FunctionDecl*>> callers;
  std::map<const FunctionDecl*, unsigned> seed, tier;

  void edge(const FunctionDecl* from, const FunctionDecl* to) {
    if (from && to)
      callers[to->getCanonicalDecl()].insert(from->getCanonicalDecl());
  }
  void mark(const FunctionDecl* f, unsigned t) {
    if (f) seed[f->getCanonicalDecl()] |= t;
  }
  void solve() {
    std::vector<const FunctionDecl*> work;
    for (auto& [f, t] : seed) {
      tier[f] |= t;
      work.push_back(f);
    }
    while (!work.empty()) {
      auto* f = work.back();
      work.pop_back();
      unsigned t = tier[f];
      for (auto* c : callers[f]) {
        unsigned& ct = tier[c];
        if ((ct | t) != ct) {
          ct |= t;
          work.push_back(c);
        }
      }
    }
  }
  unsigned of(const FunctionDecl* f) const {
    if (!f) return 0;
    auto it = tier.find(f->getCanonicalDecl());
    return it == tier.end() ? 0 : it->second;
  }
};

bool callee_is_jitfn(const CallExpr* ce) {
  QualType t = ce->getCallee()->IgnoreParenImpCasts()->getType();
  return !t.isNull() && t.getAsString() == "JitFn";
}

class GraphBuilder : public RecursiveASTVisitor<GraphBuilder> {
 public:
  GraphBuilder(Graph& g, SourceManager& sm) : g_(g), sm_(sm) {}
  bool shouldVisitTemplateInstantiations() const { return true; }
  bool shouldVisitImplicitCode() const { return true; }

  bool TraverseFunctionDecl(FunctionDecl* f) { return in(f, [&] { return RecursiveASTVisitor::TraverseFunctionDecl(f); }); }
  bool TraverseCXXMethodDecl(CXXMethodDecl* f) { return in(f, [&] { return RecursiveASTVisitor::TraverseCXXMethodDecl(f); }); }
  bool TraverseCXXConstructorDecl(CXXConstructorDecl* f) { return in(f, [&] { return RecursiveASTVisitor::TraverseCXXConstructorDecl(f); }); }
  bool TraverseCXXDestructorDecl(CXXDestructorDecl* f) { return in(f, [&] { return RecursiveASTVisitor::TraverseCXXDestructorDecl(f); }); }
  bool TraverseCXXConversionDecl(CXXConversionDecl* f) { return in(f, [&] { return RecursiveASTVisitor::TraverseCXXConversionDecl(f); }); }
  bool TraverseLambdaExpr(LambdaExpr* l) {
    // The body belongs to the call operator; whoever calls it is an edge.
    for (auto* c : l->capture_inits())
      if (c) TraverseStmt(c);
    return in(l->getCallOperator(), [&] { return TraverseStmt(l->getBody()); });
  }

  bool VisitCallExpr(CallExpr* ce) {
    if (cur_.empty()) return true;
    auto* f = cur_.back();
    // The standard library's own virtuals and function pointers are not
    // ours to seed: what can reach program code through it is a callable we
    // hand it, an edge of its own, or a std::function, seeded at our call.
    bool system = sm_.isInSystemHeader(sm_.getSpellingLoc(f->getLocation()));
    if (auto* d = ce->getDirectCallee()) {
      g_.edge(f, d);
      if (system) return true;
      if (is_std_function_call(d)) g_.mark(f, kIndirect);
      if (auto* m = dyn_cast<CXXMethodDecl>(d))
        if (m->isVirtual() && !isa<CXXOperatorCallExpr>(ce)) {
          bool qualified = false;
          if (auto* mc = dyn_cast<CXXMemberCallExpr>(ce))
            if (auto* me = dyn_cast<MemberExpr>(mc->getCallee()->IgnoreParens()))
              qualified = me->hasQualifier();
          if (!qualified) g_.mark(f, kIndirect);
        }
    } else if (!isa<CXXPseudoDestructorExpr>(ce->getCallee()->IgnoreParens())) {
      if (callee_is_jitfn(ce)) g_.mark(f, kInvoke);
      else if (!system) g_.mark(f, kIndirect);
    }
    return true;
  }
  bool VisitCXXConstructExpr(CXXConstructExpr* e) {
    if (cur_.empty()) return true;
    g_.edge(cur_.back(), e->getConstructor());
    if (!e->getType().isNull())
      if (auto* rd = e->getType()->getAsCXXRecordDecl())
        if (rd->hasDefinition())
          if (auto* d = rd->getDestructor()) g_.edge(cur_.back(), d);
    return true;
  }
  bool VisitCXXDeleteExpr(CXXDeleteExpr* e) {
    if (cur_.empty() || e->getDestroyedType().isNull()) return true;
    if (auto* rd = e->getDestroyedType()->getAsCXXRecordDecl())
      if (rd->hasDefinition())
        if (auto* d = rd->getDestructor()) g_.edge(cur_.back(), d);
    return true;
  }

 private:
  template <class F>
  bool in(FunctionDecl* f, F body) {
    cur_.push_back(f);
    bool r = body();
    cur_.pop_back();
    return r;
  }
  Graph& g_;
  SourceManager& sm_;
  std::vector<const FunctionDecl*> cur_;
};

// ---------------------------------------------------------------------------
// Pass 2: hazards in the runtime's own functions.

class HazardFinder : public RecursiveASTVisitor<HazardFinder> {
 public:
  HazardFinder(ASTContext& ctx, const Graph& g, const std::string& root,
               std::set<std::string>& out, std::set<std::string>& seen)
      : ctx_(ctx), sm_(ctx.getSourceManager()), g_(g), root_(root),
        out_(out), seen_(seen) {
    jit_fn_ = pointee_of("JitFn");
    fast_fn_ = pointee_of("JitIterFastFn");
  }
  bool shouldVisitTemplateInstantiations() const { return true; }

  bool VisitFunctionDecl(FunctionDecl* f) {
    if (!f->doesThisDeclarationHaveABody() || f->isDependentContext()) return true;
    if (!ours(f->getLocation())) return true;
    seen_.insert(rel(file_of(f->getLocation())));
    analyze(f);
    return true;
  }

 private:
  QualType pointee_of(const char* alias) {
    auto r = ctx_.getTranslationUnitDecl()->lookup(&ctx_.Idents.get(alias));
    for (auto* d : r)
      if (auto* td = dyn_cast<TypedefNameDecl>(d))
        if (auto* p = td->getUnderlyingType()->getAs<PointerType>())
          return p->getPointeeType();
    return QualType();
  }
  // The parameter that is the running closure, when `f` is a closure body.
  const ParmVarDecl* running_closure(const FunctionDecl* f) {
    auto same = [&](QualType t) {
      return !t.isNull() && ctx_.hasSameType(f->getType(), t);
    };
    if (same(jit_fn_) && f->getNumParams() > 1) return f->getParamDecl(1);
    if (same(fast_fn_) && f->getNumParams() > 0) return f->getParamDecl(0);
    return nullptr;
  }

  std::string file_of(SourceLocation l) {
    llvm::SmallString<256> p(sm_.getFilename(sm_.getSpellingLoc(l)));
    llvm::sys::path::remove_dots(p, /*remove_dot_dot=*/true);
    return std::string(p);
  }
  std::string rel(std::string s) {
    if (s.rfind(root_ + "/", 0) == 0) s = s.substr(root_.size() + 1);
    return s;
  }
  bool ours(SourceLocation l) {
    return file_of(l).rfind(root_ + "/include/", 0) == 0;
  }
  std::string where(SourceLocation l) {
    l = sm_.getSpellingLoc(l);
    return rel(file_of(l)) + ":" + std::to_string(sm_.getSpellingLineNumber(l)) +
           ":" + std::to_string(sm_.getSpellingColumnNumber(l));
  }
  static std::string line_of(const std::string& w) {
    return w.substr(0, w.rfind(':'));
  }

  // The named function a body belongs to: a lambda's is its outer one, a
  // template instance's is its pattern, without parameters.
  static std::string key_name(const FunctionDecl* f) {
    for (;;) {
      if (auto* p = f->getTemplateInstantiationPattern()) f = p;
      auto* m = dyn_cast<CXXMethodDecl>(f);
      if (!m || !m->getParent()->isLambda()) break;
      const DeclContext* dc = m->getParent()->getDeclContext();
      while (dc && !isa<FunctionDecl>(dc)) dc = dc->getParent();
      if (!dc) break;
      f = cast<FunctionDecl>(dc);
    }
    return f->getQualifiedNameAsString();
  }

  // Through implicit casts, explicit casts and copies of a value.
  static const Expr* strip(const Expr* e) {
    for (;;) {
      e = e->IgnoreParenImpCasts();
      if (auto* c = dyn_cast<ExplicitCastExpr>(e)) { e = c->getSubExpr(); continue; }
      if (auto* c = dyn_cast<ExprWithCleanups>(e)) { e = c->getSubExpr(); continue; }
      if (auto* c = dyn_cast<MaterializeTemporaryExpr>(e)) { e = c->getSubExpr(); continue; }
      if (auto* c = dyn_cast<CXXBindTemporaryExpr>(e)) { e = c->getSubExpr(); continue; }
      if (auto* c = dyn_cast<CXXConstructExpr>(e))
        if (c->getNumArgs() == 1 && c->getConstructor()->isCopyOrMoveConstructor()) {
          e = c->getArg(0);
          continue;
        }
      return e;
    }
  }
  // The local a value is read from, if any (through casts and `.data`).
  static const VarDecl* base_var(const Expr* e) {
    for (;;) {
      e = strip(e);
      if (auto* m = dyn_cast<MemberExpr>(e)) {
        if (m->isArrow()) return nullptr;
        e = m->getBase();
        continue;
      }
      if (auto* d = dyn_cast<DeclRefExpr>(e)) return dyn_cast<VarDecl>(d->getDecl());
      return nullptr;
    }
  }
  // A read through a pointer: an arrow on the access path. A subscript of a
  // local array or pointer (`regs[i]`, `args[i]`) is the frame's own.
  static bool heap_read(const Expr* e) {
    for (;;) {
      e = strip(e);
      if (auto* m = dyn_cast<MemberExpr>(e)) {
        if (m->isArrow()) return true;
        e = m->getBase();
        continue;
      }
      if (auto* a = dyn_cast<ArraySubscriptExpr>(e)) { e = a->getBase(); continue; }
      if (auto* oc = dyn_cast<CXXOperatorCallExpr>(e))
        if (oc->getOperator() == OO_Subscript && oc->getNumArgs() > 0) {
          e = oc->getArg(0);
          continue;
        }
      return false;
    }
  }
  // A read of the running closure's own `captures`.
  bool running_capture(const Expr* e) {
    if (!running_) return false;
    for (;;) {
      e = strip(e);
      if (auto* m = dyn_cast<MemberExpr>(e)) {
        if (m->getMemberDecl()->getName() == "captures") {
          auto* d = dyn_cast<DeclRefExpr>(strip(m->getBase()));
          return d && d->getDecl() == running_;
        }
        e = m->getBase();
        continue;
      }
      if (auto* a = dyn_cast<ArraySubscriptExpr>(e)) { e = a->getBase(); continue; }
      return false;
    }
  }
  static bool borrowing_call(const Expr* e) {
    e = strip(e);
    while (auto* m = dyn_cast<MemberExpr>(e)) {
      if (m->isArrow()) break;
      e = strip(m->getBase());
    }
    if (auto* ce = dyn_cast<CallExpr>(e))
      if (auto* d = ce->getDirectCallee())
        return kBorrowingCalls.count(d->getQualifiedNameAsString()) > 0;
    return false;
  }
  bool borrowed_expr(const Expr* e, const std::set<const VarDecl*>& b) {
    if (running_capture(e)) return false;
    if (heap_read(e) || borrowing_call(e)) return true;
    if (auto* v = base_var(e)) return b.count(v) > 0;
    return false;
  }

  void emit(const char* rule, const std::string& fn, const std::string& var,
            const std::string& decl, SourceLocation site, unsigned tier,
            const std::string& callee) {
    const char* t = tier & kInvoke ? "invoke" : tier & kIndirect ? "indirect" : "-";
    out_.insert(std::string("H\t") + rule + "\t" + decl.substr(0, decl.find(':')) +
                "\t" + fn + "\t" + var + "\t" + decl + "\t" + line_of(where(site)) +
                "\t" + t + "\t" + callee);
  }

  struct Collect : RecursiveASTVisitor<Collect> {
    std::vector<std::pair<const VarDecl*, const Expr*>> flows;
    std::vector<const CallExpr*> calls;
    std::vector<const LambdaExpr*> lambdas;
    std::vector<const BinaryOperator*> stores;
    bool VisitVarDecl(VarDecl* v) {
      if (v->hasLocalStorage() && !isa<ParmVarDecl>(v) && v->hasInit() &&
          is_value_type(v->getType()))
        flows.push_back({v, v->getInit()});
      return true;
    }
    bool VisitBinaryOperator(BinaryOperator* b) {
      if (b->getOpcode() != BO_Assign) return true;
      auto* l = b->getLHS()->IgnoreParenImpCasts();
      if (auto* d = dyn_cast<DeclRefExpr>(l))
        if (auto* v = dyn_cast<VarDecl>(d->getDecl()))
          if (v->hasLocalStorage() && is_value_type(v->getType()))
            flows.push_back({v, b->getRHS()});
      if (auto* m = dyn_cast<MemberExpr>(l))
        if (is_value_type(m->getType())) stores.push_back(b);
      return true;
    }
    bool VisitCallExpr(CallExpr* c) { calls.push_back(c); return true; }
    bool VisitLambdaExpr(LambdaExpr* l) { lambdas.push_back(l); return true; }
  };

  void analyze(FunctionDecl* f) {
    running_ = running_closure(f);
    Collect col;
    col.TraverseStmt(f->getBody());  // lambda bodies included
    // Borrowed locals, to a fixed point over initializers and assignments.
    std::set<const VarDecl*> borrowed;
    for (bool changed = true; changed;) {
      changed = false;
      for (auto& [v, e] : col.flows)
        if (!borrowed.count(v) && borrowed_expr(e, borrowed)) {
          borrowed.insert(v);
          changed = true;
        }
    }
    std::string fn = key_name(f);

    // R1: a borrowed callee.
    for (auto* c : col.calls) {
      auto* d = c->getDirectCallee();
      if (!d || !is_invoke_entry(d->getNameAsString()) || c->getNumArgs() == 0) continue;
      if (!ours(c->getBeginLoc())) continue;
      const Expr* a0 = c->getArg(0);
      if (!borrowed_expr(a0, borrowed)) continue;
      auto* v = base_var(a0);
      emit("R1", fn, v ? v->getNameAsString() : "<inline>",
           where(v ? v->getLocation() : a0->getBeginLoc()), c->getBeginLoc(),
           0, d->getNameAsString());
    }

    // R2: a borrowed value kept past the call: captured into a
    // std::function, or stored into a field.
    for (auto* l : col.lambdas) {
      if (!stored_as_function(l)) continue;
      for (const auto& cap : l->captures()) {
        if (!cap.capturesVariable()) continue;
        auto* v = dyn_cast<VarDecl>(cap.getCapturedVar());
        if (v && borrowed.count(v))
          emit("R2", fn, v->getNameAsString(), where(v->getLocation()),
               l->getBeginLoc(), 0, "std::function");
      }
    }
    for (auto* s : col.stores) {
      if (!ours(s->getBeginLoc()) || !borrowed_expr(s->getRHS(), borrowed)) continue;
      auto* v = base_var(s->getRHS());
      emit("R2", fn, v ? v->getNameAsString() : "<inline>",
           where(v ? v->getLocation() : s->getBeginLoc()), s->getBeginLoc(), 0,
           "store");
    }

    // R3: live across a call that can run program code, in the body and in
    // each lambda's (a handler's body runs there).
    if (borrowed.empty()) return;
    live_across(f, fn, borrowed);
    for (auto* l : col.lambdas) {
      auto* op = l->getCallOperator();
      if (op && op->hasBody() && !op->isDependentContext())
        live_across(op, fn, borrowed);
    }
  }

  void live_across(const FunctionDecl* body, const std::string& fn,
                   const std::set<const VarDecl*>& borrowed) {
    AnalysisDeclContextManager mgr(ctx_);
    auto& bo = mgr.getCFGBuildOptions();
    bo.AddImplicitDtors = true;
    bo.AddTemporaryDtors = true;
    bo.setAllAlwaysAdd();  // LiveVariables needs every expression an element
    AnalysisDeclContext* adc = mgr.getContext(body);
    if (!adc->getCFG()) return;
    auto* lv = adc->getAnalysis<LiveVariables>();
    if (!lv) return;
    for (const CFGBlock* blk : *adc->getCFG()) {
      std::vector<CFGElement> els(blk->begin(), blk->end());
      // Live past element i: live at the start of the block's next statement,
      // or at the block's end.
      auto live_after = [&](size_t i, const VarDecl* v) {
        for (size_t j = i + 1; j < els.size(); j++)
          if (auto cs = els[j].getAs<CFGStmt>()) return lv->isLive(cs->getStmt(), v);
        return lv->isLive(blk, v);
      };
      for (size_t i = 0; i < els.size(); i++) {
        SourceLocation at;
        unsigned t = 0;
        std::string callee;
        if (auto dt = els[i].getAs<CFGImplicitDtor>()) {
          if (auto* d = dt->getDestructorDecl(ctx_)) {
            t = g_.of(d);
            callee = "~" + d->getParent()->getNameAsString();
            if (auto ad = els[i].getAs<CFGAutomaticObjDtor>())
              at = ad->getTriggerStmt()->getEndLoc();
            else if (auto td = els[i].getAs<CFGTemporaryDtor>())
              at = td->getBindTemporaryExpr()->getBeginLoc();
          }
        } else if (auto cs = els[i].getAs<CFGStmt>()) {
          const Stmt* s = cs->getStmt();
          at = s->getBeginLoc();
          if (auto* ce = dyn_cast<CallExpr>(s)) {
            if (auto* d = ce->getDirectCallee()) {
              t = g_.of(d);
              if (is_std_function_call(d)) t |= kIndirect;
              callee = d->getNameAsString();
            } else {
              t = callee_is_jitfn(ce) ? kInvoke : kIndirect;
              callee = "<indirect>";
            }
          } else if (auto* ce = dyn_cast<CXXConstructExpr>(s)) {
            t = g_.of(ce->getConstructor());
            callee = ce->getConstructor()->getNameAsString();
          }
        }
        if (!t || at.isInvalid()) continue;
        for (auto* v : borrowed)
          if (live_after(i, v))
            emit("R3", fn, v->getNameAsString(), where(v->getLocation()), at, t,
                 callee);
      }
    }
  }

  bool stored_as_function(const LambdaExpr* l) {
    DynTypedNode n = DynTypedNode::create(*l);
    for (int depth = 0; depth < 8; depth++) {
      auto ps = ctx_.getParents(n);
      if (ps.empty()) return false;
      n = ps[0];
      QualType t;
      if (auto* e = n.get<Expr>()) t = e->getType();
      else if (auto* v = n.get<VarDecl>()) t = v->getType();
      else if (auto* fd = n.get<FieldDecl>()) t = fd->getType();
      else return false;
      if (t.isNull()) return false;
      t = t.getNonReferenceType();
      if (auto* rd = t->getAsCXXRecordDecl())
        if (rd->getName() == "function" && rd->isInStdNamespace()) return true;
      if (n.get<CallExpr>() && !n.get<CXXConstructExpr>()) return false;
    }
    return false;
  }

  ASTContext& ctx_;
  SourceManager& sm_;
  const Graph& g_;
  std::string root_;
  std::set<std::string>& out_;
  std::set<std::string>& seen_;
  QualType jit_fn_, fast_fn_;
  const ParmVarDecl* running_ = nullptr;
};

class Consumer : public ASTConsumer {
 public:
  explicit Consumer(std::string root) : root_(std::move(root)) {}
  void HandleTranslationUnit(ASTContext& ctx) override {
    Graph g;
    GraphBuilder(g, ctx.getSourceManager()).TraverseDecl(ctx.getTranslationUnitDecl());
    g.solve();
    // The hazard pass, and the parent map it builds, need only what is not
    // the system's.
    auto& sm = ctx.getSourceManager();
    std::vector<Decl*> scope;
    for (auto* d : ctx.getTranslationUnitDecl()->decls())
      if (!sm.isInSystemHeader(sm.getSpellingLoc(d->getLocation()))) scope.push_back(d);
    ctx.setTraversalScope(scope);
    std::set<std::string> out, seen;
    HazardFinder(ctx, g, root_, out, seen).TraverseDecl(ctx.getTranslationUnitDecl());
    for (auto& s : out) llvm::outs() << s << "\n";
    for (auto& s : seen) llvm::outs() << "seen\t" << s << "\n";
  }

 private:
  std::string root_;
};

class Action : public ASTFrontendAction {
 public:
  explicit Action(std::string root) : root_(std::move(root)) {}
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance&,
                                                 llvm::StringRef) override {
    return std::make_unique<Consumer>(root_);
  }

 private:
  std::string root_;
};

}  // namespace

// gc_hazard <dir with compile_commands.json> <repo root> <file.cc>
int main(int argc, const char** argv) {
  if (argc != 4) {
    llvm::errs() << "usage: gc_hazard <build-dir> <repo-root> <file.cc>\n";
    return 2;
  }
  std::string err;
  auto db = tooling::CompilationDatabase::loadFromDirectory(argv[1], err);
  if (!db) {
    llvm::errs() << err << "\n";
    return 2;
  }
  tooling::ClangTool tool(*db, {argv[3]});
  tool.appendArgumentsAdjuster(tooling::getInsertArgumentAdjuster(
      "-resource-dir=" GC_HAZARD_RESOURCE_DIR, tooling::ArgumentInsertPosition::END));
  tool.appendArgumentsAdjuster(tooling::getInsertArgumentAdjuster(
      "-w", tooling::ArgumentInsertPosition::END));
  struct Factory : tooling::FrontendActionFactory {
    std::string root;
    std::unique_ptr<FrontendAction> create() override {
      return std::make_unique<Action>(root);
    }
  } f;
  f.root = argv[2];
  return tool.run(&f);
}
