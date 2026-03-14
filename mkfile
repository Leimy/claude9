</$objtype/mkfile

BIN=/$objtype/bin
CFLAGS=-FTVw
HFILES=json.h claude.h

OFILES=\
	chat.$O\
	claude.$O\
	json.$O\
	action.$O\

COMMON=claude.$O json.$O

TARG=claude9 debugpatch

%.$O: $HFILES

%.$O: %.c
	$CC $CFLAGS $stem.c

claude9: chat.$O action.$O $COMMON
	$LD $LDFLAGS -o $target $prereq

debugpatch: debugpatch.$O action.$O $COMMON
	$LD $LDFLAGS -o $target $prereq

install:V:
	cp claude9 $BIN/claude9

installall:V:
	for(objtype in $CPUS) mk $MKFLAGS install

clean:V:
	rm -f *.[$OS] [$OS].* $TARG

%.acid: %.c $HFILES
	$CC $CFLAGS -a $stem.c >$target
