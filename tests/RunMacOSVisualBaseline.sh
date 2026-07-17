#!/bin/sh
set -eu

metal_app="$1"
opengl_app="$2"
metrics="$3"
output_dir="$4"
run_id="$$"
metal_image="$output_dir/metal-$run_id.ppm"
opengl_image="$output_dir/opengl-$run_id.ppm"

mkdir -p "$output_dir"

capture() {
    application="$1"
    image="$2"
    backend="$3"
    PARTICLESATURN_CAPTURE_BASELINE="$image" PARTICLESATURN_GRAPHICS_API="$backend" "$application" &
    process_id=$!
    attempts=0
    while [ ! -f "$image" ] && [ "$attempts" -lt 200 ]; do
        sleep 0.05
        attempts=$((attempts + 1))
    done
    if [ ! -f "$image" ]; then
        kill "$process_id" 2>/dev/null || true
        wait "$process_id" 2>/dev/null || true
        return 1
    fi
    wait "$process_id"
}

capture "$metal_app" "$metal_image" metal
capture "$opengl_app" "$opengl_image" opengl41
"$metrics" "$metal_image" "$opengl_image" 2.0 0.04
