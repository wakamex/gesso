// macOS: the pasteboard's items copied whole, every type of each, and written back as they were.
#import <AppKit/AppKit.h>
#include "gs_desk.h"

struct gs_clip { void *items; };  // an NSArray of NSDictionary, type to data, retained

void gs_clipboard_free(gs_clip *s) {
    if (!s) return;
    if (s->items) CFRelease(s->items);
    free(s);
}

gs_clip *gs_clipboard_snapshot(void) {
    @autoreleasepool {
        NSMutableArray *items = [NSMutableArray array];
        for (NSPasteboardItem *item in NSPasteboard.generalPasteboard.pasteboardItems) {
            NSMutableDictionary *copy = [NSMutableDictionary dictionary];
            for (NSPasteboardType type in item.types) {
                NSData *data = [item dataForType:type];
                if (data.length) copy[type] = data;
            }
            if (copy.count) [items addObject:copy];
        }
        if (!items.count) return NULL;
        gs_clip *s = calloc(1, sizeof *s);
        s->items = (void *)CFBridgingRetain(items);
        return s;
    }
}

bool gs_clipboard_restore(gs_clip *s) {
    bool ok;
    @autoreleasepool {
        NSArray *items = (__bridge NSArray *)s->items;
        NSMutableArray *objects = [NSMutableArray array];
        for (NSDictionary *copy in items) {
            NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
            for (NSPasteboardType type in copy) [item setData:copy[type] forType:type];
            [objects addObject:item];
        }
        [NSPasteboard.generalPasteboard clearContents];
        ok = [NSPasteboard.generalPasteboard writeObjects:objects];
    }
    gs_clipboard_free(s);
    return ok;
}
