CFLAGS= -O3 
CC=gcc
LD=gcc 
NAME=numa_bench
LIB=-lm -lpthread -lnuma

all:	main

main: ${NAME}.c
	${CC} ${CFLAGS} -m64 -c ${NAME}.c
	${LD} ${NAME}.o ${LIB} -o ${NAME} 

clean:
	rm -f ${NAME} ${NAME}.o

