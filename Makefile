list:
	lsusb -v

all: pmbplay pmbpipe

bin:
	mkdir ./bin

out:
	mkdir ./out

pmbplay: pmbplay.o libpmb.o bin
	gcc -o bin/pmbplay out/pmbplay.o out/libpmb.o -lusb

pmbpipe: pmbpipe.o libpmb.o bin
	gcc -o bin/pmbpipe out/pmbpipe.o out/libpmb.o -lusb

libpmb: libpmb.o bin
	gcc -o bin/libpmb out/libpmb.o -lusb

pmbplay.o: src/pmbplay.c out
	gcc -c -o out/pmbplay.o src/pmbplay.c

pmbpipe.o: src/pmbpipe.c out
	gcc -c -o out/pmbpipe.o src/pmbpipe.c

libpmb.o: src/libpmb.c out
	gcc -c -o out/libpmb.o src/libpmb.c -lusb

clean:
	rm -rf ./out ./bin 

