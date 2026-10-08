#!/bin/bash
# On-device LLM speed and power: llama-bench per model and core set, with ODPM
# energy over each run. Screen as it is; CPUs 4-7 held online for the test.
set -u
L=/root/llm
B=$L/bin/llama-bench
P=/sys/module/pixel_odpm/parameters/energy
H=/run/user/0/omarchy-mobile-sleep-inhibit
mkdir -p $H && touch $H/llm-bench
echo "4 5 6 7" > /run/pixel-cores-hold
for c in 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu$c/online; done
cleanup() {
    rm -f /run/pixel-cores-hold $H/llm-bench
    /usr/local/sbin/pixel-display-hook cores 2>/dev/null
    pkill -f '^/root/llm/vmon' 2>/dev/null
}
trap cleanup EXIT
energy() {	# average mW since the last read: total, BIG, MID, LITTLE
    awk '/^seconds/ { s[$2] = $3; m = $2; next }
         { t[m] += $2; e[$1] = $2 }
         END { printf "%.0f %.0f %.0f %.0f", t[0]/s[0]/1000 + t[1]/s[1]/1000,
               e["S2M_VDD_CPUCL2"]/s[0]/1000, e["S3M_VDD_CPUCL1"]/s[0]/1000, e["S4M_VDD_CPUCL0"]/s[0]/1000 }' $P
}
maxtemp() { cat /sys/class/thermal/thermal_zone[0-2]/temp | sort -n | tail -1; }
# Battery voltage low-water mark during a run, sampled every 50 ms.
cat > $L/vmon <<'EOF'
#!/bin/sh
min=9999999
while :; do
    v=$(cat /sys/class/power_supply/battery/voltage_now)
    [ "$v" -lt "$min" ] && { min=$v; echo $min > /run/llm-vmin; }
    sleep 0.05
done
EOF
chmod +x $L/vmon
cat $P > /dev/null; sleep 20
echo "idle: $(energy) mW (total big mid little)"
printf "%-28s %-12s %-6s %8s %8s %8s %8s %8s %6s %6s\n" model cores test tok/s total_mW big mid little minV maxC
for m in qwen2.5-0.5b-instruct-q4_k_m qwen2.5-1.5b-instruct-q4_k_m; do
    for set in "little:0-3:4" "big:6-7:2" "big+mid:4-7:4"; do
        IFS=: read -r name cpus threads <<< "$set"
        for test in "pp128:-p 128 -n 0" "tg64:-p 0 -n 64"; do
            while [ "$(maxtemp)" -gt 50000 ]; do sleep 5; done
            setsid -f $L/vmon > /dev/null 2>&1; sleep 0.2
            cat $P > /dev/null
            out=$(taskset -c "$cpus" $B -m $L/$m.gguf -t "$threads" ${test#*:} -r 3 -o json 2>/dev/null)
            pw=$(energy); temp=$(maxtemp)
            pkill -f '^/bin/sh /root/llm/vmon'; vmin=$(cat /run/llm-vmin); rm -f /run/llm-vmin
            ts=$(echo "$out" | python3 -c 'import json,sys; print("%.1f" % json.load(sys.stdin)[0]["avg_ts"])' 2>/dev/null)
            printf "%-28s %-12s %-6s %8s %8s %8s %8s %8s %6s %6s\n" "${m%-instruct*}" "$name($threads)" "${test%%:*}" \
                "$ts" $pw "$((vmin / 1000))" "$((temp / 1000))"
        done
    done
done
