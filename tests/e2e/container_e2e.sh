#!/usr/bin/env bash
#
# Suite 10 — the container image, end to end.
#
# Proves the image does the job rather than merely starting: the CDP endpoint
# answers from *outside* the container, the agent CLI drives the page from
# inside it, refs survive between commands, and a screenshot comes back with
# real glyphs rather than the boxes a missing font produces.
#
# Usage: tests/e2e/container_e2e.sh [image]   (default: anoa:test)
set -u

IMAGE="${1:-anoa:test}"
ENGINE="${CONTAINER_ENGINE:-}"
if [ -z "$ENGINE" ]; then
    # Pick the engine that can actually see the image, not merely the first one
    # installed. On a machine with both, docker and podman keep separate image
    # stores, so "docker is available" is no reason to believe it has the image
    # podman just built — and the failure that produces looks like a broken
    # container rather than a wrong engine.
    for candidate in docker podman; do
        command -v "$candidate" >/dev/null 2>&1 || continue
        if "$candidate" image inspect "$IMAGE" >/dev/null 2>&1; then
            ENGINE="$candidate"; break
        fi
    done
fi
if [ -z "$ENGINE" ]; then
    echo "no docker or podman can see image '$IMAGE'" >&2
    echo "build it first, or set CONTAINER_ENGINE" >&2
    exit 1
fi
PLATFORM_ARG=""
[ "$ENGINE" = "podman" ] && PLATFORM_ARG="--platform linux/amd64"

NAME="anoa-e2e-$$"
PORT="${ANOA_CONTAINER_PORT:-9599}"
passed=0
failed=0

check() {
    if [ "$2" = "0" ]; then
        passed=$((passed + 1)); printf '  PASS  %s\n' "$1"
    else
        failed=$((failed + 1)); printf '  FAIL  %s%s\n' "$1" "${3:+: $3}"
    fi
}

cleanup() { $ENGINE rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

echo "Container E2E (Suite 10) — image=$IMAGE engine=$ENGINE"

# shellcheck disable=SC2086
$ENGINE run -d --name "$NAME" $PLATFORM_ARG -p "${PORT}:9222" "$IMAGE" >/dev/null 2>&1
started=$?
check "container starts" "$started"
[ "$started" = "0" ] || { echo; echo "Container E2E: $passed passed, $failed failed"; exit 1; }

# Poll rather than sleep: cold start is a property of the machine.
ready=1
for _ in $(seq 1 60); do
    if curl -fsS "http://127.0.0.1:${PORT}/json/version" >/dev/null 2>&1; then ready=0; break; fi
    sleep 1
done
check "CDP endpoint answers from outside the container" "$ready"
if [ "$ready" != "0" ]; then
    echo "--- container log ---"; $ENGINE logs "$NAME" 2>&1 | tail -20
    echo; echo "Container E2E: $passed passed, $failed failed"; exit 1
fi

ver="$(curl -fsS "http://127.0.0.1:${PORT}/json/version" | tr -d '\n')"
echo "$ver" | grep -q '"Browser": *"anoa/' ; check "reports itself as anoa" "$?" "$ver"

# The agent CLI, run inside the container against the browser in the same
# container. This is the shape a CI job or an agent sandbox would use.
# Two helpers on purpose. `inside` merges stderr because a failing command's
# reason belongs in the report; `value` keeps it out, because anything Qt or
# Chromium writes there would otherwise be parsed as part of the answer.
inside() { $ENGINE exec "$NAME" anoa "$@" 2>&1; }
value()  { $ENGINE exec "$NAME" anoa "$@" 2>/dev/null | tr -d '\r'; }

# The first navigation in a cold container is the slowest thing here: Chromium
# is starting its renderer, DNS is uncached and TLS is unresumed. Retry rather
# than widen a timeout — a fixed number that is generous on a laptop is still
# too tight on a loaded CI runner, and the retry costs nothing when it is warm.
opened=1
for _ in 1 2 3; do
    if inside open example.com | grep -q "Example Domain"; then opened=0; break; fi
    sleep 3
done
check "open renders a real page" "$opened"

snap="$(inside snapshot -i)"
echo "$snap" | grep -qE '@e[0-9]+ +link'; check "snapshot returns refs" "$?" "$(echo "$snap" | head -1)"

# The ref was minted by one exec and is used by another — separate processes,
# same page. That is the property the whole design rests on.
href="$(value get attr @e1 href)"
echo "$href" | grep -q '^https\?://'; check "a ref survives between commands" "$?" "$href"

inside click @e1 >/dev/null
inside wait --url iana >/dev/null
url="$(value eval 'location.href')"
echo "$url" | grep -q 'iana.org'; check "click navigates" "$?" "$url"

# Fonts. A container without one measures text as zero width — the glyphs are
# not there to measure — which looks like a broken browser rather than a
# missing package.
#
# Measure a span this test creates, not a page it fetched. The old check read
# example.com's <h1>; example.com was redesigned without one, querySelector
# returned null, and this failed on markup nobody here controls. It is kept
# off-screen and removed again so the screenshot check below sees the page as
# it was.
probe='(()=>{const s=document.createElement("span");s.textContent="Example Domain";'
probe="${probe}"'s.style.cssText="font:32px sans-serif;position:absolute;left:-9999px";'
probe="${probe}"'document.body.appendChild(s);const w=s.getBoundingClientRect().width;'
probe="${probe}"'s.remove();return w})()'
w="$(value eval "$probe")"
awk -v w="$w" 'BEGIN { exit !(w + 0 > 50) }'; check "text has real width (fonts present)" "$?" "width=${w}"

$ENGINE exec "$NAME" anoa screenshot /tmp/shot.png >/dev/null 2>&1
$ENGINE exec "$NAME" sh -c 'test -s /tmp/shot.png && head -c4 /tmp/shot.png | od -An -tx1 | tr -d " \n" | grep -q 89504e47'
check "screenshot is a real PNG" "$?"

inside console --clear >/dev/null
inside eval "console.log('container-marker'); 'ok'" >/dev/null
inside console | grep -q 'container-marker'; check "page-recorded console works" "$?"

# Not root: an image whose browser runs as root renders untrusted pages as root.
who="$($ENGINE exec "$NAME" id -un 2>&1 | tr -d '\r')"
[ "$who" != "root" ]; check "browser does not run as root" "$?" "running as $who"

echo
echo "Container E2E: $passed passed, $failed failed"
[ "$failed" = "0" ]
