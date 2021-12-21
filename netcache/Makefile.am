
EXTRA_DIST = bootstrap BUGS 
SUBDIRS = curl.current sqlite.current netcache include plugins test
ACLOCAL_AMFLAGS = -I config

commit:
snapshot:
	@sh $(srcdir)/bin/make_snapshot.sh

.PHONY: svnupdate

svnupdate:
	svn update
	svnrevision -n > REVISION
