#!/bin/bash
# Demonstrates concurrency control and ACID behaviour of the banking server.
#
#   tests/run_tests.sh [port]
#
# Runs against a fresh copy of the server in a temporary folder, so the real
# data/ folder is never touched.
#   1. Race conditions: 2 x 100 simultaneous opposite transfers keep the total.
#   2. One session per user: a second login to the same account is refused.
#   3. Atomicity: a server process killed BEFORE the commit point leaves no trace.
#   4. Durability: a process killed AFTER the commit point is recovered from its journal.

cd "$(dirname "$0")/.." || exit 1
make -s || exit 1

PORT=${1:-9555}
WORK=$(mktemp -d)
cp server client "$WORK"/
cd "$WORK" || exit 1
C="./client 127.0.0.1 $PORT"
SERVER=""
FAILED=0

start_server() {            # start_server [crash-point]
    BANK_CRASH_POINT="$1" ./server "$PORT" >> server.log 2>&1 &
    SERVER=$!
    sleep 0.5
}
stop_server() {
    kill -INT "$SERVER" 2>/dev/null
    wait "$SERVER" 2>/dev/null
}
cleanup() { stop_server; rm -rf "$WORK"; }
trap cleanup EXIT

check() {                   # check "description" condition...
    local what=$1; shift
    if "$@"; then echo "  PASS  $what"; else echo "  FAIL  $what"; FAILED=1; fi
}
balance()  { awk -F'|' -v id="$1" '$1 == id { print $5 }' data/customers.txt; }
total()    { awk -F'|' '{ s += $5 } END { printf "%.2f", s }' data/customers.txt; }
txn_count() { echo $(( $(stat -c %s data/transactions.txt) / 256 )); }

start_server
# Setup: admin adds an employee; the employee opens two accounts of 10000 each.
printf '4\n1\nadmin123\n1\nTest\nEmployee\nemp\n1\n8\n' | $C > /dev/null
printf '2\n1\nemp\n1\nAlice\nA\np1\n10000\n1\nBob\nB\np2\n10000\n9\n' | $C > /dev/null

echo "1. Race conditions: 2 x 100 simultaneous opposite transfers"
transfers() {               # transfers <id> <password> <to>
    printf '1\n%s\n%s\n' "$1" "$2"
    for _ in $(seq 100); do printf '4\n%s\n1\n' "$3"; done
    printf '10\n'
}
before_total=$(total); before_txn=$(txn_count)
transfers 1 p1 2 | $C > /dev/null & c1=$!
transfers 2 p2 1 | $C > /dev/null & c2=$!
wait $c1 $c2
echo "        total before $before_total, after $(total); new transaction records $(( $(txn_count) - before_txn ))"
check "total money unchanged" [ "$(total)" = "$before_total" ]
check "400 transaction records written" [ $(( $(txn_count) - before_txn )) -eq 400 ]

echo "2. One session per user"
(printf '1\n1\np1\n'; sleep 2; printf '10\n') | $C > /dev/null & holder=$!
sleep 0.7
out=$(printf '1\n1\np1\n5\n' | $C)
wait $holder
check "second login refused" grep -q "already logged in" <<< "$out"
stop_server

echo "3. Atomicity: process killed before the commit point (deposit 700)"
start_server before_commit
b=$(balance 1); t=$(txn_count)
printf '1\n1\np1\n2\n700\n' | $C > /dev/null
sleep 0.5
echo "        balance before $b, after $(balance 1)"
check "balance unchanged" [ "$(balance 1)" = "$b" ]
check "no transaction record added" [ "$(txn_count)" -eq "$t" ]
check "server discarded the journal" grep -q "discarded" server.log
stop_server

echo "4. Durability: process killed after the commit point (deposit 500)"
start_server after_commit
b=$(balance 1); t=$(txn_count)
printf '1\n1\np1\n2\n500\n' | $C > /dev/null
sleep 0.5
expected=$(awk -v b="$b" 'BEGIN { printf "%.2f", b + 500 }')
echo "        balance before $b, after $(balance 1)"
check "deposit recovered from journal" [ "$(balance 1)" = "$expected" ]
check "passbook entry recovered" [ "$(txn_count)" -eq $(( t + 1 )) ]
check "no journal left behind" [ -z "$(ls data/journal)" ]
stop_server

echo
echo "Server log:"
grep recovery server.log | sed 's/^/  /'
echo
if [ $FAILED -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; fi
exit $FAILED
