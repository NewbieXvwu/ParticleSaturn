#!/bin/sh
set -eu

metal_app="$1"
opengl_app="$2"
metrics="$3"
output_dir="$4"
run_id="$$"
metal_image="$output_dir/metal-$run_id.ppm"
opengl_image="$output_dir/opengl-$run_id.ppm"
molten_image="$output_dir/moltenvk-$run_id.ppm"
kosmic_image="$output_dir/kosmickrisp-$run_id.ppm"

mkdir -p "$output_dir"

capture() {
    application="$1"
    image="$2"
    backend="$3"
    driver="${4:-}"
    if [ "$backend" = vulkan ]; then
        PARTICLESATURN_CAPTURE_BASELINE="$image" PARTICLESATURN_GRAPHICS_API="$backend" \
            PARTICLESATURN_VULKAN_DRIVER="$driver" "$application" &
    else
        PARTICLESATURN_CAPTURE_BASELINE="$image" PARTICLESATURN_GRAPHICS_API="$backend" "$application" &
    fi
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
capture "$metal_app" "$molten_image" vulkan molten

resources_dir=$(cd "$(dirname "$metal_app")/../Resources" && pwd)
kosmic_icd="$resources_dir/Vulkan/etc/vulkan/icd.d/KosmicKrisp_icd.json"
if [ -f "$kosmic_icd" ]; then
    capture "$metal_app" "$kosmic_image" vulkan kosmic
    "$metrics" "$metal_image" "$opengl_image" 2.0 0.04
    "$metrics" "$metal_image" "$molten_image" 2.0 0.04
    "$metrics" "$metal_image" "$kosmic_image" 2.0 0.04
    "$metrics" "$molten_image" "$kosmic_image" 1.0 0.01
else
    "$metrics" "$metal_image" "$opengl_image" 2.0 0.04
    "$metrics" "$metal_image" "$molten_image" 2.0 0.04
fi
