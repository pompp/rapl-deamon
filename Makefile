CC	= gcc
#CFLAGS	= -g -DDEBUG -DCODE_DEBUG -DCOREi
#CFLAGS	= -g -DDEBUG -DCODE_DEBUG
#CFLAGS	= -g -DCOREi
CFLAGS	= -g
RM	= /bin/rm -f

all:	rapld raplc

rapld:	rapld.c
	$(CC) $(CFLAGS) rapld.c -o $@ -lm

raplc:	raplc.c
	$(CC) $(CFLAGS) raplc.c -o $@

clean:
	$(RM) a.out core *.o

veryclean:	clean
	$(RM) rapld raplc
