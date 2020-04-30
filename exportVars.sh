#!/bin/sh

export PKG_CONFIG_PATH="../../../../../gtk/source/gtk+-2.24.28:../../../../../gtk/source/pango-1.36.8:../../../../../gtk/source/glib-2.44.1:../../../../../gtk/source/cairo-1.14.0:../../../../../gtk/source/pixman-0.32.6:../../../../../gtk/source/libpng-1.6.17:../../../../../gtk/source/gdk-pixbuf-2.30.8:../../../../../gtk/source/atk-2.16.0"

export CPPFLAGS="-O2 -I$HOME/gtk/inst/include -x objective-c++ -mmacosx-version-min=10.9"

export CFLAGS="-O2 -I$HOME/gtk/inst/include -x objective-c -mmacosx-version-min=10.9"

export LDFLAGS="-O2 -mmacosx-version-min=10.9"

export LIBS="-lgtkmacintegration-gtk2 -L/Users/paulj/gtk/inst/lib"

./autogen.sh --prefix="$HOME/Projects/Geany-Compiled" --disable-vte
