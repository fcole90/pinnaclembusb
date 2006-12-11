all: pmbplay pmbpipe

pmbplay: pmbplay.o libpmb.o
	gcc -lusb -o pmbplay pmbplay.o libpmb.o

pmbpipe: pmbpipe.o libpmb.o
	gcc -lusb -o pmbpipe pmbpipe.o libpmb.o

pmbplay.o: pmbplay.c
	gcc -c -o pmbplay.o pmbplay.c

pmbpipe.o: pmbpipe.c
	gcc -c -o pmbpipe.o pmbpipe.c

libpmb.o: libpmb.c
	gcc -c -o libpmb.o libpmb.c

clean:
	rm -f *.o pmbplay

