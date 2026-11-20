/*
 * Scharfe Pultoberfläche für den GLD Editor (Retina/Vollbild).
 *
 * Die App zeichnet Taster, Fader, Regler, LEDs und Meter vorab in QPixmaps
 * in einfacher Auflösung und kopiert diese dann nur noch auf den Bildschirm.
 * Qt 4 kennt kein devicePixelRatio, deshalb bleibt das unscharf.
 *
 * Diese Bibliothek wird dem Hauptprogramm statt QtGui untergeschoben und
 * reicht alles per Re-Export an QtGui durch – bis auf die wenigen Funktionen,
 * mit denen das Hauptprogramm Pixmaps anlegt, bemalt und zeichnet:
 *
 *   QPixmap(int, int), QPixmap(QSize)   legt die Pixmap k-fach groß an und
 *                                       merkt sie sich ("markiert")
 *   width/height/size/rect              melden bei markierten die alte Größe
 *   QPainter(dev), begin(dev)           skaliert den Painter um k
 *   drawPixmap(...)                     zeichnet die große Pixmap in die
 *                                       alte Fläche
 *   copy, fill, Kopien, Zuweisungen,    behalten bzw. übertragen die
 *   QDrag::setDragCursor                Markierung
 *   drawText(Rechteck, ...)             verkleinert Text, der nicht passt
 *
 * Markiert wird über die Seriennummer der Pixmap-Daten (obere 32 Bit von
 * cacheKey()). Kopien teilen die Daten und damit die Markierung; wird
 * abgekoppelt (detach), wird die neue Seriennummer nachgetragen.
 *
 * Nur Aufrufe des Hauptprogramms laufen hier durch, QtGui-intern bleibt
 * alles unverändert. GLD_PIXMAP_SCALE=1 schaltet die Hochskalierung ab,
 * GLD_NO_TEXTFIT=1 das Einpassen von Text.
 *
 * Fallstricke der Qt-4-ABI auf dem Mac (x86_64), die hier berücksichtigt sind:
 *   - QRect liegt als y1, x1, y2, x2 im Speicher (passend zu Carbons Rect)
 *   - QFlags hat einen eigenen Kopierkonstruktor und wird als Zeiger übergeben
 */

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Qt-Wertetypen mit identischem Speicherlayout. x2/y2 sind inklusive. */
typedef struct { int y1, x1, y2, x2; } QRect;
typedef struct { double x, y, w, h; } QRectF;
typedef struct { double x, y; } QPointF;
typedef struct { int w, h; } QSize;
typedef struct QPixmap QPixmap;
typedef struct QPainter QPainter;
typedef struct QPaintDevice QPaintDevice;
typedef struct QColor QColor;
typedef struct QDrag QDrag;

/* Platz für eine temporäre QPixmap (tatsächlich 24 Byte) bzw. QFont/QFontMetrics */
typedef struct { _Alignas(16) unsigned char b[64]; } PixmapStorage;
typedef struct { _Alignas(16) unsigned char b[32]; } SmallStorage;

/* Deklaration mit dem exakten Qt-Symbolnamen, dann die Definition */
#define EXPORTED __attribute__((visibility("default")))

static int gScale = 2;

/* ---------- echte QtGui-Funktionen ---------- */

static void *qtgui(void)
{
    static void *h;
    if (!h)
        h = dlopen("@executable_path/../Frameworks/QtGui.framework/Versions/4/QtGui", RTLD_LAZY | RTLD_NOLOAD);
    return h;
}

#define REAL(var, type, name)                     \
    static __typeof__(type) var;                  \
    if (!var)                                     \
        var = (__typeof__(type))dlsym(qtgui(), name)

static int64_t cache_key(const QPixmap *pm)
{
    REAL(f, int64_t (*)(const QPixmap *), "_ZNK7QPixmap8cacheKeyEv");
    return f(pm);
}

static int real_width(const QPixmap *pm)
{
    REAL(f, int (*)(const QPixmap *), "_ZNK7QPixmap5widthEv");
    return f(pm);
}

static int real_height(const QPixmap *pm)
{
    REAL(f, int (*)(const QPixmap *), "_ZNK7QPixmap6heightEv");
    return f(pm);
}

static void real_pixmap_dtor(QPixmap *pm)
{
    REAL(f, void (*)(QPixmap *), "_ZN7QPixmapD1Ev");
    f(pm);
}

