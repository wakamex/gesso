// macOS: the User Notifications framework. It serves app bundles only (a bare executable has no
// identity to file notifications under), asks the user's leave once, and reports a click to a
// delegate on the main thread.
#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include "gs_notify.h"

static void (*on_clicked)(const char *tag);
static bool ready;

@interface GSNotifyDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation GSNotifyDelegate
// A click on a notification, or its default action from the Notification Centre.
- (void)userNotificationCenter:(UNUserNotificationCenter *)center didReceiveNotificationResponse:(UNNotificationResponse *)response withCompletionHandler:(void (^)(void))done {
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier] && on_clicked) on_clicked(response.notification.request.identifier.UTF8String);
    done();
}
// Shown even while the app is in front, which the system would otherwise keep quiet.
- (void)userNotificationCenter:(UNUserNotificationCenter *)center willPresentNotification:(UNNotification *)notification withCompletionHandler:(void (^)(UNNotificationPresentationOptions))done {
    done(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}
@end

static GSNotifyDelegate *delegate;

bool gs_notify_init(const char *name, const char *id, const char *icon, void (*clicked)(const char *tag)) {
    if (ready) return true;
    @autoreleasepool {
        // (the name, identifier and icon are the bundle's own here)
        if (!NSBundle.mainBundle.bundleIdentifier) return false;
        on_clicked = clicked;
        UNUserNotificationCenter *center = UNUserNotificationCenter.currentNotificationCenter;
        delegate = [[GSNotifyDelegate alloc] init];
        center.delegate = delegate;
        [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound completionHandler:^(BOOL granted, NSError *error) {
            if (granted) return;
            [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {  // (0 not asked yet, 1 denied, 2 allowed)
                NSLog(@"gs_notify: notifications are not allowed for this app (authorization status %ld)%@%@", (long)settings.authorizationStatus, error ? @": " : @"", error ? error.localizedDescription : @"");
            }];
        }];
        ready = true;
    }
    return true;
}

bool gs_notify_show(const char *tag, const char *title, const char *body) {
    if (!ready || !tag || !*tag) return false;
    @autoreleasepool {
        UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
        content.title = [NSString stringWithUTF8String:title] ?: @"";
        content.body = [NSString stringWithUTF8String:body ? body : ""] ?: @"";
        // A request with the identifier of an earlier one takes its place.
        UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:[NSString stringWithUTF8String:tag] ?: @"" content:content trigger:nil];
        [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:request withCompletionHandler:^(NSError *error) {
            if (error) NSLog(@"gs_notify: %@", error.localizedDescription);
        }];
    }
    return true;  // (handed over; whether it shows is the user's setting)
}

void gs_notify_close(const char *tag) {
    if (!ready || !tag) return;
    @autoreleasepool {
        NSString *identifier = [NSString stringWithUTF8String:tag];
        if (identifier) [UNUserNotificationCenter.currentNotificationCenter removeDeliveredNotificationsWithIdentifiers:@[ identifier ]];
    }
}
