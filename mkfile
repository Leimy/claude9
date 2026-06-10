</$objtype/mkfile

BIN=$home/bin/$objtype
RCBIN=/rc/bin
SKILLSDIR=$home/lib/claude9/skills

TARG=claude9fs

COMMONO=claude.$O json.$O

claude9fs: claude9fs.$O $COMMONO
	$LD $LDFLAGS -o $target claude9fs.$O $COMMONO

%.$O: %.c
	$CC $CFLAGS $stem.c

claude9fs.$O claude.$O json.$O: claude.h json.h

clean:V:
	rm -f *.[$OS] claude9fs

install:V: claude9fs
	cp claude9fs $BIN/
	cp claudetalk $RCBIN/
	chmod +x $RCBIN/claudetalk
	mkdir -p $SKILLSDIR
	for(f in skills/*) cp $f $SKILLSDIR/

install_test:V: claude9fs
	cp claude9fs $BIN/claude9fs_test
	cp claudetalk_test $RCBIN/claudetalk_test
	chmod +x $RCBIN/claudetalk
	mkdir -p $SKILLSDIR
	for(f in skills/*) cp $f $SKILLSDIR/

nuke:V: clean
	rm -f $BIN/claude9fs $RCBIN/claudetalk
	rm -rf $home/lib/claude9