/* ---------- Markierungen (Menge von Seriennummern) ---------- */

static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *gSet;
static size_t gCap, gCount;

static uint32_t serial_of(const QPixmap *pm)
{
    return (uint32_t)((uint64_t)cache_key(pm) >> 32);
}

static size_t slot_for(uint32_t *set, size_t cap, uint32_t s)
{
    size_t i = (s * 2654435761u) & (cap - 1);
    while (set[i] && set[i] != s)
        i = (i + 1) & (cap - 1);
    return i;
}

static void mark_serial(uint32_t s)
{
    if (!s)
        return;
    pthread_mutex_lock(&gLock);
    if ((gCount + 1) * 2 > gCap) {
        size_t ncap = gCap ? gCap * 2 : 4096;
        uint32_t *nset = calloc(ncap, sizeof *nset);
        for (size_t i = 0; i < gCap; i++)
            if (gSet[i])
                nset[slot_for(nset, ncap, gSet[i])] = gSet[i];
        free(gSet);
        gSet = nset;
        gCap = ncap;
    }
    size_t i = slot_for(gSet, gCap, s);
    if (!gSet[i]) {
        gSet[i] = s;
        gCount++;
    }
    pthread_mutex_unlock(&gLock);
}

static int is_marked_serial(uint32_t s)
{
    if (!s || !gCap)
        return 0;
    pthread_mutex_lock(&gLock);
    int hit = gSet[slot_for(gSet, gCap, s)] == s;
    pthread_mutex_unlock(&gLock);
    return hit;
}

static int is_marked(const QPixmap *pm)
{
    return gScale > 1 && is_marked_serial(serial_of(pm));
}

/* Ist das Malgerät eine QPixmap? (Vergleich des vtable-Zeigers) */
static const QPixmap *as_pixmap(const QPaintDevice *dev)
{
    static void *vtbl;
    if (!vtbl) {
        void *sym = dlsym(qtgui(), "_ZTV7QPixmap");
        vtbl = sym ? (char *)sym + 16 : (void *)-1;
    }
    return dev && *(void *const *)dev == vtbl ? (const QPixmap *)dev : NULL;
}

/* Das Malgerät eines Painters, falls es eine QPixmap ist */
static const QPixmap *painter_pixmap(QPainter *p)
{
    REAL(dev, QPaintDevice *(*)(const QPainter *), "_ZNK8QPainter6deviceEv");
    return as_pixmap(dev(p));
}

__attribute__((constructor)) static void init(void)
{
    const char *s = getenv("GLD_PIXMAP_SCALE");
    if (s)
        gScale = atoi(s) > 0 ? atoi(s) : 1;
}

/* ---------- Anlegen und Kopieren ---------- */

void gld_pixmap_ctor_ii(QPixmap *self, int w, int h) __asm__("__ZN7QPixmapC1Eii") EXPORTED;
void gld_pixmap_ctor_ii(QPixmap *self, int w, int h)
{
    REAL(f, void (*)(QPixmap *, int, int), "_ZN7QPixmapC1Eii");
    if (gScale > 1 && w > 0 && h > 0) {
        f(self, w * gScale, h * gScale);
        mark_serial(serial_of(self));
    } else {
        f(self, w, h);
    }
}

void gld_pixmap_ctor_size(QPixmap *self, const QSize *size) __asm__("__ZN7QPixmapC1ERK5QSize") EXPORTED;
void gld_pixmap_ctor_size(QPixmap *self, const QSize *size)
{
    gld_pixmap_ctor_ii(self, size->w, size->h);
}

/* Kopien: Qt teilt normalerweise die Daten (gleiche Seriennummer). Wird aber
   kopiert, während auf der Quelle gemalt wird, macht Qt eine echte Kopie mit
   neuer Seriennummer – die Markierung muss mit. */
void gld_pixmap_copy_ctor(QPixmap *self, const QPixmap *other) __asm__("__ZN7QPixmapC1ERKS_") EXPORTED;
void gld_pixmap_copy_ctor(QPixmap *self, const QPixmap *other)
{
    REAL(f, void (*)(QPixmap *, const QPixmap *), "_ZN7QPixmapC1ERKS_");
    f(self, other);
    if (is_marked(other))
        mark_serial(serial_of(self));
}

