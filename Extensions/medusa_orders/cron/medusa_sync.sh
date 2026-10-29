#!/bin/bash
# GotGrit shop <-> FA timer (w2vy cron, every 15 min): FA imports shipped shop
# orders, then pushes prices + stock to the shop (medusa.php action=cron).
# Config ~/.config/medusa-sync.env (mode 600): FA_URL FA_USER FA_PASS MAIL_TO.
# Mails each error the first time it shows, then once a day while it lasts.
# A 502/503/504/52x from the shop (dev backend mid-rebuild, a Cloudflare blip)
# is mailed only when the next run hits it again.
# ROUTE: the shop URL (www, through Cloudflare) failed and FA used the Flux
# address instead; mailed after ROUTE_MAIL_AFTER runs in a row (default 3),
# with one more mail when the shop URL works again.
set -u
. "$HOME/.config/medusa-sync.env"
STATE="$HOME/.local/state/medusa-sync"
LOG="$HOME/logs/medusa_sync.log"
mkdir -p "$STATE" "$(dirname "$LOG")"
exec 9>"$STATE/lock"
flock -n 9 || { echo "$(TZ=America/New_York date '+%F %T ET') previous run still going" >> "$LOG"; exit 0; }

out=$(curl -s -m 600 --data-urlencode "user_name_entry_field=$FA_USER" --data-urlencode "password=$FA_PASS" \
    -d company_login_name=0 -d ui_mode=1 -d action=cron "$FA_URL/modules/medusa_orders/medusa.php" 2>&1)
rc=$?
errors=$(printf '%s\n' "$out" | grep '^ERROR: ' | sed 's/^ERROR: //')
recovered=''
route=$(printf '%s\n' "$out" | grep -m1 '^ROUTE: ' | sed 's/^ROUTE: //')
runs=$(cat "$STATE/route-runs" 2>/dev/null || echo 0)
if [ -n "$route" ]; then
    runs=$((runs + 1)); echo "$runs" > "$STATE/route-runs"
    [ -s "$STATE/route-since" ] || TZ=America/New_York date '+%F %H:%M ET' > "$STATE/route-since"
    if [ "$runs" -ge "${ROUTE_MAIL_AFTER:-3}" ]; then
        # Same text every run (no counts), so it mails once, then daily.
        host=${route%% failed*}; alt=${route##*used }
        errors+=$'\n'"$host unreachable since $(cat "$STATE/route-since") (Cloudflare to Flux); the shop answers at $alt and the sync is running through it"
    fi
else
    if [ "$runs" -ge "${ROUTE_MAIL_AFTER:-3}" ]; then
        recovered="The shop URL works again (it failed from $(cat "$STATE/route-since") to $(TZ=America/New_York date '+%F %H:%M ET'), $runs runs)."
    fi
    rm -f "$STATE/route-runs" "$STATE/route-since"
fi
if ! printf '%s\n' "$out" | grep -qx 'DONE'; then
    errors+=$'\n'"run did not finish (curl exit $rc): $(printf '%s' "$out" | sed 's/<[^>]*>//g' | tail -c 400 | tr '\n' ' ')"
fi
# Import and push often hit the same error: one line each, or a single shop
# 502 would count as "seen twice" below.
errors=$(printf '%s\n' "$errors" | sed '/^$/d' | awk '!seen[$0]++')

# Log: one line when nothing happened, else the interesting lines.
busy=$(printf '%s\n' "$out" | grep -v -x -E 'DONE|No new orders to import\.|0 variant\(s\) updated|Stock links changed on 0 product\(s\)|Stock levels unchanged')
if [ -z "$busy" ]; then echo "$(TZ=America/New_York date '+%F %T ET') ok" >> "$LOG"
else printf '%s\n' "$busy" | sed "s/^/$(TZ=America/New_York date '+%F %T ET') /" >> "$LOG"; fi

# Mail: new errors now, repeats once a day; forget errors that went away.
now=$(date +%s); new=0; seen=()
while IFS= read -r e; do
    [ -z "$e" ] && continue
    h=$(printf '%s' "$e" | sha1sum | cut -c1-16); seen+=("$h")
    if [[ "$e" =~ HTTP\ (50[234]|52[0-9]) ]] && [ ! -e "$STATE/again-$h" ]; then
        touch "$STATE/again-$h"; continue
    fi
    last=$(cat "$STATE/err-$h" 2>/dev/null || echo 0)
    if [ $((now - last)) -ge 86400 ]; then new=1; echo "$now" > "$STATE/err-$h"; fi
done <<< "$errors"
for f in "$STATE"/err-* "$STATE"/again-*; do
    [ -e "$f" ] || continue
    h=${f##*/err-}; h=${h##*/again-}; [[ " ${seen[*]:-} " == *" $h "* ]] || rm -f "$f"
done
if [ -n "$recovered" ]; then
    {
        echo "From: GotGrit shop sync <tom@moltentech.us>"
        echo "To: $MAIL_TO"
        echo "Subject: GotGrit shop sync: shop URL back"
        echo
        echo "$recovered"
        echo
        echo "Log: $LOG"
    } | /usr/sbin/sendmail -f tom@moltentech.us -t
fi
if [ "$new" = 1 ]; then
    {
        echo "From: GotGrit shop sync <tom@moltentech.us>"
        echo "To: $MAIL_TO"
        echo "Subject: GotGrit shop sync: $(printf '%s\n' "$errors" | wc -l) problem(s)"
        echo
        echo "The FA <-> shop timer ($FA_URL) hit these at $(TZ=America/New_York date '+%F %H:%M ET'):"
        echo
        printf '%s\n' "$errors" | sed 's/^/- /'
        echo
        echo "Each problem is mailed once, then daily while it lasts. Log: $LOG"
    } | /usr/sbin/sendmail -f tom@moltentech.us -t
fi
exit 0
