plugindir = $(PLUGINDIR)
plugin_LTLIBRARIES = libipod.la

libipod_la_SOURCES = \
	rb-ipod-plugin.c			\
	$(top_srcdir)/sources/rb-ipod-source.c	\
	$(top_srcdir)/sources/rb-ipod-source.h
	
libipod_la_LDFLAGS = $(PLUGIN_LIBTOOL_FLAGS)
libipod_la_LIBADD = 					\
	$(top_builddir)/lib/librb.la			\
	$(HAL_LIBS)					\
	$(IPOD_LIBS)

INCLUDES = 						\
        -DGNOMELOCALEDIR=\""$(datadir)/locale"\"        \
	-DG_LOG_DOMAIN=\"Rhythmbox\"		 	\
	-I$(top_srcdir) 				\
	-I$(top_srcdir)/lib                        	\
	-I$(top_srcdir)/metadata                       	\
	-I$(top_srcdir)/player                       	\
	-I$(top_srcdir)/rhythmdb                       	\
	-I$(top_srcdir)/widgets                    	\
	-I$(top_srcdir)/sources                    	\
	-I$(top_srcdir)/iradio                    	\
	-I$(top_srcdir)/podcast                    	\
	-I$(top_srcdir)/remote				\
	-I$(top_builddir)/remote			\
	-I$(top_srcdir)/plugins				\
	-I$(top_srcdir)/shell				\
	-DPIXMAP_DIR=\""$(datadir)/pixmaps"\"		\
	-DSHARE_DIR=\"$(pkgdatadir)\"                   \
	-DDATADIR=\""$(datadir)"\"			\
	$(RHYTHMBOX_CFLAGS)				\
	$(WARN_CFLAGS)					\
	$(HAL_CFLAGS)					\
	$(IPOD_CFLAGS)					\
	-D_XOPEN_SOURCE -D_BSD_SOURCE

plugin_in_files = ipod.rb-plugin.desktop.in

%.rb-plugin: %.rb-plugin.desktop.in $(INTLTOOL_MERGE) $(wildcard $(top_srcdir)/po/*po) ; $(INTLTOOL_MERGE) $(top_srcdir)/po $< $@ -d -u -c $(top_builddir)/po/.intltool-merge-cache

plugin_DATA = $(plugin_in_files:.rb-plugin.desktop.in=.rb-plugin)

CLEANFILES = $(plugin_DATA)
DISTCLEANFILES = $(plugin_DATA)