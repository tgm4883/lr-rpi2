include ( ../../../../settings.pro )

QT += xml sql network

contains(QT_VERSION, ^4\\.[0-9]\\..*) {
CONFIG += qtestlib
}
contains(QT_VERSION, ^5\\.[0-9]\\..*) {
QT += testlib
}

TEMPLATE = app
TARGET = test_mpegtables
DEPENDPATH += . ../..
INCLUDEPATH += . ../.. ../../mpeg ../../../libmythui ../../../libmyth ../../../libmythbase

# FIXME hack to make the linker to its thing
LIBS += ../../pespacket.o ../../mpegtables.o ../../mpegdescriptors.o ../../dvbdescriptors.o ../../atscdescriptors.o ../../sctedescriptors.o ../../iso6937tables.o ../../freesat_huffman.o ../../atsc_huffman.o

LIBS += -L../.. -lmythtv-$$LIBVERSION
LIBS += -L../../../libmythui -lmythui-$$LIBVERSION
LIBS += -L../../../libmyth -lmyth-$$LIBVERSION
LIBS += -L../../../libmythbase -lmythbase-$$LIBVERSION
LIBS += -L../../../../external/FFmpeg/libavutil -lmythavutil

contains(QMAKE_CXX, "g++") {
  QMAKE_CXXFLAGS += -O0 -fprofile-arcs -ftest-coverage
  QMAKE_LFLAGS += -fprofile-arcs
}

QMAKE_LFLAGS += -Wl,$$_RPATH_$(PWD)/../../../../external/zeromq/src/.libs/
QMAKE_LFLAGS += -Wl,$$_RPATH_$(PWD)/../../../../external/nzmqt/src/
QMAKE_LFLAGS += -Wl,$$_RPATH_$(PWD)/../../../../external/qjson/lib/
QMAKE_LFLAGS += -Wl,$$_RPATH_$(PWD)/../..
QMAKE_LFLAGS += -Wl,$$_RPATH_$(PWD)/../../../libmythui
QMAKE_LFLAGS += -Wl,$$_RPATH_$(PWD)/../../../libmythupnp
QMAKE_LFLAGS += -Wl,$$_RPATH_$(PWD)/../../../libmythbase
QMAKE_LFLAGS += -Wl,$$_RPATH_$(PWD)/../..
QMAKE_LFLAGS += -Wl,$$_RPATH_$(PWD)/../../../../external/FFmpeg/libavutil

# Input
HEADERS += test_mpegtables.h
SOURCES += test_mpegtables.cpp

QMAKE_CLEAN += $(TARGET) $(TARGETA) $(TARGETD) $(TARGET0) $(TARGET1) $(TARGET2)
QMAKE_CLEAN += ; rm -f *.gcov *.gcda *.gcno

LIBS += $$EXTRA_LIBS $$LATE_LIBS
