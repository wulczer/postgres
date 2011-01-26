# contrib/hstore/Makefile

MODULE_big = hstore

OBJS = hstore_io.o hstore_op.o hstore_gist.o hstore_gin.o hstore_compat.o \
	hstore_plpython.o crc32.o

ifeq ($(with_python),yes)

PG_CPPFLAGS := -I$(srcdir) -I$(top_builddir)/src/pl/plpython \
			$(python_includespec) -DHSTORE_PLPYTHON_SUPPORT
SHLIB_LINK = $(python_libspec) $(python_additional_libs) \
		$(filter -lintl,$(LIBS)) $(CPPFLAGS)
endif

DATA_built = hstore.sql
DATA = uninstall_hstore.sql
REGRESS = hstore

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/hstore
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
