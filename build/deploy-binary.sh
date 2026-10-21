#!/bin/bash
set -euo pipefail
cd /home/builder/yasem-build/yasem-obj
make -j4
SSH="sshpass -p $YASEM_SSH_PASS ssh -o StrictHostKeyChecking=no"
SCP="sshpass -p $YASEM_SSH_PASS scp -o StrictHostKeyChecking=no"
HOST="${YASEM_SSH_HOST:-root@YOUR_BOX_IP}"
$SSH $HOST 'systemctl stop yasem.service; sleep 1; killall -9 yasem 2>/dev/null || true; sleep 1'
$SCP /home/builder/yasem-build/yasem-obj/yasem $HOST:/storage/yasem/yasem
$SSH $HOST 'chmod 755 /storage/yasem/yasem; rm -f /storage/yasem/yasem.log; systemctl start yasem.service; sleep 10; echo ACTIVE=$(systemctl is-active yasem.service); tail -n 20 /storage/yasem/yasem.log'

