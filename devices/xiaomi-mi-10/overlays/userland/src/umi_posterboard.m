#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSURL *(*original_base_store_url)(id, SEL);

static NSURL *posterboard_base_store_url(id service, SEL selector)
{
    NSURL *base = original_base_store_url(service, selector);
    NSString *path = base.path;

    if (!base.isFileURL || (![path isEqualToString:@"/var/mobile/Library/Application Support"] &&
                            ![path isEqualToString:@"/private/var/mobile/Library/Application Support"])) {
        return base;
    }

    // Without an app container the stock URL is shared by system services.
    // Scope PosterBoard's store reset to its own directory.
    NSURL *store = [base URLByAppendingPathComponent:@"com.apple.PosterBoard" isDirectory:YES];
    NSError *error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:store
                               withIntermediateDirectories:NO
                                                attributes:@{NSFilePosixPermissions: @0700}
                                                     error:&error] &&
        (error.code != NSFileWriteFileExistsError || ![error.domain isEqualToString:NSCocoaErrorDomain])) {
        fprintf(stderr, "UMI PosterBoard: could not create isolated store (error %ld)\n", (long)error.code);
    }
    return store;
}

__attribute__((constructor))
static void isolate_posterboard_store(void)
{
    if (strcmp(getprogname(), "PosterBoard") != 0) {
        return;
    }

    Class glue = objc_getClass("PBFPosterExtensionDataStoreXPCServiceGlue");
    SEL selector = sel_registerName("_baseDataStoreURL");
    Method method = class_getInstanceMethod(glue, selector);
    if (method == NULL || strcmp(method_getTypeEncoding(method), "@16@0:8") != 0) {
        fprintf(stderr, "UMI PosterBoard: store URL method unavailable\n");
        return;
    }

    original_base_store_url = (NSURL *(*)(id, SEL))method_getImplementation(method);
    method_setImplementation(method, (IMP)posterboard_base_store_url);
    fprintf(stderr, "UMI PosterBoard: store isolation installed\n");
}
