#!/bin/bash
# Builds the engine and runs the `live` subcommand: pulls the winning backtest
# runs from the backtesting-winners-current Elasticsearch alias (the newest
# weekly winners index, repointed by each load), instantiates their
# strategies, then binds a UDP socket on the live tick stream and routes each
# decoded tick (tickPacket) to the cached strategies (one worker thread each).
# Entries are gated by a Redis trade lock per (strategy UUID, direction) plus
# an open-trade cap; orders are placed for real via the IG REST API
# (brokerOrderSink -> orderChannel, session tokens from DynamoDB). The broker
# owns positions and exits — the engine mirrors the position book from Redis
# and only emits opens/closes. Nothing is persisted to QuestDB. Blocks until
# SIGINT/SIGTERM.
#
# Config is read from the environment by LiveCommand (defaults in parens):
#   LIVE_BIND_ADDR           UDP bind address  (127.0.0.1)
#   LIVE_UDP_PORT            UDP bind port     (11110, == UDPPorts.PortLive)
#   LIVE_MIN_SCORE           winner floor on results.performanceScore (20)
#   LIVE_MAX_DRAWDOWN_PERCENT  hard ceiling on results.maxDrawdownPercent (10)
#   LIVE_MIN_CALMAR_SCORE    floor on results.calmarScore (30, Calmar ratio ~2)
#   LIVE_TRADE_LOCK_SECONDS  Redis trade-lock TTL (30)
#   REDIS_HOST               Redis host for locks/positions (127.0.0.1:6379)
#   TRADING_ENVIRONMENT      IG environment, DynamoDB key Auth#<env> (demo)
#   ELASTIC_HOST             winners source (http://localhost:9200);
#                            with ELASTIC_USER / ELASTIC_USER_PASSWORD if the
#                            cluster needs basic auth. Reachable ES with at
#                            least one qualifying run is REQUIRED — otherwise
#                            the process exits 1 at startup.
#
# AWS credentials (SDK default chain: AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY
# / AWS_DEFAULT_REGION or a profile) are needed to pull the IG session from the
# MarketDataLive DynamoDB table.
#
# An optional first argument overrides the bind port (argv[2] to the engine):
#   sh scripts/live.sh           # bind 11110 (or $LIVE_UDP_PORT)
#   sh scripts/live.sh 22222     # bind 22222

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

# exec so signals reach the engine directly for clean shutdown. "$@" passes an
# optional bind-port override straight through to the subcommand.
exec ./"$BUILD_DIR/$EXECUTABLE_NAME" live "$@"
