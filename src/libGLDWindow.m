/*
 * Vollbild und freies Skalieren für die Fenster des GLD Editor.
 *
 * Die Pultoberfläche ("... Editor Surface") und die Touchscreen-Ansicht
 * ("GLD-... Editor") haben eine feste Pixelgröße (setFixedSize), Qt 4.8 kann
 * sie nicht skalieren. Diese Erweiterung macht die Fenster vergrößerbar
 * (festes Seitenverhältnis) und vollbildfähig und lässt AppKit den Inhalt
 * skalieren: Die Qt-Ansicht bekommt einen größeren Frame, behält aber ihre
 * ursprünglichen Bounds. AppKit skaliert dadurch das Zeichnen und rechnet
 * Mauspositionen (convertPoint:fromView:) automatisch zurück.
 *
 * Qt darf von der Größenänderung nichts erfahren, sonst passt es das Widget
 * an die neue Größe an. Deshalb wird -[QCocoaWindowDelegate windowDidResize:]
 * für diese Fenster übersprungen.
 *
 * Wird über libATSCompat geladen (siehe patch-gld-editor.sh).
 */

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

/* Übernommene Fenster mit ihrer ursprünglichen Inhaltsgröße in Qt-Punkten */
typedef struct {
    __unsafe_unretained NSWindow *window;
    NSSize base;
} Scaled;

#define MAX_WINDOWS 8
static Scaled gScaled[MAX_WINDOWS];
static int gCount;

static IMP gQtDidResize;    /* originales -[QCocoaWindowDelegate windowDidResize:] */
static IMP gQtWillResize;   /* originales -[QCocoaWindowDelegate windowWillResize:toSize:] */
static IMP gQtDrawRect;     /* originales -[QCocoaView drawRect:] */
static IMP gQtNeedsDisplay; /* originales -[QCocoaView setNeedsDisplayInRect:] */
static IMP gWinZoom;        /* originales -[QCocoaWindow zoom:] (geerbt von NSWindow) */
static IMP gQtShouldZoom;   /* originales -[QCocoaWindowDelegate windowShouldZoom:toFrame:] */

static Scaled *scaled_for_window(NSWindow *w)
{
    for (int i = 0; i < gCount; i++)
        if (gScaled[i].window == w)
            return &gScaled[i];
    return NULL;
}

static Scaled *scaled_for_view(NSView *v)
{
    for (int i = 0; i < gCount; i++)
        if (gScaled[i].window.contentView == v)
            return &gScaled[i];
    return NULL;
}

/* Qt deaktiviert den grünen Knopf bei Fenstern mit fester Größe */
static void enable_zoom_button(NSWindow *w)
{
    NSButton *z = [w standardWindowButton:NSWindowZoomButton];
    if (z && !z.enabled)
        z.enabled = YES;
}

/* Gleichmäßig skalieren und zentrieren: Die Bounds sind so groß, dass die
   Basisgröße mit dem Faktor s in den Frame passt, und um den Rand verschoben. */
static void apply_scale(Scaled *e)
{
    NSView *view = e->window.contentView;
    if (!view)
        return;
    enable_zoom_button(e->window);
    NSSize f = view.frame.size;
    CGFloat s = MIN(f.width / e->base.width, f.height / e->base.height);
    if (s <= 0)
        return;
    NSSize b = NSMakeSize(f.width / s, f.height / s);
    NSPoint o = NSMakePoint(-(b.width - e->base.width) / 2, -(b.height - e->base.height) / 2);
    if (NSEqualSizes(view.bounds.size, b) && NSEqualPoints(view.bounds.origin, o))
        return;
    [view setBoundsSize:b];
    [view setBoundsOrigin:o];
    [view setNeedsDisplay:YES];
}

static void gld_windowDidResize(id self, SEL _cmd, NSNotification *note)
{
    Scaled *e = scaled_for_window(note.object);
    if (e) {
        apply_scale(e);
        return;
    }
    ((void (*)(id, SEL, NSNotification *))gQtDidResize)(self, _cmd, note);
}

/* Qt zwingt das Fenster hier auf die feste Widget-Größe zurück */
static NSSize gld_windowWillResize(id self, SEL _cmd, NSWindow *window, NSSize size)
{
    if (scaled_for_window(window))
        return size;
    return ((NSSize (*)(id, SEL, NSWindow *, NSSize))gQtWillResize)(self, _cmd, window, size);
}

