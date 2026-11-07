// Compiles Metal Shading Language files with the system's compiler, the way the game will, on all cores, and sorts the failures: check_msl list-of-files
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *list = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[1]] encoding:NSUTF8StringEncoding error:nil];
        NSArray<NSString *> *files = [[list componentsSeparatedByString:@"\n"] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"length > 0"]];
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        MTLCompileOptions *opt = [MTLCompileOptions new];
        opt.preserveInvariance = YES;
        NSMutableDictionary<NSString *, NSNumber *> *why = [NSMutableDictionary dictionary];
        NSMutableDictionary<NSString *, NSString *> *example = [NSMutableDictionary dictionary];
        __block long fine = 0, failed = 0;
        NSObject *lock = [NSObject new];
        double t0 = [NSDate timeIntervalSinceReferenceDate];
        dispatch_apply(files.count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
            @autoreleasepool {
                NSString *src = [NSString stringWithContentsOfFile:files[i] encoding:NSUTF8StringEncoding error:nil];
                NSError *e = nil;
                id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opt error:&e];
                @synchronized(lock) {
                    if (lib) {
                        fine++;
                    } else {
                        failed++;
                        NSString *text = e.localizedDescription;
                        NSRange r = [text rangeOfString:@"error: "];
                        NSString *line = r.location == NSNotFound ? text : [text substringFromIndex:r.location];
                        line = [line componentsSeparatedByString:@"\n"].firstObject;
                        if (line.length > 100) line = [line substringToIndex:100];
                        why[line] = @(why[line].longValue + 1);
                        if (!example[line]) example[line] = [NSString stringWithFormat:@"%@\n%@", files[i].lastPathComponent, [text substringToIndex:MIN(text.length, 500u)]];
                    }
                }
            }
        });
        printf("compiled %ld: %ld fine, %ld failed, %.0f s\n", fine + failed, fine, failed, [NSDate timeIntervalSinceReferenceDate] - t0);
        for (NSString *k in [why keysSortedByValueUsingComparator:^(NSNumber *a, NSNumber *b) { return [b compare:a]; }]) {
            printf("%6ld %s\n       %s\n", why[k].longValue, k.UTF8String, example[k].UTF8String);
        }
    }
    return 0;
}
