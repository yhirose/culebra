; Upstream LLVM bug, reduced out of culebra. Not a culebra test — nothing
; here depends on culebra — it is kept so the workaround in
; JIT::apply_fast_codegen can be retired once LLVM fixes this.
;
; At CodeGenOptLevel::None (llc -O0) the AArch64 backend miscompiles a
; landing pad that carries more than ten live phi values. RegAllocFast
; reloads the incoming values into the scratch registers x8-x17, runs out
; after ten, and sources the eleventh straight from x1 — which the
; Itanium EH ABI has already overwritten with the exception selector. The
; eleventh phi therefore arrives as 1 instead of its incoming value.
;
; Ten phis is fine; eleven is the threshold. -O1 and above use the greedy
; allocator and are correct.
;
;   llc -O0 -filetype=obj landingpad_phi.ll -o probe.o
;   c++ -std=c++17 driver.cc probe.o -o probe && ./probe
;   -> "arg 10: expected 110, got 1" + "MISCOMPILE"  (llc -O0)
;   -> "ok"                                          (llc -O1/-O2/-O3)

target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-darwin24.6.0"

declare void @may_throw()
declare void @sink(i64)
declare ptr @__cxa_begin_catch(ptr)
declare i32 @__gxx_personality_v0(...)

define void @probe(i64 %a0, i64 %a1, i64 %a2, i64 %a3, i64 %a4, i64 %a5, i64 %a6, i64 %a7, i64 %a8, i64 %a9, i64 %a10, i64 %n) personality ptr @__gxx_personality_v0 {
entry:
  %c = icmp eq i64 %n, 0
  br i1 %c, label %b1, label %b2

b1:
  invoke void @may_throw() to label %done unwind label %pad

b2:
  invoke void @may_throw() to label %done unwind label %pad

done:
  ret void

pad:
  %v0 = phi i64 [ %a0, %b1 ], [ 0, %b2 ]
  %v1 = phi i64 [ %a1, %b1 ], [ 0, %b2 ]
  %v2 = phi i64 [ %a2, %b1 ], [ 0, %b2 ]
  %v3 = phi i64 [ %a3, %b1 ], [ 0, %b2 ]
  %v4 = phi i64 [ %a4, %b1 ], [ 0, %b2 ]
  %v5 = phi i64 [ %a5, %b1 ], [ 0, %b2 ]
  %v6 = phi i64 [ %a6, %b1 ], [ 0, %b2 ]
  %v7 = phi i64 [ %a7, %b1 ], [ 0, %b2 ]
  %v8 = phi i64 [ %a8, %b1 ], [ 0, %b2 ]
  %v9 = phi i64 [ %a9, %b1 ], [ 0, %b2 ]
  %v10 = phi i64 [ %a10, %b1 ], [ 0, %b2 ]
  %e = landingpad { ptr, i32 } catch ptr null
  %p = extractvalue { ptr, i32 } %e, 0
  %cc = call ptr @__cxa_begin_catch(ptr %p)
  call void @sink(i64 %v0)
  call void @sink(i64 %v1)
  call void @sink(i64 %v2)
  call void @sink(i64 %v3)
  call void @sink(i64 %v4)
  call void @sink(i64 %v5)
  call void @sink(i64 %v6)
  call void @sink(i64 %v7)
  call void @sink(i64 %v8)
  call void @sink(i64 %v9)
  call void @sink(i64 %v10)
  ret void
}
