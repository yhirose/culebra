#!/usr/bin/env bash
# Assert a node from a closed View cannot damage the next View's GL objects.
#
# Usage: misc/aot_axes/probe_scene_view_epoch.sh <culebra binary>
#
# No gate runs this: CULEBRA_ENABLE_SCENE is OFF in every CMake and CI lane, so
# the Scene TU is not even compiled there. Build a driver that has it with
#
#   just dev -DCULEBRA_ENABLE_SCENE=ON
#
# then run this against build-dev/culebra. Needs xvfb-run (a window is opened).
#
# The program builds a mesh under one View, keeps the node past that View's
# death, and drops it inside the NEXT View's context. Both Views build their
# mesh at the same point in their setup, so the dead View's ids name the live
# View's buffers — that collision is what makes an unload through them
# observable, and is the reason the first version of this probe passed against
# the broken binary. Three orders, two backends, one frame each: all six must
# be identical, and identical to the run that never opened the first View.
set -eu

bin=${1:?usage: probe_scene_view_epoch.sh <culebra binary>}
work=${TMPDIR:-/tmp}/culebra-scene-view-epoch
rm -rf "$work" && mkdir -p "$work"

cat > "$work/epoch.cul" <<'CUL'
# ARGV[0]: "ctrl" = only the second View | "late" = drop the stale node after
# the frame | "early" = drop it before (the order that used to delete the live
# View's buffers). ARGV[1]: screenshot path.
mode = Sys.argv[0]
out = Sys.argv[1]

fn quad(n) {                      # a 2x2 ground quad, both windings (no culling)
  n.vertex(-1.0, 0.0, -1.0, 0.0, 1.0, 0.0)
  n.vertex(1.0, 0.0, -1.0, 0.0, 1.0, 0.0)
  n.vertex(1.0, 0.0, 1.0, 0.0, 1.0, 0.0)
  n.vertex(-1.0, 0.0, 1.0, 0.0, 1.0, 0.0)
  n.tri(0, 1, 2)
  n.tri(0, 2, 3)
  n.tri(2, 1, 0)
  n.tri(3, 2, 0)
  n.build()
}

mut stale = nil
if mode != "ctrl" {
  let a = Scene.View.new(320, 240, "a")
  let n = a.add_mesh()
  quad(n)
  stale = n
  a.drop()                        # closes the context; `n` keeps the dead ids
}

let b = Scene.View.new(320, 240, "b")
b.camera(4.0, 3.0, 4.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 55.0)
let mine = b.add_mesh()
quad(mine)                        # takes the ids the dead View was given
mine.tint(220, 60, 60)
b.add_box(1.0, 1.0, 1.0).move(0.0, 1.0, 0.0)

if mode == "early" { stale.drop() }
b.render_3d()
b.present()
b.screenshot(out)
if mode == "late" { stale.drop() }
b.drop()
CUL

fail=0
ref=""
for backend in "--vm" "--jit"; do
  tag=${backend#--}
  for mode in ctrl late early; do
    shot="$work/$tag.$mode.png"
    if ! xvfb-run -a "$bin" $backend "$work/epoch.cul" "$mode" "$shot" \
            > "$work/$tag.$mode.log" 2>&1; then
      echo "probe_scene_view_epoch: FAIL ($tag $mode did not run)" >&2
      tail -3 "$work/$tag.$mode.log" >&2
      fail=1
      continue
    fi
    sum=$(md5sum "$shot" | cut -d' ' -f1)
    [[ -n "$ref" ]] || ref=$sum
    if [[ "$sum" != "$ref" ]]; then
      echo "probe_scene_view_epoch: FAIL ($tag $mode drew a different frame)" >&2
      fail=1
    fi
  done
done
[[ $fail -eq 0 ]] || exit 1
echo "probe_scene_view_epoch: OK"
