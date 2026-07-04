CC = vc
CFLAGS = -c89
LDFLAGS = -ldos

hello: hello.o
	$(CC) $(LDFLAGS) -o hello hello.o

hello.o: hello.c
	$(CC) $(CFLAGS) -c hello.c

clean:
	delete hello.o hello
