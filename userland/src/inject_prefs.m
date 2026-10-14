/*
 * libredwood-prefs.dylib: code of ours inside the Settings app (its process is "Preferences"; see
 * inject.c for how a library of ours gets into a stock program). It does what PreferenceLoader
 * does for tweaks on a jailbroken phone, and in the same way, so that a pane of ours is added the
 * way a tweak adds one:
 *
 *   - an entry is a property list with a dictionary "entry" in
 *     /redwood/Library/PreferenceLoader/Preferences/ (and in /Library/PreferenceLoader/Preferences/,
 *     where a jailbreak keeps them), as tweaks ship them: label, cell (PSLinkCell), icon, bundle,
 *     isController, detail;
 *   - the bundle it names is a preference bundle in /redwood/Library/PreferenceBundles/ (then
 *     /Library/PreferenceBundles/, then /System/Library/PreferenceBundles/), loaded only when the
 *     entry is tapped;
 *   - the entries, sorted by their labels, are one group of their own in Settings' main list, put
 *     where PreferenceLoader puts it: after Apple's own settings, at the end of the list as it is
 *     first built (the App Store applications' group, which comes later, goes below it).
 *
 * After rpetrich's and DHowett's PreferenceLoader (Tweak.xm, prefs.xm), without Substrate: the
 * root list controller's -specifiers is replaced with the runtime's own calls. The first pane of
 * ours this way is HackiOS's (src/hackios_pane.m).
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <objc/runtime.h>

/* ---- Preferences.framework, as much of it as is used (a private framework: no headers) ---- */

@interface PSSpecifier : NSObject
+ (instancetype)groupSpecifierWithName:(NSString *)name;
- (void)setProperty:(id)value forKey:(NSString *)key;
@property (nonatomic, retain) NSString *name;
@property (nonatomic, retain) NSString *identifier;
@end

@interface PSListController : UIViewController
@end

extern NSArray *SpecifiersFromPlist(NSDictionary *plist, PSSpecifier *previous, id target, NSString *plistName, NSBundle *bundle,
    NSString **title, NSString **specifierID, PSListController *callerList, NSMutableArray **bundleControllers);
extern NSString *PSLazilyLoadedBundleKey;

static NSString *const entry_directories[] = { @"/redwood/Library/PreferenceLoader/Preferences", @"/Library/PreferenceLoader/Preferences" };
static NSString *const bundle_directories[] = { @"/redwood/Library/PreferenceBundles", @"/Library/PreferenceBundles",
	                                        @"/System/Library/PreferenceBundles" };

/* An entry's specifiers (PreferenceLoader's -specifiersFromEntry:...), or nil. */
static NSArray *
entry_specifiers(PSListController *root, NSDictionary *entry, NSString *title)
{
	NSString *name = entry[@"bundle"], *path = nil;
	NSMutableArray *controllers = nil;
	NSArray *specifiers;
	NSBundle *bundle;

	if (![name isKindOfClass:NSString.class]) {
		NSLog(@"HackiOS: the entry %@ names no bundle: left out", title);
		return nil;
	}
	for (unsigned i = 0; i < sizeof(bundle_directories) / sizeof(bundle_directories[0]) && path == nil; i++) {
		NSString *candidate = [NSString stringWithFormat:@"%@/%@.bundle", bundle_directories[i], name];

		path = [NSFileManager.defaultManager fileExistsAtPath:candidate] ? candidate : nil;
	}
	if (path == nil || (bundle = [NSBundle bundleWithPath:path]) == nil) {
		NSLog(@"HackiOS: the entry %@: no bundle %@ in any of the places for them: left out", title, name);
		return nil;
	}
	specifiers = SpecifiersFromPlist(@{ @"items": @[ entry ] }, nil, root, title, bundle, NULL, NULL, root, &controllers);
	if ([entry[@"isController"] boolValue]) {
		for (PSSpecifier *specifier in specifiers) {
			[specifier setProperty:path forKey:PSLazilyLoadedBundleKey];        /* (the bundle is loaded when tapped) */
			if (specifier.name == nil) {
				specifier.name = title;
			}
		}
	}
	return specifiers.count ? specifiers : nil;
}

static NSArray *(*root_specifiers)(id self, SEL _cmd);
static Ivar specifiers_ivar;

static NSArray *
our_root_specifiers(id self, SEL _cmd)
{
	bool first = object_getIvar(self, specifiers_ivar) == nil;
	NSArray *list = root_specifiers(self, _cmd);
	NSMutableArray *loaded = [NSMutableArray array], *mutable;
	NSUInteger at;

	if (!first || ![list isKindOfClass:NSMutableArray.class]) {
		return list;
	}
	for (unsigned d = 0; d < sizeof(entry_directories) / sizeof(entry_directories[0]); d++) {
		NSArray *files = [NSFileManager.defaultManager subpathsOfDirectoryAtPath:entry_directories[d] error:NULL];

		for (NSString *file in [files sortedArrayUsingSelector:@selector(compare:)]) {
			NSDictionary *plist = [file.pathExtension isEqualToString:@"plist"] ? [NSDictionary dictionaryWithContentsOfFile:
			    [entry_directories[d] stringByAppendingPathComponent:file]] : nil;
			NSArray *specifiers = [plist[@"entry"] isKindOfClass:NSDictionary.class] ? entry_specifiers(self, plist[@"entry"],
			    file.lastPathComponent.stringByDeletingPathExtension) : nil;

			if (specifiers != nil) {
				[loaded addObjectsFromArray:specifiers];
			}
		}
	}
	if (loaded.count == 0) {
		return list;
	}
	[loaded sortUsingComparator:^NSComparisonResult(PSSpecifier *a, PSSpecifier *b) {
		return [a.name localizedCaseInsensitiveCompare:b.name];
	}];
	[loaded insertObject:[PSSpecifier groupSpecifierWithName:nil] atIndex:0];
	/* (PreferenceLoader puts the group after the one that holds TWITTER, where there is one: iOS 6 to 10; else at the end) */
	mutable = (NSMutableArray *)list;
	at = mutable.count;
	for (NSUInteger i = 0; i < mutable.count; i++) {
		if ([[mutable[i] identifier] isEqualToString:@"TWITTER"]) {
			for (at = i + 1; at < mutable.count && ![[mutable[at] valueForKey:@"cellType"] isEqual:@0]; at++) {
			}
			break;
		}
	}
	[mutable insertObjects:loaded atIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(at, loaded.count)]];
	return list;
}

__attribute__((constructor))
static void
start(void)
{
	Class root = objc_getClass("PSUIPrefsListController"), list = objc_getClass("PSListController");
	Method method = root != Nil ? class_getInstanceMethod(root, @selector(specifiers)) : NULL;

	specifiers_ivar = list != Nil ? class_getInstanceVariable(list, "_specifiers") : NULL;
	if (method == NULL || specifiers_ivar == NULL) {
		NSLog(@"HackiOS: no PSUIPrefsListController -specifiers here: Settings' main list is left as it is");
		return;
	}
	/* (the root's own method if it has one; else one of its own is added: PSListController's stays as it is for every other pane) */
	root_specifiers = (NSArray *(*)(id, SEL))method_getImplementation(method);
	if (!class_addMethod(root, @selector(specifiers), (IMP)our_root_specifiers, method_getTypeEncoding(method))) {
		method_setImplementation(method, (IMP)our_root_specifiers);
	}
}
