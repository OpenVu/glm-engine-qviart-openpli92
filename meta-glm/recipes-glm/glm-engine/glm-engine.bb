SUMMARY = "GLM OpenGL ES rendering/animation engine for Qviart Dual 4K"
DESCRIPTION = "Shared library built from glm-engine.cpp for OpenPLi Qviart Dual 4K."
SECTION = "graphics"
LICENSE = "CLOSED"
PV = "1.0"
PR = "r0"

SRC_URI = " \
    file://glm-engine.cpp \
    file://stb_image.h \
    file://stb_image_resize.h \
    file://stb_truetype.h \
    file://glm/ \
"

S = "${WORKDIR}"

# GLM and stb headers are bundled in this source tree. The linked target
# libraries are provided by the OpenPLi 9.2/Qviart build environment.
DEPENDS += " \
    curl \
    sqlite3 \
    freetype \
    harfbuzz \
    virtual/egl \
    virtual/libgles2 \
"

PACKAGE_ARCH = "${MACHINE_ARCH}"

EXTRA_OEMAKE = ""

# The source exports its C API from an extern \"C\" block and is consumed as
# an unversioned /usr/lib/libglm.so shared object by the Enigma2 side.
do_compile() {
    ${CXX} ${CXXFLAGS} ${CPPFLAGS} \
        -I${S} \
        -I${STAGING_INCDIR}/freetype2 \
        -I${STAGING_INCDIR}/harfbuzz \
        -shared -fPIC -std=c++11 -pthread \
        -Wl,-soname,libglm.so \
        -o ${B}/libglm.so ${S}/glm-engine.cpp \
        ${LDFLAGS} \
        -lEGL -lGLESv2 \
        -lcurl -lsqlite3 -lfreetype -lharfbuzz \
        -ldl -lm
}

do_install() {
    install -d ${D}${libdir}
    install -m 0755 ${B}/libglm.so ${D}${libdir}/libglm.so
}

# libglm.so is intentionally an unversioned runtime library used directly by
# the Enigma2 Python integration. Prevent OE from moving it to -dev.
FILES_SOLIBSDEV = ""
FILES:${PN} += "${libdir}/libglm.so"

# The source may optionally dlopen vendor GPU libraries (for example
# libvugles2.so). That dependency is intentionally runtime-discovered.
INSANE_SKIP:${PN} += "dev-so file-rdeps"
