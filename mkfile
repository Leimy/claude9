</$objtype/mkfile

BIN=$home/bin/$objtype
RCBIN=/rc/bin
SKILLSDIR=$home/lib/claude9/skills

TARG=claude9fs claudegraph

COMMONO=claude.$O openai.$O json.$O

LIBVIEW=/usr/dave/work/libview/libview.a$O

all:V: claude9fs claudegraph

claude9fs: claude9fs.$O $COMMONO
	$LD $LDFLAGS -o $target claude9fs.$O $COMMONO

claudegraph: claudegraph.$O $LIBVIEW
	$LD $LDFLAGS -o $target claudegraph.$O $LIBVIEW

claudegraph.$O: claudegraph.c
	$CC $CFLAGS -I/usr/dave/work/libview claudegraph.c

$LIBVIEW:V:
	@{cd /usr/dave/work/libview && mk}

# Unit tests: build with "mk tests", then run ./tests yourself.
# tests.c #includes claude.c to reach its static internals, so
# it links with json.$O only (claude.$O would duplicate symbols).
tests: tests.$O openai.$O json.$O
	$LD $LDFLAGS -o $target tests.$O openai.$O json.$O

tests.$O: tests.c claude.c claude.h json.h claudeimpl.h
	$CC $CFLAGS tests.c

%.$O: %.c
	$CC $CFLAGS $stem.c

claude9fs.$O claude.$O openai.$O json.$O: claude.h json.h
claude.$O openai.$O: claudeimpl.h

clean:V:
	rm -f *.[$OS] claude9fs claudegraph tests

install:V: claude9fs claudegraph
	cp claude9fs $BIN/
	cp claudegraph $BIN/
	cp claudetalk $RCBIN/
	chmod +x $RCBIN/claudetalk
	mkdir -p $SKILLSDIR
	for(f in skills/*) cp $f $SKILLSDIR/

nuke:V: clean
	rm -f $BIN/claude9fs $BIN/claudegraph $RCBIN/claudetalk
	rm -rf $home/lib/claude9
