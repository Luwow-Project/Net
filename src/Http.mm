#import <Foundation/Foundation.h>

#include "Http.h"

#include <cstdio>

@interface LuwowHttpReceiver : NSObject <NSURLSessionDataDelegate>
@property(nonatomic) dispatch_semaphore_t done;
@property(nonatomic, strong) NSURLResponse* answer;
@property(nonatomic, strong) NSError* failure;
@property(nonatomic, strong) NSMutableData* body;
@property(nonatomic) FILE* file;
@property(nonatomic) BOOL fileFailed;
@property(nonatomic) unsigned long long received;
@property(nonatomic) const char* saveTo;
@end

@implementation LuwowHttpReceiver
- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    self.answer = response;
    NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse*)response).statusCode : 0;
    if (self.saveTo && status >= 200 && status < 300) {
        self.file = fopen(self.saveTo, "wb");
        if (!self.file) self.fileFailed = YES;
    }
    completionHandler(self.fileFailed ? NSURLSessionResponseCancel : NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
    self.received += data.length;
    if (self.file) {
        [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
            if (fwrite(bytes, 1, range.length, self.file) != range.length) {
                self.fileFailed = YES;
                *stop = YES;
            }
        }];
        if (self.fileFailed) [dataTask cancel];
    } else {
        [self.body appendData:data];
    }
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    self.failure = error;
    if (self.file) {
        fclose(self.file);
        self.file = nullptr;
    }
    dispatch_semaphore_signal(self.done);
}
@end

namespace Luwow::Net {
    // NSURLSession answers on its own queue, the calling thread waits for it
    bool sendHttpRequest(const HttpRequest& request, HttpResponse& response, std::string& error) {
        @autoreleasepool {
            NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:request.url.c_str()]];
            if (!url || !url.scheme || !([url.scheme isEqualToString:@"http"] || [url.scheme isEqualToString:@"https"])) {
                error = "only valid http and https URLs are supported";
                return false;
            }

            NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:url];
            urlRequest.HTTPMethod = [NSString stringWithUTF8String:request.method.c_str()];
            for (const auto& [name, value] : request.headers) {
                [urlRequest setValue:[NSString stringWithUTF8String:value.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];
            }
            if (!request.body.empty()) urlRequest.HTTPBody = [NSData dataWithBytes:request.body.data() length:request.body.size()];
            if (request.timeout >= 0) urlRequest.timeoutInterval = request.timeout;

            LuwowHttpReceiver* receiver = [[LuwowHttpReceiver alloc] init];
            receiver.done = dispatch_semaphore_create(0);
            receiver.body = [NSMutableData data];
            receiver.saveTo = request.saveTo.empty() ? nullptr : request.saveTo.c_str();

            // A session of its own, since the delegate is the session's; it's let go once the request is done
            NSURLSession* session = [NSURLSession sessionWithConfiguration:[NSURLSessionConfiguration defaultSessionConfiguration]
                                                                  delegate:receiver
                                                             delegateQueue:nil];
            NSURLSessionDataTask* task = [session dataTaskWithRequest:urlRequest];
            [task resume];
            dispatch_semaphore_wait(receiver.done, DISPATCH_TIME_FOREVER);
            [session finishTasksAndInvalidate];

            if (receiver.fileFailed) {
                if (!request.saveTo.empty()) std::remove(request.saveTo.c_str());
                error = "could not write the file to save to";
                return false;
            }
            NSURLResponse* answer = receiver.answer;
            if (receiver.failure || ![answer isKindOfClass:[NSHTTPURLResponse class]]) {
                if (!request.saveTo.empty() && receiver.answer) std::remove(request.saveTo.c_str());
                error = receiver.failure ? std::string(receiver.failure.localizedDescription.UTF8String) : "no response";
                return false;
            }

            NSHTTPURLResponse* http = (NSHTTPURLResponse*)answer;
            response.status = static_cast<int>(http.statusCode);
            response.statusMessage = [NSHTTPURLResponse localizedStringForStatusCode:http.statusCode].UTF8String;
            for (NSString* name in http.allHeaderFields) {
                NSString* value = [http.allHeaderFields[name] description];
                response.headers.emplace_back(name.UTF8String, value.UTF8String);
            }
            response.bodyLength = static_cast<size_t>(receiver.received);
            NSData* body = receiver.body;
            if (body.length > 0) response.body.assign(static_cast<const char*>(body.bytes), body.length);
            return true;
        }
    }
} // namespace Luwow::Net