QPixmap *gld_pixmap_assign(QPixmap *self, const QPixmap *other) __asm__("__ZN7QPixmapaSERKS_") EXPORTED;
QPixmap *gld_pixmap_assign(QPixmap *self, const QPixmap *other)
{
    REAL(f, QPixmap *(*)(QPixmap *, const QPixmap *), "_ZN7QPixmapaSERKS_");
    int marked = is_marked(other);
    QPixmap *r = f(self, other);
    if (marked)
        mark_serial(serial_of(self));
    return r;
}

void gld_pixmap_fill(QPixmap *self, const QColor *c) __asm__("__ZN7QPixmap4fillERK6QColor") EXPORTED;
void gld_pixmap_fill(QPixmap *self, const QColor *c)
{
    REAL(f, void (*)(QPixmap *, const QColor *), "_ZN7QPixmap4fillERK6QColor");
    uint32_t before = serial_of(self);
    f(self, c);
    if (is_marked_serial(before))
        mark_serial(serial_of(self));
}

void gld_pixmap_copy(QPixmap *ret, const QPixmap *self, const QRect *r) __asm__("__ZNK7QPixmap4copyERK5QRect") EXPORTED;
void gld_pixmap_copy(QPixmap *ret, const QPixmap *self, const QRect *r)
{
    REAL(f, void (*)(QPixmap *, const QPixmap *, const QRect *), "_ZNK7QPixmap4copyERK5QRect");
    if (!is_marked(self)) {
        f(ret, self, r);
        return;
    }
    QRect big = *r;
    /* Leeres Rechteck heißt bei Qt "alles" */
    if (r->x2 >= r->x1 && r->y2 >= r->y1) {
        int k = gScale;
        big.x1 = r->x1 * k;
        big.y1 = r->y1 * k;
        big.x2 = (r->x2 + 1) * k - 1;
        big.y2 = (r->y2 + 1) * k - 1;
    }
    f(ret, self, &big);
    mark_serial(serial_of(ret));
}

/* ---------- Größe ---------- */

int gld_pixmap_width(const QPixmap *self) __asm__("__ZNK7QPixmap5widthEv") EXPORTED;
int gld_pixmap_width(const QPixmap *self)
{
    int w = real_width(self);
    return is_marked(self) ? w / gScale : w;
}

int gld_pixmap_height(const QPixmap *self) __asm__("__ZNK7QPixmap6heightEv") EXPORTED;
int gld_pixmap_height(const QPixmap *self)
{
    int h = real_height(self);
    return is_marked(self) ? h / gScale : h;
}

QSize gld_pixmap_size(const QPixmap *self) __asm__("__ZNK7QPixmap4sizeEv") EXPORTED;
QSize gld_pixmap_size(const QPixmap *self)
{
    QSize s = {gld_pixmap_width(self), gld_pixmap_height(self)};
    return s;
}

QRect gld_pixmap_rect(const QPixmap *self) __asm__("__ZNK7QPixmap4rectEv") EXPORTED;
QRect gld_pixmap_rect(const QPixmap *self)
{
    QRect r = {.x1 = 0, .y1 = 0, .x2 = gld_pixmap_width(self) - 1, .y2 = gld_pixmap_height(self) - 1};
    return r;
}

/* ---------- Bemalen ---------- */

/* Qt koppelt eine geteilte Pixmap erst beim ersten Malbefehl ab (neue
   Seriennummer), nicht schon beim Anlegen des Painters. Deshalb merkt sich
   jeder Painter auf einer markierten Pixmap die Pixmap; beim Beenden wird
   die dann aktuelle Seriennummer nachmarkiert. */
typedef struct { QPainter *painter; const QPixmap *pm; } ActivePainter;
static ActivePainter gActive[64];

static void remember_painter(QPainter *p, const QPixmap *pm)
{
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < 64; i++)
        if (!gActive[i].painter || gActive[i].painter == p) {
            gActive[i].painter = p;
            gActive[i].pm = pm;
            break;
        }
    pthread_mutex_unlock(&gLock);
}

static const QPixmap *forget_painter(QPainter *p)
{
    const QPixmap *pm = NULL;
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < 64; i++)
        if (gActive[i].painter == p) {
            pm = gActive[i].pm;
            gActive[i].painter = NULL;
            break;
        }
    pthread_mutex_unlock(&gLock);
    return pm;
}

static void start_painting(QPainter *p, const QPixmap *pm, uint32_t before)
{
    REAL(scale, void (*)(QPainter *, double, double), "_ZN8QPainter5scaleEdd");
    if (gScale <= 1 || !is_marked_serial(before))
        return;
    mark_serial(serial_of(pm));
    scale(p, gScale, gScale);
    remember_painter(p, pm);
}

