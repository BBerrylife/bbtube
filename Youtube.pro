APP_NAME = Youtube

CONFIG += qt warn_on cascades10
LIBS += -lbb
LIBS += -lbbdata
LIBS += -lbbmultimedia
LIBS += -lbbsystem
LIBS += -lscreen
LIBS += -laudio_manager
# For AES-256-CBC encryption of the stored Google session cookie
# (src/auth/CookieCrypto.cpp), using the OpenSSL headers/lib shipped as
# part of the BB10/QNX NDK (openssl/aes.h, openssl/sha.h, openssl/rand.h).
# If this fails to link ("cannot find -lcrypto"), check
# ${QNX_TARGET}/{armle-v7,x86}/lib for the actual library filename in your
# SDK install and adjust this line accordingly -- see the comment in
# src/auth/CookieCrypto.hpp for context.
LIBS += -lcrypto
# For bb::device::HardwareInfo (also used by CookieCrypto, to derive the
# per-device AES key).
LIBS += -lbbdevice
QT += network

include(config.pri)
