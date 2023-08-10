# See LICENSE for license details.
target := isc-test

CC ?= gcc

obj := $(patsubst %.c,%.o,$(wildcard src/*.c sample/*.c))

$(target): $(obj)
	@cd out && $(CC) $(obj) -lpthread -o $@
	@echo "make $@ done."

$(obj): %.o: %.c
	@mkdir -p `dirname out/$@`
	@$(CC) -Wall -Werror -Iinclude $< -c -o out/$@

clean:
	@rm -rf out
	@echo "make $@ done."

test:
	sudo out/$(target)