static void finish_painting(QPainter *p)
{
    const QPixmap *pm = forget_painter(p);
    if (pm)
        mark_serial(serial_of(pm));
}

void gld_painter_ctor(QPainter *self, QPaintDevice *dev) __asm__("__ZN8QPainterC1EP12QPaintDevice") EXPORTED;
void gld_painter_ctor(QPainter *self, QPaintDevice *dev)
{
    REAL(f, void (*)(QPainter *, QPaintDevice *), "_ZN8QPainterC1EP12QPaintDevice");
    const QPixmap *pm = as_pixmap(dev);
    uint32_t before = pm ? serial_of(pm) : 0;
    f(self, dev);
    if (pm)
        start_painting(self, pm, before);
}

int gld_painter_begin(QPainter *self, QPaintDevice *dev) __asm__("__ZN8QPainter5beginEP12QPaintDevice") EXPORTED;
int gld_painter_begin(QPainter *self, QPaintDevice *dev)
{
    REAL(f, int (*)(QPainter *, QPaintDevice *), "_ZN8QPainter5beginEP12QPaintDevice");
    const QPixmap *pm = as_pixmap(dev);
    uint32_t before = pm ? serial_of(pm) : 0;
    int ok = f(self, dev) & 1;
    if (ok && pm)
        start_painting(self, pm, before);
    return ok;
}

int gld_painter_end(QPainter *self) __asm__("__ZN8QPainter3endEv") EXPORTED;
int gld_painter_end(QPainter *self)
{
    REAL(f, int (*)(QPainter *), "_ZN8QPainter3endEv");
    int ok = f(self) & 1;
    finish_painting(self);
    return ok;
}

void gld_painter_dtor(QPainter *self) __asm__("__ZN8QPainterD1Ev") EXPORTED;
void gld_painter_dtor(QPainter *self)
{
    REAL(f, void (*)(QPainter *), "_ZN8QPainterD1Ev");
    f(self);
    finish_painting(self);
}

/* ---------- Zeichnen ---------- */

void gld_draw_pixmap_rect(QPainter *self, const QRectF *target, const QPixmap *pm, const QRectF *source) __asm__("__ZN8QPainter10drawPixmapERK6QRectFRK7QPixmapS2_") EXPORTED;
void gld_draw_pixmap_rect(QPainter *self, const QRectF *target, const QPixmap *pm, const QRectF *source)
{
    REAL(f, void (*)(QPainter *, const QRectF *, const QPixmap *, const QRectF *),
         "_ZN8QPainter10drawPixmapERK6QRectFRK7QPixmapS2_");
    if (!is_marked(pm)) {
        f(self, target, pm, source);
        return;
    }
    /* Qts Regeln für fehlende Maße (z. B. aus drawPixmap(x, y, pm, sx, sy, -1, -1))
       hier mit der alten Größe anwenden – Qt selbst würde die echte nehmen:
       Quellbreite <= 0 heißt "bis zum Rand", Zielbreite < 0 heißt "wie die Quelle" */
    double k = gScale;
    double W = (double)real_width(pm) / k, H = (double)real_height(pm) / k;
    QRectF s = *source, t = *target;
    if (s.w <= 0)
        s.w = W - s.x;
    if (s.h <= 0)
        s.h = H - s.y;
    if (t.w < 0)
        t.w = s.w;
    if (t.h < 0)
        t.h = s.h;
    QRectF src = {s.x * k, s.y * k, s.w * k, s.h * k};
    f(self, &t, pm, &src);
}

void gld_draw_pixmap_point(QPainter *self, const QPointF *p, const QPixmap *pm) __asm__("__ZN8QPainter10drawPixmapERK7QPointFRK7QPixmap") EXPORTED;
void gld_draw_pixmap_point(QPainter *self, const QPointF *p, const QPixmap *pm)
{
    REAL(f, void (*)(QPainter *, const QPointF *, const QPixmap *),
         "_ZN8QPainter10drawPixmapERK7QPointFRK7QPixmap");
    if (!is_marked(pm)) {
        f(self, p, pm);
        return;
    }
    int w = real_width(pm), h = real_height(pm);
    /* Quelle in alten Punkten übergeben – gld_draw_pixmap_rect rechnet selbst um */
    QRectF target = {p->x, p->y, (double)w / gScale, (double)h / gScale};
    QRectF src = {0, 0, (double)w / gScale, (double)h / gScale};
    gld_draw_pixmap_rect(self, &target, pm, &src);
}

