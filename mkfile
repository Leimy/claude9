</$objtype/mkfile

BIN=$home/bin/$objtype

TARG=claude9 claude9fs

COMMONO=claude.$O action.$O json.$O

claude9:V: chat.$O $COMMONO
	$LD $LDFLAGS -o $target chat.$O $COMMONO

claude9fs:V: claude9fs.$O $COMMONO
	$LD $LDFLAGS -o $target claude9fs.$O $COMMONO

%.$O: %.c
	$CC $CFLAGS $stem.c

clean:V:
	rm -f *.[$OS] claude9 claude9fs

install:V: claude9 claude9fs
	cp claude9 $BIN/
	cp claude9fs $BIN/

nuke:V: clean
	rm -f $BIN/claude9 $BIN/claude9fs