/* Ränder außerhalb der Qt-Fläche schwarz füllen */
static void gld_drawRect(NSView *self, SEL _cmd, NSRect dirty)
{
    ((void (*)(id, SEL, NSRect))gQtDrawRect)(self, _cmd, dirty);
    Scaled *e = scaled_for_view(self);
    if (!e)
        return;
    NSRect all = self.bounds;
    NSRect qt = NSMakeRect(0, 0, e->base.width, e->base.height);
    [NSColor.blackColor set];
    if (NSMinX(qt) > NSMinX(all))
        NSRectFill(NSMakeRect(NSMinX(all), NSMinY(all), NSMinX(qt) - NSMinX(all), NSHeight(all)));
    if (NSMaxX(all) > NSMaxX(qt))
        NSRectFill(NSMakeRect(NSMaxX(qt), NSMinY(all), NSMaxX(all) - NSMaxX(qt), NSHeight(all)));
    if (NSMinY(qt) > NSMinY(all))
        NSRectFill(NSMakeRect(NSMinX(all), NSMinY(all), NSWidth(all), NSMinY(qt) - NSMinY(all)));
    if (NSMaxY(all) > NSMaxY(qt))
        NSRectFill(NSMakeRect(NSMinX(all), NSMaxY(qt), NSWidth(all), NSMaxY(all) - NSMaxY(qt)));
}

/* Bei krummem Skalierungsfaktor liegen Widget-Kanten zwischen zwei
   Bildschirmpixeln; beim Neuzeichnen kleiner Bereiche bleiben dort Striche
   mit altem Inhalt stehen. Skaliert wird deshalb immer alles neu gezeichnet
   (AppKit fasst die Aufrufe zu einem Zeichenvorgang pro Bild zusammen). */
static void gld_setNeedsDisplayInRect(NSView *self, SEL _cmd, NSRect r)
{
    if (scaled_for_view(self) && !NSEqualSizes(self.bounds.size, self.frame.size))
        r = self.bounds;
    ((void (*)(id, SEL, NSRect))gQtNeedsDisplay)(self, _cmd, r);
}

/* Grüner Knopf: Qt macht daraus "Zoomen", das bei fester Größe nichts tut.
   Für übernommene Fenster stattdessen Vollbild umschalten. */
static void gld_zoom(NSWindow *self, SEL _cmd, id sender)
{
    if (scaled_for_window(self)) {
        [self toggleFullScreen:sender];
        return;
    }
    ((void (*)(id, SEL, id))gWinZoom)(self, _cmd, sender);
}

static BOOL gld_windowShouldZoom(id self, SEL _cmd, NSWindow *window, NSRect frame)
{
    if (scaled_for_window(window)) {
        dispatch_async(dispatch_get_main_queue(), ^{ [window toggleFullScreen:nil]; });
        return NO;
    }
    return ((BOOL (*)(id, SEL, NSWindow *, NSRect))gQtShouldZoom)(self, _cmd, window, frame);
}

/* Ersetzt eine Methode nur in dieser Klasse. Erbt die Klasse die Methode
   (z. B. setNeedsDisplayInRect: von NSView), wird sie hier hinzugefügt statt
   in der Oberklasse ersetzt – sonst wären alle Ansichten betroffen. */
static IMP swap(const char *cls, SEL sel, IMP imp)
{
    Class c = objc_getClass(cls);
    Method m = c ? class_getInstanceMethod(c, sel) : NULL;
    if (!m)
        return NULL;
    IMP old = method_getImplementation(m);
    if (class_addMethod(c, sel, imp, method_getTypeEncoding(m)))
        return old;
    return method_setImplementation(m, imp);
}

static void hook_qt(void)
{
    if (gQtDidResize)
        return;
    gQtDidResize = swap("QCocoaWindowDelegate", @selector(windowDidResize:), (IMP)gld_windowDidResize);
    gQtWillResize = swap("QCocoaWindowDelegate", @selector(windowWillResize:toSize:), (IMP)gld_windowWillResize);
    gQtDrawRect = swap("QCocoaView", @selector(drawRect:), (IMP)gld_drawRect);
    gQtNeedsDisplay = swap("QCocoaView", @selector(setNeedsDisplayInRect:), (IMP)gld_setNeedsDisplayInRect);
    gWinZoom = swap("QCocoaWindow", @selector(zoom:), (IMP)gld_zoom);
    gQtShouldZoom = swap("QCocoaWindowDelegate", @selector(windowShouldZoom:toFrame:), (IMP)gld_windowShouldZoom);
}