/* Drag-Cursor in alter Größe übergeben */
void gld_drag_set_cursor(QDrag *self, const QPixmap *pm, int action) __asm__("__ZN5QDrag13setDragCursorERK7QPixmapN2Qt10DropActionE") EXPORTED;
void gld_drag_set_cursor(QDrag *self, const QPixmap *pm, int action)
{
    REAL(f, void (*)(QDrag *, const QPixmap *, int), "_ZN5QDrag13setDragCursorERK7QPixmapN2Qt10DropActionE");
    REAL(scaled, void (*)(QPixmap *, const QPixmap *, const QSize *, int, int),
         "_ZNK7QPixmap6scaledERK5QSizeN2Qt15AspectRatioModeENS3_18TransformationModeE");
    if (!is_marked(pm)) {
        f(self, pm, action);
        return;
    }
    PixmapStorage tmp;
    QSize s = {real_width(pm) / gScale, real_height(pm) / gScale};
    scaled((QPixmap *)&tmp, pm, &s, 0 /* IgnoreAspectRatio */, 1 /* SmoothTransformation */);
    f(self, (QPixmap *)&tmp, action);
    real_pixmap_dtor((QPixmap *)&tmp);
}

/* ---------- Text einpassen ----------
 * Die Oberfläche wurde für die Schriftbreiten unter Windows gebaut; auf dem
 * Mac ist Arial etwas breiter, und manche Beschriftungen ("Compressor",
 * "DCA", "Custom 1") werden abgeschnitten. Passt ein Text nicht in sein
 * Rechteck, wird die Schrift nur für diesen Aufruf verkleinert (höchstens
 * auf 70 %) und danach wiederhergestellt.
 * Bei Text mit Umbruch (Qt::TextWordWrap) zählt zusätzlich die Höhe: ein
 * einzelnes Wort wie "DCA" kann nicht umbrechen und ragt sonst über den Rand. */

#define TEXT_WORD_WRAP 0x1000
#define ALIGN_HCENTER 0x4
#define MIN_TEXT_SCALE 0.7

/* Wie weit der Text über sein Feld hinausragt: > 1 heißt "passt nicht".
   Breite immer, Höhe nur bei Text mit Umbruch. */
static double overflow(QPainter *p, const QRect *r, int flags, const void *text, double avail)
{
    REAL(metrics, void (*)(SmallStorage *, const QPainter *), "_ZNK8QPainter11fontMetricsEv");
    REAL(bounds, QRect (*)(const SmallStorage *, const QRect *, int, const void *, int, int *),
         "_ZNK12QFontMetrics12boundingRectERK5QRectiRK7QStringiPi");
    REAL(metrics_dtor, void (*)(SmallStorage *), "_ZN12QFontMetricsD1Ev");
    SmallStorage m;
    metrics(&m, p);
    QRect b = bounds(&m, r, flags, text, 0, NULL);
    metrics_dtor(&m);
    double w = b.x2 - b.x1 + 1, h = b.y2 - b.y1 + 1, rh = r->y2 - r->y1 + 1;
    double ratio = w / avail;
    if ((flags & TEXT_WORD_WRAP) && rh > 0 && h / rh > ratio)
        ratio = h / rh;
    return ratio;
}

static void set_font(QPainter *p, const SmallStorage *font)
{
    REAL(f, void (*)(QPainter *, const SmallStorage *), "_ZN8QPainter7setFontERK5QFont");
    f(p, font);
}

static void font_dtor(SmallStorage *font)
{
    REAL(f, void (*)(SmallStorage *), "_ZN5QFontD1Ev");
    f(font);
}

/* Verkleinert bei Bedarf die Schrift; gibt 1 zurück, wenn *saved die
   ursprüngliche Schrift enthält und wiederhergestellt werden muss */
