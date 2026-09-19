#!/usr/bin/env bash
set -euo pipefail

readonly script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly engine_dir="$(cd "${script_dir}/.." && pwd)"
readonly compose_file="${engine_dir}/docker-compose.local.yml"
readonly compose=(docker compose -f "${compose_file}")

cleanup() {
    "${compose[@]}" down --volumes --remove-orphans
}
trap cleanup EXIT

# The production Dockerfile deliberately consumes prebuilt frontend assets.
# Keep this explicit so a deployment cannot accidentally serve stale assets.
(cd "${engine_dir}/../my-web" && npm run build)
"${compose[@]}" up --build --detach --wait

for _ in $(seq 1 30); do
    if curl --fail --silent --show-error --max-time 3 \
        http://127.0.0.1:19443/health | grep -q '"ok"'; then
        echo 'LOCAL-DEPLOY-OK'
        exit 0
    fi
    sleep 1
done

"${compose[@]}" logs --no-color
exit 1
