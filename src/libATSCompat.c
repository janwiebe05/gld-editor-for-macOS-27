/*
 * Ersatz für die ATS-Schriftfunktionen, die QtGui 4.8 in
 * QFontDatabase::addApplicationFont() benutzt.
 *
 * Ab macOS 27 sind diese Funktionen stillgelegt: ATSFontActivateFromMemory()
 * meldet Erfolg, liefert aber Container 0, und ATSFontFindFromContainer()
 * findet nichts. Qt kann dadurch keine eingebetteten Schriften mehr laden,
 * und der GLD Editor stürzt beim Start in cApplicationUtility::LoadFont() ab.
 *
 * Diese Bibliothek wird QtGui statt ApplicationServices untergeschoben,
 * bildet die fünf Funktionen mit CoreText nach und reicht alles andere per
 * Re-Export an das echte ApplicationServices durch.
 *
 * Ein "Container" und der darin enthaltene "Font" haben hier dieselbe
 * Nummer: Index in der Tabelle + 1 (0 bedeutet bei ATS "ungültig").
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <dlfcn.h>
#include <pthread.h>

/* Eigene Typnamen, die ATS-Header sind im aktuellen SDK nicht mehr nutzbar */
typedef UInt32 ContainerRef;
typedef UInt32 FontRef;

enum { kParamErr = -50, kInvalidFontErr = -985 };

#define EXPORT __attribute__((visibility("default")))
#define MAX_FONTS 256

static CGFontRef gFonts[MAX_FONTS];
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;

static CGFontRef font_for(UInt32 ref)
{
    CGFontRef f = NULL;
    pthread_mutex_lock(&gLock);
    if (ref >= 1 && ref <= MAX_FONTS)
        f = gFonts[ref - 1];
    pthread_mutex_unlock(&gLock);
    return f;
}

static OSStatus activate(CFDataRef data, ContainerRef *outContainer)
{
    if (!data || !outContainer)
        return kParamErr;
    *outContainer = 0;

    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGFontRef font = provider ? CGFontCreateWithDataProvider(provider) : NULL;
    if (provider)
        CGDataProviderRelease(provider);
    if (!font)
        return kInvalidFontErr;

    /* Schlägt fehl, wenn die Schrift schon registriert ist – für Qt ist sie
       dann trotzdem über ihren PostScript-Namen auffindbar. */
    CTFontManagerRegisterGraphicsFont(font, NULL);

    OSStatus status = kParamErr;
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < MAX_FONTS; i++) {
        if (!gFonts[i]) {
            gFonts[i] = font;
            *outContainer = (ContainerRef)(i + 1);
            status = noErr;
            break;
        }
    }
    pthread_mutex_unlock(&gLock);
    if (status != noErr) {
        CTFontManagerUnregisterGraphicsFont(font, NULL);
        CGFontRelease(font);
    }
    return status;
}

EXPORT OSStatus ATSFontActivateFromMemory(void *data, ByteCount length, UInt32 context,
                                          UInt32 format, void *reserved, UInt32 options,
                                          ContainerRef *outContainer)
{
    (void)context; (void)format; (void)reserved; (void)options;
    if (!data || !length)
        return kParamErr;
    /* Kopie: Qt gibt seinen Puffer direkt nach dem Aufruf frei */
    CFDataRef cfData = CFDataCreate(NULL, data, (CFIndex)length);
    OSStatus status = activate(cfData, outContainer);
    if (cfData)
        CFRelease(cfData);
    return status;
}

EXPORT OSStatus ATSFontActivateFromFileReference(const void *fsRef, UInt32 context,
                                                 UInt32 format, void *refCon, UInt32 options,
                                                 ContainerRef *outContainer)
{
    (void)context; (void)format; (void)refCon; (void)options;
    /* FSRef -> URL über die (veraltete) CoreServices-Funktion, zur Laufzeit gesucht */
    typedef CFURLRef (*url_from_fsref_t)(CFAllocatorRef, const void *);
    url_from_fsref_t urlFromFSRef = (url_from_fsref_t)dlsym(RTLD_DEFAULT, "CFURLCreateFromFSRef");
    if (!fsRef || !urlFromFSRef)
        return kParamErr;

    CFURLRef url = urlFromFSRef(NULL, fsRef);
    if (!url)
        return kParamErr;
    CFDataRef data = NULL;
    CFReadStreamRef stream = CFReadStreamCreateWithFile(NULL, url);
    if (stream && CFReadStreamOpen(stream)) {
        CFMutableDataRef buf = CFDataCreateMutable(NULL, 0);
        UInt8 chunk[65536];
        CFIndex n;
        while ((n = CFReadStreamRead(stream, chunk, sizeof chunk)) > 0)
            CFDataAppendBytes(buf, chunk, n);
        CFReadStreamClose(stream);
        data = buf;
    }
    if (stream)
        CFRelease(stream);
    CFRelease(url);

    OSStatus status = activate(data, outContainer);
    if (data)
        CFRelease(data);
    return status;
}

EXPORT OSStatus ATSFontFindFromContainer(ContainerRef container, UInt32 options,
                                         ItemCount count, FontRef *outFonts,
                                         ItemCount *outCount)
{
    (void)options;
    if (!font_for(container))
        return kParamErr;
    if (count > 0 && outFonts)
        outFonts[0] = container;
    if (outCount)
        *outCount = 1;
    return noErr;
}

EXPORT OSStatus ATSFontGetPostScriptName(FontRef font, UInt32 options, CFStringRef *outName)
{
    (void)options;
    CGFontRef f = font_for(font);
    if (!f || !outName)
        return kInvalidFontErr;
    *outName = CGFontCopyPostScriptName(f);
    return *outName ? noErr : kInvalidFontErr;
}

/*
 * Bitmaps im skalierten Fenster (siehe libGLDWindow.m): Qt 4 skaliert Pixmaps
 * ohne SmoothPixmapTransform mit "nächster Pixel" – vergrößert sieht das
 * treppig aus. QtGui bezieht diese Funktion über ApplicationServices, also
 * über diese Bibliothek; hier wird immer hochwertig interpoliert.
 */

static void *cg_sym(const char *name)
{
    static void *cg;
    if (!cg)
        cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
    return dlsym(cg, name);
}

EXPORT void CGContextSetInterpolationQuality(CGContextRef c, CGInterpolationQuality q)
{
    static void (*real)(CGContextRef, CGInterpolationQuality);
    if (!real)
        real = cg_sym("CGContextSetInterpolationQuality");
    (void)q;
    real(c, kCGInterpolationHigh);
}

EXPORT OSStatus ATSFontDeactivate(ContainerRef container, void *refCon, UInt32 options)
{
    (void)refCon; (void)options;
    CGFontRef f = NULL;
    pthread_mutex_lock(&gLock);
    if (container >= 1 && container <= MAX_FONTS) {
        f = gFonts[container - 1];
        gFonts[container - 1] = NULL;
    }
    pthread_mutex_unlock(&gLock);
    if (!f)
        return kParamErr;
    CTFontManagerUnregisterGraphicsFont(f, NULL);
    CGFontRelease(f);
    return noErr;
}