static int fit_text(QPainter *p, double avail, const QRect *r, int flags, const void *text, SmallStorage *saved)
{
    static int disabled = -1;
    if (disabled < 0)
        disabled = getenv("GLD_NO_TEXTFIT") != NULL;
    if (disabled || avail <= 0)
        return 0;

    /* Malt die App in eine Pixmap, schneidet auch deren Rand ab – oft ist das
       Rechteck breiter als die Pixmap. Sichtbar ist nur die Schnittmenge; bei
       zentriertem Text zählt der kleinere Abstand zur Mitte doppelt. */
    const QPixmap *pm = painter_pixmap(p);
    if (pm) {
        double lo = r->x1 > 0 ? r->x1 : 0;
        double hi = r->x2 + 1;
        double dev = gld_pixmap_width(pm);
        if (hi > dev)
            hi = dev;
        double visible = hi - lo;
        if (flags & ALIGN_HCENTER) {
            double c = (r->x1 + r->x2 + 1) / 2.0;
            double half = c - lo < hi - c ? c - lo : hi - c;
            visible = 2 * half;
        }
        if (visible < avail)
            avail = visible;
        if (avail <= 0)
            return 0;
    }
    /* Etwas Luft zum Rahmen, sonst berührt z. B. "Custom 1" die Kante;
       kleine Felder wie die Mute-Taster bekommen weniger, damit sie nicht schrumpfen */
    if (avail >= 40)
        avail -= 4;
    else if (avail > 20)
        avail -= 2;
    double over = overflow(p, r, flags, text, avail);
    if (over <= 1.0 + 0.5 / avail)
        return 0;

    REAL(font, const SmallStorage *(*)(const QPainter *), "_ZNK8QPainter4fontEv");
    REAL(font_copy, void (*)(SmallStorage *, const SmallStorage *), "_ZN5QFontC1ERKS_");
    REAL(point_size, double (*)(const SmallStorage *), "_ZNK5QFont10pointSizeFEv");
    REAL(pixel_size, int (*)(const SmallStorage *), "_ZNK5QFont9pixelSizeEv");
    REAL(set_point_size, void (*)(SmallStorage *, double), "_ZN5QFont13setPointSizeFEd");

    font_copy(saved, font(p));
    double size = point_size(saved);
    if (size <= 0)
        size = pixel_size(saved); /* auf dem Mac: 72 dpi, Punkt = Pixel */
    if (size <= 0) {
        font_dtor(saved);
        return 0;
    }

    SmallStorage f;
    font_copy(&f, saved);
    double scale = 1.0 / over;
    for (int i = 0; i < 4; i++) {
        double sc = scale * 0.98;
        if (sc < MIN_TEXT_SCALE)
            sc = MIN_TEXT_SCALE;
        set_point_size(&f, size * sc);
        set_font(p, &f);
        over = overflow(p, r, flags, text, avail);
        if (over <= 1.0 + 0.5 / avail || sc <= MIN_TEXT_SCALE)
            break;
        scale = sc / over;
    }
    font_dtor(&f);
    return 1;
}

static void restore_font(QPainter *p, SmallStorage *saved)
{
    set_font(p, saved);
    font_dtor(saved);
}

void gld_draw_text_rect(QPainter *self, const QRect *r, int flags, const void *text, QRect *br)
    __asm__("__ZN8QPainter8drawTextERK5QRectiRK7QStringPS0_") EXPORTED;
void gld_draw_text_rect(QPainter *self, const QRect *r, int flags, const void *text, QRect *br)
{
    REAL(f, void (*)(QPainter *, const QRect *, int, const void *, QRect *),
         "_ZN8QPainter8drawTextERK5QRectiRK7QStringPS0_");
    SmallStorage saved;
    int fitted = fit_text(self, r->x2 - r->x1 + 1, r, flags, text, &saved);
    f(self, r, flags, text, br);
    if (fitted)
        restore_font(self, &saved);
}

void gld_draw_text_rectf(QPainter *self, const QRectF *r, int flags, const void *text, QRectF *br)
    __asm__("__ZN8QPainter8drawTextERK6QRectFiRK7QStringPS0_") EXPORTED;
void gld_draw_text_rectf(QPainter *self, const QRectF *r, int flags, const void *text, QRectF *br)
{
    REAL(f, void (*)(QPainter *, const QRectF *, int, const void *, QRectF *),
         "_ZN8QPainter8drawTextERK6QRectFiRK7QStringPS0_");
    QRect ir = {.x1 = (int)r->x, .y1 = (int)r->y, .x2 = (int)(r->x + r->w) - 1, .y2 = (int)(r->y + r->h) - 1};
    SmallStorage saved;
    int fitted = fit_text(self, r->w, &ir, flags, text, &saved);
    f(self, r, flags, text, br);
    if (fitted)
        restore_font(self, &saved);
}