/* Pultoberfläche "GLD-112 Editor Surface" und Touchscreen "GLD-112 Editor" */
static BOOL is_editor_window(NSWindow *w)
{
    NSString *t = w.title;
    return [t hasPrefix:@"GLD"] && ([t hasSuffix:@" Editor Surface"] || [t hasSuffix:@" Editor"]);
}

static void adopt_window(NSWindow *w)
{
    if (gCount >= MAX_WINDOWS || scaled_for_window(w) || !is_editor_window(w))
        return;
    NSView *view = w.contentView;
    if (!view || view.frame.size.width < 100)
        return;

    hook_qt();
    if (!gQtDidResize || !gQtWillResize || !gQtDrawRect || !gQtNeedsDisplay)
        return; /* Ohne Hooks würde Qt gegen die Skalierung arbeiten */

    Scaled *e = &gScaled[gCount++];
    e->window = w;
    e->base = view.frame.size;

    w.styleMask |= NSWindowStyleMaskResizable;
    w.collectionBehavior = (w.collectionBehavior & ~NSWindowCollectionBehaviorFullScreenAuxiliary)
                           | NSWindowCollectionBehaviorFullScreenPrimary;
    w.contentMinSize = e->base;
    w.contentMaxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    w.minSize = w.frame.size;
    w.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    w.contentAspectRatio = e->base;
    w.backgroundColor = NSColor.blackColor;
    enable_zoom_button(w);

    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:NSWindowDidBecomeKeyNotification object:w queue:nil
                usingBlock:^(NSNotification *n) { enable_zoom_button(n.object); }];
    for (NSNotificationName name in @[ NSWindowDidResizeNotification, NSWindowDidEnterFullScreenNotification,
                                       NSWindowDidExitFullScreenNotification ])
        [nc addObserverForName:name object:w queue:nil usingBlock:^(NSNotification *n) {
            Scaled *s = scaled_for_window(n.object);
            if (s)
                apply_scale(s);
        }];
}

__attribute__((constructor)) static void gld_window_init(void)
{
    if (getenv("GLD_NO_FULLSCREEN"))
        return;

    /* Qt 4 soll direkt über CoreGraphics zeichnen statt in eine Pixelgrafik:
       Text und Linien bleiben dann bei jeder Skalierung und auf Retina scharf.
       Wird vor QApplication gelesen; GLD_RASTER=1 behält den alten Modus. */
    if (!getenv("GLD_RASTER"))
        setenv("QT_GRAPHICSSYSTEM", "native", 0);

    /* ⌃⌘F schaltet Vollbild für das aktive Fenster um – Qt verschluckt die
       Systemkombination */
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *e) {
            NSEventModifierFlags mods = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
            BOOL ctrlCmdF = (mods & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) ==
                                (NSEventModifierFlagControl | NSEventModifierFlagCommand) &&
                            [e.charactersIgnoringModifiers.lowercaseString isEqualToString:@"f"];
            BOOL fnF = (mods & NSEventModifierFlagFunction) &&
                       [e.charactersIgnoringModifiers.lowercaseString isEqualToString:@"f"];
            if (!(ctrlCmdF || fnF) || !gCount)
                return e;
            NSWindow *target = scaled_for_window(NSApp.keyWindow) ? NSApp.keyWindow : gScaled[0].window;
            [target toggleFullScreen:nil];
            return nil;
        }];
    });

    void (^check)(NSNotification *) = ^(NSNotification *n) {
        if ([n.object isKindOfClass:[NSWindow class]])
            adopt_window(n.object);
    };
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:NSWindowDidBecomeMainNotification object:nil queue:nil usingBlock:check];
    [nc addObserverForName:NSWindowDidBecomeKeyNotification object:nil queue:nil usingBlock:check];
    [nc addObserverForName:NSWindowDidUpdateNotification object:nil queue:nil usingBlock:check];
}
