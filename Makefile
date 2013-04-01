#CFLAGS = -Ofast -flto
CFLAGS = -g
CC = gcc

LIB_OBJS := buffered.o hc-128.o protocol.o rabbit.o salsa20.o sosemanuk.o util.o umac.o
TESTS := algorithms_test buffering_test umac_test

.PHONY : all tests clean

libestream.a: $(LIB_OBJS)
	ar rcs libestream.a $(LIB_OBJS)

-include $(LIB_OBJS:.o=.d)

tests: $(TESTS)

all: libestream.a tests

%_test: libestream.a tests/%_test.o
	$(CC) $(CFLAGS) tests/$*_test.o libestream.a -o $*_test

%.o: %.c
	$(CC) -c $(CFLAGS) -I. $*.c -o $*.o
	$(CC) -MM $(CFLAGS) -I. $*.c > $*.d

clean:
	-rm -f libestream.a *.o *.d tests/*.o tests/*.d $(TESTS)
