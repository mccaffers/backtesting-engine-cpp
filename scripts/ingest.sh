#!/bin/bash
# Builds the engine and runs the `ingest` subcommand: it binds a UDP socket and
# streams decoded ticks (tickPacket) into QuestDB via ILP-over-HTTP
# (source/ingest/ingestCommand.cppm). Blocks until SIGINT/SIGTERM.
#
# Config is read from the environment by IngestCommand (defaults in parens):
#   QUESTDB_HOST      QuestDB host                (127.0.0.1)
#   QUESTDB_ILP_PORT  QuestDB ILP/HTTP port       (9000)
#   INGEST_BIND_ADDR  UDP bind address            (127.0.0.1)
#   INGEST_UDP_PORT   UDP bind port               (11111, == UDPPorts.PortSave)
#
# An optional first argument overrides the bind port (argv[2] to the engine):
#   sh scripts/ingest.sh           # bind 11111 (or $INGEST_UDP_PORT)
#   sh scripts/ingest.sh 22222     # bind 22222

current_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

if ! source "$current_dir/build.sh"; then
    echo "Error: Build failed. Aborting."
    exit 1
fi

if [ ! -f "$BUILD_DIR/$EXECUTABLE_NAME" ]; then
    echo "Error: Executable $EXECUTABLE_NAME not found in $BUILD_DIR."
    ls -la "$BUILD_DIR"
    exit 1
fi

# Non-fatal QuestDB reachability check: the receiver still binds and buffers if
# QuestDB is down (the writer sheds once its buffer fills), so warn rather than
# abort.
quest_host="${QUESTDB_HOST:-127.0.0.1}"
quest_port="${QUESTDB_ILP_PORT:-9000}"
# Probe a real endpoint (/exec), not "/": QuestDB's web console root can stall
# indefinitely, which made a 1s timeout report a healthy DB as unreachable.
if ! curl --fail --silent --max-time 2 "http://$quest_host:$quest_port/exec?query=SELECT%201" >/dev/null 2>&1; then
    echo "Warning: QuestDB not reachable at http://$quest_host:$quest_port — writes will be dropped until it comes up."
fi

# exec so signals reach the engine directly for clean shutdown. "$@" passes an
# optional bind-port override straight through to the subcommand.
exec ./"$BUILD_DIR/$EXECUTABLE_NAME" ingest "$@"
