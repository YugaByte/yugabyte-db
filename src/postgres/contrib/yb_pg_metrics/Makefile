# contrib/yb_pg_metrics/Makefile

MODULE_big = yb_pg_metrics
OBJS = yb_pg_metrics.o $(WIN32RES)

EXTENSION = yb_pg_metrics
DATA =
PGFILEDESC = "yb_pg_metrics - YugaByte metrics"
SHLIB_LINK += -L$(YB_BUILD_ROOT)/lib -lserver_process -lyb_common
subdir = contrib/yb_pg_metrics
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
