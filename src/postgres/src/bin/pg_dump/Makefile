#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_dump
#
# Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/pg_dump/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "ysql_dump/pg_restore/ysql_dumpall - backup and restore YSQL databases"
PGAPPICON=win32

subdir = src/bin/pg_dump
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS=	pg_backup_archiver.o pg_backup_db.o pg_backup_custom.o \
	pg_backup_null.o pg_backup_tar.o pg_backup_directory.o \
	pg_backup_utils.o parallel.o compress_io.o dumputils.o $(WIN32RES)

ifndef YB_BUILD_ROOT
$(error YB_BUILD_ROOT is not set)
endif

LIBS += -L$(YB_BUILD_ROOT)/lib -lyb_util

all: ysql_dump pg_restore ysql_dumpall

ysql_dump: pg_dump.o common.o pg_dump_sort.o $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) pg_dump.o common.o pg_dump_sort.o $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -lyb_pggate -lyb_common -o $@$(X)

pg_restore: pg_restore.o $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) pg_restore.o $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

ysql_dumpall: pg_dumpall.o dumputils.o | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) pg_dumpall.o dumputils.o $(WIN32RES) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) ysql_dump$(X) '$(DESTDIR)$(bindir)'/ysql_dump$(X)
	$(INSTALL_PROGRAM) pg_restore$(X) '$(DESTDIR)$(bindir)'/pg_restore$(X)
	$(INSTALL_PROGRAM) ysql_dumpall$(X) '$(DESTDIR)$(bindir)'/ysql_dumpall$(X)

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, ysql_dump$(X) pg_restore$(X) ysql_dumpall$(X))

clean distclean maintainer-clean:
	rm -f pg_dump$(X) pg_restore$(X) pg_dumpall$(X) $(OBJS) pg_dump.o common.o pg_dump_sort.o pg_restore.o pg_dumpall.o
	rm -rf tmp_check
