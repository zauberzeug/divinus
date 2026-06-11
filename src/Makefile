SRC = $(shell find ./ -name '*.c')
OBJ = ../divinus

.PHONY: clean divinus
divinus: $(OBJ)

$(OBJ): $(SRC:%.c=%.o)
	$(CC) $^ -rdynamic $(OPT) -o $@

%.o: %.c Makefile
	$(CC) -c $< $(OPT) -MMD -MP -o $@

-include $(SRC:%.c=%.d)

clean:
	rm -rf $(SRC:%.c=%.o) $(SRC:%.c=%.d) $(OBJ)
