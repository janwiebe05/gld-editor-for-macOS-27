#!/bin/zsh
# Checks whether GLD Editor (x86_64, Qt 4.8) can still run on this Mac and
# writes everything needed for troubleshooting into a report on the Desktop.
# Usage: tools/gld-check.sh ["/path/to/GLD Editor.app"]

APP="${1:-}"
if [[ -z "$APP" ]]; then
  for c in "/Applications/GLD Editor 1.61 (macOS 27).app" "$HOME/Applications/GLD Editor 1.61 (macOS 27).app" \
           "/Applications/GLD Editor 1.61.app"; do
    [[ -d "$c" ]] && { APP="$c"; break; }
  done
fi
REPORT="$HOME/Desktop/GLD-Report.txt"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

check() {
  echo "GLD Editor report, $(date '+%Y-%m-%d %H:%M')"
  echo "macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(uname -m), $(sysctl -n hw.model)"
  echo "App: ${APP:-NOT FOUND}"
  [[ -d "$APP" ]] || { echo "No GLD Editor app found in /Applications or ~/Applications."; return 1; }
  echo "Quarantine: $(xattr -p com.apple.quarantine "$APP" 2>/dev/null || echo none)"
  echo

  if [[ $(uname -m) == arm64 ]]; then
    if arch -x86_64 /usr/bin/true 2>/dev/null; then
      echo "OK      Rosetta 2 installed"
    else
      echo "MISSING Rosetta 2  ->  softwareupdate --install-rosetta --agree-to-license"
      return 1
    fi
  fi

  # All system libraries the app, its frameworks and plug-ins link against
  LIBS=$(find "$APP/Contents" -type f | while read -r f; do
    otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}'
  done | grep -E '^/(usr/lib|System)/' | sort -u)

  cat > "$TMP/dl.c" <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
int main(int c, char **v) {
  int bad = 0;
  for (int i = 1; i < c; i++) {
    void *h = dlopen(v[i], RTLD_LAZY);
    printf("%-7s %s\n", h ? "OK" : "MISSING", v[i]);
    if (!h) bad++;
  }
  return bad;
}
EOF

  if [[ -z "$LIBS" ]]; then
    echo "Note: otool not found (xcode-select --install), library check skipped"
  elif ! clang -arch x86_64 -o "$TMP/dl" "$TMP/dl.c" 2>/dev/null; then
    echo "Note: clang not found (xcode-select --install), library check skipped"
  else
    arch -x86_64 "$TMP/dl" ${(f)LIBS}
    BAD=$?
    echo
    if (( BAD == 0 )); then
      echo "All system libraries present."
    else
      echo "$BAD library/libraries missing."
    fi
  fi

  echo
  if pgrep -f "MacOS/GLD Editor" >/dev/null; then
    echo "A GLD Editor is already running – quit it and run this check again."
    return 1
  fi

  # Working directory "/" like a launch from the Finder.
  # After an unclean exit the next launch only cleans up and quits silently
  # with exit code 0 – hence up to two attempts.
  cd /
  # Watch for 25 seconds: on the very first launch Rosetta translates the 74 MB
  # executable, so a crash while loading fonts shows up only after 8+ seconds
  for try in 1 2; do
    echo "Starting the app and watching it for 25 seconds (attempt $try) ..."
    arch -x86_64 "$APP/Contents/MacOS/GLD Editor" > "$TMP/run.log" 2>&1 &
    PID=$!
    for _ in {1..25}; do
      sleep 1
      kill -0 $PID 2>/dev/null || break
    done
    if kill -0 $PID 2>/dev/null; then
      echo "OK      App is running (PID $PID). Check the window, then quit the app normally."
      break
    fi
    wait $PID
    RC=$?
    if (( try == 2 || RC != 0 )); then
      echo "ERROR   The app exited (exit code $RC). Output:"
      cat "$TMP/run.log"
      break
    fi
  done

  # Newest crash report of the last 7 days
  CRASH=( "$HOME"/Library/Logs/DiagnosticReports/GLD\ Editor*(N.om[1]m-7) )
  if (( ${#CRASH} )); then
    echo
    echo "===== Crash report: ${CRASH[1]:t} ====="
    head -c 60000 "${CRASH[1]}"
  fi
}

check 2>&1 | tee "$REPORT"
echo
echo "Report saved: $REPORT"
