#!/bin/bash
sleep 25
P=$(cat /etc/krg-peers)
{
  echo "=== KRGREPORT $(hostname) node=$(cat /etc/krg-node-id) ==="
  echo "-- free --"
  /usr/local/bin/krgsys free "$P"
  echo "-- top --"
  /usr/local/bin/krgsys top "$P"
  echo "-- ps by node --"
  /usr/local/bin/krgtool show "$P" | awk 'NF>2 && $2 ~ /^[0-9]+$/ {c[int($2/16777216)]++} END {for (k in c) printf "node%-3d %6d\n", k, c[k]}' | sort -k1.5 -n
  echo "END"
} > /var/log/krgreport.log 2>&1