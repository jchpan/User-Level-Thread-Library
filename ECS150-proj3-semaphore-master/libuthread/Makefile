# default: target library
lib := libuthread.a
lib_deps := queue.o thread.o sem.o tps.o

# gcc flags
CC := gcc
CFLAGS := -Wall -Werror
## Debug flags
ifneq ($(D),1)
CFLAGS	+= -O2
else
CFLAGS	+= -O0
CFLAGS	+= -g
endif

all: $(lib)

# dependency tracking
deps := $(pastubst %.o,%.d,$(objs))
-include $(deps)
DEPFLAGS = -MMD -MF $(@:.o=.d)

$(lib): $(lib_deps)
	ar rcs $@ $^

# generic pattern
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $< $(DEPFLAGS)

# reserved object files, DO NOT REMOVE!
reserved := queue.o thread.o

# clean
.PHONY: clean
clean:
	-for i in $(reserved); do mv $$i $${i}_temp; done
	-rm *.o *.x *.out
	for i in $(reserved); do mv $${i}_temp $$i; done
