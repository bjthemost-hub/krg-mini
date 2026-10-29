#!/bin/sh
# node 41 side: write into the shared tree, print what we see
echo "hello-from-41 $(date -Is)" > /krg/from41.txt
chmod 644 /krg/from41.txt
echo "--- ls /krg ---"
ls -l /krg
echo "--- content ---"
cat /krg/from41.txt
echo "--- inode/size ---"
stat -c '%i %s %n' /krg/from41.txt
echo "--- if a file from another node exists, read it ---"
if [ -f /krg/from17.txt ]; then cat /krg/from17.txt; stat -c '%i %s %n' /krg/from17.txt; else echo "(no from17.txt yet)"; fi
