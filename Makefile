CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11 -Icommon
BIN     := bin

all: $(BIN)/client $(BIN)/server $(BIN)/dispatcher

$(BIN)/client: client/client.c common/dns_protocol.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<
$(BIN)/server: server/server.c common/dns_protocol.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<
$(BIN)/dispatcher: dispatcher/dispatcher.c common/dns_protocol.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<
$(BIN):
	mkdir -p $(BIN)

test: all
	bash tests/test_normal.sh
	bash tests/test_failover.sh
	bash tests/test_server_failure.sh
	bash tests/test_packet_loss.sh
	bash tests/test_duplicate.sh
	bash tests/test_dispatcher.sh

clean:
	rm -rf $(BIN)

.PHONY: all test clean
