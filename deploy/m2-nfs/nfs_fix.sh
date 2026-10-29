#!/bin/sh
set -e
printf '/srv/krg 192.168.6.0/24(rw,sync,no_subtree_check,no_root_squash,insecure)\n' > /etc/exports
exportfs -ra
exportfs -v
