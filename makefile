CFLAGS += -std=c89 -Wall -Wextra -Werror -pedantic -fmax-errors=1 -g
LDLIBS += -lreadline

.PHONY: clean

client: client.o

clean:
	$(RM) client.o client
