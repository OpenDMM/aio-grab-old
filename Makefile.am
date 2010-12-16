AM_CFLAGS = \
	@LIBPNG_CFLAGS@ \
	-Wall -Wextra -std=c99 -ggdb3

bin_PROGRAMS = grab

grab_SOURCES = main.c
grab_LDADD = @LIBJPEG_LIBS@ @LIBPNG_LIBS@
